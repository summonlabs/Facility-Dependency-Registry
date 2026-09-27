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
using fdep_test::make_spec;
using fdep_test::rack;
using fdep_test::service;

namespace {

bool has_cycle_with(const CycleReport& report, const std::vector<DependencyNodeRef>& nodes) {
  for (const auto& cycle : report.cycles) {
    if (cycle.nodes == nodes) {
      return true;
    }
  }
  return false;
}

}  // namespace

FDEP_TEST(cycles, the_acyclic_obligation_is_satisfied_by_construction) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  harness.add(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
  harness.add(asset("a"), loop("l1"), DependencyKind::CooledBy);
  harness.add(asset("a"), rack("r1"), DependencyKind::HousedIn);

  const auto report = harness.snapshot().verify_acyclic_obligation();
  FDEP_REQUIRE_OK(report);
  FDEP_CHECK(report.value().satisfied);
  FDEP_CHECK(report.value().violations.empty());
  FDEP_CHECK(report.value().nodes_examined > 0);
  FDEP_CHECK(report.value().edges_examined > 0);
  FDEP_CHECK_EQ(report.value().generation.value(), std::uint64_t{4});
}

FDEP_TEST(cycles, a_power_cycle_is_rejected_with_the_cycle_in_the_message) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  harness.add(feed("f1"), feed("f2"), DependencyKind::RequiresPowerFrom);
  const auto closing = harness.try_add(make_spec(feed("f2"), feed("f1"), DependencyKind::RequiresPowerFrom),
                                       harness.generation());
  FDEP_CHECK_CODE(closing, ErrorCode::ProhibitedCycle);
  FDEP_CHECK(closing.error().detail().find("electrical-domain:f1") != std::string::npos);
  FDEP_CHECK(closing.error().detail().find("electrical-domain:f2") != std::string::npos);
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{2});
}

FDEP_TEST(cycles, the_endpoint_rules_make_cross_kind_cycles_inexpressible) {
  // HousedIn runs from an asset, service or composed domain to a rack, and
  // RequiresPowerFrom can only ever end at an electrical domain. Nothing can
  // point back at a rack except HousedIn, whose source may not be a rack, or a
  // kind whose cycles are legal. That is why the acyclic obligation is a single
  // subgraph over all three acyclic-required kinds rather than one obligation
  // per kind: a mixed-kind cycle cannot be written down in the first place, and
  // the property test in test_property.cpp checks that claim on random graphs.
  Harness harness = Harness::ephemeral();
  harness.add(service("s1"), rack("r1"), DependencyKind::HousedIn);

  // A rack cannot be housed in anything, and cannot depend on anything except
  // an electrical domain.
  FDEP_CHECK_CODE(harness.try_add(make_spec(rack("r1"), service("s1"), DependencyKind::HousedIn),
                                  harness.generation()),
                  ErrorCode::EndpointDomainNotAllowed);
  FDEP_CHECK_CODE(harness.try_add(make_spec(rack("r1"), asset("a"), DependencyKind::RequiresPowerFrom),
                                  harness.generation()),
                  ErrorCode::EndpointDomainNotAllowed);
  FDEP_CHECK_CODE(harness.try_add(make_spec(rack("r1"), service("s1"), DependencyKind::RequiresPowerFrom),
                                  harness.generation()),
                  ErrorCode::EndpointDomainNotAllowed);

  // The one legal edge out of a rack reaches an electrical domain, and nothing
  // can reach back to the rack through an acyclic-required kind.
  FDEP_REQUIRE_OK(harness.try_add(make_spec(rack("r1"), feed("f1"), DependencyKind::RequiresPowerFrom),
                                  harness.generation()));
  FDEP_CHECK_CODE(harness.try_add(make_spec(feed("f1"), service("s1"), DependencyKind::HousedIn),
                                  harness.generation()),
                  ErrorCode::EndpointDomainNotAllowed);
  const auto report = harness.snapshot().verify_acyclic_obligation();
  FDEP_REQUIRE_OK(report);
  FDEP_CHECK(report.value().satisfied);
}

FDEP_TEST(cycles, a_control_cycle_is_legal_and_enumerated) {
  Harness harness = Harness::ephemeral();
  const DependencyEdgeId first = harness.add(asset("a"), asset("b"), DependencyKind::ControlDependsOn);
  const DependencyEdgeId second = harness.add(asset("b"), asset("c"), DependencyKind::ControlDependsOn);
  const DependencyEdgeId third = harness.add(asset("c"), asset("a"), DependencyKind::ControlDependsOn);
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{3});

  const auto report = harness.snapshot().verify_acyclic_obligation();
  FDEP_REQUIRE_OK(report);
  FDEP_CHECK(report.value().satisfied);

  CycleRequest request;
  request.max_length = 8;
  request.max_cycles = 16;
  request.max_nodes = 256;
  const auto cycles = harness.snapshot().enumerate_cycles(request);
  FDEP_REQUIRE_OK(cycles);
  FDEP_REQUIRE(cycles.value().cycles.size() == 1);
  const CycleRecord& cycle = cycles.value().cycles[0];
  FDEP_CHECK_EQ(cycle.length(), std::size_t{3});
  FDEP_CHECK(cycle.nodes[0] == asset("a"));
  FDEP_CHECK(cycle.nodes[1] == asset("b"));
  FDEP_CHECK(cycle.nodes[2] == asset("c"));
  FDEP_REQUIRE(cycle.edges.size() == 3);
  FDEP_CHECK_EQ(cycle.edges[0].value(), first.value());
  FDEP_CHECK_EQ(cycle.edges[1].value(), second.value());
  FDEP_CHECK_EQ(cycle.edges[2].value(), third.value());
  FDEP_CHECK(cycle.contains(asset("b")));
  FDEP_CHECK(!cycle.contains(asset("z")));
  FDEP_CHECK(!cycles.value().truncated);
  FDEP_CHECK(cycles.value().stop == TraversalStop::Complete);
}

FDEP_TEST(cycles, a_mutual_edge_is_a_two_cycle) {
  Harness harness = Harness::ephemeral();
  harness.add(asi("a"), dfi("b"), DependencyKind::ComposedDomainDependsOn, DependencyStrength::Soft,
              Direction::Mutual);
  CycleRequest request;
  request.max_length = 4;
  request.max_cycles = 8;
  request.max_nodes = 64;
  const auto cycles = harness.snapshot().enumerate_cycles(request);
  FDEP_REQUIRE_OK(cycles);
  FDEP_REQUIRE(cycles.value().cycles.size() == 1);
  FDEP_CHECK_EQ(cycles.value().cycles[0].length(), std::size_t{2});
  FDEP_CHECK(cycles.value().cycles[0].nodes[0] == asi("a"));
  FDEP_CHECK(cycles.value().cycles[0].nodes[1] == dfi("b"));
  // The same edge is used in both directions.
  FDEP_CHECK_EQ(cycles.value().cycles[0].edges[0].value(), cycles.value().cycles[0].edges[1].value());
}

FDEP_TEST(cycles, every_elementary_cycle_is_reported_once) {
  Harness harness = Harness::ephemeral();
  // A four node ring plus both diagonals over {a, c}. Every elementary cycle
  // contains a, so all of them start there:
  //   a-c-a, a-b-c-a, a-c-d-a and a-b-c-d-a.
  harness.add(asset("a"), asset("b"), DependencyKind::ControlDependsOn);
  harness.add(asset("b"), asset("c"), DependencyKind::ControlDependsOn);
  harness.add(asset("c"), asset("d"), DependencyKind::ControlDependsOn);
  harness.add(asset("d"), asset("a"), DependencyKind::ControlDependsOn);
  harness.add(asset("a"), asset("c"), DependencyKind::ControlDependsOn);
  harness.add(asset("c"), asset("a"), DependencyKind::ControlDependsOn);

  CycleRequest request;
  request.max_length = 8;
  request.max_cycles = 32;
  request.max_nodes = 64;
  const auto cycles = harness.snapshot().enumerate_cycles(request);
  FDEP_REQUIRE_OK(cycles);
  FDEP_REQUIRE(cycles.value().cycles.size() == 4);
  // Ordered by length, so the two node cycle comes first.
  FDEP_CHECK_EQ(cycles.value().cycles[0].length(), std::size_t{2});
  FDEP_CHECK(has_cycle_with(cycles.value(), {asset("a"), asset("c")}));
  FDEP_CHECK(has_cycle_with(cycles.value(), {asset("a"), asset("b"), asset("c")}));
  FDEP_CHECK(has_cycle_with(cycles.value(), {asset("a"), asset("c"), asset("d")}));
  FDEP_CHECK(has_cycle_with(cycles.value(), {asset("a"), asset("b"), asset("c"), asset("d")}));

  // Each cycle starts at its canonically smallest member.
  for (const auto& cycle : cycles.value().cycles) {
    for (std::size_t index = 1; index < cycle.nodes.size(); ++index) {
      FDEP_CHECK(cycle.nodes[0] < cycle.nodes[index]);
    }
  }
}

FDEP_TEST(cycles, the_length_bound_hides_longer_cycles) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), asset("b"), DependencyKind::ControlDependsOn);
  harness.add(asset("b"), asset("c"), DependencyKind::ControlDependsOn);
  harness.add(asset("c"), asset("d"), DependencyKind::ControlDependsOn);
  harness.add(asset("d"), asset("a"), DependencyKind::ControlDependsOn);

  CycleRequest request;
  request.max_length = 3;
  request.max_cycles = 8;
  request.max_nodes = 64;
  const auto cycles = harness.snapshot().enumerate_cycles(request);
  FDEP_REQUIRE_OK(cycles);
  FDEP_CHECK(cycles.value().cycles.empty());

  request.max_length = 4;
  const auto longer = harness.snapshot().enumerate_cycles(request);
  FDEP_REQUIRE_OK(longer);
  FDEP_REQUIRE(longer.value().cycles.size() == 1);
  FDEP_CHECK_EQ(longer.value().cycles[0].length(), std::size_t{4});
}

FDEP_TEST(cycles, the_count_bound_is_reported_as_truncation) {
  Harness harness = Harness::ephemeral();
  // Four nodes, both diagonals: several cycles.
  harness.add(asset("a"), asset("b"), DependencyKind::ControlDependsOn);
  harness.add(asset("b"), asset("c"), DependencyKind::ControlDependsOn);
  harness.add(asset("c"), asset("d"), DependencyKind::ControlDependsOn);
  harness.add(asset("d"), asset("a"), DependencyKind::ControlDependsOn);
  harness.add(asset("a"), asset("c"), DependencyKind::ControlDependsOn);
  harness.add(asset("c"), asset("a"), DependencyKind::ControlDependsOn);

  CycleRequest request;
  request.max_length = 8;
  request.max_cycles = 1;
  request.max_nodes = 64;
  const auto cycles = harness.snapshot().enumerate_cycles(request);
  FDEP_REQUIRE_OK(cycles);
  FDEP_CHECK_EQ(cycles.value().cycles.size(), std::size_t{1});
  FDEP_CHECK(cycles.value().truncated);
  FDEP_CHECK(cycles.value().stop == TraversalStop::ResultLimit);
}

FDEP_TEST(cycles, request_bounds_are_validated_against_the_configuration) {
  RegistryLimits limits;
  limits.max_cycle_length = 6;
  limits.max_cycles = 4;
  limits.max_analysis_nodes = 64;
  Harness harness = Harness::ephemeral(limits);
  harness.add(asset("a"), asset("b"), DependencyKind::ControlDependsOn);

  CycleRequest request;
  request.max_length = 7;
  request.max_cycles = 4;
  request.max_nodes = 64;
  FDEP_CHECK_CODE(harness.snapshot().enumerate_cycles(request), ErrorCode::RequestLimitExceeded);

  request.max_length = 6;
  request.max_cycles = 5;
  FDEP_CHECK_CODE(harness.snapshot().enumerate_cycles(request), ErrorCode::RequestLimitExceeded);

  request.max_cycles = 0;
  FDEP_CHECK_CODE(harness.snapshot().enumerate_cycles(request), ErrorCode::RequestLimitExceeded);

  request.max_cycles = 4;
  request.max_length = 1;
  FDEP_CHECK_CODE(harness.snapshot().enumerate_cycles(request), ErrorCode::RequestLimitExceeded);

  request.max_length = 6;
  request.max_nodes = 65;
  FDEP_CHECK_CODE(harness.snapshot().enumerate_cycles(request), ErrorCode::RequestLimitExceeded);

  request.max_nodes = 0;
  FDEP_CHECK_CODE(harness.snapshot().enumerate_cycles(request), ErrorCode::RequestLimitExceeded);

  request.max_nodes = 64;
  FDEP_REQUIRE_OK(harness.snapshot().enumerate_cycles(request));
}

FDEP_TEST(cycles, analysis_bound_is_enforced) {
  RegistryLimits limits;
  limits.max_analysis_nodes = 4;
  limits.max_cycle_length = 2;
  Harness harness = Harness::ephemeral(limits);
  for (int index = 0; index < 8; ++index) {
    harness.add(asset("node-" + std::to_string(index)), feed("f1"), DependencyKind::RequiresPowerFrom);
  }
  CycleRequest request;
  request.max_length = 2;
  request.max_cycles = 4;
  request.max_nodes = 4;
  FDEP_CHECK_CODE(harness.snapshot().enumerate_cycles(request), ErrorCode::AnalysisLimitExceeded);
}

FDEP_TEST(cycles, filters_apply_and_an_empty_graph_reports_nothing) {
  Harness harness = Harness::ephemeral();
  const auto empty = harness.snapshot().enumerate_cycles(CycleRequest{});
  FDEP_REQUIRE_OK(empty);
  FDEP_CHECK(empty.value().cycles.empty());
  FDEP_CHECK_EQ(empty.value().nodes_examined, std::size_t{0});

  harness.add(asset("a"), asset("b"), DependencyKind::ControlDependsOn, DependencyStrength::Advisory);
  harness.add(asset("b"), asset("a"), DependencyKind::ControlDependsOn, DependencyStrength::Advisory);

  CycleRequest request;
  request.max_length = 8;
  request.max_cycles = 8;
  request.max_nodes = 64;
  request.filter.strengths = DependencyStrengthMask::of(DependencyStrength::Soft);
  const auto soft_only = harness.snapshot().enumerate_cycles(request);
  FDEP_REQUIRE_OK(soft_only);
  FDEP_CHECK(soft_only.value().cycles.empty());

  request.filter.strengths = DependencyStrengthMask::all();
  const auto all = harness.snapshot().enumerate_cycles(request);
  FDEP_REQUIRE_OK(all);
  FDEP_CHECK_EQ(all.value().cycles.size(), std::size_t{1});
  FDEP_CHECK_EQ(all.value().cycles[0].length(), std::size_t{2});
}
