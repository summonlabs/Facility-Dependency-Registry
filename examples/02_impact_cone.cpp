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

/// Example 02: the bounded impact cone.
///
/// An impact cone answers "what depends on this node", by following edges
/// backwards from the origin. Which strengths propagate is the caller's
/// decision, because strength is a declaration about consequences and this
/// repository never invents one.

#include <iostream>
#include <string>

#include "support.hpp"

namespace fdep = facility_dependency_registry;

using fdep_examples::add_edge;
using fdep_examples::asi;
using fdep_examples::asset;
using fdep_examples::check;
using fdep_examples::declare_ref;
using fdep_examples::dfi;
using fdep_examples::feed;
using fdep_examples::make_spec;
using fdep_examples::unwrap;
using fdep_examples::yes_no;

namespace {

void print_cone(const char* title, const fdep::ImpactCone& cone) {
  std::cout << title << "\n";
  std::cout << "  impacted=" << cone.size() << " stop=" << fdep::to_token(cone.stop)
            << " truncated=" << yes_no(cone.truncated) << " nodes-examined=" << cone.nodes_examined
            << " edges-examined=" << cone.edges_examined << '\n';
  for (const auto& entry : cone.entries) {
    std::cout << "  depth " << entry.depth << "  " << entry.node.to_canonical() << "  via edge "
              << fdep::to_text(entry.via_edge) << " from " << entry.via_node.to_canonical() << '\n';
  }
}

}  // namespace

int main() {
  auto registry = unwrap(fdep::DependencyRegistry::open_ephemeral(), "open an ephemeral registry");

  const fdep::DependencyNodeRef origin = feed("feed-a1");
  const fdep::DependencyNodeRef node1 = asset("pod-1-node-1");
  const fdep::DependencyNodeRef node2 = asset("pod-1-node-2");
  const fdep::DependencyNodeRef node3 = asset("pod-1-node-3");
  const fdep::DependencyNodeRef node4 = asset("pod-1-node-4");
  const fdep::DependencyNodeRef cluster = asi("asi-pod-1");
  const fdep::DependencyNodeRef fabric1 = dfi("dfi-fabric-1");
  const fdep::DependencyNodeRef fabric2 = dfi("dfi-fabric-2");

  declare_ref(registry, origin, "observed in the electrical domain export");
  declare_ref(registry, node1, "observed in the asset inventory");
  declare_ref(registry, node2, "observed in the asset inventory");
  declare_ref(registry, node3, "observed in the asset inventory");
  declare_ref(registry, node4, "observed in the asset inventory");
  declare_ref(registry, cluster, "observed in the composed ASI domain export");
  declare_ref(registry, fabric1, "observed in the composed DFI domain export");
  declare_ref(registry, fabric2, "observed in the composed DFI domain export");

  // The feed fans out to four assets. Three declare a hard consequence, one
  // declares only degradation.
  add_edge(registry, make_spec(node1, origin, fdep::DependencyKind::RequiresPowerFrom, fdep::DependencyStrength::Hard,
                               fdep::Direction::DependsOn, fdep::LifecycleState::Active));
  add_edge(registry, make_spec(node2, origin, fdep::DependencyKind::RequiresPowerFrom, fdep::DependencyStrength::Hard,
                               fdep::Direction::DependsOn, fdep::LifecycleState::Active));
  add_edge(registry, make_spec(node3, origin, fdep::DependencyKind::RequiresPowerFrom, fdep::DependencyStrength::Hard,
                               fdep::Direction::DependsOn, fdep::LifecycleState::Active));
  add_edge(registry, make_spec(node4, origin, fdep::DependencyKind::RequiresPowerFrom, fdep::DependencyStrength::Soft,
                               fdep::Direction::DependsOn, fdep::LifecycleState::Active));

  // Two control dependencies continue the chain away from the feed, so the cone
  // is more than one level deep.
  add_edge(registry, make_spec(cluster, node1, fdep::DependencyKind::ControlDependsOn,
                               fdep::DependencyStrength::Hard, fdep::Direction::DependsOn,
                               fdep::LifecycleState::Active));
  add_edge(registry, make_spec(fabric1, node2, fdep::DependencyKind::ControlDependsOn,
                               fdep::DependencyStrength::Soft, fdep::Direction::DependsOn,
                               fdep::LifecycleState::Active));
  add_edge(registry, make_spec(fabric2, cluster, fdep::DependencyKind::ControlDependsOn,
                               fdep::DependencyStrength::Hard, fdep::Direction::DependsOn,
                               fdep::LifecycleState::Active));

  const fdep::RegistrySnapshot snapshot = registry.snapshot();
  std::cout << "origin " << origin.to_canonical() << " at generation " << fdep::to_text(snapshot.generation())
            << '\n';

  fdep::ImpactConeRequest hard_request;
  hard_request.origin = origin;
  const fdep::ImpactCone hard_cone = unwrap(snapshot.impact_cone(hard_request), "impact_cone(hard)");

  fdep::ImpactConeRequest soft_request;
  soft_request.origin = origin;
  soft_request.strengths =
      fdep::DependencyStrengthMask::of(fdep::DependencyStrength::Hard).with(fdep::DependencyStrength::Soft);
  const fdep::ImpactCone soft_cone = unwrap(snapshot.impact_cone(soft_request), "impact_cone(hard+soft)");

  std::cout << "\n-- hard dependencies only (the default reading) --\n";
  print_cone("impact cone", hard_cone);

  std::cout << "\n-- hard and soft dependencies --\n";
  print_cone("impact cone", soft_cone);

  std::cout << "\n-- what changed --\n";
  std::cout << "admitting soft edges to the cone adds:";
  for (const auto& entry : soft_cone.entries) {
    if (!hard_cone.reached(entry.node)) {
      std::cout << "\n  depth " << entry.depth << "  " << entry.node.to_canonical();
    }
  }
  std::cout << '\n';
  std::cout << "hard-only impacts " << hard_cone.size() << " node(s); hard+soft impacts " << soft_cone.size()
            << " node(s)\n";
  std::cout << "the origin itself is level 0 and is never listed: the cone holds what is reached from it\n";

  // Nothing is truncated above, so a caller can read the cone as complete.
  check(!hard_cone.truncated && !soft_cone.truncated, "the cone fits inside its bounds");
  check(hard_cone.stop == fdep::TraversalStop::Complete, "the hard cone completed");
  return 0;
}
