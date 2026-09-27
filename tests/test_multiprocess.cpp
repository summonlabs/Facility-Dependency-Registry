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

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "facility_dependency_registry/facility_dependency_registry.hpp"
#include "test_harness.hpp"
#include "test_process.hpp"
#include "test_support.hpp"

using namespace facility_dependency_registry;
using fdep_test::asset;
using fdep_test::feed;
using fdep_test::Harness;

namespace {

/// Child modes used by the multi process tests. Each one is a real second
/// operating system process running this same executable.
constexpr char kWriterHold[] = "writer-hold";
constexpr char kWriterCrash[] = "writer-crash";
constexpr char kReaderProbe[] = "reader-probe";

void register_modes() {
  // Opens the store read-write, commits two edges named after a tag, announces
  // itself, waits for the go file, then closes cleanly and exits 0.
  fdep_test::register_child_mode(kWriterHold, [](const std::vector<std::string>& args) {
    if (args.size() != 4) {
      return 64;
    }
    const std::string tag = args[3];
    RegistryOpenRequest request;
    request.root = std::filesystem::path{args[0]};
    auto registry = DependencyRegistry::open(request);
    if (!registry) {
      return 10;
    }
    for (int index = 0; index < 2; ++index) {
      const std::string prefix = tag.empty() ? std::string{"child"} : "child-" + tag;
      RegisterEdgeRequest register_request;
      register_request.context.expected_generation = registry.value().generation();
      register_request.spec = fdep_test::make_spec(asset(prefix + "-" + std::to_string(index)), feed("f1"),
                                                   DependencyKind::RequiresPowerFrom);
      if (!registry.value().register_edge(register_request).has_value()) {
        return 11;
      }
    }
    {
      std::ofstream ready{std::filesystem::path{args[1]}, std::ios::binary};
      ready << "ready\n";
    }
    const std::filesystem::path go{args[2]};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{60};
    while (!std::filesystem::exists(go)) {
      if (std::chrono::steady_clock::now() > deadline) {
        return 12;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    if (!registry.value().close().ok()) {
      return 13;
    }
    return 0;
  });

  // Opens the store read-write, commits two edges, and dies without running any
  // cleanup while still holding the writer lock.
  fdep_test::register_child_mode(kWriterCrash, [](const std::vector<std::string>& args) {
    if (args.size() != 1) {
      return 64;
    }
    RegistryOpenRequest request;
    request.root = std::filesystem::path{args[0]};
    auto registry = DependencyRegistry::open(request);
    if (!registry) {
      return 10;
    }
    for (int index = 0; index < 2; ++index) {
      RegisterEdgeRequest register_request;
      register_request.context.expected_generation = registry.value().generation();
      register_request.spec =
          fdep_test::make_spec(asset("crashed-" + std::to_string(index)), feed("f1"),
                               DependencyKind::RequiresPowerFrom);
      if (!registry.value().register_edge(register_request).has_value()) {
        return 11;
      }
    }
    std::cout << "generation " << registry.value().generation().value() << "\n";
    std::cout.flush();
    std::_Exit(98);
  });

  // Opens the store read-only and reports what it sees.
  fdep_test::register_child_mode(kReaderProbe, [](const std::vector<std::string>& args) {
    if (args.size() != 1) {
      return 64;
    }
    RegistryOpenRequest request;
    request.root = std::filesystem::path{args[0]};
    request.store.mode = OpenMode::ReadOnly;
    auto registry = DependencyRegistry::open(request);
    if (!registry) {
      std::cout << "open-failed " << to_token(registry.error().code()) << "\n";
      return 0;
    }
    std::cout << "generation " << registry.value().generation().value() << " edges "
              << registry.value().snapshot().edge_count() << " recovery "
              << to_token(registry.value().recovery_report().outcome) << "\n";
    return 0;
  });
}

struct Registration {
  Registration() { register_modes(); }
};

const Registration kRegistration{};

std::filesystem::path temp_root(std::string_view label) { return fdep_test::make_temp_directory(label); }

}  // namespace

FDEP_TEST(multiprocess, a_child_writer_locks_out_this_process) {
  const std::filesystem::path root = temp_root("mp-lock");
  const std::filesystem::path ready = root / "ready.marker";
  const std::filesystem::path go = root / "go.marker";

  fdep_test::ChildProcess child =
      fdep_test::ChildProcess::start(kWriterHold, {root.string(), ready.string(), go.string(), ""},
                                     root / "child.out");
  FDEP_REQUIRE(child.valid());
  FDEP_REQUIRE(fdep_test::wait_for_file(ready, child));

  // The child holds the writer lock. A read-write open here must fail, and a
  // read-only open must succeed and see the committed state.
  RegistryOpenRequest blocked;
  blocked.root = root;
  const auto refused = DependencyRegistry::open(blocked);
  FDEP_CHECK_CODE(refused, ErrorCode::StoreLocked);
  FDEP_CHECK(refused.error().detail().find(std::to_string(child.process_id())) != std::string::npos);

  std::string probe_output;
  const int probe_exit = fdep_test::run_child(kReaderProbe, {root.string()}, probe_output);
  FDEP_CHECK_EQ(probe_exit, 0);
  FDEP_CHECK(probe_output.find("generation 2") != std::string::npos);
  FDEP_CHECK(probe_output.find("edges 2") != std::string::npos);
  FDEP_CHECK(probe_output.find("recovery loaded-current") != std::string::npos);

  // Let the child finish and release the lock.
  {
    std::ofstream stream{go, std::ios::binary};
    stream << "go\n";
  }
  int exit_code = -1;
  std::string output;
  FDEP_REQUIRE(child.wait(exit_code, output));
  FDEP_CHECK_EQ(exit_code, 0);

  // Now this process can take the store, and it loads what the child wrote.
  Harness harness = Harness::durable(root);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{2});
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{2});
  FDEP_CHECK(harness.snapshot().find_edge(EdgeKey{asset("child-0"), feed("f1"),
                                                  DependencyKind::RequiresPowerFrom}) != nullptr);
  FDEP_REQUIRE(harness.registry().close().ok());
  fdep_test::remove_tree(root);
}

FDEP_TEST(multiprocess, a_crashed_writer_leaves_no_stale_lock) {
  const std::filesystem::path root = temp_root("mp-crash");
  std::string output;
  const int exit_code = fdep_test::run_child(kWriterCrash, {root.string()}, output);
  FDEP_CHECK_EQ(exit_code, 98);
  FDEP_CHECK(output.find("generation 2") != std::string::npos);

  // The process died without closing anything, so the operating system
  // released the lock. A new writer takes over and loads the committed
  // generation. No incarnation record is trusted as authority.
  Harness harness = Harness::durable(root);
  FDEP_CHECK(harness.registry().recovery_report().outcome == RecoveryOutcome::LoadedCurrent);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{2});
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{2});
  // The lock file now records this process, not the dead one: the record is a
  // diagnostic that a new writer overwrites, never a token that fences.
  const auto status = harness.registry().store_status();
  FDEP_REQUIRE_OK(status);
  FDEP_CHECK(status.value().writer_lock_held);
  FDEP_CHECK_EQ(status.value().lock_incarnation.process_id(),
                harness.registry().writer_incarnation().process_id());
  FDEP_REQUIRE(harness.registry().close().ok());
  fdep_test::remove_tree(root);
}

FDEP_TEST(multiprocess, state_written_by_one_process_is_read_by_another) {
  const std::filesystem::path root = temp_root("mp-handoff");
  ContentDigest digest;
  {
    Harness harness = Harness::durable(root);
    harness.declare(asset("declared"), fdep_test::provenance("change-1", "alice", 5, "observed"));
    harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
    harness.add(asset("b"), feed("f2"), DependencyKind::RequiresPowerFrom);
    digest = harness.snapshot().state_digest();
    FDEP_REQUIRE(harness.registry().close().ok());
  }

  std::string output;
  const int exit_code = fdep_test::run_child(kReaderProbe, {root.string()}, output);
  FDEP_CHECK_EQ(exit_code, 0);
  FDEP_CHECK(output.find("generation 3") != std::string::npos);
  FDEP_CHECK(output.find("edges 2") != std::string::npos);

  // A second child sees exactly the same state, so the digest is stable across
  // processes and not an artefact of one process's memory.
  std::string second_output;
  FDEP_CHECK_EQ(fdep_test::run_child(kReaderProbe, {root.string()}, second_output), 0);
  FDEP_CHECK(second_output == output);

  Harness harness = Harness::durable(root);
  FDEP_CHECK_EQ(harness.snapshot().state_digest(), digest);
  FDEP_REQUIRE(harness.registry().close().ok());
  fdep_test::remove_tree(root);
}

FDEP_TEST(multiprocess, repeated_child_restarts_keep_the_generation_monotonic) {
  const std::filesystem::path root = temp_root("mp-restart");
  const auto run_writer = [&root](const std::string& tag, int attempt) {
    const std::filesystem::path ready = root / ("ready-" + std::to_string(attempt) + ".marker");
    const std::filesystem::path go = root / ("go-" + std::to_string(attempt) + ".marker");
    fdep_test::ChildProcess child = fdep_test::ChildProcess::start(
        kWriterHold, {root.string(), ready.string(), go.string(), tag}, root / ("child-" + std::to_string(attempt) + ".out"));
    if (!child.valid()) {
      fdep_test::fail_now("could not start the child writer");
    }
    if (!fdep_test::wait_for_file(ready, child)) {
      fdep_test::fail_now("the child writer never announced itself");
    }
    {
      std::ofstream stream{go, std::ios::binary};
      stream << "go\n";
    }
    int exit_code = -1;
    std::string output;
    if (!child.wait(exit_code, output)) {
      fdep_test::fail_now("the child writer could not be waited for");
    }
    FDEP_CHECK_EQ(exit_code, 0);
  };

  for (int attempt = 0; attempt < 3; ++attempt) {
    run_writer(std::to_string(attempt), attempt);
  }

  Harness harness = Harness::durable(root);
  FDEP_REQUIRE(harness.snapshot().edge_count() == 6);
  const DependencyGeneration after_three_writers = harness.generation();
  FDEP_CHECK_EQ(after_three_writers.value(), std::uint64_t{6});
  FDEP_REQUIRE(harness.registry().close().ok());

  // A fourth restart that declares exactly what the first one declared is an
  // idempotent no-op across the process boundary: nothing is published, and
  // the generation does not move.
  run_writer("0", 3);

  Harness reopened = Harness::durable(root);
  FDEP_CHECK_EQ(reopened.generation().value(), after_three_writers.value());
  FDEP_CHECK_EQ(reopened.snapshot().edge_count(), std::size_t{6});
  FDEP_CHECK(reopened.snapshot().find_edge(EdgeKey{asset("child-0-0"), feed("f1"),
                                                   DependencyKind::RequiresPowerFrom}) != nullptr);
  FDEP_REQUIRE(reopened.registry().close().ok());
  fdep_test::remove_tree(root);
}

FDEP_TEST(multiprocess, a_child_never_sees_a_partially_published_generation) {
  const std::filesystem::path root = temp_root("mp-atomic");
  {
    Harness harness = Harness::durable(root);
    for (int index = 0; index < 8; ++index) {
      harness.add(asset("node-" + std::to_string(index)), feed("f1"), DependencyKind::RequiresPowerFrom);
    }
    FDEP_REQUIRE(harness.registry().close().ok());
  }

  // Ten concurrent readers, each a real process, while this process publishes
  // more generations. Every reader must see an intact generation.
  Harness writer = Harness::durable(root);
  std::vector<std::string> outputs;
  for (int attempt = 0; attempt < 4; ++attempt) {
    RegisterEdgeRequest request;
    request.context.expected_generation = writer.generation();
    request.spec = fdep_test::make_spec(asset("extra-" + std::to_string(attempt)), feed("f1"),
                                        DependencyKind::RequiresPowerFrom);
    FDEP_REQUIRE(writer.registry().register_edge(request).has_value());

    std::string output;
    const int exit_code = fdep_test::run_child(kReaderProbe, {root.string()}, output);
    FDEP_CHECK_EQ(exit_code, 0);
    FDEP_CHECK(output.find("recovery loaded-current") != std::string::npos);
    outputs.push_back(output);
  }
  FDEP_CHECK_EQ(outputs.size(), std::size_t{4});
  for (std::size_t index = 0; index < outputs.size(); ++index) {
    FDEP_CHECK(outputs[index].find("generation " + std::to_string(index + 9)) != std::string::npos);
  }
  FDEP_REQUIRE(writer.registry().close().ok());
  fdep_test::remove_tree(root);
}
