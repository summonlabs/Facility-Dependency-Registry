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

constexpr char kAbortAtStep[] = "publish-abort";
constexpr char kUpdateAbortAtStep[] = "publish-update-abort";
constexpr char kKillAtStep[] = "publish-kill";
constexpr int kAborted = 97;
constexpr int kRendezvousExpired = 96;

/// Every published step, in the order the publication protocol performs them.
constexpr PublishStep kAllSteps[] = {
    PublishStep::Begun,          PublishStep::TransientWritten, PublishStep::TransientSynced,
    PublishStep::TransientVerified, PublishStep::GenerationRenamed, PublishStep::DirectorySynced,
    PublishStep::PointerReplaced, PublishStep::PointerSynced,   PublishStep::Pruned,
    PublishStep::Completed};

/// How many generations a reopen must see beyond the one that was current
/// before the crash. The pointer is only authoritative once it has been
/// replaced, which happens at PointerReplaced.
int generation_delta_after(PublishStep step) {
  return static_cast<int>(step) >= static_cast<int>(PublishStep::PointerReplaced) ? 1 : 0;
}

void register_modes() {
  // Publishes two generations and dies without cleanup at a named step of the
  // first one. The tag keeps the declarations distinct across restarts, so the
  // publication really does happen on every attempt.
  fdep_test::register_child_mode(kAbortAtStep, [](const std::vector<std::string>& args) {
    if (args.size() != 3) {
      return 64;
    }
    const int step_number = std::stoi(args[1]);
    const std::string tag = args[2];
    PublishFaultHooks hooks;
    hooks.after_step = [step_number](PublishStep step, DependencyGeneration) {
      if (static_cast<int>(step) == step_number) {
        std::cout << "died after " << to_token(step) << "\n";
        std::cout.flush();
        std::_Exit(kAborted);
      }
    };
    RegistryOpenRequest request;
    request.root = std::filesystem::path{args[0]};
    request.store.faults = hooks;
    auto registry = DependencyRegistry::open(request);
    if (!registry) {
      std::cout << "open failed " << to_token(registry.error().code()) << "\n";
      return 10;
    }
    for (int index = 0; index < 2; ++index) {
      RegisterEdgeRequest register_request;
      register_request.context.expected_generation = registry.value().generation();
      register_request.spec =
          fdep_test::make_spec(asset("crash-" + tag + "-" + std::to_string(index)), feed("f1"),
                               DependencyKind::RequiresPowerFrom);
      if (!registry.value().register_edge(register_request).has_value()) {
        return 11;
      }
    }
    return 0;
  });

  // Publishes one update and dies without cleanup at a named step.
  fdep_test::register_child_mode(kUpdateAbortAtStep, [](const std::vector<std::string>& args) {
    if (args.size() != 3) {
      return 64;
    }
    const int step_number = std::stoi(args[1]);
    DependencyEdgeId id{};
    if (!parse_edge_id(args[2], id)) {
      return 65;
    }
    PublishFaultHooks hooks;
    hooks.after_step = [step_number](PublishStep step, DependencyGeneration) {
      if (static_cast<int>(step) == step_number) {
        std::cout << "died after " << to_token(step) << "\n";
        std::cout.flush();
        std::_Exit(kAborted);
      }
    };
    RegistryOpenRequest request;
    request.root = std::filesystem::path{args[0]};
    request.store.faults = hooks;
    auto registry = DependencyRegistry::open(request);
    if (!registry) {
      return 10;
    }
    const auto* record = registry.value().snapshot().find_edge(id);
    if (record == nullptr) {
      return 12;
    }
    UpdateEdgeRequest update;
    update.context.expected_generation = registry.value().generation();
    update.id = id;
    update.expected_revision = record->revision();
    update.strength = DependencyStrength::Soft;
    update.provenance = fdep_test::provenance("change-9", "operator", 9, "revised");
    if (!registry.value().update_edge(update).has_value()) {
      return 13;
    }
    return 0;
  });

  // Announces that a transient generation has been written, then waits to be
  // killed from outside.
  fdep_test::register_child_mode(kKillAtStep, [](const std::vector<std::string>& args) {
    if (args.size() != 2) {
      return 64;
    }
    const std::filesystem::path marker{args[1]};
    PublishFaultHooks hooks;
    hooks.after_step = [marker](PublishStep step, DependencyGeneration) {
      if (step != PublishStep::TransientWritten) {
        return;
      }
      {
        std::ofstream stream{marker, std::ios::binary};
        stream << "transient written\n";
      }
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{60};
      while (true) {
        if (std::chrono::steady_clock::now() > deadline) {
          std::_Exit(kRendezvousExpired);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
      }
    };
    RegistryOpenRequest request;
    request.root = std::filesystem::path{args[0]};
    request.store.faults = hooks;
    auto registry = DependencyRegistry::open(request);
    if (!registry) {
      return 10;
    }
    RegisterEdgeRequest register_request;
    register_request.context.expected_generation = registry.value().generation();
    register_request.spec = fdep_test::make_spec(asset("killed"), feed("f1"), DependencyKind::RequiresPowerFrom);
    if (!registry.value().register_edge(register_request).has_value()) {
      return 11;
    }
    return 0;
  });
}

struct Registration {
  Registration() { register_modes(); }
};

const Registration kRegistration{};

/// Creates a store holding four edges at generation four and returns its root.
std::filesystem::path build_store(std::string_view label) {
  const std::filesystem::path root = fdep_test::make_temp_directory(label);
  Harness harness = Harness::durable(root);
  for (int index = 0; index < 4; ++index) {
    harness.add(asset("base-" + std::to_string(index)), feed("f1"), DependencyKind::RequiresPowerFrom);
  }
  if (!harness.registry().close().ok()) {
    fdep_test::fail_now("could not close the store the crash test built");
  }
  return root;
}

std::size_t count_transient(const std::filesystem::path& root) {
  std::size_t total = 0;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator{root, error}) {
    if (error) {
      break;
    }
    const std::string name = entry.path().filename().string();
    if (name.rfind("tmp-", 0) == 0) {
      ++total;
    }
  }
  return total;
}

/// Reopens a store after a crash and checks that what came back is one of the
/// two states the protocol allows, and that it is usable.
void verify_recovered(const std::filesystem::path& root, std::uint64_t expected_generation,
                      std::size_t expected_edges) {
  Harness harness = Harness::durable(root);
  const RecoveryReport& report = harness.registry().recovery_report();
  FDEP_CHECK(report.outcome == RecoveryOutcome::LoadedCurrent ||
             report.outcome == RecoveryOutcome::LoadedFallback);
  FDEP_CHECK(report.outcome != RecoveryOutcome::RefusedCorrupt);
  FDEP_CHECK_EQ(harness.generation().value(), expected_generation);
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), expected_edges);

  const auto obligation = harness.snapshot().verify_acyclic_obligation();
  FDEP_REQUIRE_OK(obligation);
  FDEP_CHECK(obligation.value().satisfied);

  // The recovered state is authoritative, not read-only: the next mutation
  // continues from it. The node name is derived from the generation so that
  // repeated recoveries in one test stay distinct declarations.
  harness.add(asset("after-recovery-" + std::to_string(expected_generation)), feed("f1"),
              DependencyKind::RequiresPowerFrom);
  FDEP_CHECK_EQ(harness.generation().value(), expected_generation + 1);
  FDEP_REQUIRE(harness.registry().close().ok());
  FDEP_CHECK_EQ(count_transient(root), std::size_t{0});
}

}  // namespace

FDEP_TEST(crash, every_publication_step_survives_a_process_death) {
  for (const PublishStep step : kAllSteps) {
    const std::string tag = std::to_string(static_cast<int>(step));
    const std::filesystem::path root = build_store("crash-step-" + tag);
    std::string output;
    const int exit_code = fdep_test::run_child(kAbortAtStep, {root.string(), tag, tag}, output);
    FDEP_CHECK_EQ(exit_code, kAborted);
    FDEP_CHECK(output.find("died after") != std::string::npos);

    const auto delta = static_cast<std::uint64_t>(generation_delta_after(step));
    verify_recovered(root, 4 + delta, static_cast<std::size_t>(4 + delta));
    fdep_test::remove_tree(root);
  }
}

FDEP_TEST(crash, a_killed_writer_leaves_the_previous_generation_authoritative) {
  const std::filesystem::path root = build_store("crash-kill");
  const std::filesystem::path marker = root / "killed.marker";

  fdep_test::ChildProcess child =
      fdep_test::ChildProcess::start(kKillAtStep, {root.string(), marker.string()}, root / "killed.out");
  FDEP_REQUIRE(child.valid());
  FDEP_REQUIRE(fdep_test::wait_for_file(marker, child));

  // A transient file exists at this moment, and CURRENT still names the
  // previous generation. Killing right here is the worst case for the
  // protocol: a complete generation file has been written but not published.
  FDEP_CHECK(count_transient(root) >= 1);
  child.terminate();

  verify_recovered(root, 4, 4);
  fdep_test::remove_tree(root);
}
FDEP_TEST(crash, repeated_crashes_never_lose_the_last_good_generation) {
  const std::filesystem::path root = build_store("crash-repeat");
  std::uint64_t base_generation = 4;
  std::size_t base_edges = 4;
  int iteration = 0;
  for (const PublishStep step : kAllSteps) {
    const std::string tag = std::to_string(iteration);
    std::string output;
    const int exit_code = fdep_test::run_child(kAbortAtStep, {root.string(), std::to_string(static_cast<int>(step)), tag},
                                               output);
    FDEP_CHECK_EQ(exit_code, kAborted);
    const auto delta = static_cast<std::uint64_t>(generation_delta_after(step));
    verify_recovered(root, base_generation + delta, static_cast<std::size_t>(base_edges + delta));
    // verify_recovered committed one more mutation, so the next crash starts
    // from there.
    base_generation += delta + 1;
    base_edges += static_cast<std::size_t>(delta) + 1;
    ++iteration;
  }
  fdep_test::remove_tree(root);
}

FDEP_TEST(crash, a_crash_during_an_update_never_leaves_a_torn_change) {
  for (const PublishStep step : kAllSteps) {
    const std::filesystem::path root =
        build_store("crash-update-" + std::to_string(static_cast<int>(step)));
    DependencyEdgeId id{};
    DependencyGeneration before{};
    {
      Harness harness = Harness::durable(root);
      const std::span<const DependencyEdgeRecord> edges = harness.snapshot().edges();
      FDEP_REQUIRE(!edges.empty());
      id = edges.front().id();
      before = harness.generation();
      FDEP_CHECK(edges.front().strength() == DependencyStrength::Hard);
      FDEP_CHECK_EQ(edges.front().revision().value(), std::uint64_t{1});
      FDEP_REQUIRE(harness.registry().close().ok());
    }

    std::string output;
    const int exit_code = fdep_test::run_child(
        kUpdateAbortAtStep, {root.string(), std::to_string(static_cast<int>(step)), to_text(id)}, output);
    FDEP_CHECK_EQ(exit_code, kAborted);

    Harness harness = Harness::durable(root);
    const auto* record = harness.snapshot().find_edge(id);
    FDEP_REQUIRE(record != nullptr);
    const auto delta = static_cast<std::uint64_t>(generation_delta_after(step));
    FDEP_CHECK_EQ(harness.generation().value(), before.value() + delta);
    if (delta == 0) {
      // The publication did not cross the pointer replacement, so the change
      // must not be visible at all.
      FDEP_CHECK(record->strength() == DependencyStrength::Hard);
      FDEP_CHECK_EQ(record->revision().value(), std::uint64_t{1});
      FDEP_CHECK_EQ(record->provenance().principal(), std::string{"test-principal"});
    } else {
      // The publication is complete and durable, so the change is visible in
      // full: strength, revision and provenance moved together.
      FDEP_CHECK(record->strength() == DependencyStrength::Soft);
      FDEP_CHECK_EQ(record->revision().value(), std::uint64_t{2});
      FDEP_CHECK_EQ(record->provenance().principal(), std::string{"operator"});
      FDEP_CHECK_EQ(record->last_modified_generation().value(), before.value() + 1);
      FDEP_CHECK_EQ(record->registered_generation().value(), 1u);
    }
    FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{4});
    FDEP_REQUIRE(harness.registry().close().ok());
    fdep_test::remove_tree(root);
  }
}

FDEP_TEST(crash, a_crash_before_the_first_publication_leaves_an_empty_store) {
  const std::filesystem::path root = fdep_test::make_temp_directory("crash-first");
  std::string output;
  const int exit_code = fdep_test::run_child(
      kAbortAtStep, {root.string(), std::to_string(static_cast<int>(PublishStep::TransientWritten)), "first"},
      output);
  FDEP_CHECK_EQ(exit_code, kAborted);

  // Nothing was ever published, so the store is still a fresh empty store and
  // the half written transient file is removed rather than interpreted.
  FDEP_CHECK(count_transient(root) >= 1);
  Harness harness = Harness::durable(root);
  FDEP_CHECK(harness.registry().recovery_report().outcome == RecoveryOutcome::FreshEmpty);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{0});
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{0});
  FDEP_CHECK_EQ(harness.registry().recovery_report().transient_files_removed, std::uint32_t{1});
  FDEP_REQUIRE(harness.registry().close().ok());
  fdep_test::remove_tree(root);
}
