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
#include "test_support.hpp"

using namespace facility_dependency_registry;
using fdep_test::asset;
using fdep_test::feed;
using fdep_test::Harness;
using fdep_test::rack;
using fdep_test::service;

namespace {

/// feed -> asset-0 -> asset-1 -> ... with an extra soft branch, so that the
/// hard cone and the all-strength cone differ.
void build_fan(Harness& harness, int count) {
  for (int index = 0; index < count; ++index) {
    if (index == 0) {
      harness.add(asset("node-0"), feed("f1"), DependencyKind::RequiresPowerFrom, DependencyStrength::Hard);
    } else {
      harness.add(asset("node-" + std::to_string(index)), asset("node-" + std::to_string(index - 1)),
                  DependencyKind::ControlDependsOn, DependencyStrength::Hard);
    }
  }
  harness.add(asset("soft-1"), asset("node-1"), DependencyKind::ControlDependsOn, DependencyStrength::Soft);
  harness.add(asset("soft-2"), asset("soft-1"), DependencyKind::ControlDependsOn, DependencyStrength::Soft);
}

}  // namespace

FDEP_TEST(impact, hard_cone_stops_at_a_soft_edge) {
  Harness harness = Harness::ephemeral();
  build_fan(harness, 4);

  ImpactConeRequest request;
  request.origin = feed("f1");
  request.max_depth = 16;
  request.max_nodes = 256;
  const auto cone = harness.snapshot().impact_cone(request);
  FDEP_REQUIRE_OK(cone);
  FDEP_CHECK(!cone.value().truncated);
  FDEP_CHECK(cone.value().stop == TraversalStop::Complete);

  FDEP_CHECK(cone.value().reached(asset("node-0")));
  FDEP_CHECK(cone.value().reached(asset("node-3")));
  // The soft branch is not part of a hard impact cone.
  FDEP_CHECK(!cone.value().reached(asset("soft-1")));
  FDEP_CHECK(!cone.value().reached(asset("soft-2")));
  FDEP_CHECK_EQ(cone.value().size(), std::size_t{4});
  FDEP_CHECK_EQ(cone.value().depth_of(asset("node-0")).value(), 1u);
  FDEP_CHECK_EQ(cone.value().depth_of(asset("node-3")).value(), 4u);
  FDEP_CHECK_EQ(cone.value().depth_of(feed("f1")).value(), 0u);
  FDEP_CHECK(!cone.value().depth_of(service("nothing")).has_value());
}

FDEP_TEST(impact, widening_the_strength_set_widens_the_cone) {
  Harness harness = Harness::ephemeral();
  build_fan(harness, 4);

  ImpactConeRequest request;
  request.origin = feed("f1");
  request.strengths = DependencyStrengthMask::all();
  request.max_depth = 16;
  request.max_nodes = 256;
  const auto cone = harness.snapshot().impact_cone(request);
  FDEP_REQUIRE_OK(cone);
  FDEP_CHECK(cone.value().reached(asset("soft-1")));
  FDEP_CHECK(cone.value().reached(asset("soft-2")));
  FDEP_CHECK_EQ(cone.value().size(), std::size_t{6});
}

FDEP_TEST(impact, advisory_edges_are_excluded_by_default) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("node-0"), feed("f1"), DependencyKind::RequiresPowerFrom);
  harness.add(asset("advisory-1"), asset("node-0"), DependencyKind::ControlDependsOn,
              DependencyStrength::Advisory);
  harness.add(asset("hard-1"), asset("node-0"), DependencyKind::ControlDependsOn, DependencyStrength::Hard);

  ImpactConeRequest request;
  request.origin = feed("f1");
  request.max_depth = 8;
  request.max_nodes = 64;
  const auto cone = harness.snapshot().impact_cone(request);
  FDEP_REQUIRE_OK(cone);
  FDEP_CHECK(cone.value().reached(asset("hard-1")));
  FDEP_CHECK(!cone.value().reached(asset("advisory-1")));
}

FDEP_TEST(impact, kind_filter_restricts_the_cone) {
  Harness harness = Harness::ephemeral();
  harness.add(rack("r1"), feed("f1"), DependencyKind::RequiresPowerFrom);
  harness.add(asset("node-1"), rack("r1"), DependencyKind::HousedIn);
  harness.add(service("s1"), rack("r1"), DependencyKind::HousedIn);

  ImpactConeRequest request;
  request.origin = feed("f1");
  request.max_depth = 8;
  request.max_nodes = 64;

  request.kinds = DependencyKindMask::of(DependencyKind::RequiresPowerFrom);
  const auto power_only = harness.snapshot().impact_cone(request);
  FDEP_REQUIRE_OK(power_only);
  FDEP_CHECK_EQ(power_only.value().size(), std::size_t{1});
  FDEP_CHECK(power_only.value().reached(rack("r1")));
  FDEP_CHECK(!power_only.value().reached(asset("node-1")));

  request.kinds = DependencyKindMask::of(DependencyKind::HousedIn);
  const auto housing_only = harness.snapshot().impact_cone(request);
  FDEP_REQUIRE_OK(housing_only);
  FDEP_CHECK(housing_only.value().entries.empty());

  request.kinds = DependencyKindMask::all();
  const auto everything = harness.snapshot().impact_cone(request);
  FDEP_REQUIRE_OK(everything);
  FDEP_CHECK_EQ(everything.value().size(), std::size_t{3});
  FDEP_CHECK(everything.value().reached(asset("node-1")));
  FDEP_CHECK(everything.value().reached(service("s1")));
}

FDEP_TEST(impact, bounds_and_rejections_are_explicit) {
  Harness harness = Harness::ephemeral();
  build_fan(harness, 6);

  ImpactConeRequest request;
  request.origin = feed("f1");
  request.max_depth = 2;
  request.max_nodes = 256;
  const auto shallow = harness.snapshot().impact_cone(request);
  FDEP_REQUIRE_OK(shallow);
  FDEP_CHECK(shallow.value().truncated);
  FDEP_CHECK(shallow.value().stop == TraversalStop::DepthLimit);
  FDEP_CHECK_EQ(shallow.value().size(), std::size_t{2});

  request.max_depth = 64;
  request.max_nodes = 3;
  const auto narrow = harness.snapshot().impact_cone(request);
  FDEP_REQUIRE_OK(narrow);
  FDEP_CHECK(narrow.value().truncated);
  FDEP_CHECK(narrow.value().stop == TraversalStop::NodeLimit);
  FDEP_CHECK_EQ(narrow.value().size(), std::size_t{3});

  request.max_depth = 0;
  request.max_nodes = 3;
  FDEP_CHECK_CODE(harness.snapshot().impact_cone(request), ErrorCode::RequestLimitExceeded);

  request.max_depth = 4;
  request.max_nodes = 16;
  request.origin = DependencyNodeRef{};
  FDEP_CHECK_CODE(harness.snapshot().impact_cone(request), ErrorCode::InvalidNodeReference);

  // An origin the graph has never seen has an empty cone, not an error.
  request.origin = asset("unknown-origin");
  const auto empty = harness.snapshot().impact_cone(request);
  FDEP_REQUIRE_OK(empty);
  FDEP_CHECK(empty.value().entries.empty());
  FDEP_CHECK(empty.value().stop == TraversalStop::Complete);
}

FDEP_TEST(impact, results_are_deterministic_across_repeated_runs) {
  Harness harness = Harness::ephemeral();
  build_fan(harness, 8);
  ImpactConeRequest request;
  request.origin = feed("f1");
  request.strengths = DependencyStrengthMask::all();
  request.max_depth = 32;
  request.max_nodes = 512;

  const auto first = harness.snapshot().impact_cone(request);
  FDEP_REQUIRE_OK(first);
  for (int attempt = 0; attempt < 5; ++attempt) {
    const auto again = harness.snapshot().impact_cone(request);
    FDEP_REQUIRE_OK(again);
    FDEP_CHECK_EQ(again.value().entries.size(), first.value().entries.size());
    for (std::size_t index = 0; index < first.value().entries.size(); ++index) {
      FDEP_CHECK(again.value().entries[index].node == first.value().entries[index].node);
      FDEP_CHECK_EQ(again.value().entries[index].depth, first.value().entries[index].depth);
      FDEP_CHECK_EQ(again.value().entries[index].via_edge.value(),
                    first.value().entries[index].via_edge.value());
    }
  }
}
