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
#include <vector>

#include "facility_dependency_registry/facility_dependency_registry.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

using namespace facility_dependency_registry;
using fdep_test::asi;
using fdep_test::asset;
using fdep_test::dfi;
using fdep_test::feed;
using fdep_test::Harness;
using fdep_test::loop;
using fdep_test::rack;
using fdep_test::service;

namespace {

/// A chain asset-00 -> electrical-domain:f1, asset-01 -> asset-00, ... so the
/// graph has a single path of a known length.
void build_chain(Harness& harness, std::size_t length) {
  harness.add(asset("node-0"), feed("f1"), DependencyKind::RequiresPowerFrom);
  for (std::size_t index = 1; index < length; ++index) {
    harness.add(asset("node-" + std::to_string(index)), asset("node-" + std::to_string(index - 1)),
                DependencyKind::ControlDependsOn);
  }
}

}  // namespace

FDEP_TEST(traversal, closure_layers_are_deterministic) {
  Harness harness = Harness::ephemeral();
  // The root depends on two nodes, and one of those depends on a third, so the
  // walk has two layers and a deterministic order inside each of them.
  const DependencyEdgeId to_feed =
      harness.add(asset("root"), feed("f1"), DependencyKind::RequiresPowerFrom);
  harness.add(asset("root"), loop("l1"), DependencyKind::CooledBy);
  const DependencyEdgeId to_upstream =
      harness.add(feed("f1"), feed("f2"), DependencyKind::RequiresPowerFrom);

  TraversalRequest request;
  request.root = asset("root");
  request.direction = TraversalDirection::Dependencies;
  request.max_depth = 4;
  request.max_nodes = 64;
  const auto result = harness.snapshot().transitive_closure(request);
  FDEP_REQUIRE_OK(result);
  const TraversalResult& closure = result.value();

  FDEP_CHECK(closure.stop == TraversalStop::Complete);
  FDEP_CHECK(!closure.truncated);
  FDEP_REQUIRE(closure.entries.size() == 3);
  FDEP_CHECK(closure.entries[0].node == feed("f1"));
  FDEP_CHECK_EQ(closure.entries[0].depth, 1u);
  FDEP_CHECK_EQ(closure.entries[0].via_edge.value(), to_feed.value());
  FDEP_CHECK(closure.entries[0].via_node == asset("root"));
  FDEP_CHECK(closure.entries[1].node == loop("l1"));
  FDEP_CHECK_EQ(closure.entries[1].depth, 1u);
  FDEP_CHECK(closure.entries[2].node == feed("f2"));
  FDEP_CHECK_EQ(closure.entries[2].depth, 2u);
  FDEP_CHECK_EQ(closure.entries[2].via_edge.value(), to_upstream.value());
  FDEP_CHECK(closure.entries[2].via_node == feed("f1"));

  FDEP_CHECK(closure.reached(asset("root")));
  FDEP_CHECK(closure.reached(feed("f2")));
  FDEP_CHECK(!closure.reached(asset("never-mentioned")));
  FDEP_CHECK_EQ(closure.depth_of(asset("root")).value(), 0u);
  FDEP_CHECK_EQ(closure.depth_of(feed("f2")).value(), 2u);
  FDEP_CHECK(!closure.depth_of(asset("never-mentioned")).has_value());
  FDEP_CHECK_EQ(closure.size(), std::size_t{3});
  FDEP_CHECK(closure.edges_examined > 0);

  // The same query twice gives identical answers.
  const auto again = harness.snapshot().transitive_closure(request);
  FDEP_REQUIRE_OK(again);
  FDEP_CHECK_EQ(again.value().entries.size(), closure.entries.size());
  for (std::size_t index = 0; index < closure.entries.size(); ++index) {
    FDEP_CHECK(again.value().entries[index].node == closure.entries[index].node);
    FDEP_CHECK_EQ(again.value().entries[index].depth, closure.entries[index].depth);
    FDEP_CHECK_EQ(again.value().entries[index].via_edge.value(), closure.entries[index].via_edge.value());
  }
}

FDEP_TEST(traversal, dependent_direction_walks_backwards) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("node-0"), feed("f1"), DependencyKind::RequiresPowerFrom);
  harness.add(asset("node-1"), asset("node-0"), DependencyKind::ControlDependsOn);
  harness.add(asset("node-2"), asset("node-1"), DependencyKind::ControlDependsOn);

  TraversalRequest request;
  request.root = feed("f1");
  request.direction = TraversalDirection::Dependents;
  request.max_depth = 8;
  request.max_nodes = 64;
  const auto result = harness.snapshot().transitive_closure(request);
  FDEP_REQUIRE_OK(result);
  FDEP_REQUIRE(result.value().entries.size() == 3);
  FDEP_CHECK(result.value().entries[0].node == asset("node-0"));
  FDEP_CHECK(result.value().entries[1].node == asset("node-1"));
  FDEP_CHECK(result.value().entries[2].node == asset("node-2"));
  FDEP_CHECK_EQ(std::string{to_token(result.value().direction)}, std::string{"dependents"});
}

FDEP_TEST(traversal, root_missing_from_the_graph_yields_an_empty_complete_result) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  TraversalRequest request;
  request.root = asset("not-here");
  const auto result = harness.snapshot().transitive_closure(request);
  FDEP_REQUIRE_OK(result);
  FDEP_CHECK(result.value().entries.empty());
  FDEP_CHECK(result.value().stop == TraversalStop::Complete);
  FDEP_CHECK(!result.value().truncated);
}

FDEP_TEST(traversal, depth_limit_truncates_explicitly) {
  Harness harness = Harness::ephemeral();
  build_chain(harness, 10);

  TraversalRequest request;
  request.root = feed("f1");
  request.direction = TraversalDirection::Dependents;
  request.max_depth = 3;
  request.max_nodes = 100;
  const auto result = harness.snapshot().transitive_closure(request);
  FDEP_REQUIRE_OK(result);
  FDEP_CHECK_EQ(result.value().entries.size(), std::size_t{3});
  FDEP_CHECK(result.value().stop == TraversalStop::DepthLimit);
  FDEP_CHECK(result.value().truncated);
  FDEP_CHECK_EQ(std::string{to_token(result.value().stop)}, std::string{"depth-limit"});

  // A depth that reaches the end of the chain is complete, not truncated.
  request.max_depth = 20;
  const auto complete = harness.snapshot().transitive_closure(request);
  FDEP_REQUIRE_OK(complete);
  FDEP_CHECK_EQ(complete.value().entries.size(), std::size_t{10});
  FDEP_CHECK(complete.value().stop == TraversalStop::Complete);
  FDEP_CHECK(!complete.value().truncated);
}

FDEP_TEST(traversal, depth_limit_is_not_claimed_when_the_frontier_is_a_leaf) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  TraversalRequest request;
  request.root = feed("f1");
  request.direction = TraversalDirection::Dependents;
  request.max_depth = 1;
  request.max_nodes = 10;
  const auto result = harness.snapshot().transitive_closure(request);
  FDEP_REQUIRE_OK(result);
  FDEP_REQUIRE(result.value().entries.size() == 1);
  // The frontier at depth 1 has nothing beyond it, so nothing was cut off.
  FDEP_CHECK(result.value().stop == TraversalStop::Complete);
  FDEP_CHECK(!result.value().truncated);
}

FDEP_TEST(traversal, node_limit_truncates_canonically_and_explicitly) {
  Harness harness = Harness::ephemeral();
  for (int index = 0; index < 10; ++index) {
    harness.add(asset("node-" + std::to_string(index)), rack("r1"), DependencyKind::HousedIn);
  }
  TraversalRequest request;
  request.root = rack("r1");
  request.direction = TraversalDirection::Dependents;
  request.max_depth = 4;
  request.max_nodes = 4;
  const auto result = harness.snapshot().transitive_closure(request);
  FDEP_REQUIRE_OK(result);
  FDEP_REQUIRE(result.value().entries.size() == 4);
  FDEP_CHECK(result.value().stop == TraversalStop::NodeLimit);
  FDEP_CHECK(result.value().truncated);
  // The partial answer is the canonically first part of the reachable set.
  for (int index = 0; index < 4; ++index) {
    FDEP_CHECK(result.value().entries[static_cast<std::size_t>(index)].node ==
               asset("node-" + std::to_string(index)));
  }
}

FDEP_TEST(traversal, requests_above_the_configured_bounds_are_rejected) {
  RegistryLimits limits;
  limits.max_traversal_depth = 5;
  limits.max_traversal_nodes = 50;
  Harness harness = Harness::ephemeral(limits);
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);

  TraversalRequest request;
  request.root = feed("f1");
  request.max_depth = 6;
  request.max_nodes = 10;
  FDEP_CHECK_CODE(harness.snapshot().transitive_closure(request), ErrorCode::RequestLimitExceeded);

  request.max_depth = 5;
  request.max_nodes = 51;
  FDEP_CHECK_CODE(harness.snapshot().transitive_closure(request), ErrorCode::RequestLimitExceeded);

  request.max_depth = 0;
  request.max_nodes = 10;
  FDEP_CHECK_CODE(harness.snapshot().transitive_closure(request), ErrorCode::RequestLimitExceeded);

  request.max_depth = 5;
  request.max_nodes = 0;
  FDEP_CHECK_CODE(harness.snapshot().transitive_closure(request), ErrorCode::RequestLimitExceeded);

  request.max_depth = 5;
  request.max_nodes = 50;
  FDEP_REQUIRE_OK(harness.snapshot().transitive_closure(request));
}

FDEP_TEST(traversal, mutual_edges_are_traversed_both_ways) {
  Harness harness = Harness::ephemeral();
  harness.add(asi("a"), dfi("b"), DependencyKind::ComposedDomainDependsOn, DependencyStrength::Soft,
              Direction::Mutual);
  harness.add(asset("load-1"), asi("a"), DependencyKind::ControlDependsOn);
  harness.add(asset("load-2"), dfi("b"), DependencyKind::ControlDependsOn);

  TraversalRequest request;
  request.root = asi("a");
  request.direction = TraversalDirection::Dependencies;
  request.max_depth = 4;
  request.max_nodes = 32;
  const auto forward = harness.snapshot().transitive_closure(request);
  FDEP_REQUIRE_OK(forward);
  FDEP_CHECK(forward.value().reached(dfi("b")));
  FDEP_CHECK(!forward.value().reached(asset("load-1")));
  FDEP_CHECK(!forward.value().reached(asset("load-2")));

  // Reading the graph backwards from the same node reaches both loads: they
  // depend on the composed domains, and the composed domains are coupled.
  request.direction = TraversalDirection::Dependents;
  const auto backward = harness.snapshot().transitive_closure(request);
  FDEP_REQUIRE_OK(backward);
  FDEP_CHECK(backward.value().reached(asset("load-1")));
  FDEP_CHECK(backward.value().reached(dfi("b")));
  FDEP_CHECK(backward.value().reached(asset("load-2")));
}

FDEP_TEST(traversal, a_proposed_edge_is_visible_but_not_followed_by_default) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom, DependencyStrength::Hard,
              Direction::DependsOn, LifecycleState::Proposed);

  TraversalRequest request;
  request.root = asset("a");
  request.max_depth = 4;
  request.max_nodes = 32;
  const auto in_force = harness.snapshot().transitive_closure(request);
  FDEP_REQUIRE_OK(in_force);
  FDEP_CHECK(in_force.value().entries.empty());

  request.filter.lifecycles = LifecycleMask::of(LifecycleState::Proposed);
  const auto proposed = harness.snapshot().transitive_closure(request);
  FDEP_REQUIRE_OK(proposed);
  FDEP_REQUIRE(proposed.value().entries.size() == 1);
  FDEP_CHECK(proposed.value().entries[0].node == feed("f1"));
}

FDEP_TEST(traversal, cycles_do_not_explode_the_walk) {
  Harness harness = Harness::ephemeral();
  // A control cycle of eight nodes, each also depending on a facility service.
  constexpr int kRingSize = 8;
  for (int index = 0; index < kRingSize; ++index) {
    harness.add(asset("node-" + std::to_string(index)),
                asset("node-" + std::to_string((index + 1) % kRingSize)),
                DependencyKind::ControlDependsOn);
  }
  harness.add(asset("node-0"), service("s1"), DependencyKind::ServedBy);

  TraversalRequest request;
  request.root = asset("node-0");
  request.max_depth = 64;
  request.max_nodes = 1000;
  const auto result = harness.snapshot().transitive_closure(request);
  FDEP_REQUIRE_OK(result);
  // Every node is reported exactly once even though the walk is cyclic.
  FDEP_CHECK_EQ(result.value().entries.size(), std::size_t{8});
  FDEP_CHECK(result.value().stop == TraversalStop::Complete);
  FDEP_CHECK(result.value().reached(service("s1")));
  std::vector<DependencyNodeRef> seen;
  for (const auto& entry : result.value().entries) {
    FDEP_CHECK(std::find(seen.begin(), seen.end(), entry.node) == seen.end());
    seen.push_back(entry.node);
  }
}

FDEP_TEST(traversal, invalid_direction_is_rejected) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  TraversalRequest request;
  request.root = asset("a");
  request.direction = static_cast<TraversalDirection>(0);
  FDEP_CHECK_CODE(harness.snapshot().transitive_closure(request), ErrorCode::InvalidArguments);
  request.direction = TraversalDirection::Dependencies;
  request.root = DependencyNodeRef{};
  FDEP_CHECK_CODE(harness.snapshot().transitive_closure(request), ErrorCode::InvalidNodeReference);
}

FDEP_TEST(traversal, self_consistent_depth_and_reachability_helpers) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  harness.add(asset("b"), asset("a"), DependencyKind::ControlDependsOn);
  harness.add(asset("c"), asset("b"), DependencyKind::ControlDependsOn);
  harness.add(asset("a"), loop("l1"), DependencyKind::CooledBy);

  TraversalRequest request;
  request.root = feed("f1");
  request.direction = TraversalDirection::Dependents;
  request.max_depth = 8;
  request.max_nodes = 64;
  const auto result = harness.snapshot().transitive_closure(request);
  FDEP_REQUIRE_OK(result);
  for (const auto& entry : result.value().entries) {
    const auto depth = result.value().depth_of(entry.node);
    FDEP_REQUIRE(depth.has_value());
    FDEP_CHECK_EQ(*depth, entry.depth);
    FDEP_CHECK(result.value().reached(entry.node));
  }
}
