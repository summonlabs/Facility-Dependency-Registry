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
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "facility_dependency_registry/facility_dependency_registry.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

using namespace facility_dependency_registry;
using fdep_test::Harness;
using fdep_test::Rng;

namespace {

const NodeDomain kDomains[] = {NodeDomain::Asset,          NodeDomain::Rack,
                               NodeDomain::ElectricalDomain, NodeDomain::CoolingDomain,
                               NodeDomain::FacilityService,  NodeDomain::AsiDomain,
                               NodeDomain::DfiDomain};

const DependencyKind kKinds[] = {DependencyKind::RequiresPowerFrom, DependencyKind::CooledBy,
                                 DependencyKind::HousedIn,          DependencyKind::ServedBy,
                                 DependencyKind::ControlDependsOn,  DependencyKind::ComposedDomainDependsOn};

const DependencyStrength kStrengths[] = {DependencyStrength::Hard, DependencyStrength::Soft,
                                         DependencyStrength::Advisory};

DependencyNodeRef random_node(Rng& rng, int index) {
  const NodeDomain domain = kDomains[rng.below(static_cast<std::uint32_t>(std::size(kDomains)))];
  return DependencyNodeRef::create(domain, "n" + std::to_string(index)).value();
}

struct Attempt {
  DependencyNodeRef from{};
  DependencyNodeRef to{};
  DependencyKind kind = DependencyKind::RequiresPowerFrom;
  DependencyStrength strength = DependencyStrength::Hard;
  Direction direction = Direction::DependsOn;
};

/// Builds one random graph by attempting `attempts` declarations over `nodes`
/// node references. Every attempt is recorded with the outcome the registry
/// produced, so the properties below can reason about what was refused and why.
struct RandomGraph {
  std::vector<DependencyNodeRef> nodes{};
  std::vector<Attempt> attempts{};
  std::vector<ErrorCode> outcomes{};
  std::vector<DependencyEdgeId> accepted_ids{};
};

RandomGraph build_random_graph(Rng& rng, int node_count) {
  RandomGraph graph;
  graph.nodes.reserve(static_cast<std::size_t>(node_count));
  for (int index = 0; index < node_count; ++index) {
    graph.nodes.push_back(random_node(rng, index));
  }
  return graph;
}

/// Registers the attempts of `graph` against `harness`, keeping the graph
/// usable across several calls so a scenario can interleave mutations.
void apply_attempts(Harness& harness, RandomGraph& graph, Rng& rng, int first, int last, bool bias_towards_acyclic) {
  for (int index = first; index < last; ++index) {
    Attempt attempt;
    attempt.from = graph.nodes[rng.below(static_cast<std::uint32_t>(graph.nodes.size()))];
    attempt.to = graph.nodes[rng.below(static_cast<std::uint32_t>(graph.nodes.size()))];
    const std::uint32_t kind_pick = bias_towards_acyclic ? rng.below(3) : rng.below(6);
    attempt.kind = kKinds[kind_pick];
    attempt.strength = kStrengths[rng.below(3)];
    attempt.direction = (rng.below(4) == 0) ? Direction::Mutual : Direction::DependsOn;

    const auto outcome = harness.try_add(fdep_test::make_spec(attempt.from, attempt.to, attempt.kind,
                                                             attempt.strength, attempt.direction,
                                                             LifecycleState::Active),
                                         harness.generation());
    graph.attempts.push_back(attempt);
    if (outcome) {
      graph.outcomes.push_back(ErrorCode::Ok);
      graph.accepted_ids.push_back(outcome.value().id);
    } else {
      graph.outcomes.push_back(outcome.error().code());
    }
  }
}

void check_graph_invariants(const RegistrySnapshot& snapshot) {
  const std::span<const DependencyEdgeRecord> edges = snapshot.edges();
  for (std::size_t index = 0; index < edges.size(); ++index) {
    if (index > 0 && edges[index] < edges[index - 1]) {
      fdep_test::fail_now("the property run produced a non canonical edge order");
    }
    const auto& record = edges[index];
    DependencyEdgeSpec spec;
    spec.source = record.source();
    spec.target = record.target();
    spec.kind = record.kind();
    spec.strength = record.strength();
    spec.direction = record.direction();
    // The stored lifecycle may be anything, so validate the payload the way
    // the registry does and check the lifecycle separately.
    spec.initial_lifecycle = LifecycleState::Active;
    spec.constraints = record.constraints();
    spec.provenance = record.provenance();
    if (!spec.validate(snapshot.limits()).ok()) {
      fdep_test::fail_now("a stored edge does not satisfy the declaration rules");
    }
    const auto ordinal = static_cast<unsigned>(record.lifecycle());
    if (ordinal < 1 || ordinal > kLifecycleStateCount) {
      fdep_test::fail_now("a stored edge is in an unrecognised lifecycle state");
    }
  }
  const auto obligation = snapshot.verify_acyclic_obligation();
  if (!obligation || !obligation.value().satisfied) {
    fdep_test::fail_now("a stored graph violates the acyclic obligation");
  }
}

}  // namespace

FDEP_TEST(property, random_typed_graphs_stay_well_formed) {
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    std::cout << "        " << fdep_test::seed_text(seed) << " nodes=40 attempts=120\n";
    Rng rng{seed};
    Harness harness = Harness::ephemeral();
    RandomGraph graph = build_random_graph(rng, 40);
    apply_attempts(harness, graph, rng, 0, 120, false);

    check_graph_invariants(harness.snapshot());
    FDEP_CHECK_EQ(harness.snapshot().edge_count(), graph.accepted_ids.size());

    std::size_t rejected = 0;
    for (const auto code : graph.outcomes) {
      if (code != ErrorCode::Ok && code != ErrorCode::DuplicateEdge) {
        ++rejected;
        // Only the two documented refusals may appear for these declarations.
        FDEP_CHECK(code == ErrorCode::EndpointDomainNotAllowed ||
                   code == ErrorCode::DirectionNotAllowedForKind ||
                   code == ErrorCode::ProhibitedCycle || code == ErrorCode::SelfDependencyProhibited);
      }
    }
    FDEP_CHECK(rejected > 0);

    // Every accepted edge is present exactly once, and the key set matches.
    std::set<std::string> keys;
    for (const auto& record : harness.snapshot().edges()) {
      FDEP_CHECK(keys.insert(record.key().to_text()).second);
    }
    FDEP_CHECK_EQ(keys.size(), graph.accepted_ids.size());
  }
}

FDEP_TEST(property, acyclic_kinds_never_produce_a_stored_cycle) {
  std::size_t prohibited_seen = 0;
  for (std::uint64_t seed = 100; seed <= 107; ++seed) {
    std::cout << "        " << fdep_test::seed_text(seed) << " nodes=24 attempts=200 bias=acyclic\n";
    Rng rng{seed};
    // A small node pool with the acyclic kinds favoured makes cycles likely to
    // be attempted, which is exactly what this property wants to stress.
    Harness harness = Harness::ephemeral();
    RandomGraph graph = build_random_graph(rng, 24);
    apply_attempts(harness, graph, rng, 0, 200, true);

    for (const auto code : graph.outcomes) {
      if (code == ErrorCode::ProhibitedCycle) {
        ++prohibited_seen;
      }
    }
    check_graph_invariants(harness.snapshot());
  }
  // The generator does attempt cycles; if it never did, the property would be
  // vacuous and this check would catch that.
  FDEP_CHECK(prohibited_seen > 0);
}

FDEP_TEST(property, registration_is_deterministic_for_a_fixed_seed) {
  for (std::uint64_t seed = 200; seed <= 205; ++seed) {
    Rng first_rng{seed};
    Harness first = Harness::ephemeral();
    RandomGraph first_graph = build_random_graph(first_rng, 30);
    apply_attempts(first, first_graph, first_rng, 0, 90, false);

    Rng second_rng{seed};
    Harness second = Harness::ephemeral();
    RandomGraph second_graph = build_random_graph(second_rng, 30);
    apply_attempts(second, second_graph, second_rng, 0, 90, false);

    FDEP_CHECK(first.snapshot().state_digest() == second.snapshot().state_digest());
    FDEP_CHECK(first_graph.outcomes == second_graph.outcomes);
    FDEP_CHECK_EQ(first.generation().value(), second.generation().value());

    const auto first_text = export_text(first.snapshot());
    const auto second_text = export_text(second.snapshot());
    FDEP_REQUIRE_OK(first_text);
    FDEP_REQUIRE_OK(second_text);
    FDEP_CHECK(first_text.value() == second_text.value());
  }
}

FDEP_TEST(property, deletion_and_reinsertion_never_reuses_an_identity) {
  for (std::uint64_t seed = 300; seed <= 305; ++seed) {
    Rng rng{seed};
    Harness harness = Harness::ephemeral();
    RandomGraph graph = build_random_graph(rng, 30);
    apply_attempts(harness, graph, rng, 0, 80, false);
    FDEP_REQUIRE(!graph.accepted_ids.empty());

    // Record every identity that has ever existed.
    std::set<std::uint64_t> used_ids;
    for (const auto id : graph.accepted_ids) {
      used_ids.insert(id.value());
    }

    // Remove a third of the edges, in a shuffled but reproducible order.
    std::vector<DependencyEdgeId> removed;
    for (const auto id : graph.accepted_ids) {
      if (rng.below(3) == 0) {
        const auto* record = harness.snapshot().find_edge(id);
        FDEP_REQUIRE(record != nullptr);
        const auto status = harness.remove(id, record->revision());
        FDEP_REQUIRE(status.ok());
        removed.push_back(id);
      }
    }
    check_graph_invariants(harness.snapshot());
    FDEP_CHECK_EQ(harness.snapshot().edge_count(), graph.accepted_ids.size() - removed.size());

    // Re-register the removed declarations. They are new edges with new
    // identities, and the identities that went away are never handed out again.
    std::size_t reinserted = 0;
    for (std::size_t position = 0; position < graph.attempts.size(); ++position) {
      if (graph.outcomes[position] != ErrorCode::Ok) {
        continue;
      }
      const DependencyEdgeId original = graph.accepted_ids[reinserted];
      ++reinserted;
      if (std::find(removed.begin(), removed.end(), original) == removed.end()) {
        continue;
      }
      const Attempt& attempt = graph.attempts[position];
      const auto outcome = harness.try_add(
          fdep_test::make_spec(attempt.from, attempt.to, attempt.kind, attempt.strength, attempt.direction,
                               LifecycleState::Active),
          harness.generation());
      FDEP_REQUIRE_OK(outcome);
      FDEP_CHECK(!used_ids.count(outcome.value().id.value()));
      used_ids.insert(outcome.value().id.value());
      FDEP_CHECK_EQ(outcome.value().revision.value(), std::uint64_t{1});
      FDEP_CHECK(!outcome.value().already_present);
    }
    check_graph_invariants(harness.snapshot());
  }
}

FDEP_TEST(property, stale_generations_are_always_refused_and_change_nothing) {
  for (std::uint64_t seed = 400; seed <= 403; ++seed) {
    Rng rng{seed};
    Harness harness = Harness::ephemeral();
    RandomGraph graph = build_random_graph(rng, 20);
    apply_attempts(harness, graph, rng, 0, 40, false);

    const DependencyGeneration live = harness.generation();
    const ContentDigest digest = harness.snapshot().state_digest();
    const std::size_t count = harness.snapshot().edge_count();

    // Every generation below the live one is stale, and every stale attempt is
    // refused without touching the state.
    for (std::uint64_t offset = 1; offset <= 3 && offset <= live.value(); ++offset) {
      const DependencyGeneration stale = DependencyGeneration::from_value(live.value() - offset);
      const auto outcome = harness.try_add(fdep_test::make_spec(graph.nodes[0], graph.nodes[1],
                                                               DependencyKind::ControlDependsOn),
                                           stale);
      FDEP_CHECK_CODE(outcome, ErrorCode::StaleGeneration);
    }
    // A generation above the live one is not "stale" but is still not current.
    const auto ahead = harness.try_add(
        fdep_test::make_spec(graph.nodes[0], graph.nodes[1], DependencyKind::ControlDependsOn),
        DependencyGeneration::from_value(live.value() + 1));
    FDEP_CHECK_CODE(ahead, ErrorCode::StaleGeneration);

    FDEP_CHECK_EQ(harness.generation().value(), live.value());
    FDEP_CHECK_EQ(harness.snapshot().state_digest(), digest);
    FDEP_CHECK_EQ(harness.snapshot().edge_count(), count);
  }
}

FDEP_TEST(property, bounded_traversals_are_deterministic_and_bounded) {
  for (std::uint64_t seed = 500; seed <= 505; ++seed) {
    Rng rng{seed};
    Harness harness = Harness::ephemeral();
    RandomGraph graph = build_random_graph(rng, 50);
    apply_attempts(harness, graph, rng, 0, 150, false);
    const RegistrySnapshot snapshot = harness.snapshot();
    FDEP_REQUIRE(!snapshot.nodes().empty());

    for (int attempt = 0; attempt < 12; ++attempt) {
      const DependencyNodeRef root = snapshot.nodes()[rng.below(
          static_cast<std::uint32_t>(snapshot.nodes().size()))];
      TraversalRequest request;
      request.root = root;
      request.direction = rng.below(2) == 0 ? TraversalDirection::Dependencies : TraversalDirection::Dependents;
      request.max_depth = 1 + rng.below(6);
      request.max_nodes = 1 + rng.below(20);
      request.filter.lifecycles = LifecycleMask::all();

      const auto first = snapshot.transitive_closure(request);
      FDEP_REQUIRE_OK(first);
      FDEP_CHECK(first.value().entries.size() <= request.max_nodes);
      FDEP_CHECK_EQ(first.value().truncated, first.value().stop != TraversalStop::Complete);
      if (first.value().stop == TraversalStop::NodeLimit) {
        FDEP_CHECK_EQ(first.value().entries.size(), static_cast<std::size_t>(request.max_nodes));
      }
      if (first.value().stop == TraversalStop::DepthLimit) {
        for (const auto& entry : first.value().entries) {
          FDEP_CHECK(entry.depth <= request.max_depth);
        }
      }
      // Every reported node is distinct, and no edge is reported twice for the
      // same node.
      std::set<std::string> seen;
      for (const auto& entry : first.value().entries) {
        FDEP_CHECK(seen.insert(entry.node.to_canonical()).second);
        FDEP_CHECK(entry.depth >= 1);
        FDEP_CHECK(entry.depth <= request.max_depth);
        FDEP_CHECK(is_assigned(entry.via_edge));
      }
      const auto second = snapshot.transitive_closure(request);
      FDEP_REQUIRE_OK(second);
      FDEP_CHECK_EQ(second.value().entries.size(), first.value().entries.size());
      FDEP_CHECK(second.value().stop == first.value().stop);
      for (std::size_t index = 0; index < first.value().entries.size(); ++index) {
        FDEP_CHECK(second.value().entries[index].node == first.value().entries[index].node);
        FDEP_CHECK_EQ(second.value().entries[index].depth, first.value().entries[index].depth);
        FDEP_CHECK_EQ(second.value().entries[index].via_edge.value(),
                      first.value().entries[index].via_edge.value());
      }
    }
  }
}

FDEP_TEST(property, reachability_agrees_between_the_closure_and_the_path_explanation) {
  for (std::uint64_t seed = 600; seed <= 603; ++seed) {
    Rng rng{seed};
    Harness harness = Harness::ephemeral();
    RandomGraph graph = build_random_graph(rng, 24);
    apply_attempts(harness, graph, rng, 0, 80, false);
    const RegistrySnapshot snapshot = harness.snapshot();
    FDEP_REQUIRE(!snapshot.nodes().empty());

    for (int attempt = 0; attempt < 6; ++attempt) {
      const DependencyNodeRef root =
          snapshot.nodes()[rng.below(static_cast<std::uint32_t>(snapshot.nodes().size()))];
      TraversalRequest request;
      request.root = root;
      request.direction = TraversalDirection::Dependencies;
      request.max_depth = 8;
      request.max_nodes = 64;
      request.filter.lifecycles = LifecycleMask::all();
      const auto closure = snapshot.transitive_closure(request);
      FDEP_REQUIRE_OK(closure);

      for (const auto& entry : closure.value().entries) {
        PathRequest path;
        path.from = root;
        path.to = entry.node;
        path.direction = TraversalDirection::Dependencies;
        path.max_depth = 8;
        path.max_nodes = 64;
        path.filter.lifecycles = LifecycleMask::all();
        const auto explained = snapshot.explain_path(path);
        FDEP_REQUIRE_OK(explained);
        FDEP_CHECK(explained.value().found);
        FDEP_CHECK(explained.value().hops() <= entry.depth);
        if (explained.value().found) {
          // The explanation is a real chain of stored edges.
          for (const auto& step : explained.value().steps) {
            FDEP_CHECK(snapshot.find_edge(step.edge) != nullptr);
          }
        }
      }
    }
  }
}

FDEP_TEST(property, strongly_connected_components_partition_the_graph) {
  std::size_t cyclic_total = 0;
  for (std::uint64_t seed = 700; seed <= 705; ++seed) {
    Rng rng{seed};
    Harness harness = Harness::ephemeral();
    RandomGraph graph = build_random_graph(rng, 30);
    apply_attempts(harness, graph, rng, 0, 140, false);
    const RegistrySnapshot snapshot = harness.snapshot();

    ComponentRequest request;
    request.include_singletons = true;
    request.max_nodes = 4096;
    request.filter.lifecycles = LifecycleMask::all();
    const auto result = snapshot.strongly_connected_components(request);
    FDEP_REQUIRE_OK(result);

    std::map<std::string, std::size_t> membership;
    for (std::size_t index = 0; index < result.value().components.size(); ++index) {
      const auto& component = result.value().components[index];
      FDEP_CHECK(!component.members.empty());
      for (const auto& member : component.members) {
        FDEP_CHECK(membership.emplace(member.to_canonical(), index).second);
      }
      FDEP_CHECK_EQ(component.cyclic, component.size() > 1);
      if (component.cyclic) {
        ++cyclic_total;
      }
    }
    // Every node of the graph is in exactly one component, and every edge is
    // inside exactly one component or between two of them.
    FDEP_CHECK_EQ(membership.size(), snapshot.node_count());

    for (const auto& component : result.value().components) {
      if (!component.cyclic) {
        continue;
      }
      // A cyclic component really is strongly connected: each member reaches
      // the next one through a stored path.
      for (std::size_t index = 0; index + 1 < component.members.size(); ++index) {
        PathRequest path;
        path.from = component.members[index];
        path.to = component.members[index + 1];
        path.direction = TraversalDirection::Dependencies;
        path.max_depth = 16;
        path.max_nodes = 128;
        path.filter.lifecycles = LifecycleMask::all();
        const auto explained = snapshot.explain_path(path);
        FDEP_REQUIRE_OK(explained);
        FDEP_CHECK(explained.value().found);
      }
    }
  }
  FDEP_CHECK(cyclic_total > 0);
}

FDEP_TEST(property, legal_cycles_are_enumerable_and_never_explode_the_analysis) {
  std::size_t total_cycles = 0;
  for (std::uint64_t seed = 800; seed <= 805; ++seed) {
    Rng rng{seed};
    Harness harness = Harness::ephemeral();
    // A dense control dependency graph: many mutual and cyclic relations.
    RandomGraph graph = build_random_graph(rng, 12);
    for (int index = 0; index < 60; ++index) {
      Attempt attempt;
      attempt.from = graph.nodes[rng.below(12)];
      attempt.to = graph.nodes[rng.below(12)];
      attempt.kind = DependencyKind::ControlDependsOn;
      attempt.strength = kStrengths[rng.below(3)];
      attempt.direction = rng.below(2) == 0 ? Direction::Mutual : Direction::DependsOn;
      const auto outcome = harness.try_add(
          fdep_test::make_spec(attempt.from, attempt.to, attempt.kind, attempt.strength, attempt.direction,
                               LifecycleState::Active),
          harness.generation());
      graph.attempts.push_back(attempt);
      graph.outcomes.push_back(outcome ? ErrorCode::Ok : outcome.error().code());
    }
    check_graph_invariants(harness.snapshot());

    CycleRequest request;
    request.max_length = 6;
    request.max_cycles = 32;
    request.max_nodes = 256;
    request.filter.lifecycles = LifecycleMask::all();
    const auto report = harness.snapshot().enumerate_cycles(request);
    FDEP_REQUIRE_OK(report);
    FDEP_CHECK(report.value().cycles.size() <= request.max_cycles);
    for (const auto& cycle : report.value().cycles) {
      FDEP_CHECK(cycle.length() >= 2);
      FDEP_CHECK(cycle.length() <= request.max_length);
      FDEP_CHECK_EQ(cycle.edges.size(), cycle.nodes.size());
      // The cycle is a real chain: each step's edge exists and runs from one
      // member to the next.
      for (std::size_t index = 0; index < cycle.nodes.size(); ++index) {
        const auto* record = harness.snapshot().find_edge(cycle.edges[index]);
        FDEP_REQUIRE(record != nullptr);
        const bool forward = record->source() == cycle.nodes[index] &&
                             record->target() == cycle.nodes[(index + 1) % cycle.nodes.size()];
        const bool backward = record->direction() == Direction::Mutual &&
                              record->target() == cycle.nodes[index] &&
                              record->source() == cycle.nodes[(index + 1) % cycle.nodes.size()];
        FDEP_CHECK(forward || backward);
      }
      // The smallest member comes first, which is what makes the enumeration
      // canonical.
      for (std::size_t index = 1; index < cycle.nodes.size(); ++index) {
        FDEP_CHECK(cycle.nodes[0] < cycle.nodes[index]);
      }
    }
    total_cycles += report.value().cycles.size();

    // Repeating the analysis gives the same answer.
    const auto again = harness.snapshot().enumerate_cycles(request);
    FDEP_REQUIRE_OK(again);
    FDEP_CHECK_EQ(again.value().cycles.size(), report.value().cycles.size());
  }
  FDEP_CHECK(total_cycles > 0);
}

FDEP_TEST(property, an_impact_cone_is_a_subset_of_the_wider_reading) {
  for (std::uint64_t seed = 900; seed <= 903; ++seed) {
    Rng rng{seed};
    Harness harness = Harness::ephemeral();
    RandomGraph graph = build_random_graph(rng, 30);
    apply_attempts(harness, graph, rng, 0, 120, false);
    const RegistrySnapshot snapshot = harness.snapshot();
    FDEP_REQUIRE(!snapshot.nodes().empty());

    for (int attempt = 0; attempt < 8; ++attempt) {
      const DependencyNodeRef origin =
          snapshot.nodes()[rng.below(static_cast<std::uint32_t>(snapshot.nodes().size()))];
      ImpactConeRequest hard;
      hard.origin = origin;
      hard.max_depth = 8;
      hard.max_nodes = 128;
      const auto hard_cone = snapshot.impact_cone(hard);
      FDEP_REQUIRE_OK(hard_cone);

      ImpactConeRequest wide = hard;
      wide.strengths = DependencyStrengthMask::all();
      const auto wide_cone = snapshot.impact_cone(wide);
      FDEP_REQUIRE_OK(wide_cone);

      std::set<std::string> wide_nodes;
      for (const auto& entry : wide_cone.value().entries) {
        wide_nodes.insert(entry.node.to_canonical());
      }
      for (const auto& entry : hard_cone.value().entries) {
        FDEP_CHECK(wide_nodes.count(entry.node.to_canonical()) == 1);
      }
      FDEP_CHECK(hard_cone.value().size() <= wide_cone.value().size());

      // Both are deterministic.
      const auto again = snapshot.impact_cone(hard);
      FDEP_REQUIRE_OK(again);
      FDEP_CHECK_EQ(again.value().size(), hard_cone.value().size());
      for (std::size_t index = 0; index < hard_cone.value().entries.size(); ++index) {
        FDEP_CHECK(again.value().entries[index].node == hard_cone.value().entries[index].node);
        FDEP_CHECK_EQ(again.value().entries[index].depth, hard_cone.value().entries[index].depth);
      }
    }
  }
}

FDEP_TEST(property, a_graph_full_of_cycles_still_answers_every_query) {
  // A deliberately hostile shape: one densely cyclic core plus a long tail.
  // Nothing here may recurse forever, overflow a bound, or take unbounded time.
  for (std::uint64_t seed = 1000; seed <= 1002; ++seed) {
    Rng rng{seed};
    Harness harness = Harness::ephemeral();
    constexpr int kCore = 40;
    std::vector<DependencyNodeRef> core;
    core.reserve(kCore);
    for (int index = 0; index < kCore; ++index) {
      core.push_back(DependencyNodeRef::create(NodeDomain::Asset, "core-" + std::to_string(index)).value());
    }
    for (int index = 0; index < kCore; ++index) {
      for (int step = 1; step <= 3; ++step) {
        const auto& from = core[static_cast<std::size_t>(index)];
        const auto& to = core[static_cast<std::size_t>((index + step) % kCore)];
        harness.add(from, to, DependencyKind::ControlDependsOn, kStrengths[rng.below(3)]);
      }
    }
    for (int index = 0; index < 40; ++index) {
      const auto node = DependencyNodeRef::create(NodeDomain::Asset, "tail-" + std::to_string(index)).value();
      harness.add(node, core[static_cast<std::size_t>(index) % kCore], DependencyKind::ControlDependsOn);
    }
    check_graph_invariants(harness.snapshot());

    TraversalRequest request;
    request.root = core[0];
    request.direction = TraversalDirection::Dependents;
    request.max_depth = 64;
    request.max_nodes = 4096;
    request.filter.lifecycles = LifecycleMask::all();
    const auto closure = harness.snapshot().transitive_closure(request);
    FDEP_REQUIRE_OK(closure);
    // The whole graph is reachable from the core, and each node appears once.
    // The root itself is not repeated in the closure.
    FDEP_CHECK_EQ(closure.value().entries.size(), static_cast<std::size_t>(kCore + 40 - 1));
    FDEP_CHECK(closure.value().stop == TraversalStop::Complete);

    ComponentRequest components;
    components.include_singletons = true;
    components.max_nodes = 4096;
    components.filter.lifecycles = LifecycleMask::all();
    const auto result = harness.snapshot().strongly_connected_components(components);
    FDEP_REQUIRE_OK(result);
    FDEP_REQUIRE(!result.value().components.empty());
    // The core is one large component.
    bool found_core = false;
    for (const auto& component : result.value().components) {
      if (component.size() == static_cast<std::size_t>(kCore)) {
        found_core = true;
      }
    }
    FDEP_CHECK(found_core);

    CycleRequest cycles;
    cycles.max_length = 8;
    cycles.max_cycles = 16;
    cycles.max_nodes = 4096;
    cycles.filter.lifecycles = LifecycleMask::all();
    const auto report = harness.snapshot().enumerate_cycles(cycles);
    FDEP_REQUIRE_OK(report);
    FDEP_CHECK(report.value().cycles.size() <= cycles.max_cycles);
  }
}
