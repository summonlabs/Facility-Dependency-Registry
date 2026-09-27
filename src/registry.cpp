// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "facility_dependency_registry/registry.hpp"

#include <mutex>
#include <string>
#include <utility>

#include "registry_internal.hpp"

#include "file_ops.hpp"

namespace facility_dependency_registry {

std::string_view to_token(JournalOperation operation) noexcept {
  switch (operation) {
    case JournalOperation::Open:
      return "open";
    case JournalOperation::Close:
      return "close";
    case JournalOperation::RegisterEdge:
      return "register-edge";
    case JournalOperation::UpdateEdge:
      return "update-edge";
    case JournalOperation::TransitionEdge:
      return "transition-edge";
    case JournalOperation::RemoveEdge:
      return "remove-edge";
    case JournalOperation::DeclareRef:
      return "declare-ref";
    case JournalOperation::WithdrawRef:
      return "withdraw-ref";
    case JournalOperation::Publish:
      return "publish";
  }
  return "unknown-operation";
}

std::string JournalEntry::to_text() const {
  std::string result{"generation="};
  result.append(facility_dependency_registry::to_text(generation));
  result.append(" operation=");
  result.append(to_token(operation));
  result.append(" outcome=");
  result.append(facility_dependency_registry::to_token(outcome));
  if (is_assigned(edge_id)) {
    result.append(" edge=");
    result.append(facility_dependency_registry::to_text(edge_id));
  }
  if (node.valid()) {
    result.append(" node=");
    result.append(node.to_canonical());
  }
  result.append(" recorded-at-ms=");
  result.append(std::to_string(recorded_at_unix_ms));
  if (!detail.empty()) {
    result.append(" detail=\"");
    result.append(detail);
    result.push_back('"');
  }
  return result;
}

DependencyRegistry::DependencyRegistry() noexcept = default;
DependencyRegistry::~DependencyRegistry() {
  if (impl_ != nullptr) {
    // The destructor closes cleanly: it waits for an in-flight mutation and
    // releases the writer lock. It never publishes anything.
    std::lock_guard<std::mutex> guard(impl_->mutation_mutex);
    impl_->closed = true;
    if (impl_->store != nullptr) {
      // Releasing the writer lock cannot fail in a way this destructor could
      // report; the lock is released by the operating system regardless.
      static_cast<void>(impl_->store->close());
    }
  }
}

DependencyRegistry::DependencyRegistry(DependencyRegistry&&) noexcept = default;
DependencyRegistry& DependencyRegistry::operator=(DependencyRegistry&&) noexcept = default;

Result<DependencyRegistry> DependencyRegistry::open(const RegistryOpenRequest& request) {
  DependencyRegistry registry;
  registry.impl_ = std::make_unique<detail::RegistryImpl>();
  auto& impl = *registry.impl_;
  impl.limits = request.store.limits;
  impl.clock = request.clock != nullptr ? request.clock : std::make_shared<SystemClock>();

  if (const auto status = impl.limits.validate(); !status.ok()) {
    return Result<DependencyRegistry>::failure(status.error());
  }

  RegistrySnapshot snapshot;
  RecoveryReport report;
  auto store = DurableStore::open(request.root, request.store, snapshot, report);
  if (!store) {
    return Result<DependencyRegistry>::failure(store.error());
  }
  impl.store = std::move(store).value();
  impl.durable = true;
  impl.incarnation = impl.store->incarnation();
  impl.recovery = report;
  impl.state.store(snapshot.detail_state(), std::memory_order_release);

  JournalEntry entry;
  entry.operation = JournalOperation::Open;
  entry.generation = snapshot.generation();
  entry.outcome = ErrorCode::Ok;
  entry.recorded_at_unix_ms = impl.now_ms();
  entry.detail = std::string{"recovery="} + std::string{to_token(report.outcome)} +
                 " root=" + detail::to_utf8(request.root);
  impl.record(std::move(entry));

  return registry;
}

Result<DependencyRegistry> DependencyRegistry::open_ephemeral(const EphemeralOptions& options) {
  if (const auto status = options.limits.validate(); !status.ok()) {
    return Result<DependencyRegistry>::failure(status.error());
  }
  DependencyRegistry registry;
  registry.impl_ = std::make_unique<detail::RegistryImpl>();
  auto& impl = *registry.impl_;
  impl.limits = options.limits;
  impl.clock = options.clock != nullptr ? options.clock : std::make_shared<SystemClock>();
  impl.durable = false;

  const RegistrySnapshot snapshot = RegistrySnapshot::empty(impl.limits);
  impl.state.store(snapshot.detail_state(), std::memory_order_release);

  JournalEntry entry;
  entry.operation = JournalOperation::Open;
  entry.generation = kInitialGeneration;
  entry.outcome = ErrorCode::Ok;
  entry.recorded_at_unix_ms = impl.now_ms();
  entry.detail = "ephemeral registry";
  impl.record(std::move(entry));

  return registry;
}

DependencyGeneration DependencyRegistry::generation() const noexcept {
  if (impl_ == nullptr) {
    return kInitialGeneration;
  }
  const auto current = impl_->state.load(std::memory_order_acquire);
  return current == nullptr ? kInitialGeneration : current->generation();
}

bool DependencyRegistry::durable() const noexcept { return impl_ != nullptr && impl_->durable; }

bool DependencyRegistry::closed() const noexcept {
  if (impl_ == nullptr) {
    return true;
  }
  std::lock_guard<std::mutex> guard(impl_->mutation_mutex);
  return impl_->closed;
}

const RegistryLimits& DependencyRegistry::limits() const noexcept {
  static const RegistryLimits kFallback{};
  return impl_ == nullptr ? kFallback : impl_->limits;
}

const RecoveryReport& DependencyRegistry::recovery_report() const noexcept {
  static const RecoveryReport kFallback{};
  return impl_ == nullptr ? kFallback : impl_->recovery;
}

WriterIncarnation DependencyRegistry::writer_incarnation() const {
  return impl_ == nullptr ? WriterIncarnation{} : impl_->incarnation;
}

std::shared_ptr<const Clock> DependencyRegistry::clock() const noexcept {
  return impl_ == nullptr ? nullptr : impl_->clock;
}

RegistrySnapshot DependencyRegistry::snapshot() const {
  if (impl_ == nullptr) {
    return RegistrySnapshot{};
  }
  return impl_->snapshot();
}

std::vector<JournalEntry> DependencyRegistry::journal() const {
  if (impl_ == nullptr) {
    return {};
  }
  std::lock_guard<std::mutex> guard(impl_->mutation_mutex);
  return {impl_->journal.begin(), impl_->journal.end()};
}

Result<StoreStatus> DependencyRegistry::store_status() const {
  if (impl_ == nullptr) {
    return Result<StoreStatus>::failure(ErrorCode::RegistryClosed, "the registry is not open");
  }
  std::lock_guard<std::mutex> guard(impl_->mutation_mutex);
  if (impl_->store == nullptr) {
    return Result<StoreStatus>::failure(ErrorCode::NotDurable,
                                        "the registry keeps its state in memory only");
  }
  return impl_->store->status();
}

Status DependencyRegistry::close() {
  if (impl_ == nullptr) {
    return Status::success();
  }
  std::lock_guard<std::mutex> guard(impl_->mutation_mutex);
  if (impl_->closed) {
    return Status::success();
  }
  impl_->closed = true;
  if (impl_->store != nullptr) {
    const Status status = impl_->store->close();
    if (!status.ok()) {
      return status;
    }
  }
  JournalEntry entry;
  entry.operation = JournalOperation::Close;
  entry.generation = generation();
  entry.outcome = ErrorCode::Ok;
  entry.recorded_at_unix_ms = impl_->now_ms();
  impl_->record(std::move(entry));
  return Status::success();
}

}  // namespace facility_dependency_registry
