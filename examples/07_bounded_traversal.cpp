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

/// Example 07: bounded traversal and explicit truncation.
///
/// Every traversal carries its own depth and node bounds, and the registry
/// reports *why* it stopped. A partial answer is never presented as a complete
/// one: `truncated` is set and `stop` names the bound that ended the walk, so a
/// caller can never mistake a prefix of the reachable set for all of it.

#include <cstdint>
#include <iostream>
#include <string>

#include "support.hpp"

namespace fdep = facility_dependency_registry;

using fdep_examples::add_edge;
using fdep_examples::asset;
using fdep_examples::check;
using fdep_examples::declare_ref;
using fdep_examples::make_spec;
using fdep_examples::unwrap;
using fdep_examples::yes_no;

namespace {

void print_run(const char* label, const fdep::TraversalResult& result) {
  std::cout << label << '\n';
  std::cout << "  reached=" << result.size() << " stop=" << fdep::to_token(result.stop)
            << " truncated=" << yes_no(result.truncated) << " nodes-examined=" << result.nodes_examined
            << " edges-examined=" << result.edges_examined << '\n';
  for (const auto& entry : result.entries) {
    std::cout << "  depth " << entry.depth << "  " << entry.node.to_canonical() << "  via edge "
              << fdep::to_text(entry.via_edge) << '\n';
  }
}

}  // namespace

int main() {
  auto registry = unwrap(fdep::DependencyRegistry::open_ephemeral(), "open an ephemeral registry");

  // root -> three children -> three grandchildren -> one great-grandchild.
  const fdep::DependencyNodeRef n01 = asset("chain-01");
  const fdep::DependencyNodeRef n02 = asset("chain-02");
  const fdep::DependencyNodeRef n03 = asset("chain-03");
  const fdep::DependencyNodeRef n04 = asset("chain-04");
  const fdep::DependencyNodeRef n05 = asset("chain-05");
  const fdep::DependencyNodeRef n06 = asset("chain-06");
  const fdep::DependencyNodeRef n07 = asset("chain-07");
  const fdep::DependencyNodeRef n08 = asset("chain-08");

  for (const auto& node : {n01, n02, n03, n04, n05, n06, n07, n08}) {
    declare_ref(registry, node, "observed in the asset inventory");
  }
  for (const auto& [from, to] : {std::pair{n01, n02}, std::pair{n01, n03}, std::pair{n01, n04},
                                 std::pair{n02, n05}, std::pair{n03, n06}, std::pair{n04, n07},
                                 std::pair{n05, n08}}) {
    add_edge(registry, make_spec(from, to, fdep::DependencyKind::ControlDependsOn, fdep::DependencyStrength::Hard,
                                 fdep::Direction::DependsOn, fdep::LifecycleState::Active));
  }

  const fdep::RegistrySnapshot snapshot = registry.snapshot();
  const fdep::RegistryLimits& limits = snapshot.limits();
  std::cout << "graph: 8 nodes, 7 edges, generation " << fdep::to_text(snapshot.generation())
            << "; configured bounds are max-traversal-depth=" << limits.max_traversal_depth
            << " max-traversal-nodes=" << limits.max_traversal_nodes << '\n';

  // -- a traversal that fits -------------------------------------------------
  fdep::TraversalRequest full_request;
  full_request.root = n01;
  full_request.direction = fdep::TraversalDirection::Dependencies;
  full_request.max_depth = 4;
  const fdep::TraversalResult full = unwrap(snapshot.transitive_closure(full_request), "transitive_closure");
  std::cout << "\n-- within bounds --\n";
  print_run("transitive_closure root=asset:chain-01 max-depth=4 max-nodes=4096", full);
  check(!full.truncated && full.stop == fdep::TraversalStop::Complete, "the whole reachable set was visited");
  check(full.reached(n08) && full.depth_of(n08).value() == 3u, "the deepest node is reached at depth 3");

  // -- a depth bound that truncates -----------------------------------------
  fdep::TraversalRequest depth_request = full_request;
  depth_request.max_depth = 2;
  const fdep::TraversalResult depth_limited =
      unwrap(snapshot.transitive_closure(depth_request), "transitive_closure(depth bound)");
  std::cout << "\n-- depth bound reached --\n";
  print_run("transitive_closure root=asset:chain-01 max-depth=2 max-nodes=4096", depth_limited);
  check(depth_limited.truncated, "a depth bound that cuts the walk reports truncation");
  check(depth_limited.stop == fdep::TraversalStop::DepthLimit, "stop names the depth bound");
  check(!depth_limited.reached(n08), "the node beyond the depth bound is not reported");

  // -- a node bound that truncates ------------------------------------------
  fdep::TraversalRequest node_request = full_request;
  node_request.max_nodes = 2;
  const fdep::TraversalResult node_limited =
      unwrap(snapshot.transitive_closure(node_request), "transitive_closure(node bound)");
  std::cout << "\n-- node bound reached --\n";
  print_run("transitive_closure root=asset:chain-01 max-depth=4 max-nodes=2", node_limited);
  check(node_limited.truncated, "a node bound that cuts the walk reports truncation");
  check(node_limited.stop == fdep::TraversalStop::NodeLimit, "stop names the node bound");
  check(node_limited.size() == 2, "the node bound is applied exactly");

  // -- a request above the configured bound ---------------------------------
  fdep::TraversalRequest over_request = full_request;
  over_request.max_depth = limits.max_traversal_depth + 1u;
  const auto rejected = snapshot.transitive_closure(over_request);
  check(!rejected, "a depth above the configured maximum is rejected");
  check(rejected.error().code() == fdep::ErrorCode::RequestLimitExceeded,
        "the rejection category must be RequestLimitExceeded");
  std::cout << "\n-- above the configured bound --\n";
  std::cout << "  requested max-depth=" << over_request.max_depth << ", configured maximum="
            << limits.max_traversal_depth << '\n';
  std::cout << "  rejected token  = " << fdep::to_token(rejected.error().code()) << '\n';
  std::cout << "  rejected detail = " << rejected.error().detail() << '\n';
  std::cout << "  an out of range bound is refused before anything is walked, never clamped to the maximum\n";
  return 0;
}
