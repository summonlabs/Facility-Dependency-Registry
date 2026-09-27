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

#include <filesystem>
#include <string>
#include <vector>

#include "facility_dependency_registry/facility_dependency_registry.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

using namespace facility_dependency_registry;
using fdep_test::asset;
using fdep_test::feed;
using fdep_test::Harness;
using fdep_test::loop;
using fdep_test::provenance;
using fdep_test::service;

namespace {

const EdgeChange* find_change(const GenerationDiff& diff, DependencyEdgeId id) {
  for (const auto& change : diff.edge_changes) {
    if (change.id == id) {
      return &change;
    }
  }
  return nullptr;
}

const EdgeFieldChange* find_field(const EdgeChange& change, std::string_view field) {
  for (const auto& entry : change.fields) {
    if (entry.field == field) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace

FDEP_TEST(diff, a_snapshot_does_not_differ_from_itself) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const RegistrySnapshot snapshot = harness.snapshot();
  const auto diff = diff_snapshots(snapshot, snapshot, 64);
  FDEP_REQUIRE_OK(diff);
  FDEP_CHECK(diff.value().empty());
  FDEP_CHECK_EQ(diff.value().unchanged_edges, std::size_t{1});
  FDEP_CHECK_EQ(diff.value().added(), std::size_t{0});
  FDEP_CHECK_EQ(diff.value().removed(), std::size_t{0});
  FDEP_CHECK_EQ(diff.value().modified(), std::size_t{0});
  FDEP_CHECK_EQ(diff.value().from.value(), std::uint64_t{1});
  FDEP_CHECK_EQ(diff.value().to.value(), std::uint64_t{1});
  FDEP_CHECK(!diff.value().truncated);
}

FDEP_TEST(diff, additions_removals_and_modifications_are_distinguished) {
  Harness harness = Harness::ephemeral();
  const DependencyEdgeId kept =
      harness.add(asset("kept"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const DependencyEdgeId removed =
      harness.add(asset("removed"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const DependencyEdgeId modified = harness.add(asset("modified"), loop("l1"), DependencyKind::CooledBy,
                                               DependencyStrength::Hard);
  const RegistrySnapshot before = harness.snapshot();

  const DependencyEdgeId added = harness.add(asset("added"), service("s1"), DependencyKind::ServedBy);
  FDEP_REQUIRE(harness.remove(removed, kInitialRevision).ok());
  {
    UpdateEdgeRequest update;
    update.context.expected_generation = harness.generation();
    update.id = modified;
    update.expected_revision = kInitialRevision;
    update.strength = DependencyStrength::Soft;
    update.provenance = provenance("change-2", "bob", 4, "revised");
    FDEP_REQUIRE(harness.registry().update_edge(update).has_value());
  }

  const RegistrySnapshot after = harness.snapshot();
  const auto diff = diff_snapshots(before, after, 64);
  FDEP_REQUIRE_OK(diff);
  const GenerationDiff& value = diff.value();
  FDEP_CHECK_EQ(value.from.value(), std::uint64_t{3});
  FDEP_CHECK_EQ(value.to.value(), std::uint64_t{6});
  FDEP_CHECK_EQ(value.edge_changes.size(), std::size_t{3});
  FDEP_CHECK_EQ(value.added(), std::size_t{1});
  FDEP_CHECK_EQ(value.removed(), std::size_t{1});
  FDEP_CHECK_EQ(value.modified(), std::size_t{1});
  FDEP_CHECK_EQ(value.unchanged_edges, std::size_t{1});

  const EdgeChange* added_change = find_change(value, added);
  FDEP_REQUIRE(added_change != nullptr);
  FDEP_CHECK(added_change->kind == EdgeChangeKind::Added);
  FDEP_CHECK_EQ(added_change->id.value(), added.value());
  FDEP_CHECK(added_change->after.source() == asset("added"));
  FDEP_CHECK_EQ(added_change->before.id().value(), std::uint64_t{0});
  FDEP_CHECK_EQ(added_change->after_revision.value(), std::uint64_t{1});
  FDEP_CHECK(added_change->fields.empty());
  FDEP_CHECK(added_change->to_text().find("added") != std::string::npos);

  const EdgeChange* removed_change = find_change(value, removed);
  FDEP_REQUIRE(removed_change != nullptr);
  FDEP_CHECK(removed_change->kind == EdgeChangeKind::Removed);
  FDEP_CHECK(removed_change->before.source() == asset("removed"));
  FDEP_CHECK_EQ(removed_change->after.id().value(), std::uint64_t{0});
  FDEP_CHECK_EQ(removed_change->before_revision.value(), std::uint64_t{1});

  const EdgeChange* modified_change = find_change(value, modified);
  FDEP_REQUIRE(modified_change != nullptr);
  FDEP_CHECK(modified_change->kind == EdgeChangeKind::Modified);
  FDEP_CHECK_EQ(modified_change->before_revision.value(), std::uint64_t{1});
  FDEP_CHECK_EQ(modified_change->after_revision.value(), std::uint64_t{2});
  FDEP_REQUIRE(find_field(*modified_change, "strength") != nullptr);
  FDEP_CHECK_EQ(find_field(*modified_change, "strength")->before, std::string{"hard"});
  FDEP_CHECK_EQ(find_field(*modified_change, "strength")->after, std::string{"soft"});
  FDEP_REQUIRE(find_field(*modified_change, "provenance") != nullptr);
  FDEP_CHECK(find_field(*modified_change, "provenance")->after.find("bob") != std::string::npos);
  FDEP_CHECK(find_field(*modified_change, "lifecycle") == nullptr);
  FDEP_CHECK(find_field(*modified_change, "direction") == nullptr);
  FDEP_CHECK(find_field(*modified_change, "constraints") == nullptr);
  FDEP_CHECK(modified_change->to_text().find("strength:hard->soft") != std::string::npos);

  FDEP_CHECK(find_change(value, kept) == nullptr);
}

FDEP_TEST(diff, a_lifecycle_transition_shows_up_as_a_modified_edge) {
  Harness harness = Harness::ephemeral();
  const DependencyEdgeId id = harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const RegistrySnapshot before = harness.snapshot();
  FDEP_REQUIRE(harness.transition(id, kInitialRevision, LifecycleState::Suspended).ok());
  const auto diff = diff_snapshots(before, harness.snapshot(), 64);
  FDEP_REQUIRE_OK(diff);
  FDEP_REQUIRE(diff.value().edge_changes.size() == 1);
  const EdgeChange& change = diff.value().edge_changes[0];
  FDEP_CHECK(change.kind == EdgeChangeKind::Modified);
  const EdgeFieldChange* lifecycle = find_field(change, "lifecycle");
  FDEP_REQUIRE(lifecycle != nullptr);
  FDEP_CHECK_EQ(lifecycle->before, std::string{"active"});
  FDEP_CHECK_EQ(lifecycle->after, std::string{"suspended"});
  const EdgeFieldChange* modified_generation = find_field(change, "last-modified-generation");
  FDEP_REQUIRE(modified_generation != nullptr);
  FDEP_CHECK_EQ(modified_generation->before, std::string{"1"});
  FDEP_CHECK_EQ(modified_generation->after, std::string{"2"});
}

FDEP_TEST(diff, declaration_changes_are_reported) {
  Harness harness = Harness::ephemeral();
  harness.declare(asset("kept"));
  harness.declare(asset("withdrawn"));
  const RegistrySnapshot before = harness.snapshot();

  harness.declare(loop("added"));
  WithdrawRefRequest withdraw;
  withdraw.context.expected_generation = harness.generation();
  withdraw.ref = asset("withdrawn");
  FDEP_REQUIRE_OK(harness.registry().withdraw_external_ref(withdraw));

  const auto diff = diff_snapshots(before, harness.snapshot(), 64);
  FDEP_REQUIRE_OK(diff);
  FDEP_CHECK(diff.value().edge_changes.empty());
  FDEP_REQUIRE(diff.value().ref_changes.size() == 2);
  // The merge join walks both declaration lists in canonical order, so the
  // withdrawal of an asset reference is reported before the addition of a
  // cooling domain reference.
  FDEP_CHECK(!diff.value().ref_changes[0].added);
  FDEP_CHECK(diff.value().ref_changes[0].ref == asset("withdrawn"));
  FDEP_CHECK(diff.value().ref_changes[0].before.principal() == std::string{"test-principal"});
  FDEP_CHECK(diff.value().ref_changes[1].added);
  FDEP_CHECK(diff.value().ref_changes[1].ref == loop("added"));
  FDEP_CHECK(diff.value().ref_changes[1].after.source_id() == std::string{"test-source"});
  FDEP_CHECK(!diff.value().empty());
  FDEP_CHECK(diff.value().ref_changes[0].to_text().find("withdrew") == 0);
  FDEP_CHECK(diff.value().ref_changes[1].to_text().find("declared") == 0);
}

FDEP_TEST(diff, the_reverse_direction_swaps_additions_and_removals) {
  Harness harness = Harness::ephemeral();
  const DependencyEdgeId first = harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const RegistrySnapshot before = harness.snapshot();
  const DependencyEdgeId second = harness.add(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const RegistrySnapshot after = harness.snapshot();

  const auto forward = diff_snapshots(before, after, 64);
  FDEP_REQUIRE_OK(forward);
  FDEP_CHECK_EQ(forward.value().added(), std::size_t{1});
  FDEP_CHECK_EQ(forward.value().removed(), std::size_t{0});
  FDEP_CHECK_EQ(forward.value().unchanged_edges, std::size_t{1});

  const auto backward = diff_snapshots(after, before, 64);
  FDEP_REQUIRE_OK(backward);
  FDEP_CHECK_EQ(backward.value().added(), std::size_t{0});
  FDEP_CHECK_EQ(backward.value().removed(), std::size_t{1});
  FDEP_CHECK_EQ(backward.value().unchanged_edges, std::size_t{1});
  FDEP_REQUIRE(backward.value().edge_changes.size() == 1);
  FDEP_CHECK_EQ(backward.value().edge_changes[0].id.value(), second.value());

  // A diff between an empty state and a populated one reports everything.
  const auto from_empty = diff_snapshots(RegistrySnapshot::empty(), after, 64);
  FDEP_REQUIRE_OK(from_empty);
  FDEP_CHECK_EQ(from_empty.value().added(), std::size_t{2});
  FDEP_CHECK_EQ(from_empty.value().unchanged_edges, std::size_t{0});
  FDEP_CHECK_EQ(from_empty.value().from.value(), std::uint64_t{0});
  FDEP_CHECK(find_change(from_empty.value(), first) != nullptr);
}

FDEP_TEST(diff, the_change_bound_is_reported_as_truncation) {
  Harness harness = Harness::ephemeral();
  const RegistrySnapshot empty = RegistrySnapshot::empty();
  for (int index = 0; index < 5; ++index) {
    harness.add(asset("node-" + std::to_string(index)), feed("f1"), DependencyKind::RequiresPowerFrom);
  }
  const auto diff = diff_snapshots(empty, harness.snapshot(), 2);
  FDEP_REQUIRE_OK(diff);
  FDEP_CHECK(diff.value().truncated);
  FDEP_CHECK_EQ(diff.value().edge_changes.size(), std::size_t{2});

  const auto complete = diff_snapshots(empty, harness.snapshot(), 5);
  FDEP_REQUIRE_OK(complete);
  FDEP_CHECK(!complete.value().truncated);
  FDEP_CHECK_EQ(complete.value().edge_changes.size(), std::size_t{5});

  const auto none_allowed = diff_snapshots(empty, harness.snapshot(), 0);
  FDEP_REQUIRE_OK(none_allowed);
  FDEP_CHECK(none_allowed.value().truncated);
  FDEP_CHECK(none_allowed.value().edge_changes.empty());
}

FDEP_TEST(diff, a_cancelled_diff_reports_cancellation) {
  Harness harness = Harness::ephemeral();
  const RegistrySnapshot empty = RegistrySnapshot::empty();
  for (int index = 0; index < 4; ++index) {
    harness.add(asset("node-" + std::to_string(index)), feed("f1"), DependencyKind::RequiresPowerFrom);
  }
  CancellationSource source;
  source.cancel();
  FDEP_CHECK_CODE(diff_snapshots(empty, harness.snapshot(), 64, source.token()), ErrorCode::Cancelled);
}

FDEP_TEST(diff, diffs_between_retained_generations_are_available_from_a_store) {
  const std::filesystem::path root = fdep_test::make_temp_directory("diff-generations");
  RegistryLimits limits;
  limits.max_retained_generations = 4;
  {
    Harness harness = Harness::durable(root, limits);
    harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
    harness.add(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
    harness.add(asset("c"), feed("f1"), DependencyKind::RequiresPowerFrom);
    FDEP_REQUIRE(harness.registry().close().ok());
  }

  StoreOptions options;
  options.mode = OpenMode::ReadOnly;
  options.limits = limits;
  RegistrySnapshot snapshot;
  RecoveryReport report;
  auto handle = DurableStore::open(root, options, snapshot, report);
  FDEP_REQUIRE_OK(handle);
  const auto first = handle.value()->load_generation(DependencyGeneration::from_value(1));
  FDEP_REQUIRE_OK(first);
  const auto third = handle.value()->load_generation(DependencyGeneration::from_value(3));
  FDEP_REQUIRE_OK(third);
  const auto diff = diff_snapshots(first.value(), third.value(), 16);
  FDEP_REQUIRE_OK(diff);
  FDEP_CHECK_EQ(diff.value().added(), std::size_t{2});
  FDEP_CHECK_EQ(diff.value().unchanged_edges, std::size_t{1});
  FDEP_CHECK_EQ(diff.value().from.value(), std::uint64_t{1});
  FDEP_CHECK_EQ(diff.value().to.value(), std::uint64_t{3});
  FDEP_REQUIRE(handle.value()->close().ok());
  fdep_test::remove_tree(root);
}
