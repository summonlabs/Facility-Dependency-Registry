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

/// Example 08: diffing two generations.
///
/// A snapshot is immutable and self-contained, so any two of them can be
/// compared at any time, in any order. The diff is a merge join over the
/// canonical edge order: the result depends on the two states, never on the
/// order in which the mutations that produced them were applied.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "support.hpp"

namespace fdep = facility_dependency_registry;

using fdep_examples::add_edge;
using fdep_examples::asi;
using fdep_examples::asset;
using fdep_examples::check;
using fdep_examples::cooling_loop;
using fdep_examples::declare_ref;
using fdep_examples::default_provenance;
using fdep_examples::dfi;
using fdep_examples::feed;
using fdep_examples::make_spec;
using fdep_examples::rack;
using fdep_examples::service;
using fdep_examples::unwrap;
using fdep_examples::yes_no;

namespace {

void print_diff(const char* label, const fdep::GenerationDiff& diff) {
  std::cout << label << '\n';
  std::cout << "  from=" << fdep::to_text(diff.from) << " to=" << fdep::to_text(diff.to)
            << " added=" << diff.added() << " removed=" << diff.removed() << " modified=" << diff.modified()
            << " unchanged-edges=" << diff.unchanged_edges << " truncated=" << yes_no(diff.truncated)
            << " empty=" << yes_no(diff.empty()) << '\n';
  for (const auto& change : diff.edge_changes) {
    std::cout << "  " << change.to_text() << '\n';
  }
  for (const auto& change : diff.ref_changes) {
    std::cout << "  ref " << change.to_text() << '\n';
  }
}

void print_diff(const char* label, const fdep::RegistrySnapshot& before, const fdep::RegistrySnapshot& after,
                std::uint32_t max_changes) {
  print_diff(label, unwrap(fdep::diff_snapshots(before, after, max_changes), "diff_snapshots"));
}

}  // namespace

int main() {
  auto registry = unwrap(fdep::DependencyRegistry::open_ephemeral(), "open an ephemeral registry");

  const fdep::DependencyNodeRef app = asset("app-01");
  const fdep::DependencyNodeRef feed_a = feed("feed-a");
  const fdep::DependencyNodeRef loop_west = cooling_loop("loop-west");
  const fdep::DependencyNodeRef asi_pod = asi("asi-pod-1");
  const fdep::DependencyNodeRef dfi_fabric = dfi("dfi-fabric-1");
  const fdep::DependencyNodeRef rack_01 = rack("row-a-rack-01");
  const fdep::DependencyNodeRef dcim = service("svc-dcim");
  for (const auto& node : {app, feed_a, loop_west, asi_pod, dfi_fabric, rack_01, dcim}) {
    declare_ref(registry, node, "observed before the diffed mutations");
  }

  const auto redundancy_count =
      unwrap(fdep::DependencyConstraint::make(fdep::ConstraintKind::RedundancyCount, 2), "redundancy count");

  // One snapshot per committed generation. Snapshots never change afterwards,
  // so they can be kept and compared in any order later.
  std::vector<fdep::RegistrySnapshot> snapshots;
  snapshots.push_back(registry.snapshot());  // 0: the empty graph

  const auto power = add_edge(registry, make_spec(app, feed_a, fdep::DependencyKind::RequiresPowerFrom,
                                                  fdep::DependencyStrength::Hard, fdep::Direction::DependsOn,
                                                  fdep::LifecycleState::Active, {redundancy_count}));
  const auto cooling = add_edge(registry, make_spec(app, loop_west, fdep::DependencyKind::CooledBy,
                                                    fdep::DependencyStrength::Hard, fdep::Direction::DependsOn,
                                                    fdep::LifecycleState::Active));
  snapshots.push_back(registry.snapshot());  // 1: two edges, neither touched again

  const auto composition = add_edge(registry, make_spec(asi_pod, dfi_fabric,
                                                        fdep::DependencyKind::ComposedDomainDependsOn,
                                                        fdep::DependencyStrength::Hard,
                                                        fdep::Direction::DependsOn, fdep::LifecycleState::Active));
  snapshots.push_back(registry.snapshot());  // 2

  const auto serving = add_edge(registry, make_spec(rack_01, dcim, fdep::DependencyKind::ServedBy,
                                                    fdep::DependencyStrength::Hard, fdep::Direction::DependsOn,
                                                    fdep::LifecycleState::Active));
  snapshots.push_back(registry.snapshot());  // 3

  // update: the mutable payload of an edge, under the revision it expects.
  fdep::UpdateEdgeRequest update;
  update.context.expected_generation = registry.generation();
  update.id = power.id;
  update.expected_revision = power.revision;
  update.strength = fdep::DependencyStrength::Soft;
  update.direction = fdep::Direction::DependsOn;
  update.constraints = {redundancy_count};
  update.provenance = default_provenance();
  const auto updated = unwrap(registry.update_edge(update), "update_edge");
  check(updated.changed && updated.generation_advanced, "changing the strength publishes a generation");
  snapshots.push_back(registry.snapshot());  // 4

  // transition: lifecycle is a separate, audited command.
  fdep::LifecycleTransitionRequest transition;
  transition.context.expected_generation = registry.generation();
  transition.id = composition.id;
  transition.expected_revision = composition.revision;
  transition.target = fdep::LifecycleState::Suspended;
  transition.reason = "planned control-plane work";
  const auto transitioned = unwrap(registry.transition_edge_lifecycle(transition), "transition_edge_lifecycle");
  check(!transitioned.already_in_state, "the edge was active and is now suspended");
  snapshots.push_back(registry.snapshot());  // 5

  // remove: the key is released, the identity is never reused.
  fdep::RemoveEdgeRequest remove;
  remove.context.expected_generation = registry.generation();
  remove.id = serving.id;
  remove.expected_revision = serving.revision;
  remove.reason = "service withdrawn";
  const auto removed = unwrap(registry.remove_edge(remove), "remove_edge");
  check(!removed.already_absent, "the edge was present and is now gone");
  snapshots.push_back(registry.snapshot());  // 6

  std::cout << "mutations: register, register, register, register, update, transition, remove\n";
  std::cout << "snapshots, one per committed generation (the declarations above advanced it too):";
  for (const auto& snapshot : snapshots) {
    std::cout << ' ' << fdep::to_text(snapshot.generation());
  }
  std::cout << "\n";

  std::cout << "\n-- one step at a time --\n";
  print_diff("snapshot 0 -> 1: two edges registered", snapshots[0], snapshots[1], 8);
  print_diff("snapshot 1 -> 2: one edge registered", snapshots[1], snapshots[2], 8);
  print_diff("snapshot 2 -> 3: one edge registered", snapshots[2], snapshots[3], 8);
  print_diff("snapshot 3 -> 4: one edge updated to soft", snapshots[3], snapshots[4], 8);
  print_diff("snapshot 4 -> 5: one edge suspended", snapshots[4], snapshots[5], 8);
  print_diff("snapshot 5 -> 6: one edge removed", snapshots[5], snapshots[6], 8);

  std::cout << "\n-- across several generations --\n";
  print_diff("snapshot 1 -> 6", snapshots[1], snapshots[6], 8);

  std::cout << "\n-- the change bound is explicit too --\n";
  print_diff("snapshot 1 -> 6, max-changes=1", snapshots[1], snapshots[6], 1);

  std::cout << "\n-- the diff is a property of the two states --\n";
  const fdep::GenerationDiff forward = unwrap(fdep::diff_snapshots(snapshots[1], snapshots[6], 8), "diff forward");
  const fdep::GenerationDiff backward = unwrap(fdep::diff_snapshots(snapshots[6], snapshots[1], 8), "diff backward");
  check(forward.added() == backward.removed() && forward.removed() == backward.added(),
        "reversing the pair swaps additions and removals");
  check(forward.modified() == backward.modified(), "the same edges are reported as modified either way");
  std::cout << "  forward added=" << forward.added() << " removed=" << forward.removed()
            << " modified=" << forward.modified() << '\n';
  std::cout << "  backward added=" << backward.added() << " removed=" << backward.removed()
            << " modified=" << backward.modified() << '\n';
  return 0;
}
