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

/// Example 04: generation authority.
///
/// A mutation is addressed to the generation the caller believes is current.
/// There is no "ignore generation" mode: a default constructed context expects
/// generation zero, the empty graph, and is rejected by any registry that has
/// already committed something.

#include <iostream>
#include <string>

#include "support.hpp"

namespace fdep = facility_dependency_registry;

using fdep_examples::asset;
using fdep_examples::check;
using fdep_examples::feed;
using fdep_examples::make_spec;
using fdep_examples::unwrap;
using fdep_examples::yes_no;

int main() {
  auto registry = unwrap(fdep::DependencyRegistry::open_ephemeral(), "open an ephemeral registry");
  const fdep::DependencyGeneration captured = registry.generation();
  std::cout << "captured generation " << fdep::to_text(captured) << " before any mutation\n";

  const fdep::DependencyEdgeSpec spec =
      make_spec(asset("app-01"), feed("feed-a"), fdep::DependencyKind::RequiresPowerFrom,
                fdep::DependencyStrength::Hard, fdep::Direction::DependsOn, fdep::LifecycleState::Active);

  // -- a mutation addressed to the generation that is actually current ------
  fdep::RegisterEdgeRequest first;
  first.context.expected_generation = captured;
  first.spec = spec;
  const fdep::RegisterEdgeOutcome accepted = unwrap(registry.register_edge(first), "register_edge");
  std::cout << "accepted: edge " << fdep::to_text(accepted.id) << " at revision "
            << fdep::to_text(accepted.revision) << ", generation " << fdep::to_text(accepted.generation)
            << ", generation-advanced=" << yes_no(accepted.generation_advanced) << '\n';

  // -- the same declaration retried with the stale context ------------------
  fdep::RegisterEdgeRequest retry;
  retry.context.expected_generation = captured;  // still zero, but the registry moved on
  retry.spec = spec;
  const auto rejected = registry.register_edge(retry);
  check(!rejected, "a mutation addressed to a superseded generation must be rejected");
  check(rejected.error().code() == fdep::ErrorCode::StaleGeneration,
        "the rejection category must be StaleGeneration");

  std::cout << "\nrejected token    = " << fdep::to_token(rejected.error().code()) << '\n';
  std::cout << "rejected detail   = " << rejected.error().detail() << '\n';
  std::cout << "rejected to_string= " << rejected.error().to_string() << '\n';
  std::cout << "generation after the rejection = " << fdep::to_text(registry.generation()) << '\n';
  check(registry.snapshot().edge_count() == 1, "the rejected mutation stored nothing");

  // -- an identical declaration against the current generation --------------
  // The key (endpoints and kind) is already held by an edge with exactly this
  // payload, so this is a successful no-op rather than a duplicate rejection.
  fdep::RegisterEdgeRequest again;
  again.context.expected_generation = registry.generation();
  again.spec = spec;
  const fdep::RegisterEdgeOutcome present = unwrap(registry.register_edge(again), "register_edge (repeat)");
  check(present.already_present, "an identical re-declaration is reported as already present");
  check(!present.generation_advanced, "an identical re-declaration advances no generation");
  check(present.id == accepted.id, "the existing edge keeps its identity");
  check(present.revision == accepted.revision, "the existing edge keeps its revision");
  check(registry.generation() == accepted.generation, "the registry generation is unchanged");

  std::cout << "\nre-declaration: already_present=" << yes_no(present.already_present)
            << " generation_advanced=" << yes_no(present.generation_advanced) << " edge="
            << fdep::to_text(present.id) << " revision=" << fdep::to_text(present.revision) << '\n';
  std::cout << "generation after the re-declaration = " << fdep::to_text(registry.generation())
            << " (unchanged), edge-count = " << registry.snapshot().edge_count() << '\n';

  // -- a context that was never addressed to this registry ------------------
  fdep::RegisterEdgeRequest implicit_default;  // expects generation zero
  implicit_default.spec = make_spec(asset("app-02"), feed("feed-a"), fdep::DependencyKind::RequiresPowerFrom,
                                    fdep::DependencyStrength::Hard, fdep::Direction::DependsOn,
                                    fdep::LifecycleState::Active);
  const auto default_rejected = registry.register_edge(implicit_default);
  check(!default_rejected && default_rejected.error().code() == fdep::ErrorCode::StaleGeneration,
        "a default constructed context is stale for any non-empty registry");
  std::cout << "\ndefault context token  = " << fdep::to_token(default_rejected.error().code()) << '\n';
  std::cout << "default context detail = " << default_rejected.error().detail() << '\n';
  std::cout << "final generation       = " << fdep::to_text(registry.generation()) << ", edge-count = "
            << registry.snapshot().edge_count() << '\n';
  return 0;
}
