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

/// Example 03: legal cycles, and a prohibited one.
///
/// Whether a cycle is legal is a property of the dependency *kind*, not of the
/// graph. Physical supply (power, cooling, housing) must stay acyclic, because a
/// dependency loop there is not a physical arrangement. Control, service and
/// composition relationships are frequently mutual in reality, so the registry
/// permits cycles for those kinds and reports them rather than refusing them.

#include <iostream>
#include <string>

#include "support.hpp"

namespace fdep = facility_dependency_registry;

using fdep_examples::add_edge;
using fdep_examples::asset;
using fdep_examples::check;
using fdep_examples::declare_ref;
using fdep_examples::feed;
using fdep_examples::make_spec;
using fdep_examples::rack;
using fdep_examples::unwrap;

namespace {

void print_component(const fdep::StronglyConnectedComponent& component) {
  std::cout << "  component cyclic=" << fdep_examples::yes_no(component.cyclic)
            << " members=" << component.size() << " edges=" << component.edges.size() << '\n';
  for (const auto& member : component.members) {
    std::cout << "    member " << member.to_canonical() << '\n';
  }
  for (const auto edge : component.edges) {
    std::cout << "    edge   " << fdep::to_text(edge) << '\n';
  }
}

}  // namespace

int main() {
  auto registry = unwrap(fdep::DependencyRegistry::open_ephemeral(), "open an ephemeral registry");

  // Two control planes that depend on each other's control path. Each direction
  // is its own declaration, and each is legal for ControlDependsOn.
  const fdep::DependencyNodeRef control_a = asset("ctrl-plane-a");
  const fdep::DependencyNodeRef control_b = asset("ctrl-plane-b");

  // Two racks joined by one *mutual* control dependency: a single stored edge,
  // read in both directions, which is a two-cycle by definition.
  const fdep::DependencyNodeRef rack_1 = rack("row-a-rack-01");
  const fdep::DependencyNodeRef rack_2 = rack("row-a-rack-02");

  declare_ref(registry, control_a, "observed in the asset inventory");
  declare_ref(registry, control_b, "observed in the asset inventory");
  declare_ref(registry, rack_1, "observed in the rack registry");
  declare_ref(registry, rack_2, "observed in the rack registry");

  add_edge(registry, make_spec(control_a, control_b, fdep::DependencyKind::ControlDependsOn,
                               fdep::DependencyStrength::Hard, fdep::Direction::DependsOn,
                               fdep::LifecycleState::Active));
  add_edge(registry, make_spec(control_b, control_a, fdep::DependencyKind::ControlDependsOn,
                               fdep::DependencyStrength::Hard, fdep::Direction::DependsOn,
                               fdep::LifecycleState::Active));
  add_edge(registry, make_spec(rack_1, rack_2, fdep::DependencyKind::ControlDependsOn,
                               fdep::DependencyStrength::Hard, fdep::Direction::Mutual,
                               fdep::LifecycleState::Active));

  check(!fdep::requires_acyclic(fdep::DependencyKind::ControlDependsOn),
        "a control dependency is not part of the acyclic obligation");

  const fdep::RegistrySnapshot snapshot = registry.snapshot();
  std::cout << "registered 3 control dependencies at generation " << fdep::to_text(snapshot.generation())
            << "; the graph is accepted because ControlDependsOn permits cycles\n";

  // -- (a) strongly connected components ------------------------------------
  std::cout << "\n-- (a) strongly_connected_components --\n";
  fdep::ComponentRequest component_request;
  component_request.include_singletons = false;  // a singleton is not a cycle
  const fdep::SccResult components =
      unwrap(snapshot.strongly_connected_components(component_request), "strongly_connected_components");
  std::cout << "cyclic-components=" << components.cyclic_components << " reported=" << components.components.size()
            << " nodes-examined=" << components.nodes_examined << '\n';
  for (const auto& component : components.components) {
    print_component(component);
  }

  // -- (b) enumerate_cycles -------------------------------------------------
  std::cout << "\n-- (b) enumerate_cycles --\n";
  fdep::CycleRequest cycle_request;
  cycle_request.max_length = 4;
  cycle_request.max_cycles = 16;
  const fdep::CycleReport cycles = unwrap(snapshot.enumerate_cycles(cycle_request), "enumerate_cycles");
  std::cout << "cycles=" << cycles.cycles.size() << " truncated=" << fdep_examples::yes_no(cycles.truncated)
            << " stop=" << fdep::to_token(cycles.stop) << '\n';
  for (const auto& cycle : cycles.cycles) {
    std::cout << "  cycle length=" << cycle.length() << '\n';
    std::cout << "    path  ";
    for (std::size_t index = 0; index < cycle.nodes.size(); ++index) {
      std::cout << (index == 0 ? "" : " -> ") << cycle.nodes[index].to_canonical();
    }
    if (!cycle.nodes.empty()) {
      std::cout << " -> " << cycle.nodes.front().to_canonical();
    }
    std::cout << '\n';
    std::cout << "    edges";
    for (const auto edge : cycle.edges) {
      std::cout << ' ' << fdep::to_text(edge);
    }
    std::cout << '\n';
  }

  // -- (c) a prohibited cycle ----------------------------------------------
  std::cout << "\n-- (c) a cycle the registry refuses --\n";
  const fdep::DependencyNodeRef north = feed("feed-north");
  const fdep::DependencyNodeRef south = feed("feed-south");
  declare_ref(registry, north, "observed in the electrical domain export");
  declare_ref(registry, south, "observed in the electrical domain export");
  add_edge(registry, make_spec(north, south, fdep::DependencyKind::RequiresPowerFrom,
                               fdep::DependencyStrength::Hard, fdep::Direction::DependsOn,
                               fdep::LifecycleState::Active));
  const fdep::DependencyGeneration before = registry.generation();

  fdep::RegisterEdgeRequest closing;
  closing.context.expected_generation = registry.generation();
  closing.spec = make_spec(south, north, fdep::DependencyKind::RequiresPowerFrom, fdep::DependencyStrength::Hard,
                           fdep::Direction::DependsOn, fdep::LifecycleState::Active);
  const auto rejected = registry.register_edge(closing);
  check(!rejected, "closing a power loop must be rejected");
  check(rejected.error().code() == fdep::ErrorCode::ProhibitedCycle,
        "the rejection category must be ProhibitedCycle");
  check(registry.generation() == before, "a rejected mutation publishes nothing and advances no generation");

  check(fdep::requires_acyclic(fdep::DependencyKind::RequiresPowerFrom),
        "power is part of the acyclic obligation");
  std::cout << "RequiresPowerFrom is acyclic-required: "
            << fdep_examples::yes_no(fdep::requires_acyclic(fdep::DependencyKind::RequiresPowerFrom)) << '\n';
  std::cout << "rejected token    = " << fdep::to_token(rejected.error().code()) << '\n';
  std::cout << "rejected meaning  = " << fdep::describe(rejected.error().code()) << '\n';
  std::cout << "rejected detail   = " << rejected.error().detail() << '\n';
  std::cout << "generation stayed = " << fdep::to_text(registry.generation()) << '\n';
  return 0;
}
