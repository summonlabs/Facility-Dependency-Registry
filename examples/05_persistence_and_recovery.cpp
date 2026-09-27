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

/// Example 05: durable state and recovery.
///
/// A durable registry publishes every committed generation to a store
/// directory, then reopens from it. Recovery is conservative: the newest intact
/// generation named by CURRENT is loaded, and a store that cannot be validated
/// is refused rather than quietly replaced with an empty graph.

#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

#include "support.hpp"

namespace fdep = facility_dependency_registry;

using fdep_examples::add_edge;
using fdep_examples::asset;
using fdep_examples::check;
using fdep_examples::cooling_loop;
using fdep_examples::declare_ref;
using fdep_examples::expect_ok;
using fdep_examples::fail;
using fdep_examples::feed;
using fdep_examples::make_spec;
using fdep_examples::rack;
using fdep_examples::service;
using fdep_examples::unwrap;

namespace {

/// A unique, empty directory under the system temporary directory. The name is
/// fixed and the suffix is the first free ordinal, so a run never reuses a
/// directory an earlier run left behind.
std::filesystem::path create_temporary_root() {
  std::error_code error;
  const std::filesystem::path base = std::filesystem::temp_directory_path(error);
  if (error) {
    fail("could not locate the system temporary directory");
  }
  for (int ordinal = 1; ordinal <= 64; ++ordinal) {
    const std::string name =
        ordinal == 1 ? std::string{"fdep-example-05-store"} : "fdep-example-05-store-" + std::to_string(ordinal);
    std::error_code create_error;
    if (std::filesystem::create_directory(base / name, create_error)) {
      return base / name;
    }
  }
  fail("could not create a unique temporary store directory");
}

}  // namespace

int main(int argc, char** argv) {
  bool remove_root = false;
  std::filesystem::path root;
  if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
    root = std::filesystem::path{argv[1]};
  } else {
    root = create_temporary_root();
    remove_root = true;
  }

  fdep::RegistryOpenRequest open_request;
  open_request.root = root;
  open_request.store.mode = fdep::OpenMode::ReadWrite;
  open_request.store.create_if_missing = true;

  fdep::DependencyGeneration written_generation;
  std::string written_digest;

  // -- write, then close ----------------------------------------------------
  {
    auto registry = unwrap(fdep::DependencyRegistry::open(open_request), "open the durable store");
    check(registry.durable(), "a store opened in read-write mode is durable");
    std::cout << "first open:  " << registry.recovery_report().to_text() << '\n';

    const fdep::DependencyNodeRef node = asset("row-a-rack-07-node-3");
    const fdep::DependencyNodeRef rack07 = rack("row-a-rack-07");
    const fdep::DependencyNodeRef feed_a1 = feed("feed-a1");
    const fdep::DependencyNodeRef loop_west = cooling_loop("loop-west");
    const fdep::DependencyNodeRef uplink = service("svc-network-uplink");

    declare_ref(registry, node, "observed in the asset inventory");
    declare_ref(registry, rack07, "observed in the rack registry");
    declare_ref(registry, feed_a1, "observed in the electrical domain export");
    declare_ref(registry, loop_west, "observed in the cooling domain export");
    declare_ref(registry, uplink, "observed in the service catalogue");

    add_edge(registry, make_spec(node, feed_a1, fdep::DependencyKind::RequiresPowerFrom,
                                 fdep::DependencyStrength::Hard, fdep::Direction::DependsOn,
                                 fdep::LifecycleState::Active));
    add_edge(registry, make_spec(node, loop_west, fdep::DependencyKind::CooledBy, fdep::DependencyStrength::Hard,
                                 fdep::Direction::DependsOn, fdep::LifecycleState::Active));
    add_edge(registry, make_spec(node, rack07, fdep::DependencyKind::HousedIn, fdep::DependencyStrength::Hard,
                                 fdep::Direction::DependsOn, fdep::LifecycleState::Active));
    add_edge(registry, make_spec(node, uplink, fdep::DependencyKind::ServedBy, fdep::DependencyStrength::Hard,
                                 fdep::Direction::DependsOn, fdep::LifecycleState::Active));

    const fdep::RegistrySnapshot snapshot = registry.snapshot();
    written_generation = snapshot.generation();
    written_digest = snapshot.state_digest().to_hex();
    std::cout << "committed:   generation " << fdep::to_text(written_generation) << ", edge-count "
              << snapshot.edge_count() << ", declaration-count " << snapshot.declared_ref_count() << '\n';
    std::cout << "state digest before close = " << written_digest << '\n';

    expect_ok(registry.close(), "close the durable store");
    check(registry.closed(), "the registry reports itself closed after close()");
    check(snapshot.edge_count() == 4, "a snapshot taken before close stays valid and unchanged");
  }

  // -- reopen and read the recovery report ----------------------------------
  {
    auto reopened = unwrap(fdep::DependencyRegistry::open(open_request), "reopen the durable store");
    const fdep::RegistrySnapshot snapshot = reopened.snapshot();
    const std::string loaded_digest = snapshot.state_digest().to_hex();

    std::cout << "\n-- reopen --\n";
    std::cout << "recovery report : " << reopened.recovery_report().to_text() << '\n';
    std::cout << "loaded generation = " << fdep::to_text(snapshot.generation()) << '\n';
    std::cout << "state digest      = " << loaded_digest << '\n';
    std::cout << "edge-count        = " << snapshot.edge_count() << ", declaration-count = "
              << snapshot.declared_ref_count() << ", node-count = " << snapshot.node_count() << '\n';

    check(snapshot.generation() == written_generation, "the loaded generation is the published one");
    check(loaded_digest == written_digest, "the loaded state has the digest that was written");
    check(reopened.recovery_report().outcome == fdep::RecoveryOutcome::LoadedCurrent,
          "the pointer named an intact generation and it was loaded");

    const fdep::StoreStatus store_status = unwrap(reopened.store_status(), "store_status");
    std::cout << "store: pointer-present=" << fdep_examples::yes_no(store_status.pointer_present)
              << " pointer-generation=" << fdep::to_text(store_status.pointer_generation)
              << " retained-generations=" << store_status.retained_generations.size()
              << " writer-lock-held=" << fdep_examples::yes_no(store_status.writer_lock_held)
              << " transient-files=" << store_status.transient_files << '\n';
    expect_ok(reopened.close(), "close the reopened store");
  }

  // -- leave the machine as we found it -------------------------------------
  if (remove_root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    if (error) {
      fail("could not remove the temporary store directory this example created");
    }
    std::cout << "\nremoved the temporary store directory this example created\n";
  } else {
    std::cout << "\nthe store root came from the command line, so it was left in place\n";
  }
  return 0;
}
