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

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "facility_dependency_registry/facility_dependency_registry.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

using namespace facility_dependency_registry;
using fdep_test::asset;
using fdep_test::feed;
using fdep_test::Harness;

namespace {

std::string generation_name(std::uint64_t generation) {
  std::string digits = std::to_string(generation);
  std::string name{"gen-"};
  name.append(20 - digits.size(), '0');
  name.append(digits);
  name.append(".fdepstate");
  return name;
}

std::filesystem::path gen_path(const std::filesystem::path& root, std::uint64_t generation) {
  return root / std::filesystem::path{generation_name(generation)};
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
  std::ifstream stream{path, std::ios::binary};
  std::vector<std::byte> bytes;
  char buffer[4096];
  while (stream.read(buffer, sizeof(buffer)) || stream.gcount() > 0) {
    const auto count = stream.gcount();
    for (std::streamsize index = 0; index < count; ++index) {
      bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(buffer[index])));
    }
  }
  return bytes;
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void write_text(const std::filesystem::path& path, const std::string& text) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  stream << text;
}

/// A store holding `count` edges at generation `count`.
std::filesystem::path build_store(std::string_view label, int count) {
  const std::filesystem::path root = fdep_test::make_temp_directory(label);
  Harness harness = Harness::durable(root);
  for (int index = 0; index < count; ++index) {
    harness.add(asset("node-" + std::to_string(index)), feed("f1"), DependencyKind::RequiresPowerFrom);
  }
  if (!harness.registry().close().ok()) {
    fdep_test::fail_now("could not close the store");
  }
  return root;
}

}  // namespace

FDEP_TEST(recovery, a_missing_pointer_falls_back_to_the_newest_intact_generation) {
  const std::filesystem::path root = build_store("recovery-no-pointer", 5);
  std::error_code error;
  std::filesystem::remove(root / "CURRENT", error);
  FDEP_REQUIRE(!error);

  {
    Harness harness = Harness::durable(root);
    const RecoveryReport& report = harness.registry().recovery_report();
    FDEP_CHECK(report.outcome == RecoveryOutcome::LoadedFallback);
    FDEP_CHECK(report.is_degraded());
    FDEP_CHECK_EQ(report.loaded_generation.value(), std::uint64_t{5});
    FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{5});
    FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{5});
    FDEP_CHECK_EQ(std::string{to_token(report.outcome)}, std::string{"loaded-fallback"});
    FDEP_REQUIRE(harness.registry().close().ok());
  }

  // The repair is durable: the next open is a normal, non-degraded load.
  Harness repaired = Harness::durable(root);
  FDEP_CHECK(repaired.registry().recovery_report().outcome == RecoveryOutcome::LoadedCurrent);
  FDEP_CHECK(!repaired.registry().recovery_report().is_degraded());
  FDEP_CHECK_EQ(repaired.generation().value(), std::uint64_t{5});
  FDEP_CHECK_EQ(repaired.snapshot().edge_count(), std::size_t{5});
  FDEP_REQUIRE(repaired.registry().close().ok());
  fdep_test::remove_tree(root);
}

FDEP_TEST(recovery, a_malformed_pointer_falls_back_and_is_repaired) {
  const std::filesystem::path root = build_store("recovery-bad-pointer", 3);
  write_text(root / "CURRENT", "NOT-A-POINTER\n");

  Harness harness = Harness::durable(root);
  const RecoveryReport& report = harness.registry().recovery_report();
  FDEP_CHECK(report.outcome == RecoveryOutcome::LoadedFallback);
  FDEP_CHECK_EQ(report.loaded_generation.value(), std::uint64_t{3});
  FDEP_CHECK(!report.diagnostics.empty());
  FDEP_CHECK(report.to_text().find("loaded-fallback") != std::string::npos);
  FDEP_REQUIRE(harness.registry().close().ok());

  Harness repaired = Harness::durable(root);
  FDEP_CHECK(repaired.registry().recovery_report().outcome == RecoveryOutcome::LoadedCurrent);
  FDEP_REQUIRE(repaired.registry().close().ok());
  fdep_test::remove_tree(root);
}

FDEP_TEST(recovery, a_pointer_naming_a_missing_generation_falls_back) {
  const std::filesystem::path root = build_store("recovery-dangling-pointer", 3);
  std::error_code error;
  std::filesystem::remove(gen_path(root, 3), error);
  FDEP_REQUIRE(!error);

  Harness harness = Harness::durable(root);
  const RecoveryReport& report = harness.registry().recovery_report();
  FDEP_CHECK(report.outcome == RecoveryOutcome::LoadedFallback);
  FDEP_CHECK_EQ(report.loaded_generation.value(), std::uint64_t{2});
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{2});
  FDEP_CHECK(std::find(report.rejected_generations.begin(), report.rejected_generations.end(),
                       DependencyGeneration::from_value(3)) != report.rejected_generations.end());
  FDEP_CHECK(report.diagnostics.front().find("no such generation file") != std::string::npos);
  FDEP_REQUIRE(harness.registry().close().ok());
  fdep_test::remove_tree(root);
}

FDEP_TEST(recovery, a_corrupt_newest_generation_falls_back_to_the_previous_one) {
  const std::filesystem::path root = build_store("recovery-corrupt-newest", 4);
  std::vector<std::byte> bytes = read_bytes(gen_path(root, 4));
  FDEP_REQUIRE(bytes.size() > 200);
  bytes[bytes.size() / 2] = static_cast<std::byte>(static_cast<unsigned char>(bytes[bytes.size() / 2]) ^ 0xFF);
  write_bytes(gen_path(root, 4), bytes);

  Harness harness = Harness::durable(root);
  const RecoveryReport& report = harness.registry().recovery_report();
  FDEP_CHECK(report.outcome == RecoveryOutcome::LoadedFallback);
  FDEP_CHECK_EQ(report.loaded_generation.value(), std::uint64_t{3});
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{3});
  FDEP_CHECK(std::find(report.rejected_generations.begin(), report.rejected_generations.end(),
                       DependencyGeneration::from_value(4)) != report.rejected_generations.end());
  bool mentions_integrity = false;
  for (const auto& note : report.diagnostics) {
    if (note.find("SHA-256") != std::string::npos || note.find("CRC-32") != std::string::npos) {
      mentions_integrity = true;
    }
  }
  FDEP_CHECK(mentions_integrity);

  // The superseded, corrupt generation is not resurrected by a later open:
  // the repaired pointer names the generation that was actually loaded.
  FDEP_REQUIRE(harness.registry().close().ok());
  Harness again = Harness::durable(root);
  FDEP_CHECK_EQ(again.generation().value(), std::uint64_t{3});
  FDEP_REQUIRE(again.registry().close().ok());
  fdep_test::remove_tree(root);
}

FDEP_TEST(recovery, a_store_whose_every_generation_is_corrupt_is_refused) {
  const std::filesystem::path root = build_store("recovery-all-corrupt", 3);
  for (std::uint64_t generation = 1; generation <= 3; ++generation) {
    const std::filesystem::path path = gen_path(root, generation);
    if (std::filesystem::exists(path)) {
      write_text(path, "not a generation at all");
    }
  }

  RegistryOpenRequest request;
  request.root = root;
  const auto refused = DependencyRegistry::open(request);
  FDEP_CHECK_CODE(refused, ErrorCode::StoreCorrupt);
  FDEP_CHECK(refused.error().detail().find("no usable generation") != std::string::npos);

  // The corrupt files are still there: a refused open deletes nothing.
  FDEP_CHECK(std::filesystem::exists(gen_path(root, 2)));
  FDEP_CHECK(std::filesystem::exists(gen_path(root, 3)));
  FDEP_CHECK(std::filesystem::exists(root / "CURRENT"));
  FDEP_CHECK(read_bytes(gen_path(root, 3)).size() == std::string{"not a generation at all"}.size());

  // And an empty graph was never presented in their place.
  Harness fresh = Harness::ephemeral();
  FDEP_CHECK_EQ(fresh.snapshot().edge_count(), std::size_t{0});
  fdep_test::remove_tree(root);
}

FDEP_TEST(recovery, a_store_with_a_pointer_but_no_generations_is_refused) {
  const std::filesystem::path root = build_store("recovery-pointer-only", 3);
  for (std::uint64_t generation = 1; generation <= 3; ++generation) {
    std::error_code error;
    std::filesystem::remove(gen_path(root, generation), error);
  }
  RegistryOpenRequest request;
  request.root = root;
  const auto refused = DependencyRegistry::open(request);
  FDEP_CHECK_CODE(refused, ErrorCode::StoreCorrupt);
  fdep_test::remove_tree(root);
}

FDEP_TEST(recovery, a_read_only_open_reports_but_never_repairs) {
  const std::filesystem::path root = build_store("recovery-readonly", 3);
  const std::string pointer_before = [&root]() {
    std::ifstream stream{root / "CURRENT", std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
  }();
  write_text(root / "CURRENT", "damaged\n");

  StoreOptions options;
  options.mode = OpenMode::ReadOnly;
  RegistrySnapshot snapshot;
  RecoveryReport report;
  auto handle = DurableStore::open(root, options, snapshot, report);
  FDEP_REQUIRE_OK(handle);
  FDEP_CHECK(report.outcome == RecoveryOutcome::LoadedFallback);
  FDEP_CHECK_EQ(snapshot.generation().value(), std::uint64_t{3});
  FDEP_REQUIRE(handle.value()->close().ok());

  // The damaged pointer is exactly as inspection found it. The stream is
  // closed before the tree is removed: on Windows an open handle keeps the
  // file, and therefore the directory, alive.
  {
    std::ifstream stream{root / "CURRENT", std::ios::binary};
    const std::string after{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    FDEP_CHECK_EQ(after, std::string{"damaged\n"});
    FDEP_CHECK(pointer_before != after);
  }
  fdep_test::remove_tree(root);
}

FDEP_TEST(recovery, an_empty_directory_is_a_fresh_store_in_both_modes) {
  const std::filesystem::path root = fdep_test::make_temp_directory("recovery-empty");

  StoreOptions options;
  options.mode = OpenMode::ReadOnly;
  RegistrySnapshot snapshot;
  RecoveryReport report;
  auto handle = DurableStore::open(root, options, snapshot, report);
  FDEP_REQUIRE_OK(handle);
  FDEP_CHECK(report.outcome == RecoveryOutcome::FreshEmpty);
  FDEP_CHECK_EQ(snapshot.generation().value(), std::uint64_t{0});
  FDEP_CHECK_EQ(snapshot.edge_count(), std::size_t{0});
  FDEP_REQUIRE(handle.value()->close().ok());

  // Nothing was created by the read-only open.
  FDEP_CHECK(!std::filesystem::exists(root / "CURRENT"));
  FDEP_CHECK(!std::filesystem::exists(root / "writer.lock"));

  Harness harness = Harness::durable(root);
  FDEP_CHECK(harness.registry().recovery_report().outcome == RecoveryOutcome::FreshEmpty);
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  FDEP_REQUIRE(harness.registry().close().ok());
  fdep_test::remove_tree(root);
}

FDEP_TEST(recovery, a_rejected_generation_survives_only_inside_the_retention_window) {
  const std::filesystem::path root = build_store("recovery-evidence", 3);
  write_text(gen_path(root, 3), "corrupt");
  // Plant a newer generation file as well, so both an unusable pointer target
  // and an unusable newer generation are present.
  write_text(gen_path(root, 4), "corrupt too");

  {
    Harness harness = Harness::durable(root);
    const RecoveryReport& report = harness.registry().recovery_report();
    FDEP_CHECK(report.outcome == RecoveryOutcome::LoadedFallback);
    FDEP_CHECK_EQ(report.loaded_generation.value(), std::uint64_t{2});
    FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{2});
    // Retention keeps the loaded generation and then the newest others, so the
    // newer rejected file is still on disk while the older one falls outside
    // the window. The generation that was loaded is never pruned, which is what
    // keeps the repaired pointer from dangling.
    FDEP_CHECK(std::filesystem::exists(gen_path(root, 4)));
    FDEP_CHECK(std::filesystem::exists(gen_path(root, 2)));
    FDEP_CHECK(report.generations_pruned >= 1);
    FDEP_REQUIRE(harness.registry().close().ok());
  }

  // A reopen after the repair is a plain load of the generation that was
  // recovered, not another fallback.
  Harness again = Harness::durable(root);
  FDEP_CHECK(again.registry().recovery_report().outcome == RecoveryOutcome::LoadedCurrent);
  FDEP_CHECK_EQ(again.generation().value(), std::uint64_t{2});
  FDEP_CHECK_EQ(again.snapshot().edge_count(), std::size_t{2});
  FDEP_REQUIRE(again.registry().close().ok());
  fdep_test::remove_tree(root);
}
