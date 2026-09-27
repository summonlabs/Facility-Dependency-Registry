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

/// Example 06: declared versus unresolved references.
///
/// A declaration says "this identity was observed to exist". An endpoint that
/// was never declared is still a real node of the dependency graph: edges to it
/// are stored, validated and traversed. What is missing is only the claim that
/// something exists behind the identity, and the registry reports exactly that
/// instead of inventing a node definition to make the graph look complete.

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
using fdep_examples::yes_no;

namespace {

void print_resolution(const fdep::RegistrySnapshot& snapshot, const fdep::DependencyNodeRef& ref) {
  std::cout << "  " << ref.to_canonical() << " -> " << fdep::to_token(snapshot.resolution_of(ref)) << '\n';
}

void print_unresolved(const fdep::RegistrySnapshot& snapshot) {
  const fdep::NodeRefSet unresolved = unwrap(snapshot.unresolved_endpoints(), "unresolved_endpoints");
  std::cout << "  unresolved_endpoints (" << unresolved.size() << ')';
  for (const auto& ref : unresolved.refs()) {
    std::cout << "\n    " << ref.to_canonical();
  }
  std::cout << '\n';
}

}  // namespace

int main() {
  auto registry = unwrap(fdep::DependencyRegistry::open_ephemeral(), "open an ephemeral registry");

  // Neither endpoint of this edge was ever declared. The declaration is still
  // accepted: the registry records relationships, and it does not require the
  // objects at the ends of them to be known to it first.
  const fdep::DependencyNodeRef ghost_asset = asset("never-declared-asset");
  const fdep::DependencyNodeRef ghost_feed = feed("never-declared-feed");
  add_edge(registry, make_spec(ghost_asset, ghost_feed, fdep::DependencyKind::RequiresPowerFrom,
                               fdep::DependencyStrength::Hard, fdep::Direction::DependsOn,
                               fdep::LifecycleState::Active));

  const fdep::RegistrySnapshot before = registry.snapshot();
  std::cout << "registered one edge on endpoints that were never declared; edge-count=" << before.edge_count()
            << " declaration-count=" << before.declared_ref_count() << " node-count=" << before.node_count()
            << '\n';

  std::cout << "\n-- resolution before any declaration --\n";
  print_resolution(before, ghost_asset);
  print_resolution(before, ghost_feed);
  print_resolution(before, feed("feed-nothing-mentions-this"));
  print_unresolved(before);

  // An unresolved endpoint is a first class graph node: the dependency is
  // traversable even though nothing is known about the object behind it.
  fdep::PathRequest path_request;
  path_request.from = ghost_asset;
  path_request.to = ghost_feed;
  const fdep::PathResult path = unwrap(before.explain_path(path_request), "explain_path");
  check(path.found && path.hops() == 1, "the declared dependency is traversable");
  std::cout << "\n  the dependency is traversable: " << path.steps.front().to_text() << '\n';

  // -- declare one of the two endpoints -------------------------------------
  const fdep::DeclareRefOutcome declared =
      declare_ref(registry, ghost_asset, "observed in the asset inventory");
  check(declared.generation_advanced, "a new declaration is a committed mutation");

  const fdep::RegistrySnapshot after = registry.snapshot();
  std::cout << "\n-- resolution after declaring " << ghost_asset.to_canonical() << " --\n";
  print_resolution(after, ghost_asset);
  print_resolution(after, ghost_feed);
  print_unresolved(after);

  // A declaration that no stored edge references is reported separately, so
  // "declared" and "in use" stay distinguishable.
  const fdep::DependencyNodeRef idle_rack = rack("row-z-rack-99");
  declare_ref(registry, idle_rack, "observed in the rack registry but not yet related to anything");
  const fdep::NodeRefSet idle = unwrap(registry.snapshot().unreferenced_declared_refs(),
                                       "unreferenced_declared_refs");
  std::cout << "\n  unreferenced_declared_refs (" << idle.size() << ')';
  for (const auto& ref : idle.refs()) {
    std::cout << "\n    " << ref.to_canonical();
  }
  std::cout << '\n';

  std::cout << "\n-- what the registry does and does not claim --\n";
  std::cout << "  " << ghost_feed.to_canonical()
            << " is unresolved: an edge references it, no declaration describes it.\n";
  std::cout << "  unresolved=" << yes_no(after.resolution_of(ghost_feed) == fdep::RefResolution::Unresolved)
            << " is a fact about the graph, not a failure of it. The registry never invents the object behind an\n";
  std::cout << "  identity, never fabricates a declaration for it, and never drops the edge that named it.\n";
  std::cout << "  Declaring the identity is a separate, audited mutation, made by a party that observed it.\n";
  return 0;
}
