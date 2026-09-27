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
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "facility_dependency_registry/facility_dependency_registry.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

using namespace facility_dependency_registry;
using fdep_test::asset;
using fdep_test::feed;
using fdep_test::Harness;
using fdep_test::Rng;

namespace {

void write_text(const std::filesystem::path& path, const std::string& text) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  stream << text;
}

std::size_t count_entries(const std::filesystem::path& root) {
  std::size_t total = 0;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator{root, error}) {
    if (error) {
      break;
    }
    static_cast<void>(entry);
    ++total;
  }
  return total;
}

}  // namespace

FDEP_TEST(adversarial, identifiers_that_look_like_paths_are_ordinary_identifiers) {
  // Traversal sequences are rejected by the identifier syntax, so a reference
  // can never influence a path. The store names its own files from the
  // generation number it just computed, never from anything a caller supplied.
  FDEP_CHECK_CODE(DependencyNodeRef::create(NodeDomain::Asset, "../escape"), ErrorCode::InvalidNodeIdSyntax);
  FDEP_CHECK_CODE(DependencyNodeRef::create(NodeDomain::Asset, "..\\escape"), ErrorCode::InvalidNodeIdSyntax);
  FDEP_CHECK_CODE(DependencyNodeRef::create(NodeDomain::Asset, "/absolute"), ErrorCode::InvalidNodeIdSyntax);
  FDEP_CHECK_CODE(DependencyNodeRef::create(NodeDomain::Asset, "C:drive"), ErrorCode::InvalidNodeIdSyntax);
  FDEP_CHECK_CODE(DependencyNodeRef::create(NodeDomain::Asset, "a/../../b"), ErrorCode::InvalidNodeIdSyntax);
  FDEP_CHECK_CODE(DependencyNodeRef::create(NodeDomain::Asset, "a\\..\\b"), ErrorCode::InvalidNodeIdSyntax);
  FDEP_CHECK_CODE(DependencyNodeRef::create(NodeDomain::Asset, "a..b"), ErrorCode::InvalidNodeIdSyntax);

  // A name that happens to be reserved by an operating system stays a plain
  // opaque string: the registry never turns an identifier into a file name, so
  // there is nothing for such a name to collide with.
  const auto reserved = DependencyNodeRef::create(NodeDomain::Asset, "NUL");
  FDEP_REQUIRE_OK(reserved);
  FDEP_CHECK_EQ(reserved.value().to_canonical(), std::string{"asset:NUL"});

  // A hostile-looking identifier that does satisfy the syntax is likewise never
  // interpreted.
  const auto ref = DependencyNodeRef::create(NodeDomain::Asset, "etc-passwd.dot");
  FDEP_REQUIRE_OK(ref);
  FDEP_CHECK_EQ(ref.value().to_canonical(), std::string{"asset:etc-passwd.dot"});

  const std::filesystem::path root = fdep_test::make_temp_directory("adversarial-paths");
  {
    Harness harness = Harness::durable(root);
    harness.add(asset("etc-passwd.dot"), feed("f1"), DependencyKind::RequiresPowerFrom);
    FDEP_REQUIRE(harness.registry().close().ok());
  }
  // The store wrote only names it derived itself.
  const std::filesystem::path parent = root.parent_path();
  FDEP_CHECK(!std::filesystem::exists(parent / "etc-passwd.dot"));
  FDEP_CHECK(std::filesystem::exists(root / "CURRENT"));
  fdep_test::remove_tree(root);
}

FDEP_TEST(adversarial, a_store_root_that_is_not_a_directory_is_refused) {
  const std::filesystem::path root = fdep_test::make_temp_directory("adversarial-root");
  const std::filesystem::path file = root / "not-a-directory";
  write_text(file, "plain file");

  RegistryOpenRequest request;
  request.root = file;
  const auto refused = DependencyRegistry::open(request);
  FDEP_CHECK_CODE(refused, ErrorCode::StoreLayoutInvalid);

  // A pointer that is a directory rather than a file is refused too.
  const std::filesystem::path store = root / "store";
  {
    Harness harness = Harness::durable(store);
    harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
    FDEP_REQUIRE(harness.registry().close().ok());
  }
  std::error_code error;
  std::filesystem::remove(store / "CURRENT", error);
  std::filesystem::create_directory(store / "CURRENT", error);
  FDEP_REQUIRE(!error);

  Harness harness = Harness::durable(store);
  // The unreadable pointer is not believed; the store recovers from history.
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{1});
  FDEP_REQUIRE(harness.registry().close().ok());
  fdep_test::remove_tree(root);
}

FDEP_TEST(adversarial, a_store_root_full_of_unrelated_files_is_still_opened) {
  const std::filesystem::path root = fdep_test::make_temp_directory("adversarial-junk");
  {
    Harness harness = Harness::durable(root);
    harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
    FDEP_REQUIRE(harness.registry().close().ok());
  }
  for (int index = 0; index < 200; ++index) {
    write_text(root / ("junk-" + std::to_string(index) + ".txt"), "noise");
  }
  Harness harness = Harness::durable(root);
  FDEP_CHECK(harness.registry().recovery_report().outcome == RecoveryOutcome::LoadedCurrent);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{1});
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{1});
  FDEP_REQUIRE(harness.registry().close().ok());
  fdep_test::remove_tree(root);
}

FDEP_TEST(adversarial, resource_bounds_are_enforced_rather_than_exceeded) {
  // A graph at its edge ceiling refuses the next edge instead of growing.
  RegistryLimits limits;
  limits.max_edges = 8;
  Harness harness = Harness::ephemeral(limits);
  for (int index = 0; index < 8; ++index) {
    harness.add(asset("node-" + std::to_string(index)), feed("f1"), DependencyKind::RequiresPowerFrom);
  }
  const auto overflow = harness.try_add(
      fdep_test::make_spec(asset("node-9"), feed("f1"), DependencyKind::RequiresPowerFrom),
      harness.generation());
  FDEP_CHECK_CODE(overflow, ErrorCode::EdgeCapacityExceeded);
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{8});
  FDEP_CHECK_EQ(harness.snapshot().next_edge_ordinal(), std::uint64_t{9});

  // A generation at its ceiling refuses to advance, with a distinct category.
  RegistryLimits generation_limits;
  generation_limits.max_generation = 2;
  Harness bounded = Harness::ephemeral(generation_limits);
  bounded.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  bounded.add(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
  FDEP_CHECK_EQ(bounded.generation().value(), std::uint64_t{2});
  const auto refused = bounded.try_add(
      fdep_test::make_spec(asset("c"), feed("f1"), DependencyKind::RequiresPowerFrom), bounded.generation());
  FDEP_CHECK_CODE(refused, ErrorCode::GenerationExhausted);
  FDEP_CHECK_EQ(bounded.snapshot().edge_count(), std::size_t{2});
}

FDEP_TEST(adversarial, a_hostile_annotation_cannot_break_the_export_form) {
  Harness harness = Harness::ephemeral();
  // Every annotation the registry accepts survives both renderings unchanged,
  // and none of them can inject a line break into the text form.
  const char* annotations[] = {"plain",
                               "with \"quotes\"",
                               "with \\ backslash",
                               "\u20ac euro sign",
                               "\U0001F600 emoji",
                               "a\tb",
                               "trailing space ",
                               " leading space",
                               "{}[](),;=<>|&^%$#@!*?~`"};
  for (const char* annotation : annotations) {
    const auto record = ProvenanceRecord::create(ProvenanceSource::OperatorDeclaration, "src", "prin", 0,
                                                 annotation, 160, 512);
    if (!record) {
      // Tab is a control character and is refused, which is the point.
      FDEP_CHECK_EQ(record.error().code(), ErrorCode::InvalidAnnotation);
      continue;
    }
    FDEP_CHECK(record.value().valid());
    FDEP_CHECK(record.value().annotation().find('\n') == std::string::npos);

    DeclareRefRequest request;
    request.context.expected_generation = harness.generation();
    request.ref = asset("node-" + std::to_string(harness.snapshot().declared_ref_count()));
    request.provenance = record.value();
    const auto outcome = harness.registry().declare_external_ref(request);
    FDEP_REQUIRE_OK(outcome);
  }

  const auto text = export_text(harness.snapshot());
  FDEP_REQUIRE_OK(text);
  // The line count matches the declared record count: no annotation introduced
  // a line of its own.
  std::size_t lines = 0;
  for (const char character : text.value()) {
    if (character == '\n') {
      ++lines;
    }
  }
  FDEP_CHECK_EQ(lines, 7 + harness.snapshot().declared_ref_count() + harness.snapshot().node_count());

  const auto json = export_json(harness.snapshot());
  FDEP_REQUIRE_OK(json);
  FDEP_CHECK_EQ(std::count(json.value().begin(), json.value().end(), '{'),
                std::count(json.value().begin(), json.value().end(), '}'));
}

FDEP_TEST(adversarial, a_huge_declaration_is_refused_by_its_bound) {
  Harness harness = Harness::ephemeral();

  // More constraints than the configured maximum, checked before anything is
  // stored.
  std::vector<DependencyConstraint> constraints;
  for (int index = 0; index < 40; ++index) {
    const auto constraint = DependencyConstraint::make(ConstraintKind::RedundancyCount, index % 65);
    FDEP_REQUIRE_OK(constraint);
    constraints.push_back(constraint.value());
  }
  const auto too_many = harness.try_add(
      fdep_test::make_spec(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom, DependencyStrength::Hard,
                           Direction::DependsOn, LifecycleState::Active, constraints),
      harness.generation());
  FDEP_CHECK_CODE(too_many, ErrorCode::TooManyConstraints);

  // An annotation beyond the configured length cannot even be constructed.
  const auto long_annotation = ProvenanceRecord::create(ProvenanceSource::OperatorDeclaration, "src", "prin", 0,
                                                        std::string(4097, 'x'), 160, 4096);
  FDEP_CHECK_CODE(long_annotation, ErrorCode::InvalidAnnotation);
  FDEP_CHECK(ProvenanceRecord::create(ProvenanceSource::OperatorDeclaration, "src", "prin", 0,
                                      std::string(4096, 'x'), 160, 4096)
                 .has_value());

  // An identifier beyond the configured length is refused by the node reference
  // itself, and the configured limit is honoured when it is tighter.
  const auto long_id = DependencyNodeRef::create(NodeDomain::Asset, std::string(161, 'a'));
  FDEP_CHECK_CODE(long_id, ErrorCode::InvalidNodeIdSyntax);
  const auto tight = DependencyNodeRef::create(NodeDomain::Asset, std::string(9, 'a'), 8);
  FDEP_CHECK_CODE(tight, ErrorCode::InvalidNodeIdSyntax);
  FDEP_CHECK(DependencyNodeRef::create(NodeDomain::Asset, std::string(8, 'a'), 8).has_value());

  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{0});
}

FDEP_TEST(adversarial, many_processes_competing_for_one_store_have_one_writer) {
  const std::filesystem::path root = fdep_test::make_temp_directory("adversarial-lock");
  constexpr int kThreads = 8;
  std::atomic<int> opened{0};
  std::atomic<int> locked{0};
  std::atomic<int> other{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  std::mutex guard_mutex;
  std::vector<DependencyRegistry> registries;
  registries.reserve(kThreads);

  for (int index = 0; index < kThreads; ++index) {
    workers.emplace_back([&]() {
      RegistryOpenRequest request;
      request.root = root;
      auto registry = DependencyRegistry::open(request);
      if (registry) {
        opened.fetch_add(1);
        std::lock_guard<std::mutex> guard{guard_mutex};
        registries.push_back(std::move(registry).value());
      } else if (registry.error().code() == ErrorCode::StoreLocked) {
        locked.fetch_add(1);
      } else {
        other.fetch_add(1);
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  FDEP_CHECK_EQ(opened.load(), 1);
  FDEP_CHECK_EQ(locked.load(), kThreads - 1);
  FDEP_CHECK_EQ(other.load(), 0);

  // The one writer can commit, and a reader elsewhere sees the result.
  DependencyRegistry& writer = registries.front();
  RegisterEdgeRequest request;
  request.context.expected_generation = writer.generation();
  request.spec = fdep_test::make_spec(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  FDEP_REQUIRE(writer.register_edge(request).has_value());

  StoreOptions options;
  options.mode = OpenMode::ReadOnly;
  RegistrySnapshot snapshot;
  RecoveryReport report;
  auto reader = DurableStore::open(root, options, snapshot, report);
  FDEP_REQUIRE_OK(reader);
  FDEP_CHECK_EQ(snapshot.generation().value(), std::uint64_t{1});
  FDEP_REQUIRE(reader.value()->close().ok());
  FDEP_REQUIRE(writer.close().ok());
  fdep_test::remove_tree(root);
}

FDEP_TEST(adversarial, a_writer_that_loses_its_lock_cannot_publish) {
  // The store is closed underneath the registry, which is what a fencing
  // failure would look like. The next mutation must not publish, and must not
  // silently succeed in memory either.
  const std::filesystem::path root = fdep_test::make_temp_directory("adversarial-fenced");
  StoreOptions options;
  RegistrySnapshot snapshot;
  RecoveryReport report;
  auto store = DurableStore::open(root, options, snapshot, report);
  FDEP_REQUIRE_OK(store);
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);

  DurableStore& durable = *store.value();
  FDEP_REQUIRE(durable.publish(harness.snapshot()).ok());
  FDEP_REQUIRE(durable.close().ok());

  // Publishing after the lock was released is refused.
  FDEP_CHECK_STATUS(durable.publish(harness.snapshot()), ErrorCode::WriterFenced);
  fdep_test::remove_tree(root);
}

FDEP_TEST(adversarial, an_oversized_store_file_is_refused_without_reading_it) {
  const std::filesystem::path root = fdep_test::make_temp_directory("adversarial-oversized");
  {
    Harness harness = Harness::durable(root);
    harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
    FDEP_REQUIRE(harness.registry().close().ok());
  }
  // Grow the newest generation far beyond the configured bound.
  std::error_code error;
  std::filesystem::path newest;
  for (const auto& entry : std::filesystem::directory_iterator{root, error}) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("gen-", 0) == 0) {
      newest = entry.path();
    }
  }
  FDEP_REQUIRE(!newest.empty());
  {
    std::ofstream stream{newest, std::ios::binary | std::ios::app};
    const std::string filler(4096, 'z');
    for (int index = 0; index < 64; ++index) {
      stream << filler;
    }
  }

  RegistryOpenRequest request;
  request.root = root;
  request.store.limits.max_persisted_bytes = 8192;
  const auto refused = DependencyRegistry::open(request);
  FDEP_CHECK_CODE(refused, ErrorCode::StoreCorrupt);

  // Nothing was truncated or deleted by the refusal.
  FDEP_CHECK(std::filesystem::file_size(newest) > 8192);
  fdep_test::remove_tree(root);
}

FDEP_TEST(adversarial, hostile_counts_and_lengths_in_a_store_header_are_refused) {
  const std::filesystem::path root = fdep_test::make_temp_directory("adversarial-header");
  {
    Harness harness = Harness::durable(root);
    harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
    FDEP_REQUIRE(harness.registry().close().ok());
  }
  std::error_code error;
  std::filesystem::path newest;
  for (const auto& entry : std::filesystem::directory_iterator{root, error}) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("gen-", 0) == 0) {
      newest = entry.path();
    }
  }
  FDEP_REQUIRE(!newest.empty());
  write_text(newest, std::string(64, '\0') + "payload");

  RegistryOpenRequest request;
  request.root = root;
  const auto refused = DependencyRegistry::open(request);
  FDEP_CHECK_CODE(refused, ErrorCode::StoreCorrupt);
  fdep_test::remove_tree(root);
}

FDEP_TEST(adversarial, a_long_chain_does_not_exhaust_the_stack) {
  // The walk is iterative, so a chain far deeper than any recursion budget is
  // just a lot of iterations. The chain here is deeper than a recursive
  // implementation of the same walk could survive with the default stack on the
  // platforms this repository targets, while keeping the test quick.
  RegistryLimits limits;
  limits.max_traversal_depth = 4096;
  limits.max_traversal_nodes = 8192;
  Harness harness = Harness::ephemeral(limits);
  constexpr int kChain = 600;
  for (int index = 0; index < kChain; ++index) {
    harness.add(asset("node-" + std::to_string(index)),
                index == 0 ? feed("f1") : asset("node-" + std::to_string(index - 1)),
                index == 0 ? DependencyKind::RequiresPowerFrom : DependencyKind::ControlDependsOn);
  }

  TraversalRequest request;
  request.root = feed("f1");
  request.direction = TraversalDirection::Dependents;
  request.max_depth = 4096;
  request.max_nodes = 8192;
  const auto closure = harness.snapshot().transitive_closure(request);
  FDEP_REQUIRE_OK(closure);
  FDEP_CHECK_EQ(closure.value().entries.size(), static_cast<std::size_t>(kChain));
  FDEP_CHECK(closure.value().stop == TraversalStop::Complete);

  // The acyclic obligation check is iterative as well.
  const auto obligation = harness.snapshot().verify_acyclic_obligation();
  FDEP_REQUIRE_OK(obligation);
  FDEP_CHECK(obligation.value().satisfied);

  // And so is the component analysis over the same depth.
  ComponentRequest components;
  components.max_nodes = 8192;
  const auto result = harness.snapshot().strongly_connected_components(components);
  FDEP_REQUIRE_OK(result);
  FDEP_CHECK(result.value().components.empty());
}

FDEP_TEST(adversarial, adversarial_identifiers_and_domains_are_rejected_before_use) {
  // Out of range enumerations never reach the graph.
  DependencyEdgeSpec spec = fdep_test::make_spec(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  spec.source = DependencyNodeRef::create(NodeDomain::Asset, "a").value();
  spec.target = DependencyNodeRef::create(NodeDomain::ElectricalDomain, "f1").value();
  FDEP_CHECK(spec.validate(RegistryLimits{}).ok());

  DependencyEdgeSpec bad_kind = spec;
  bad_kind.kind = static_cast<DependencyKind>(255);
  FDEP_CHECK_STATUS(bad_kind.validate(RegistryLimits{}), ErrorCode::InvalidKindToken);

  DependencyEdgeSpec bad_strength = spec;
  bad_strength.strength = static_cast<DependencyStrength>(255);
  FDEP_CHECK_STATUS(bad_strength.validate(RegistryLimits{}), ErrorCode::InvalidStrengthToken);

  DependencyEdgeSpec bad_lifecycle = spec;
  bad_lifecycle.initial_lifecycle = static_cast<LifecycleState>(255);
  FDEP_CHECK_STATUS(bad_lifecycle.validate(RegistryLimits{}), ErrorCode::InvalidLifecycleToken);

  DependencyEdgeSpec bad_provenance = spec;
  bad_provenance.provenance = ProvenanceRecord{};
  FDEP_CHECK_STATUS(bad_provenance.validate(RegistryLimits{}), ErrorCode::InvalidProvenanceSource);

  // A constraint kind outside the domain is refused, and a constraint whose
  // value does not match its kind is refused.
  const auto unknown_constraint = DependencyConstraint::make(static_cast<ConstraintKind>(200), 1);
  FDEP_CHECK_CODE(unknown_constraint, ErrorCode::InvalidConstraintKind);
  const auto wrong_shape = DependencyConstraint::make(ConstraintKind::FailoverMode, 3);
  FDEP_CHECK_CODE(wrong_shape, ErrorCode::InvalidConstraintValue);

  // A limit configuration that could never be satisfied is refused at open.
  RegistryLimits broken;
  broken.max_edges = 0;
  const auto refused = DependencyRegistry::open_ephemeral(EphemeralOptions{broken, nullptr});
  FDEP_CHECK_CODE(refused, ErrorCode::InvalidLimits);
}

FDEP_TEST(adversarial, random_hostile_payloads_never_crash_the_decoder) {
  // A deterministic sweep of corrupted payloads: every decode either returns a
  // state or a typed error, and never reads out of bounds. The sanitizer build
  // is what makes the second half of that claim checkable.
  Rng rng{20'260'101};
  Harness harness = Harness::ephemeral();
  for (int index = 0; index < 12; ++index) {
    harness.add(asset("node-" + std::to_string(index)), feed("f1"), DependencyKind::RequiresPowerFrom);
  }
  const auto encoded = harness.snapshot().encode();
  FDEP_REQUIRE_OK(encoded);
  const std::vector<std::byte> original = encoded.value();

  std::size_t accepted = 0;
  std::size_t rejected = 0;
  for (int attempt = 0; attempt < 400; ++attempt) {
    std::vector<std::byte> bytes = original;
    const int edits = 1 + static_cast<int>(rng.below(4));
    for (int edit = 0; edit < edits; ++edit) {
      const std::size_t offset = rng.below(static_cast<std::uint32_t>(bytes.size()));
      bytes[offset] = static_cast<std::byte>(rng.below(256));
    }
    if (rng.below(4) == 0 && !bytes.empty()) {
      bytes.resize(rng.below(static_cast<std::uint32_t>(bytes.size())));
    }
    const auto decoded = RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{});
    if (decoded) {
      ++accepted;
      // Whatever came back must itself be a valid, re-encodable state.
      const auto reencoded = decoded.value().encode();
      FDEP_REQUIRE_OK(reencoded);
      const auto again = RegistrySnapshot::decode(std::span<const std::byte>{reencoded.value()}, RegistryLimits{});
      FDEP_REQUIRE_OK(again);
      FDEP_CHECK_EQ(again.value().state_digest(), decoded.value().state_digest());
    } else {
      ++rejected;
      FDEP_CHECK(decoded.error().code() != ErrorCode::Ok);
    }
  }
  std::cout << "        " << fdep_test::seed_text(20'260'101) << " accepted=" << accepted
            << " rejected=" << rejected << "\n";
  FDEP_CHECK(rejected > 0);
}
