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

#include <algorithm>
#include <string>
#include <vector>

#include "facility_dependency_registry/facility_dependency_registry.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

using namespace facility_dependency_registry;
using fdep_test::asi;
using fdep_test::asset;
using fdep_test::feed;
using fdep_test::Harness;
using fdep_test::loop;
using fdep_test::rack;
using fdep_test::service;

FDEP_TEST(query, direct_lookups_are_canonically_ordered) {
  Harness harness = Harness::ephemeral();
  // One node with three dependents, registered in a deliberately scrambled
  // order.
  harness.add(asset("z"), rack("r1"), DependencyKind::HousedIn);
  harness.add(asset("a"), rack("r1"), DependencyKind::HousedIn);
  harness.add(service("m"), rack("r1"), DependencyKind::HousedIn);

  const auto dependents = harness.snapshot().direct_dependents(rack("r1"));
  FDEP_REQUIRE_OK(dependents);
  FDEP_REQUIRE(dependents.value().size() == 3);
  // Canonical order is by source reference: domain ordinal first, then the
  // identifier bytes, so both assets come before the facility service.
  FDEP_CHECK(dependents.value()[0].source() == asset("a"));
  FDEP_CHECK(dependents.value()[1].source() == asset("z"));
  FDEP_CHECK(dependents.value()[2].source() == service("m"));

  const auto dependencies = harness.snapshot().direct_dependencies(asset("a"));
  FDEP_REQUIRE_OK(dependencies);
  FDEP_REQUIRE(dependencies.value().size() == 1);
  FDEP_CHECK(dependencies.value()[0].target() == rack("r1"));

  // A node the graph has never seen answers with an empty list, not an error.
  const auto unknown = harness.snapshot().direct_dependencies(asset("nope"));
  FDEP_REQUIRE_OK(unknown);
  FDEP_CHECK(unknown.value().empty());

  // An invalid reference is rejected rather than treated as absent.
  const auto invalid = harness.snapshot().direct_dependencies(DependencyNodeRef{});
  FDEP_CHECK_CODE(invalid, ErrorCode::InvalidNodeReference);
}

FDEP_TEST(query, filters_select_by_kind_strength_and_lifecycle) {
  Harness harness = Harness::ephemeral();
  const DependencyEdgeId power = harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom,
                                             DependencyStrength::Hard);
  const DependencyEdgeId served = harness.add(asset("a"), service("s1"), DependencyKind::ServedBy,
                                              DependencyStrength::Advisory);
  harness.add(asset("a"), loop("l1"), DependencyKind::CooledBy, DependencyStrength::Soft);

  EdgeFilter power_only;
  power_only.kinds = DependencyKindMask::of(DependencyKind::RequiresPowerFrom);
  const auto filtered = harness.snapshot().direct_dependencies(asset("a"), power_only);
  FDEP_REQUIRE_OK(filtered);
  FDEP_REQUIRE(filtered.value().size() == 1);
  FDEP_CHECK_EQ(filtered.value()[0].id().value(), power.value());

  EdgeFilter hard_only;
  hard_only.strengths = DependencyStrengthMask::of(DependencyStrength::Hard);
  const auto hard = harness.snapshot().direct_dependencies(asset("a"), hard_only);
  FDEP_REQUIRE_OK(hard);
  FDEP_REQUIRE(hard.value().size() == 1);
  FDEP_CHECK_EQ(hard.value()[0].id().value(), power.value());

  // Suspending an edge removes it from the default filter but not from the
  // unfiltered reading.
  FDEP_REQUIRE(harness.transition(served, kInitialRevision, LifecycleState::Suspended).ok());
  const auto in_force = harness.snapshot().direct_dependencies(asset("a"));
  FDEP_REQUIRE_OK(in_force);
  FDEP_CHECK_EQ(in_force.value().size(), std::size_t{2});
  const auto everything = harness.snapshot().direct_dependencies(asset("a"), EdgeFilter::any());
  FDEP_REQUIRE_OK(everything);
  FDEP_CHECK_EQ(everything.value().size(), std::size_t{3});

  EdgeFilter suspended;
  suspended.lifecycles = LifecycleMask::of(LifecycleState::Suspended);
  const auto suspended_only = harness.snapshot().direct_dependencies(asset("a"), suspended);
  FDEP_REQUIRE_OK(suspended_only);
  FDEP_REQUIRE(suspended_only.value().size() == 1);
  FDEP_CHECK_EQ(suspended_only.value()[0].id().value(), served.value());

  // An empty kind mask matches nothing rather than everything.
  EdgeFilter none;
  none.kinds = DependencyKindMask::none();
  const auto empty = harness.snapshot().direct_dependencies(asset("a"), none);
  FDEP_REQUIRE_OK(empty);
  FDEP_CHECK(empty.value().empty());
}

FDEP_TEST(query, direct_lookup_result_bound_is_enforced) {
  RegistryLimits limits;
  limits.max_direct_results = 3;
  Harness harness = Harness::ephemeral(limits);
  for (int index = 0; index < 5; ++index) {
    harness.add(asset("node-" + std::to_string(index)), rack("r1"), DependencyKind::HousedIn);
  }
  const auto result = harness.snapshot().direct_dependents(rack("r1"));
  FDEP_CHECK_CODE(result, ErrorCode::QueryResultLimitExceeded);
}

FDEP_TEST(query, nodes_are_reported_in_canonical_order_and_include_declarations) {
  Harness harness = Harness::ephemeral();
  harness.declare(asset("declared-only"));
  harness.add(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);

  const auto nodes = harness.snapshot().nodes();
  FDEP_REQUIRE(nodes.size() == 3);
  FDEP_CHECK(nodes[0] == asset("b"));
  FDEP_CHECK(nodes[1] == asset("declared-only"));
  FDEP_CHECK(nodes[2] == feed("f1"));
  for (std::size_t index = 1; index < nodes.size(); ++index) {
    FDEP_CHECK(nodes[index - 1] < nodes[index]);
  }
  FDEP_CHECK(harness.snapshot().has_node(asset("declared-only")));
  FDEP_CHECK(!harness.snapshot().has_node(asset("never-mentioned")));
}

FDEP_TEST(query, resolution_distinguishes_declared_unresolved_and_absent) {
  Harness harness = Harness::ephemeral();
  harness.declare(asset("a"));
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);

  const RegistrySnapshot snapshot = harness.snapshot();
  FDEP_CHECK(snapshot.resolution_of(asset("a")) == RefResolution::Declared);
  FDEP_CHECK(snapshot.resolution_of(feed("f1")) == RefResolution::Unresolved);
  FDEP_CHECK(snapshot.resolution_of(rack("r1")) == RefResolution::Absent);
  FDEP_CHECK_EQ(std::string{to_token(RefResolution::Declared)}, std::string{"declared"});
  FDEP_CHECK_EQ(std::string{to_token(RefResolution::Unresolved)}, std::string{"unresolved"});
  FDEP_CHECK_EQ(std::string{to_token(RefResolution::Absent)}, std::string{"absent"});

  const auto unresolved = snapshot.unresolved_endpoints();
  FDEP_REQUIRE_OK(unresolved);
  FDEP_CHECK_EQ(unresolved.value().size(), std::size_t{1});
  FDEP_CHECK(unresolved.value().refs()[0] == feed("f1"));

  const auto unreferenced = snapshot.unreferenced_declared_refs();
  FDEP_REQUIRE_OK(unreferenced);
  FDEP_CHECK(unreferenced.value().empty());

  harness.declare(asi("spare"));
  const auto unreferenced_after = harness.snapshot().unreferenced_declared_refs();
  FDEP_REQUIRE_OK(unreferenced_after);
  FDEP_REQUIRE(unreferenced_after.value().size() == 1);
  FDEP_CHECK(unreferenced_after.value().refs()[0] == asi("spare"));

  const auto* declaration = harness.snapshot().find_declared_ref(asset("a"));
  FDEP_REQUIRE(declaration != nullptr);
  FDEP_CHECK_EQ(declaration->declared_generation().value(), std::uint64_t{1});
  FDEP_CHECK(!declaration->to_text().empty());
  FDEP_CHECK(harness.snapshot().find_declared_ref(asset("never")) == nullptr);
}

FDEP_TEST(query, find_edge_by_identity_and_by_key_agree) {
  Harness harness = Harness::ephemeral();
  const DependencyEdgeId id = harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const RegistrySnapshot snapshot = harness.snapshot();

  const auto* by_id = snapshot.find_edge(id);
  const auto* by_key = snapshot.find_edge(EdgeKey{asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom});
  FDEP_REQUIRE(by_id != nullptr);
  FDEP_REQUIRE(by_key != nullptr);
  FDEP_CHECK(by_id == by_key);

  FDEP_CHECK(snapshot.find_edge(DependencyEdgeId::from_value(99)) == nullptr);
  FDEP_CHECK(snapshot.find_edge(EdgeKey{asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom}) == nullptr);
  // The same endpoints with a different kind are a different edge.
  FDEP_CHECK(snapshot.find_edge(EdgeKey{asset("a"), feed("f1"), DependencyKind::CooledBy}) == nullptr);

  const EdgeKey key{asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom};
  FDEP_CHECK(key.to_text().find("requires-power-from") != std::string::npos);
  FDEP_CHECK(!key.to_text().empty());
  const EdgeKey other_feed{asset("a"), feed("f2"), DependencyKind::RequiresPowerFrom};
  const EdgeKey same_key{asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom};
  FDEP_CHECK(key == same_key);
  FDEP_CHECK(key != other_feed);
}

FDEP_TEST(query, records_render_and_compare_structurally) {
  Harness harness = Harness::ephemeral();
  const DependencyEdgeId id = harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const DependencyEdgeRecord* record = harness.snapshot().find_edge(id);
  FDEP_REQUIRE(record != nullptr);

  DependencyEdgeRecord copy = *record;
  FDEP_CHECK(copy == *record);
  FDEP_CHECK(!(copy < *record));
  FDEP_CHECK(!(*record < copy));

  const std::string text = record->to_text();
  FDEP_CHECK(text.find("edge id=1") != std::string::npos);
  FDEP_CHECK(text.find("kind=requires-power-from") != std::string::npos);
  FDEP_CHECK(text.find("source=asset:a") != std::string::npos);
  FDEP_CHECK(text.find("target=electrical-domain:f1") != std::string::npos);
  FDEP_CHECK(text.find("constraints=none") != std::string::npos);
  FDEP_CHECK(text.find("provenance={") != std::string::npos);

  const DependencyEdgeRecord empty;
  FDEP_CHECK(empty != *record);
  FDEP_CHECK(!empty.to_text().empty());
}
