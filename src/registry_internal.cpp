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

#include "registry_internal.hpp"

#include <string>
#include <utility>

namespace facility_dependency_registry {
namespace detail {

RegistrySnapshot RegistryImpl::snapshot() const {
  auto snapshot = make_snapshot(state.load(std::memory_order_acquire));
  if (!snapshot) {
    return RegistrySnapshot{};
  }
  return std::move(snapshot).value();
}

std::int64_t RegistryImpl::now_ms() const {
  if (clock) {
    return clock->now_unix_ms();
  }
  return 0;
}

void RegistryImpl::record(JournalEntry entry) { record_journal(*this, std::move(entry)); }

void record_journal(RegistryImpl& impl, JournalEntry entry) {
  const std::size_t bound = impl.limits.max_journal_entries;
  if (bound == 0) {
    return;
  }
  if (impl.journal.size() >= bound) {
    impl.journal.pop_front();
  }
  impl.journal.push_back(std::move(entry));
}

Status check_authority(const RegistryImpl& impl, const MutationContext& context) {
  if (impl.closed) {
    return Status::failure(ErrorCode::RegistryClosed, "the registry is closed and accepts no mutations");
  }
  const auto current = impl.state.load(std::memory_order_acquire);
  const DependencyGeneration generation = current == nullptr ? kInitialGeneration : current->generation();
  if (context.expected_generation != generation) {
    return Status::failure(ErrorCode::StaleGeneration,
                           "the mutation expected generation " +
                               facility_dependency_registry::to_text(context.expected_generation) +
                               " but the registry is at generation " +
                               facility_dependency_registry::to_text(generation));
  }
  if (context.cancellation.cancelled()) {
    return Status::failure(ErrorCode::Cancelled, "the mutation was cancelled before it was planned");
  }
  return Status::success();
}

Status commit_state(RegistryImpl& impl, std::shared_ptr<const GraphState> next) {
  if (next == nullptr) {
    return Status::failure(ErrorCode::InvalidArguments, "the next state is empty");
  }
  if (impl.durable) {
    if (impl.store == nullptr) {
      return Status::failure(ErrorCode::NotDurable, "the registry has no durable store");
    }
    auto snapshot = make_snapshot(next);
    if (!snapshot) {
      return snapshot.status();
    }
    const Status published = impl.store->publish(snapshot.value());
    if (!published.ok()) {
      // Nothing is visible to readers: the in-memory state is still the one
      // the last successful publication produced.
      return published;
    }
  }
  impl.state.store(std::move(next), std::memory_order_release);
  return Status::success();
}

Result<std::shared_ptr<const GraphState>> build_next_state(const GraphState& current,
                                                           DependencyGeneration generation,
                                                           std::uint64_t next_edge_ordinal,
                                                           std::vector<DependencyEdgeRecord> edges,
                                                           std::string_view what) {
  std::vector<DeclaredExternalRef> refs{current.declared_refs().begin(), current.declared_refs().end()};
  auto next = GraphState::build(current.limits(), generation, next_edge_ordinal, std::move(edges), std::move(refs));
  if (!next && next.error().code() == ErrorCode::ProhibitedCycle) {
    std::string detail{what};
    detail.append(" would violate the acyclic obligation: ");
    detail.append(next.error().detail());
    return Result<std::shared_ptr<const GraphState>>::failure(ErrorCode::ProhibitedCycle, std::move(detail));
  }
  return next;
}

Result<std::shared_ptr<const GraphState>> build_next_state_with_refs(const GraphState& current,
                                                                    DependencyGeneration generation,
                                                                    std::vector<DeclaredExternalRef> refs,
                                                                    std::string_view what) {
  std::vector<DependencyEdgeRecord> edges{current.edges().begin(), current.edges().end()};
  auto next = GraphState::build(current.limits(), generation, current.next_edge_ordinal(), std::move(edges),
                                std::move(refs));
  if (!next && next.error().code() == ErrorCode::ProhibitedCycle) {
    std::string detail{what};
    detail.append(" would violate the acyclic obligation: ");
    detail.append(next.error().detail());
    return Result<std::shared_ptr<const GraphState>>::failure(ErrorCode::ProhibitedCycle, std::move(detail));
  }
  return next;
}

MutationScope::MutationScope(RegistryImpl& impl, JournalOperation operation) : impl_(impl) {
  entry_.operation = operation;
  entry_.recorded_at_unix_ms = impl.now_ms();
  const auto current = impl.state.load(std::memory_order_acquire);
  entry_.generation = current == nullptr ? kInitialGeneration : current->generation();
}

MutationScope::~MutationScope() { impl_.record(std::move(entry_)); }

void MutationScope::reject(const Status& status) {
  entry_.outcome = status.code();
  entry_.detail = status.error().detail();
}

void MutationScope::reject(const RegistryError& error) {
  entry_.outcome = error.code();
  entry_.detail = error.detail();
}

void MutationScope::accept(DependencyEdgeId id, std::string detail) {
  entry_.outcome = ErrorCode::Ok;
  entry_.edge_id = id;
  entry_.detail = std::move(detail);
}

void MutationScope::accept(DependencyNodeRef node, std::string detail) {
  entry_.outcome = ErrorCode::Ok;
  entry_.node = std::move(node);
  entry_.detail = std::move(detail);
}

}  // namespace detail
}  // namespace facility_dependency_registry
