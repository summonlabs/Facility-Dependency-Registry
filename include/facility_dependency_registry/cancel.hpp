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

#ifndef FACILITY_DEPENDENCY_REGISTRY_CANCEL_HPP
#define FACILITY_DEPENDENCY_REGISTRY_CANCEL_HPP

#include <atomic>
#include <memory>

namespace facility_dependency_registry {

/// A cooperative cancellation token.
///
/// A default constructed token is never cancelled and owns no shared state.
/// Tokens produced by a CancellationSource share one flag, are cheap to copy
/// and are safe to poll from many threads.
///
/// Cancellation is observed at defined points: between traversal expansion
/// steps and immediately before a mutation commits. A cancelled request
/// returns the Cancelled category and publishes nothing. A mutation that has
/// already crossed its commit boundary cannot be cancelled, and reports its
/// committed outcome instead.
class CancellationToken {
 public:
  CancellationToken() noexcept = default;

  [[nodiscard]] bool cancelled() const noexcept {
    return flag_ != nullptr && flag_->load(std::memory_order_acquire);
  }

  /// True when this token is connected to a source and could ever be cancelled.
  [[nodiscard]] bool cancellable() const noexcept { return flag_ != nullptr; }

 private:
  friend class CancellationSource;
  explicit CancellationToken(std::shared_ptr<std::atomic<bool>> flag) noexcept : flag_(std::move(flag)) {}

  std::shared_ptr<std::atomic<bool>> flag_{};
};

/// The cancelling half of a CancellationToken.
class CancellationSource {
 public:
  CancellationSource() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

  [[nodiscard]] CancellationToken token() const noexcept { return CancellationToken{flag_}; }

  /// Idempotent and thread safe. Cancellation is one-way: there is no reset.
  void cancel() noexcept { flag_->store(true, std::memory_order_release); }

  [[nodiscard]] bool cancelled() const noexcept { return flag_->load(std::memory_order_acquire); }

 private:
  std::shared_ptr<std::atomic<bool>> flag_;
};

}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_CANCEL_HPP
