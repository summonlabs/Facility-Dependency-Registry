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

#ifndef FACILITY_DEPENDENCY_REGISTRY_LIFECYCLE_HPP
#define FACILITY_DEPENDENCY_REGISTRY_LIFECYCLE_HPP

#include <cstdint>
#include <optional>
#include <string_view>

#include "facility_dependency_registry/mask.hpp"

namespace facility_dependency_registry {

/// Lifecycle of one dependency edge.
///
/// An edge is a declaration, and the lifecycle says how much authority that
/// declaration currently carries. Values are contiguous and start at 1.
enum class LifecycleState : std::uint8_t {
  /// Declared but not in force. A proposed edge is stored, validated and
  /// visible, but no traversal follows it unless the caller asks for it.
  Proposed = 1,
  /// In force. This is the only state a default traversal follows.
  Active = 2,
  /// Temporarily not in force, but still declared and still validated. Its
  /// dependency key remains reserved and its cycles are still prohibited.
  Suspended = 3,
  /// Terminal. A retired edge remains visible for audit, cannot be updated or
  /// transitioned again, is never traversed, and reserves its key until it is
  /// removed outright.
  Retired = 4,
};

inline constexpr std::size_t kLifecycleStateCount = 4;

using LifecycleMask = EnumMask<LifecycleState, kLifecycleStateCount>;

[[nodiscard]] std::string_view to_token(LifecycleState state) noexcept;
[[nodiscard]] std::string_view describe(LifecycleState state) noexcept;
[[nodiscard]] std::optional<LifecycleState> parse_lifecycle_state(std::string_view token) noexcept;

/// True for states from which no transition is legal.
[[nodiscard]] constexpr bool is_terminal(LifecycleState state) noexcept {
  return state == LifecycleState::Retired;
}

/// The legal transition table.
///
/// | from      | to                    |
/// | --------- | --------------------- |
/// | Proposed  | Active, Retired       |
/// | Active    | Suspended, Retired    |
/// | Suspended | Active, Retired       |
/// | Retired   | -                     |
///
/// A transition to the current state is not legal either; the registry treats
/// a repeat of the current state as an idempotent no-op and reports that
/// outcome instead of silently advancing the revision.
[[nodiscard]] bool transition_allowed(LifecycleState from, LifecycleState to) noexcept;

/// True for states whose edges participate in traversal by default.
[[nodiscard]] constexpr bool is_in_force(LifecycleState state) noexcept {
  return state == LifecycleState::Active;
}

}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_LIFECYCLE_HPP
