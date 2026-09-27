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

/// Example 01: open a registry, declare external references, register edges of
/// several kinds, and read the graph back.
///
/// The registry never owns a facility object. Every endpoint below is an opaque
/// identity that some other registry owns; declaring it records only that the
/// identity was observed to exist.

#include <cstddef>
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
using fdep_examples::dfi;
using fdep_examples::feed;
using fdep_examples::make_spec;
using fdep_examples::rack;
using fdep_examples::service;
using fdep_examples::unwrap;

namespace {

/// Renders one direct lookup, which is a bounded list of stored edges.
void print_edges(const char* title, const std::vector<fdep::DependencyEdgeRecord>& edges) {
  std::cout << title << " (" << edges.size() << ")\n";
  for (const auto& record : edges) {
    std::cout << "  " << record.to_text() << '\n';
  }
}

}  // namespace

int main() {
  auto registry = unwrap(fdep::DependencyRegistry::open_ephemeral(), "open an ephemeral registry");
  check(!registry.durable(), "an ephemeral registry reports itself as non-durable");
  check(registry.generation() == fdep::kInitialGeneration, "a fresh registry starts at generation zero");

  // -- declare the external identities this example refers to --------------
  const fdep::DependencyNodeRef node3 = asset("row-a-rack-07-node-3");
  const fdep::DependencyNodeRef node2 = asset("row-a-rack-07-node-2");
  const fdep::DependencyNodeRef rack07 = rack("row-a-rack-07");
  const fdep::DependencyNodeRef feed_a1 = feed("feed-a1");
  const fdep::DependencyNodeRef feed_a2 = feed("feed-a2");
  const fdep::DependencyNodeRef loop_west = cooling_loop("loop-west");
  const fdep::DependencyNodeRef uplink = service("svc-network-uplink");
  const fdep::DependencyNodeRef backup = service("svc-backup-uplink");
  const fdep::DependencyNodeRef asi_pod = asi("asi-pod-1");
  const fdep::DependencyNodeRef dfi_fabric = dfi("dfi-fabric-1");

  declare_ref(registry, node3, "observed in the asset inventory");
  declare_ref(registry, node2, "observed in the asset inventory");
  declare_ref(registry, rack07, "observed in the rack registry");
  declare_ref(registry, feed_a1, "observed in the electrical domain export");
  declare_ref(registry, feed_a2, "observed in the electrical domain export");
  declare_ref(registry, loop_west, "observed in the cooling domain export");
  declare_ref(registry, uplink, "observed in the service catalogue");
  declare_ref(registry, backup, "observed in the service catalogue");
  declare_ref(registry, asi_pod, "observed in the composed ASI domain export");
  declare_ref(registry, dfi_fabric, "observed in the composed DFI domain export");

  // -- register edges of several kinds -------------------------------------
  // Each call is addressed to the generation the caller believes is current,
  // so an accidental retry can never be applied twice.
  const auto redundancy_class =
      unwrap(fdep::DependencyConstraint::make(fdep::ConstraintKind::RedundancyClass, "2N"), "redundancy class");
  const auto redundancy_count =
      unwrap(fdep::DependencyConstraint::make(fdep::ConstraintKind::RedundancyCount, 2), "redundancy count");
  const auto failover =
      unwrap(fdep::DependencyConstraint::make(fdep::ConstraintKind::FailoverMode, "automatic"), "failover mode");
  const auto latency =
      unwrap(fdep::DependencyConstraint::make(fdep::ConstraintKind::MaxLatencyMicros, 1500), "latency");
  const auto bandwidth =
      unwrap(fdep::DependencyConstraint::make(fdep::ConstraintKind::MinBandwidthMbps, 10000), "bandwidth");

  const auto power = add_edge(registry, make_spec(node3, feed_a1, fdep::DependencyKind::RequiresPowerFrom,
                                                  fdep::DependencyStrength::Hard, fdep::Direction::DependsOn,
                                                  fdep::LifecycleState::Active,
                                                  {redundancy_class, redundancy_count, failover}));
  const auto cooling = add_edge(registry, make_spec(node3, loop_west, fdep::DependencyKind::CooledBy,
                                                    fdep::DependencyStrength::Hard, fdep::Direction::DependsOn,
                                                    fdep::LifecycleState::Active, {latency}));
  const auto housing = add_edge(registry, make_spec(node3, rack07, fdep::DependencyKind::HousedIn,
                                                    fdep::DependencyStrength::Hard, fdep::Direction::DependsOn,
                                                    fdep::LifecycleState::Active));
  const auto serving = add_edge(registry, make_spec(node3, uplink, fdep::DependencyKind::ServedBy,
                                                    fdep::DependencyStrength::Hard, fdep::Direction::DependsOn,
                                                    fdep::LifecycleState::Active, {bandwidth}));
  const auto composition = add_edge(registry, make_spec(asi_pod, dfi_fabric,
                                                        fdep::DependencyKind::ComposedDomainDependsOn,
                                                        fdep::DependencyStrength::Hard,
                                                        fdep::Direction::DependsOn, fdep::LifecycleState::Active));

  // A proposed declaration is stored, validated and visible, but no default
  // traversal follows it: it carries no authority until it is activated.
  const auto proposed = add_edge(registry, make_spec(node3, backup, fdep::DependencyKind::ServedBy,
                                                     fdep::DependencyStrength::Advisory, fdep::Direction::DependsOn,
                                                     fdep::LifecycleState::Proposed));
  const auto second_power = add_edge(registry, make_spec(node2, feed_a2, fdep::DependencyKind::RequiresPowerFrom,
                                                         fdep::DependencyStrength::Hard, fdep::Direction::DependsOn,
                                                         fdep::LifecycleState::Active));

  std::cout << "generation is " << fdep::to_text(registry.generation())
            << "; every declaration and every registration advanced it by one\n";
  check(power.revision.value() == std::uint64_t{1} && cooling.revision.value() == std::uint64_t{1} &&
            housing.revision.value() == std::uint64_t{1},
        "the first revision of an edge is 1");
  check(serving.id.value() == std::uint64_t{4} && composition.id.value() == std::uint64_t{5} &&
            proposed.id.value() == std::uint64_t{6} && second_power.id.value() == std::uint64_t{7},
        "edge identities are allocated in registration order");

  // -- the canonical text export -------------------------------------------
  const fdep::RegistrySnapshot snapshot = registry.snapshot();
  std::cout << "stored: " << snapshot.declared_ref_count() << " declarations and " << snapshot.edge_count()
            << " edges over " << snapshot.node_count() << " nodes\n";
  std::cout << "\n-- canonical text export --\n";
  std::cout << unwrap(fdep::export_text(snapshot), "export_text");

  // -- direct lookups -------------------------------------------------------
  std::cout << "\n-- direct dependencies of " << node3.to_canonical() << ", edges in force --\n";
  print_edges("matching edges",
              unwrap(snapshot.direct_dependencies(node3), "direct_dependencies"));

  // The default filter reads the authoritative graph: only active edges. The
  // proposed declaration is still stored, and `EdgeFilter::any()` shows it.
  std::cout << "\n-- direct dependencies of " << node3.to_canonical() << ", every stored edge --\n";
  print_edges("matching edges",
              unwrap(snapshot.direct_dependencies(node3, fdep::EdgeFilter::any()), "direct_dependencies(any)"));

  std::cout << "\n-- direct dependents of " << feed_a1.to_canonical() << ", edges in force --\n";
  print_edges("matching edges", unwrap(snapshot.direct_dependents(feed_a1), "direct_dependents"));

  // -- authority ------------------------------------------------------------
  std::cout << "\n-- authority --\n";
  std::cout << "generation     = " << fdep::to_text(snapshot.generation()) << '\n';
  std::cout << "state-digest   = " << snapshot.state_digest().to_hex() << '\n';
  std::cout << "content-digest = " << snapshot.content_digest().to_hex() << '\n';
  std::cout << "edge-count     = " << snapshot.edge_count() << ", declaration-count = "
            << snapshot.declared_ref_count() << ", node-count = " << snapshot.node_count() << '\n';
  return 0;
}
