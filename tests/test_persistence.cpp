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
#include <memory>
#include <sstream>
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
using fdep_test::loop;
using fdep_test::rack;
using fdep_test::service;

namespace {

/// A temporary store root that removes itself when the test ends.
class TempStore {
 public:
  explicit TempStore(std::string_view label) : root_(fdep_test::make_temp_directory(label)) {}
  ~TempStore() { fdep_test::remove_tree(root_); }
  TempStore(const TempStore&) = delete;
  TempStore& operator=(const TempStore&) = delete;

  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

  [[nodiscard]] std::vector<std::string> file_names() const {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{root_, error}) {
      if (!error) {
        names.push_back(entry.path().filename().string());
      }
    }
    std::sort(names.begin(), names.end());
    return names;
  }

  [[nodiscard]] std::size_t count_with_prefix(std::string_view prefix) const {
    std::size_t total = 0;
    for (const auto& name : file_names()) {
      if (name.rfind(prefix, 0) == 0) {
        ++total;
      }
    }
    return total;
  }

  [[nodiscard]] std::string read(std::string_view name) const {
    std::ifstream stream{root_ / std::filesystem::path{std::string{name}}, std::ios::binary};
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
  }

  [[nodiscard]] std::filesystem::path path(std::string_view name) const {
    return root_ / std::filesystem::path{std::string{name}};
  }

 private:
  std::filesystem::path root_;
};

}  // namespace

FDEP_TEST(persistence, a_fresh_store_is_created_and_opens_empty) {
  TempStore store{"persistence-fresh"};
  FDEP_CHECK(store.file_names().empty());

  Harness harness = Harness::durable(store.root());
  FDEP_CHECK(harness.registry().durable());
  FDEP_CHECK(harness.registry().writer_incarnation().valid());
  FDEP_CHECK(harness.registry().recovery_report().outcome == RecoveryOutcome::FreshEmpty);
  FDEP_CHECK(!harness.registry().recovery_report().is_degraded());
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{0});
  FDEP_CHECK_EQ(std::string{to_token(harness.registry().recovery_report().outcome)},
                std::string{"fresh-empty"});

  // A fresh store has a lock file but no generation and no pointer yet.
  const auto status = harness.registry().store_status();
  FDEP_REQUIRE_OK(status);
  FDEP_CHECK(!status.value().pointer_present);
  FDEP_CHECK(status.value().retained_generations.empty());
  FDEP_CHECK(status.value().writer_lock_held);
  FDEP_CHECK_EQ(status.value().total_bytes, std::uint64_t{0});
  FDEP_CHECK(!status.value().to_text().empty());
  FDEP_REQUIRE(harness.registry().close().ok());

  // Reopening the same store finds the same empty state, not a fresh one.
  Harness reopened = Harness::durable(store.root());
  FDEP_CHECK(reopened.registry().recovery_report().outcome == RecoveryOutcome::FreshEmpty);
  FDEP_CHECK_EQ(reopened.generation().value(), std::uint64_t{0});
  FDEP_REQUIRE(reopened.registry().close().ok());
}

FDEP_TEST(persistence, committed_state_survives_close_and_reopen_exactly) {
  TempStore store{"persistence-reopen"};
  ContentDigest digest_before;
  DependencyEdgeId first_id{};
  {
    Harness harness = Harness::durable(store.root());
    harness.declare(asset("declared"), fdep_test::provenance("change-1", "alice", 7, "observed"));
    first_id = harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
    const auto constraint = DependencyConstraint::make(ConstraintKind::RedundancyClass, "2N");
    FDEP_REQUIRE_OK(constraint);
    harness.add(fdep_test::make_spec(asset("b"), loop("l1"), DependencyKind::CooledBy,
                                     DependencyStrength::Soft, Direction::DependsOn, LifecycleState::Active,
                                     {constraint.value()},
                                     fdep_test::provenance("change-2", "bob", 8, "cooled")));
    digest_before = harness.snapshot().state_digest();
    FDEP_REQUIRE(harness.registry().close().ok());
  }

  // The store layout is exactly as documented.
  const std::vector<std::string> names = store.file_names();
  FDEP_CHECK(std::find(names.begin(), names.end(), "CURRENT") != names.end());
  FDEP_CHECK(std::find(names.begin(), names.end(), "writer.lock") != names.end());
  FDEP_CHECK_EQ(store.count_with_prefix("gen-"), std::size_t{2});
  FDEP_CHECK_EQ(store.count_with_prefix("tmp-"), std::size_t{0});
  FDEP_CHECK(store.read("CURRENT").find("FDEPCUR1") == 0);

  {
    Harness harness = Harness::durable(store.root());
    FDEP_CHECK(harness.registry().recovery_report().outcome == RecoveryOutcome::LoadedCurrent);
    FDEP_CHECK(!harness.registry().recovery_report().is_degraded());
    FDEP_CHECK_EQ(harness.registry().recovery_report().loaded_generation.value(), std::uint64_t{3});
    FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{3});
    FDEP_CHECK_EQ(harness.snapshot().state_digest(), digest_before);
    FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{2});
    FDEP_CHECK_EQ(harness.snapshot().declared_ref_count(), std::size_t{1});

    const auto* record = harness.snapshot().find_edge(first_id);
    FDEP_REQUIRE(record != nullptr);
    FDEP_CHECK(record->source() == asset("a"));
    FDEP_CHECK(record->target() == feed("f1"));
    FDEP_CHECK_EQ(record->revision().value(), std::uint64_t{1});
    FDEP_CHECK_EQ(record->registered_generation().value(), std::uint64_t{2});
    const auto* declaration = harness.snapshot().find_declared_ref(asset("declared"));
    FDEP_REQUIRE(declaration != nullptr);
    FDEP_CHECK_EQ(declaration->provenance().principal(), std::string{"alice"});
    FDEP_CHECK_EQ(declaration->provenance().recorded_at_unix_ms(), std::int64_t{7});

    // Mutations continue from the loaded generation.
    harness.add(asset("c"), service("s1"), DependencyKind::ServedBy);
    FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{4});
    FDEP_REQUIRE(harness.registry().close().ok());
  }

  Harness third = Harness::durable(store.root());
  FDEP_CHECK_EQ(third.generation().value(), std::uint64_t{4});
  FDEP_CHECK_EQ(third.snapshot().edge_count(), std::size_t{3});
  FDEP_REQUIRE(third.registry().close().ok());
}

FDEP_TEST(persistence, superseded_generations_are_retained_then_pruned) {
  TempStore store{"persistence-retention"};
  std::vector<ContentDigest> digests;
  DependencyGeneration final_generation{};
  {
    Harness harness = Harness::durable(store.root());
    for (int index = 0; index < 6; ++index) {
      harness.add(asset("node-" + std::to_string(index)), feed("f1"), DependencyKind::RequiresPowerFrom);
      digests.push_back(harness.snapshot().state_digest());
    }
    final_generation = harness.generation();
    FDEP_REQUIRE(harness.registry().close().ok());
  }
  FDEP_CHECK_EQ(final_generation.value(), std::uint64_t{6});
  // The configured retention is two, so only the two newest remain.
  FDEP_CHECK_EQ(store.count_with_prefix("gen-"), std::size_t{2});

  {
    Harness harness = Harness::durable(store.root());
    const auto status = harness.registry().store_status();
    FDEP_REQUIRE_OK(status);
    FDEP_REQUIRE(status.value().retained_generations.size() == 2);
    FDEP_CHECK_EQ(status.value().retained_generations[0].value(), std::uint64_t{6});
    FDEP_CHECK_EQ(status.value().retained_generations[1].value(), std::uint64_t{5});
    FDEP_CHECK(status.value().pointer_present);
    FDEP_CHECK_EQ(status.value().pointer_generation.value(), std::uint64_t{6});
    FDEP_CHECK(status.value().total_bytes > 0);
    FDEP_CHECK(status.value().writer_lock_held);
    FDEP_REQUIRE(harness.registry().close().ok());
  }
}

FDEP_TEST(persistence, a_larger_retention_window_keeps_more_history) {
  TempStore store{"persistence-window"};
  RegistryLimits limits;
  limits.max_retained_generations = 4;
  {
    Harness harness = Harness::durable(store.root(), limits);
    for (int index = 0; index < 5; ++index) {
      harness.add(asset("node-" + std::to_string(index)), feed("f1"), DependencyKind::RequiresPowerFrom);
    }
    FDEP_REQUIRE(harness.registry().close().ok());
  }
  FDEP_CHECK_EQ(store.count_with_prefix("gen-"), std::size_t{4});

  Harness harness = Harness::durable(store.root(), limits);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{5});
  FDEP_REQUIRE(harness.registry().close().ok());
}

FDEP_TEST(persistence, a_second_writer_is_locked_out) {
  TempStore store{"persistence-lock"};
  Harness first = Harness::durable(store.root());
  first.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const auto incarnation = first.registry().writer_incarnation();
  FDEP_CHECK(incarnation.valid());

  RegistryOpenRequest request;
  request.root = store.root();
  const auto second = DependencyRegistry::open(request);
  FDEP_CHECK_CODE(second, ErrorCode::StoreLocked);
  FDEP_CHECK(second.error().detail().find("writer") != std::string::npos);
  FDEP_CHECK(second.error().detail().find(std::to_string(incarnation.process_id())) != std::string::npos);

  // The lock record names the holder.
  FDEP_CHECK(store.read("writer.lock").find("pid=") == 0);

  FDEP_REQUIRE(first.registry().close().ok());
  Harness third = Harness::durable(store.root());
  FDEP_CHECK_EQ(third.generation().value(), std::uint64_t{1});
  FDEP_REQUIRE(third.registry().close().ok());
}

FDEP_TEST(persistence, a_read_only_store_never_writes_and_still_reads) {
  TempStore store{"persistence-readonly"};
  {
    Harness harness = Harness::durable(store.root());
    harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
    harness.declare(asset("a"));
    FDEP_REQUIRE(harness.registry().close().ok());
  }
  const std::vector<std::string> before = store.file_names();

  StoreOptions options;
  options.mode = OpenMode::ReadOnly;
  RegistrySnapshot snapshot;
  RecoveryReport report;
  auto store_handle = DurableStore::open(store.root(), options, snapshot, report);
  FDEP_REQUIRE_OK(store_handle);
  DurableStore& read_only = *store_handle.value();
  FDEP_CHECK(read_only.read_only());
  FDEP_CHECK(!read_only.lock_held());
  FDEP_CHECK(report.outcome == RecoveryOutcome::LoadedCurrent);
  FDEP_CHECK_EQ(snapshot.generation().value(), std::uint64_t{2});
  FDEP_CHECK_EQ(snapshot.edge_count(), std::size_t{1});

  // Reading the current state again works.
  const auto reloaded = read_only.load();
  FDEP_REQUIRE_OK(reloaded);
  FDEP_CHECK_EQ(reloaded.value().state_digest(), snapshot.state_digest());

  // The retained generations are readable directly.
  const auto retained = read_only.retained_generations();
  FDEP_REQUIRE_OK(retained);
  FDEP_CHECK(!retained.value().empty());
  const auto older = read_only.load_generation(DependencyGeneration::from_value(1));
  FDEP_REQUIRE_OK(older);
  FDEP_CHECK_EQ(older.value().edge_count(), std::size_t{1});
  FDEP_CHECK_EQ(older.value().declared_ref_count(), std::size_t{0});

  // Publishing through a read-only store is refused.
  FDEP_CHECK_STATUS(read_only.publish(snapshot), ErrorCode::StoreReadOnly);

  const auto status = read_only.status();
  FDEP_REQUIRE_OK(status);
  FDEP_CHECK(status.value().read_only);
  FDEP_CHECK(!status.value().writer_lock_held);
  FDEP_REQUIRE(read_only.close().ok());

  // Nothing on disk changed.
  FDEP_CHECK(store.file_names() == before);
}

FDEP_TEST(persistence, a_read_only_open_of_a_missing_store_fails) {
  TempStore store{"persistence-missing"};
  const std::filesystem::path missing = store.root() / "not-here";
  StoreOptions options;
  options.mode = OpenMode::ReadOnly;
  RegistrySnapshot snapshot;
  RecoveryReport report;
  const auto handle = DurableStore::open(missing, options, snapshot, report);
  FDEP_CHECK_CODE(handle, ErrorCode::StoreNotFound);

  StoreOptions no_create;
  no_create.create_if_missing = false;
  const auto second = DurableStore::open(missing, no_create, snapshot, report);
  FDEP_CHECK_CODE(second, ErrorCode::StoreNotFound);

  FDEP_CHECK_CODE(DurableStore::open(std::filesystem::path{}, no_create, snapshot, report),
                  ErrorCode::InvalidStorePath);
}

FDEP_TEST(persistence, publish_refuses_a_generation_that_is_not_the_next_one) {
  TempStore store{"persistence-publish"};
  StoreOptions options;
  RegistrySnapshot snapshot;
  RecoveryReport report;
  auto handle = DurableStore::open(store.root(), options, snapshot, report);
  FDEP_REQUIRE_OK(handle);
  DurableStore& durable = *handle.value();
  FDEP_CHECK_EQ(durable.committed_generation().value(), std::uint64_t{0});

  // Generation zero is not "the next one" once the store is open at zero: the
  // next generation is one.
  FDEP_CHECK_STATUS(durable.publish(snapshot), ErrorCode::StaleGeneration);

  // Build a state at generation two and try to publish it first.
  Harness ephemeral = Harness::ephemeral();
  ephemeral.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  ephemeral.add(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
  FDEP_CHECK_STATUS(durable.publish(ephemeral.snapshot()), ErrorCode::StaleGeneration);

  // Publishing generation one out of order fails; the store is unchanged.
  Harness one = Harness::ephemeral();
  one.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const auto first_publish = durable.publish(one.snapshot());
  FDEP_REQUIRE(first_publish.ok());
  FDEP_CHECK_EQ(durable.committed_generation().value(), std::uint64_t{1});

  // Publishing the same generation again is stale, not a silent overwrite.
  FDEP_CHECK_STATUS(durable.publish(one.snapshot()), ErrorCode::StaleGeneration);

  const auto status = durable.status();
  FDEP_REQUIRE_OK(status);
  FDEP_CHECK_EQ(status.value().pointer_generation.value(), std::uint64_t{1});
  FDEP_REQUIRE(durable.close().ok());

  // The published state is what a fresh open loads.
  Harness reopened = Harness::durable(store.root());
  FDEP_CHECK_EQ(reopened.generation().value(), std::uint64_t{1});
  FDEP_CHECK_EQ(reopened.snapshot().state_digest(), one.snapshot().state_digest());
  FDEP_REQUIRE(reopened.registry().close().ok());
}

FDEP_TEST(persistence, transient_files_are_removed_and_never_interpreted) {
  TempStore store{"persistence-transient"};
  {
    Harness harness = Harness::durable(store.root());
    harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
    FDEP_REQUIRE(harness.registry().close().ok());
  }
  // Plant a transient file, as a crashed publish would have left behind.
  {
    std::ofstream stream{store.path("tmp-abcdef.tmp"), std::ios::binary};
    stream << "this is not a generation";
  }
  FDEP_CHECK_EQ(store.count_with_prefix("tmp-"), std::size_t{1});

  Harness harness = Harness::durable(store.root());
  FDEP_CHECK_EQ(harness.registry().recovery_report().transient_files_removed, std::uint32_t{1});
  FDEP_CHECK_EQ(store.count_with_prefix("tmp-"), std::size_t{0});
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{1});
  FDEP_REQUIRE(harness.registry().close().ok());
}

FDEP_TEST(persistence, unrelated_files_in_the_store_root_are_ignored) {
  TempStore store{"persistence-unrelated"};
  {
    Harness harness = Harness::durable(store.root());
    harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
    FDEP_REQUIRE(harness.registry().close().ok());
  }
  {
    std::ofstream stream{store.path("README.txt"), std::ios::binary};
    stream << "operator note";
  }
  {
    std::ofstream stream{store.path("gen-notanumber.fdepstate"), std::ios::binary};
    stream << "junk";
  }
  Harness harness = Harness::durable(store.root());
  FDEP_CHECK(harness.registry().recovery_report().outcome == RecoveryOutcome::LoadedCurrent);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{1});
  FDEP_REQUIRE(harness.registry().close().ok());
}

FDEP_TEST(persistence, store_status_reports_a_released_lock_honestly) {
  TempStore store{"persistence-status"};
  {
    Harness harness = Harness::durable(store.root());
    harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
    FDEP_REQUIRE(harness.registry().close().ok());
  }
  StoreOptions options;
  options.mode = OpenMode::ReadOnly;
  RegistrySnapshot snapshot;
  RecoveryReport report;
  auto handle = DurableStore::open(store.root(), options, snapshot, report);
  FDEP_REQUIRE_OK(handle);
  const auto status = handle.value()->status();
  FDEP_REQUIRE_OK(status);
  FDEP_CHECK(!status.value().writer_lock_held);
  FDEP_CHECK(status.value().pointer_present);
  FDEP_CHECK_EQ(status.value().pointer_generation.value(), std::uint64_t{1});
  FDEP_CHECK(status.value().to_text().find("read-only") != std::string::npos);
  FDEP_REQUIRE(handle.value()->close().ok());
}

FDEP_TEST(persistence, generation_the_store_does_not_hold_is_reported) {
  TempStore store{"persistence-missing-generation"};
  StoreOptions options;
  options.mode = OpenMode::ReadOnly;
  RegistrySnapshot snapshot;
  RecoveryReport report;
  auto handle = DurableStore::open(store.root(), options, snapshot, report);
  if (!handle) {
    FDEP_CHECK(handle.error().code() == ErrorCode::StoreNotFound);
    return;
  }
  const auto missing = handle.value()->load_generation(DependencyGeneration::from_value(9));
  FDEP_CHECK_CODE(missing, ErrorCode::StoreNotFound);
  const auto current = handle.value()->load();
  FDEP_CHECK_CODE(current, ErrorCode::StoreIoError);
  FDEP_REQUIRE(handle.value()->close().ok());
}

FDEP_TEST(persistence, recovery_report_renders_its_outcome) {
  TempStore store{"persistence-report"};
  {
    Harness harness = Harness::durable(store.root());
    harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
    FDEP_REQUIRE(harness.registry().close().ok());
  }
  Harness harness = Harness::durable(store.root());
  const std::string text = harness.registry().recovery_report().to_text();
  FDEP_CHECK(text.find("outcome=loaded-current") != std::string::npos);
  FDEP_CHECK(text.find("generation=1") != std::string::npos);
  FDEP_CHECK(text.find("rejected=none") != std::string::npos);
  FDEP_REQUIRE(harness.registry().close().ok());
}
