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

#include "facility_dependency_registry/lifecycle.hpp"

#include <array>

namespace facility_dependency_registry {
namespace {

struct LifecycleDescriptor {
  LifecycleState state;
  std::string_view token;
  std::string_view description;
};

constexpr std::array<LifecycleDescriptor, kLifecycleStateCount> kLifecycleTable{{
    {LifecycleState::Proposed, "proposed",
     "declared but not in force; visible, validated, and not traversed by default"},
    {LifecycleState::Active, "active", "in force; the only state a default traversal follows"},
    {LifecycleState::Suspended, "suspended",
     "temporarily not in force; still declared, still validated, still reserves its key"},
    {LifecycleState::Retired, "retired",
     "terminal; retained for audit, never traversed, and no longer changeable"},
}};

// from -> allowed targets, as a mask indexed by the state's ordinal minus one.
constexpr std::array<LifecycleMask, kLifecycleStateCount> kTransitionTable{{
    LifecycleMask::of(LifecycleState::Active).with(LifecycleState::Retired),
    LifecycleMask::of(LifecycleState::Suspended).with(LifecycleState::Retired),
    LifecycleMask::of(LifecycleState::Active).with(LifecycleState::Retired),
    LifecycleMask::none(),
}};

bool in_range(LifecycleState state) noexcept {
  const auto ordinal = static_cast<unsigned>(state);
  return ordinal >= 1 && ordinal <= kLifecycleStateCount;
}

}  // namespace

std::string_view to_token(LifecycleState state) noexcept {
  if (!in_range(state)) {
    return "unknown-lifecycle-state";
  }
  return kLifecycleTable[static_cast<std::size_t>(state) - 1].token;
}

std::string_view describe(LifecycleState state) noexcept {
  if (!in_range(state)) {
    return "unrecognised lifecycle state";
  }
  return kLifecycleTable[static_cast<std::size_t>(state) - 1].description;
}

std::optional<LifecycleState> parse_lifecycle_state(std::string_view token) noexcept {
  for (const auto& entry : kLifecycleTable) {
    if (entry.token == token) {
      return entry.state;
    }
  }
  return std::nullopt;
}

bool transition_allowed(LifecycleState from, LifecycleState to) noexcept {
  if (!in_range(from) || !in_range(to)) {
    return false;
  }
  return kTransitionTable[static_cast<std::size_t>(from) - 1].contains(to);
}

}  // namespace facility_dependency_registry
