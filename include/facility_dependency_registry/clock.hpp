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

#ifndef FACILITY_DEPENDENCY_REGISTRY_CLOCK_HPP
#define FACILITY_DEPENDENCY_REGISTRY_CLOCK_HPP

#include <atomic>
#include <cstdint>
#include <memory>

namespace facility_dependency_registry {

/// The only source of wall-clock time in this repository.
///
/// Time is recorded for audit. It never grants authority, never orders
/// mutations and never appears in a state digest, so a registry behaves
/// identically under a system clock and a fixed test clock.
class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock();

  /// Milliseconds since the Unix epoch. Values outside the accepted range are
  /// rejected by the caller, not repaired by the clock.
  ///
  /// A clock is read while the reading registry holds its mutation lock, so an
  /// implementation must be cheap and must not call back into the registry
  /// that is asking it for the time.
  [[nodiscard]] virtual std::int64_t now_unix_ms() const = 0;
};

/// Reads the host clock.
class SystemClock final : public Clock {
 public:
  [[nodiscard]] std::int64_t now_unix_ms() const override;
};

/// A clock that never advances on its own. Used by every deterministic test.
class FixedClock final : public Clock {
 public:
  explicit FixedClock(std::int64_t now_unix_ms = 0) noexcept : now_(now_unix_ms) {}

  [[nodiscard]] std::int64_t now_unix_ms() const override { return now_.load(std::memory_order_relaxed); }

  void set(std::int64_t now_unix_ms) noexcept { now_.store(now_unix_ms, std::memory_order_relaxed); }

  void advance(std::int64_t delta_ms) noexcept {
    now_.store(now_.load(std::memory_order_relaxed) + delta_ms, std::memory_order_relaxed);
  }

 private:
  std::atomic<std::int64_t> now_{0};
};

}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_CLOCK_HPP
