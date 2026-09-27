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
using fdep_test::dfi;
using fdep_test::feed;
using fdep_test::Harness;
using fdep_test::service;

namespace {

bool has_component(const SccResult& result, const std::vector<DependencyNodeRef>& members) {
  for (const auto& component : result.components) {
    if (component.members == members) {
      return true;
    }
  }
  return false;
}

}  // namespace

FDEP_TEST(scc, an_acyclic_graph_has_no_components_by_default) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  harness.add(asset("b"), asset("a"), DependencyKind::ControlDependsOn);
  harness.add(asset("c"), asset("b"), DependencyKind::ControlDependsOn);

  ComponentRequest request;
  request.max_nodes = 1024;
  const auto result = harness.snapshot().strongly_connected_components(request);
  FDEP_REQUIRE_OK(result);
  FDEP_CHECK(result.value().components.empty());
  FDEP_CHECK_EQ(result.value().cyclic_components, std::size_t{0});
  FDEP_CHECK_EQ(result.value().nodes_examined, std::size_t{4});
  FDEP_CHECK_EQ(result.value().edges_examined, std::size_t{3});

  request.include_singletons = true;
  const auto with_singletons = harness.snapshot().strongly_connected_components(request);
  FDEP_REQUIRE_OK(with_singletons);
  FDEP_CHECK_EQ(with_singletons.value().components.size(), std::size_t{4});
  for (const auto& component : with_singletons.value().components) {
    FDEP_CHECK_EQ(component.size(), std::size_t{1});
    FDEP_CHECK(!component.cyclic);
  }
}

FDEP_TEST(scc, a_control_cycle_is_one_component) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), asset("b"), DependencyKind::ControlDependsOn);
  harness.add(asset("b"), asset("c"), DependencyKind::ControlDependsOn);
  harness.add(asset("c"), asset("a"), DependencyKind::ControlDependsOn);
  harness.add(asset("d"), asset("a"), DependencyKind::ControlDependsOn);

  ComponentRequest request;
  request.max_nodes = 1024;
  const auto result = harness.snapshot().strongly_connected_components(request);
  FDEP_REQUIRE_OK(result);
  FDEP_REQUIRE(result.value().components.size() == 1);
  FDEP_CHECK_EQ(result.value().cyclic_components, std::size_t{1});
  const auto& component = result.value().components[0];
  FDEP_CHECK(component.cyclic);
  FDEP_REQUIRE(component.members.size() == 3);
  FDEP_CHECK(component.members[0] == asset("a"));
  FDEP_CHECK(component.members[1] == asset("b"));
  FDEP_CHECK(component.members[2] == asset("c"));
  FDEP_REQUIRE(component.edges.size() == 3);
  FDEP_CHECK(std::is_sorted(component.edges.begin(), component.edges.end()));
  FDEP_CHECK_EQ(component.edges[0].value(), std::uint64_t{1});  // a -> b
  FDEP_CHECK_EQ(component.edges[1].value(), std::uint64_t{2});  // b -> c
  FDEP_CHECK_EQ(component.edges[2].value(), std::uint64_t{3});  // c -> a
}

FDEP_TEST(scc, a_mutual_edge_makes_a_two_node_component) {
  Harness harness = Harness::ephemeral();
  harness.add(asi("a"), dfi("b"), DependencyKind::ComposedDomainDependsOn, DependencyStrength::Soft,
              Direction::Mutual);
  harness.add(asset("load"), feed("f1"), DependencyKind::RequiresPowerFrom);

  ComponentRequest request;
  request.max_nodes = 1024;
  const auto result = harness.snapshot().strongly_connected_components(request);
  FDEP_REQUIRE_OK(result);
  FDEP_REQUIRE(result.value().components.size() == 1);
  FDEP_CHECK_EQ(result.value().cyclic_components, std::size_t{1});
  FDEP_CHECK(has_component(result.value(), {asi("a"), dfi("b")}));
  FDEP_REQUIRE(result.value().components[0].edges.size() == 1);
}

FDEP_TEST(scc, components_are_ordered_by_their_smallest_member) {
  Harness harness = Harness::ephemeral();
  // Two disjoint cycles.
  harness.add(asset("m"), asset("n"), DependencyKind::ControlDependsOn);
  harness.add(asset("n"), asset("m"), DependencyKind::ControlDependsOn);
  harness.add(asset("x"), asset("y"), DependencyKind::ControlDependsOn);
  harness.add(asset("y"), asset("x"), DependencyKind::ControlDependsOn);

  ComponentRequest request;
  request.max_nodes = 1024;
  const auto result = harness.snapshot().strongly_connected_components(request);
  FDEP_REQUIRE_OK(result);
  FDEP_REQUIRE(result.value().components.size() == 2);
  FDEP_CHECK(result.value().components[0].members.front() == asset("m"));
  FDEP_CHECK(result.value().components[1].members.front() == asset("x"));
  FDEP_CHECK_EQ(result.value().cyclic_components, std::size_t{2});

  // Repeating the analysis gives the same decomposition.
  const auto again = harness.snapshot().strongly_connected_components(request);
  FDEP_REQUIRE_OK(again);
  FDEP_REQUIRE(again.value().components.size() == 2);
  for (std::size_t index = 0; index < 2; ++index) {
    FDEP_CHECK(again.value().components[index].members == result.value().components[index].members);
    FDEP_CHECK(again.value().components[index].edges == result.value().components[index].edges);
  }
}

FDEP_TEST(scc, filters_change_what_counts_as_a_cycle) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), asset("b"), DependencyKind::ControlDependsOn, DependencyStrength::Advisory);
  harness.add(asset("b"), asset("a"), DependencyKind::ControlDependsOn, DependencyStrength::Advisory);

  ComponentRequest request;
  request.max_nodes = 1024;
  // The default filter follows every strength, so the cycle is visible.
  const auto all_strengths = harness.snapshot().strongly_connected_components(request);
  FDEP_REQUIRE_OK(all_strengths);
  FDEP_CHECK_EQ(all_strengths.value().cyclic_components, std::size_t{1});

  // Restricting the strengths removes both edges, so the same graph has no
  // cycle under that reading.
  request.filter.strengths = DependencyStrengthMask::of(DependencyStrength::Hard);
  const auto hard_only = harness.snapshot().strongly_connected_components(request);
  FDEP_REQUIRE_OK(hard_only);
  FDEP_CHECK_EQ(hard_only.value().cyclic_components, std::size_t{0});
  FDEP_CHECK_EQ(hard_only.value().edges_examined, std::size_t{0});

  // A suspended edge is not part of the default reading either.
  Harness suspended = Harness::ephemeral();
  const DependencyEdgeId first = suspended.add(asset("a"), asset("b"), DependencyKind::ControlDependsOn);
  suspended.add(asset("b"), asset("a"), DependencyKind::ControlDependsOn);
  FDEP_REQUIRE(suspended.transition(first, kInitialRevision, LifecycleState::Suspended).ok());
  const auto default_reading = suspended.snapshot().strongly_connected_components(request);
  FDEP_REQUIRE_OK(default_reading);
  FDEP_CHECK_EQ(default_reading.value().cyclic_components, std::size_t{0});
}

FDEP_TEST(scc, analysis_bound_is_enforced_rather_than_partially_answered) {
  RegistryLimits limits;
  limits.max_analysis_nodes = 4;
  limits.max_cycle_length = 2;
  Harness harness = Harness::ephemeral(limits);
  for (int index = 0; index < 8; ++index) {
    harness.add(asset("node-" + std::to_string(index)), feed("f1"), DependencyKind::RequiresPowerFrom);
  }

  ComponentRequest request;
  request.max_nodes = 4;
  FDEP_CHECK_CODE(harness.snapshot().strongly_connected_components(request),
                  ErrorCode::AnalysisLimitExceeded);

  request.max_nodes = 5;
  FDEP_CHECK_CODE(harness.snapshot().strongly_connected_components(request),
                  ErrorCode::RequestLimitExceeded);

  request.max_nodes = 0;
  FDEP_CHECK_CODE(harness.snapshot().strongly_connected_components(request),
                  ErrorCode::RequestLimitExceeded);

  // A graph that fits inside the bound is analysed rather than refused.
  Harness small = Harness::ephemeral(limits);
  small.add(asset("a"), asset("b"), DependencyKind::ControlDependsOn);
  small.add(asset("b"), asset("a"), DependencyKind::ControlDependsOn);
  request.max_nodes = 4;
  const auto result = small.snapshot().strongly_connected_components(request);
  FDEP_REQUIRE_OK(result);
  FDEP_CHECK_EQ(result.value().components.size(), std::size_t{1});
}

FDEP_TEST(scc, a_large_ring_is_one_component_and_is_not_recursive) {
  Harness harness = Harness::ephemeral();
  constexpr int kRingSize = 1000;
  for (int index = 0; index < kRingSize; ++index) {
    harness.add(asset("node-" + std::to_string(index)),
                asset("node-" + std::to_string((index + 1) % kRingSize)),
                DependencyKind::ControlDependsOn);
  }
  ComponentRequest request;
  request.max_nodes = 8192;
  const auto result = harness.snapshot().strongly_connected_components(request);
  FDEP_REQUIRE_OK(result);
  FDEP_REQUIRE(result.value().components.size() == 1);
  FDEP_CHECK_EQ(result.value().components[0].size(), static_cast<std::size_t>(kRingSize));
  FDEP_CHECK_EQ(result.value().components[0].edges.size(), static_cast<std::size_t>(kRingSize));
  FDEP_CHECK_EQ(result.value().cyclic_components, std::size_t{1});
}

FDEP_TEST(scc, irrelevant_edges_do_not_create_components) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  harness.add(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
  harness.add(service("s1"), feed("f1"), DependencyKind::RequiresPowerFrom);

  ComponentRequest request;
  request.max_nodes = 64;
  const auto result = harness.snapshot().strongly_connected_components(request);
  FDEP_REQUIRE_OK(result);
  FDEP_CHECK(result.value().components.empty());
  FDEP_CHECK_EQ(result.value().cyclic_components, std::size_t{0});
  FDEP_CHECK_EQ(result.value().nodes_examined, std::size_t{4});
}

FDEP_TEST(scc, empty_graph_yields_an_empty_decomposition) {
  Harness harness = Harness::ephemeral();
  ComponentRequest request;
  const auto result = harness.snapshot().strongly_connected_components(request);
  FDEP_REQUIRE_OK(result);
  FDEP_CHECK(result.value().components.empty());
  FDEP_CHECK_EQ(result.value().nodes_examined, std::size_t{0});
  FDEP_CHECK_EQ(result.value().edges_examined, std::size_t{0});
}
