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

#include <string>

#include "facility_dependency_registry/facility_dependency_registry.hpp"
#include "test_harness.hpp"

using namespace facility_dependency_registry;

FDEP_TEST(lifecycle, states_and_tokens) {
  const LifecycleState states[] = {LifecycleState::Proposed, LifecycleState::Active, LifecycleState::Suspended,
                                   LifecycleState::Retired};
  for (const auto state : states) {
    const std::string token{to_token(state)};
    FDEP_CHECK(!token.empty());
    FDEP_CHECK(!describe(state).empty());
    const auto parsed = parse_lifecycle_state(token);
    FDEP_REQUIRE(parsed.has_value());
    FDEP_CHECK(*parsed == state);
  }
  FDEP_CHECK(!parse_lifecycle_state("").has_value());
  FDEP_CHECK(!parse_lifecycle_state("Active").has_value());
  FDEP_CHECK(!parse_lifecycle_state("deleted").has_value());
  FDEP_CHECK_EQ(to_token(static_cast<LifecycleState>(0)), std::string{"unknown-lifecycle-state"});
  FDEP_CHECK_EQ(to_token(static_cast<LifecycleState>(9)), std::string{"unknown-lifecycle-state"});
}

FDEP_TEST(lifecycle, only_active_is_in_force) {
  FDEP_CHECK(is_in_force(LifecycleState::Active));
  FDEP_CHECK(!is_in_force(LifecycleState::Proposed));
  FDEP_CHECK(!is_in_force(LifecycleState::Suspended));
  FDEP_CHECK(!is_in_force(LifecycleState::Retired));
}

FDEP_TEST(lifecycle, transition_table_is_exactly_as_documented) {
  using S = LifecycleState;
  FDEP_CHECK(transition_allowed(S::Proposed, S::Active));
  FDEP_CHECK(transition_allowed(S::Proposed, S::Retired));
  FDEP_CHECK(!transition_allowed(S::Proposed, S::Suspended));
  FDEP_CHECK(!transition_allowed(S::Proposed, S::Proposed));

  FDEP_CHECK(transition_allowed(S::Active, S::Suspended));
  FDEP_CHECK(transition_allowed(S::Active, S::Retired));
  FDEP_CHECK(!transition_allowed(S::Active, S::Proposed));
  FDEP_CHECK(!transition_allowed(S::Active, S::Active));

  FDEP_CHECK(transition_allowed(S::Suspended, S::Active));
  FDEP_CHECK(transition_allowed(S::Suspended, S::Retired));
  FDEP_CHECK(!transition_allowed(S::Suspended, S::Proposed));
  FDEP_CHECK(!transition_allowed(S::Suspended, S::Suspended));

  FDEP_CHECK(!transition_allowed(S::Retired, S::Active));
  FDEP_CHECK(!transition_allowed(S::Retired, S::Proposed));
  FDEP_CHECK(!transition_allowed(S::Retired, S::Suspended));
  FDEP_CHECK(!transition_allowed(S::Retired, S::Retired));

  // Out-of-domain values never transition anywhere.
  FDEP_CHECK(!transition_allowed(static_cast<S>(0), S::Active));
  FDEP_CHECK(!transition_allowed(S::Active, static_cast<S>(0)));
  FDEP_CHECK(!transition_allowed(static_cast<S>(9), static_cast<S>(9)));
}

FDEP_TEST(lifecycle, retirement_is_terminal) {
  FDEP_CHECK(is_terminal(LifecycleState::Retired));
  FDEP_CHECK(!is_terminal(LifecycleState::Active));
  FDEP_CHECK(!is_terminal(LifecycleState::Suspended));
  FDEP_CHECK(!is_terminal(LifecycleState::Proposed));
}

FDEP_TEST(lifecycle, masks_enumerate_in_value_order) {
  const LifecycleMask every = LifecycleMask::all();
  FDEP_CHECK_EQ(every.count(), std::size_t{4});
  FDEP_CHECK(every.contains(LifecycleState::Proposed));
  FDEP_CHECK(every.contains(LifecycleState::Retired));

  const LifecycleMask one = LifecycleMask::of(LifecycleState::Active);
  FDEP_CHECK_EQ(one.count(), std::size_t{1});
  FDEP_CHECK_EQ(one.bits(), std::uint32_t{0b0010});
  FDEP_CHECK(one.contains_any(every));
  FDEP_CHECK(!one.contains_all(every));
  FDEP_CHECK(every.contains_all(one));

  std::string order;
  every.for_each([&order](LifecycleState state) {
    if (!order.empty()) {
      order.push_back(',');
    }
    order.append(to_token(state));
  });
  FDEP_CHECK_EQ(order, std::string{"proposed,active,suspended,retired"});
  FDEP_CHECK_EQ(to_token_list(every, &to_token), order);
  FDEP_CHECK_EQ(to_token_list(LifecycleMask::none(), &to_token), std::string{"-"});

  // Values outside the domain are ignored rather than shifting the mask.
  FDEP_CHECK_EQ(LifecycleMask::of(static_cast<LifecycleState>(0)).bits(), std::uint32_t{0});
  FDEP_CHECK_EQ(LifecycleMask::of(static_cast<LifecycleState>(200)).bits(), std::uint32_t{0});
  FDEP_CHECK_EQ(LifecycleMask::from_bits(0xFFFFFFFFu), LifecycleMask::all());
}
