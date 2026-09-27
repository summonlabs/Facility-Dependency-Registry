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
using fdep_test::loop;
using fdep_test::rack;
using fdep_test::service;

namespace {

PathRequest request_for(const DependencyNodeRef& from, const DependencyNodeRef& to,
                        TraversalDirection direction = TraversalDirection::Dependencies) {
  PathRequest request;
  request.from = from;
  request.to = to;
  request.direction = direction;
  request.max_depth = 16;
  request.max_nodes = 256;
  return request;
}

}  // namespace

FDEP_TEST(paths, a_chain_is_explained_hop_by_hop) {
  Harness harness = Harness::ephemeral();
  const DependencyEdgeId first = harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const DependencyEdgeId second = harness.add(feed("f1"), feed("f2"), DependencyKind::RequiresPowerFrom);

  const auto result = harness.snapshot().explain_path(request_for(asset("a"), feed("f2")));
  FDEP_REQUIRE_OK(result);
  FDEP_CHECK(result.value().found);
  FDEP_REQUIRE(result.value().steps.size() == 2);
  FDEP_CHECK_EQ(result.value().hops(), std::size_t{2});
  FDEP_CHECK_EQ(result.value().steps[0].edge.value(), first.value());
  FDEP_CHECK(result.value().steps[0].from == asset("a"));
  FDEP_CHECK(result.value().steps[0].to == feed("f1"));
  FDEP_CHECK(result.value().steps[0].kind == DependencyKind::RequiresPowerFrom);
  FDEP_CHECK(result.value().steps[0].strength == DependencyStrength::Hard);
  FDEP_CHECK(result.value().steps[0].lifecycle == LifecycleState::Active);
  FDEP_CHECK_EQ(result.value().steps[1].edge.value(), second.value());
  FDEP_CHECK(result.value().steps[1].from == feed("f1"));
  FDEP_CHECK(result.value().steps[1].to == feed("f2"));
  FDEP_CHECK(!result.value().steps[0].to_text().empty());
}

FDEP_TEST(paths, the_shortest_path_is_reported) {
  Harness harness = Harness::ephemeral();
  // a -> b -> c -> d and a -> d directly: the direct edge wins.
  harness.add(asset("a"), asset("b"), DependencyKind::ControlDependsOn);
  harness.add(asset("b"), asset("c"), DependencyKind::ControlDependsOn);
  harness.add(asset("c"), asset("d"), DependencyKind::ControlDependsOn);
  const DependencyEdgeId direct = harness.add(asset("a"), asset("d"), DependencyKind::ControlDependsOn);

  const auto result = harness.snapshot().explain_path(request_for(asset("a"), asset("d")));
  FDEP_REQUIRE_OK(result);
  FDEP_CHECK(result.value().found);
  FDEP_REQUIRE(result.value().steps.size() == 1);
  FDEP_CHECK_EQ(result.value().steps[0].edge.value(), direct.value());
}

FDEP_TEST(paths, ties_are_broken_deterministically) {
  Harness harness = Harness::ephemeral();
  // Two two-hop routes from a to d: through b (registered first) and through c.
  harness.add(asset("a"), asset("c"), DependencyKind::ControlDependsOn);
  harness.add(asset("a"), asset("b"), DependencyKind::ControlDependsOn);
  const DependencyEdgeId via_b = harness.add(asset("b"), asset("d"), DependencyKind::ControlDependsOn);
  harness.add(asset("c"), asset("d"), DependencyKind::ControlDependsOn);

  const auto first = harness.snapshot().explain_path(request_for(asset("a"), asset("d")));
  FDEP_REQUIRE_OK(first);
  FDEP_CHECK(first.value().found);
  FDEP_REQUIRE(first.value().steps.size() == 2);
  // The layer is ordered canonically, so b is expanded before c regardless of
  // registration order.
  FDEP_CHECK(first.value().steps[1].from == asset("b"));
  FDEP_CHECK_EQ(first.value().steps[1].edge.value(), via_b.value());

  for (int attempt = 0; attempt < 4; ++attempt) {
    const auto again = harness.snapshot().explain_path(request_for(asset("a"), asset("d")));
    FDEP_REQUIRE_OK(again);
    FDEP_REQUIRE(again.value().steps.size() == first.value().steps.size());
    for (std::size_t index = 0; index < again.value().steps.size(); ++index) {
      FDEP_CHECK_EQ(again.value().steps[index].edge.value(), first.value().steps[index].edge.value());
    }
  }
}

FDEP_TEST(paths, a_missing_path_is_an_answer_not_an_error) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), asset("b"), DependencyKind::ControlDependsOn);
  harness.add(asset("c"), asset("d"), DependencyKind::ControlDependsOn);

  const auto result = harness.snapshot().explain_path(request_for(asset("a"), asset("d")));
  FDEP_REQUIRE_OK(result);
  FDEP_CHECK(!result.value().found);
  FDEP_CHECK(result.value().steps.empty());
  FDEP_CHECK(result.value().stop == TraversalStop::Complete);

  // An endpoint the graph has never seen behaves the same way.
  const auto unknown = harness.snapshot().explain_path(request_for(asset("a"), asset("never")));
  FDEP_REQUIRE_OK(unknown);
  FDEP_CHECK(!unknown.value().found);
}

FDEP_TEST(paths, a_node_reaches_itself_with_an_empty_path) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), asset("b"), DependencyKind::ControlDependsOn);
  const auto result = harness.snapshot().explain_path(request_for(asset("a"), asset("a")));
  FDEP_REQUIRE_OK(result);
  FDEP_CHECK(result.value().found);
  FDEP_CHECK(result.value().steps.empty());
}

FDEP_TEST(paths, the_reverse_direction_explains_impact) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  harness.add(asset("b"), asset("a"), DependencyKind::ControlDependsOn);

  // Reading forwards from the feed asks what the feed depends on: nothing.
  const auto forward = harness.snapshot().explain_path(request_for(feed("f1"), asset("b")));
  FDEP_REQUIRE_OK(forward);
  FDEP_CHECK(!forward.value().found);

  // Reading backwards from the same pair asks what depends on the feed, and
  // explains the impact chain hop by hop.
  const auto backward = harness.snapshot().explain_path(
      request_for(feed("f1"), asset("b"), TraversalDirection::Dependents));
  FDEP_REQUIRE_OK(backward);
  FDEP_CHECK(backward.value().found);
  FDEP_CHECK_EQ(backward.value().hops(), std::size_t{2});
  FDEP_CHECK(backward.value().steps[0].to == asset("a"));
  FDEP_CHECK(backward.value().steps[1].to == asset("b"));
}

FDEP_TEST(paths, depth_and_node_bounds_are_reported) {
  Harness harness = Harness::ephemeral();
  for (int index = 0; index < 6; ++index) {
    harness.add(asset("node-" + std::to_string(index)),
                index == 0 ? feed("f1") : asset("node-" + std::to_string(index - 1)),
                index == 0 ? DependencyKind::RequiresPowerFrom : DependencyKind::ControlDependsOn);
  }

  PathRequest request = request_for(feed("f1"), asset("node-5"), TraversalDirection::Dependents);
  request.max_depth = 2;
  const auto shallow = harness.snapshot().explain_path(request);
  FDEP_REQUIRE_OK(shallow);
  FDEP_CHECK(!shallow.value().found);
  FDEP_CHECK(shallow.value().stop == TraversalStop::DepthLimit);

  request.max_depth = 16;
  request.max_nodes = 2;
  const auto narrow = harness.snapshot().explain_path(request);
  FDEP_REQUIRE_OK(narrow);
  FDEP_CHECK(!narrow.value().found);
  FDEP_CHECK(narrow.value().stop == TraversalStop::NodeLimit);

  request.max_nodes = 256;
  const auto complete = harness.snapshot().explain_path(request);
  FDEP_REQUIRE_OK(complete);
  FDEP_CHECK(complete.value().found);
  FDEP_CHECK_EQ(complete.value().hops(), std::size_t{6});

  request.max_depth = 0;
  FDEP_CHECK_CODE(harness.snapshot().explain_path(request), ErrorCode::RequestLimitExceeded);

  PathRequest invalid = request_for(DependencyNodeRef{}, asset("node-0"));
  FDEP_CHECK_CODE(harness.snapshot().explain_path(invalid), ErrorCode::InvalidNodeReference);

  PathRequest bad_direction = request_for(feed("f1"), asset("node-0"));
  bad_direction.direction = static_cast<TraversalDirection>(0);
  FDEP_CHECK_CODE(harness.snapshot().explain_path(bad_direction), ErrorCode::InvalidArguments);
}

FDEP_TEST(paths, filters_apply_to_the_explanation) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), asset("b"), DependencyKind::ControlDependsOn, DependencyStrength::Soft);
  harness.add(asset("b"), asset("c"), DependencyKind::ControlDependsOn, DependencyStrength::Hard);

  PathRequest request = request_for(asset("a"), asset("c"));
  request.filter.strengths = DependencyStrengthMask::of(DependencyStrength::Hard);
  const auto hard_only = harness.snapshot().explain_path(request);
  FDEP_REQUIRE_OK(hard_only);
  FDEP_CHECK(!hard_only.value().found);

  request.filter.strengths = DependencyStrengthMask::all();
  const auto all = harness.snapshot().explain_path(request);
  FDEP_REQUIRE_OK(all);
  FDEP_CHECK(all.value().found);
}

FDEP_TEST(paths, a_cycle_does_not_hang_the_search) {
  Harness harness = Harness::ephemeral();
  constexpr int kRingSize = 512;
  for (int index = 0; index < kRingSize; ++index) {
    harness.add(asset("node-" + std::to_string(index)),
                asset("node-" + std::to_string((index + 1) % kRingSize)),
                DependencyKind::ControlDependsOn);
  }
  PathRequest request = request_for(asset("node-0"), asset("node-0"));
  const auto self = harness.snapshot().explain_path(request);
  FDEP_REQUIRE_OK(self);
  FDEP_CHECK(self.value().found);
  FDEP_CHECK(self.value().steps.empty());

  request = request_for(asset("node-0"), asset("node-511"), TraversalDirection::Dependents);
  request.max_depth = 64;
  request.max_nodes = 4096;
  const auto around_the_ring = harness.snapshot().explain_path(request);
  FDEP_REQUIRE_OK(around_the_ring);
  FDEP_CHECK(around_the_ring.value().found);
  FDEP_CHECK_EQ(around_the_ring.value().hops(), std::size_t{1});
}
