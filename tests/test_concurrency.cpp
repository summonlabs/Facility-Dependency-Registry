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
#include <filesystem>
#include <mutex>
#include <set>
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

namespace {

/// Every invariant a snapshot must satisfy at any moment, whatever else is
/// happening. A reader that can observe a half applied mutation would fail one
/// of these.
void check_snapshot_invariants(const RegistrySnapshot& snapshot) {
  const std::span<const DependencyEdgeRecord> edges = snapshot.edges();
  std::set<std::uint64_t> ids;
  std::set<std::string> keys;
  for (std::size_t index = 0; index < edges.size(); ++index) {
    const auto& record = edges[index];
    if (!ids.insert(record.id().value()).second) {
      fdep_test::fail_now("two edges share an identity");
    }
    if (!keys.insert(record.key().to_text()).second) {
      fdep_test::fail_now("two edges share a key");
    }
    if (index > 0 && record < edges[index - 1]) {
      fdep_test::fail_now("edges are not in canonical order");
    }
    if (!is_assigned(record.id()) || !is_assigned(record.revision())) {
      fdep_test::fail_now("an edge has no identity or revision");
    }
    if (record.id().value() >= snapshot.next_edge_ordinal()) {
      fdep_test::fail_now("an edge identity is not below the next edge ordinal");
    }
  }
  const auto obligation = snapshot.verify_acyclic_obligation();
  if (!obligation || !obligation.value().satisfied) {
    fdep_test::fail_now("a visible state violates the acyclic obligation");
  }
  // The digest is a pure function of the state, so recomputing it from the
  // same state must agree. A torn state would still be self consistent here,
  // which is why the digest membership check in the caller matters too.
  const auto encoded = snapshot.encode();
  if (!encoded) {
    fdep_test::fail_now("a visible state cannot be encoded");
  }
  if (sha256(std::span<const std::byte>{encoded.value()}) != snapshot.state_digest()) {
    fdep_test::fail_now("a visible state does not hash to its own digest");
  }
}

}  // namespace

FDEP_TEST(concurrency, competing_mutations_at_one_generation_have_exactly_one_winner) {
  Harness harness = Harness::ephemeral();
  constexpr int kThreads = 8;
  std::atomic<int> accepted{0};
  std::atomic<int> rejected{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);

  for (int index = 0; index < kThreads; ++index) {
    workers.emplace_back([&harness, &accepted, &rejected, index]() {
      RegisterEdgeRequest request;
      // Every thread believes the graph is empty. Only one can be right.
      request.context.expected_generation = kInitialGeneration;
      request.spec = fdep_test::make_spec(asset("node-" + std::to_string(index)), feed("f1"),
                                          DependencyKind::RequiresPowerFrom);
      const auto outcome = harness.registry().register_edge(request);
      if (outcome) {
        accepted.fetch_add(1);
      } else if (outcome.error().code() == ErrorCode::StaleGeneration) {
        rejected.fetch_add(1);
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  FDEP_CHECK_EQ(accepted.load(), 1);
  FDEP_CHECK_EQ(rejected.load(), kThreads - 1);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{1});
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{1});
  check_snapshot_invariants(harness.snapshot());
}

FDEP_TEST(concurrency, readers_never_observe_a_state_that_was_never_committed) {
  Harness harness = Harness::ephemeral();
  constexpr int kMutations = 120;
  constexpr int kReaders = 4;

  std::mutex digests_mutex;
  std::set<std::string> committed_digests;
  std::set<std::string> seen_before_commit;
  {
    std::lock_guard<std::mutex> guard(digests_mutex);
    committed_digests.insert(harness.snapshot().state_digest().to_hex());
  }

  std::atomic<bool> finished{false};
  std::atomic<int> observations{0};
  std::atomic<bool> reader_failed{false};
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (int index = 0; index < kReaders; ++index) {
    readers.emplace_back([&]() {
      while (!finished.load(std::memory_order_acquire)) {
        const RegistrySnapshot snapshot = harness.snapshot();
        try {
          check_snapshot_invariants(snapshot);
        } catch (const std::exception&) {
          reader_failed.store(true);
          return;
        }
        const std::string digest = snapshot.state_digest().to_hex();
        std::lock_guard<std::mutex> guard(digests_mutex);
        if (committed_digests.find(digest) == committed_digests.end()) {
          // A state published between its commit and this thread's bookkeeping
          // is expected; it is checked once the writer has stopped. A state
          // that no commit ever produced stays unknown and fails the check
          // below.
          seen_before_commit.insert(digest);
        }
        observations.fetch_add(1);
      }
    });
  }

  for (int index = 0; index < kMutations; ++index) {
    {
      std::lock_guard<std::mutex> guard(digests_mutex);
      committed_digests.insert(harness.snapshot().state_digest().to_hex());
    }
    RegisterEdgeRequest request;
    request.context.expected_generation = harness.generation();
    request.spec = fdep_test::make_spec(asset("node-" + std::to_string(index)), feed("f1"),
                                        DependencyKind::RequiresPowerFrom);
    const auto outcome = harness.registry().register_edge(request);
    FDEP_REQUIRE(outcome.has_value());
    {
      std::lock_guard<std::mutex> guard(digests_mutex);
      committed_digests.insert(harness.snapshot().state_digest().to_hex());
    }
  }
  finished.store(true, std::memory_order_release);
  for (auto& reader : readers) {
    reader.join();
  }

  FDEP_CHECK(!reader_failed.load());
  FDEP_CHECK(observations.load() > 0);
  FDEP_CHECK_EQ(harness.generation().value(), static_cast<std::uint64_t>(kMutations));
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), static_cast<std::size_t>(kMutations));
  {
    std::lock_guard<std::mutex> guard(digests_mutex);
    FDEP_CHECK_EQ(committed_digests.size(), static_cast<std::size_t>(kMutations) + 1);
    // Every state a reader ever saw is a state that was committed. A state
    // assembled from two different generations would hash to something no
    // commit produced, and would still be unknown here.
    for (const auto& digest : seen_before_commit) {
      FDEP_CHECK(committed_digests.find(digest) != committed_digests.end());
    }
  }
}

FDEP_TEST(concurrency, a_snapshot_held_by_a_reader_never_changes) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("base-0"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const RegistrySnapshot held = harness.snapshot();
  const ContentDigest digest = held.state_digest();
  const std::size_t count = held.edge_count();

  std::atomic<bool> finished{false};
  std::atomic<bool> failed{false};
  std::thread reader{[&]() {
    while (!finished.load(std::memory_order_acquire)) {
      if (held.state_digest() != digest || held.edge_count() != count) {
        failed.store(true);
        return;
      }
      const auto dependencies = held.direct_dependencies(asset("base-0"));
      if (!dependencies || dependencies.value().size() != 1) {
        failed.store(true);
        return;
      }
    }
  }};

  for (int index = 0; index < 60; ++index) {
    harness.add(asset("later-" + std::to_string(index)), feed("f1"), DependencyKind::RequiresPowerFrom);
  }
  finished.store(true, std::memory_order_release);
  reader.join();

  FDEP_CHECK(!failed.load());
  FDEP_CHECK_EQ(held.edge_count(), count);
  FDEP_CHECK_EQ(held.state_digest(), digest);
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{61});
}

FDEP_TEST(concurrency, concurrent_mutations_and_readers_on_a_durable_store) {
  const std::filesystem::path root = fdep_test::make_temp_directory("concurrency-durable");
  constexpr int kMutations = 24;
  std::atomic<bool> finished{false};
  std::atomic<bool> reader_failed{false};

  Harness harness = Harness::durable(root);
  std::thread reader{[&]() {
    while (!finished.load(std::memory_order_acquire)) {
      try {
        check_snapshot_invariants(harness.snapshot());
      } catch (const std::exception&) {
        reader_failed.store(true);
        return;
      }
    }
  }};

  for (int index = 0; index < kMutations; ++index) {
    RegisterEdgeRequest request;
    request.context.expected_generation = harness.generation();
    request.spec = fdep_test::make_spec(asset("node-" + std::to_string(index)), feed("f1"),
                                        DependencyKind::RequiresPowerFrom);
    const auto outcome = harness.registry().register_edge(request);
    FDEP_REQUIRE(outcome.has_value());
  }
  finished.store(true, std::memory_order_release);
  reader.join();
  FDEP_CHECK(!reader_failed.load());

  const RegistrySnapshot before_close = harness.snapshot();
  const ContentDigest digest = before_close.state_digest();
  FDEP_REQUIRE(harness.registry().close().ok());

  Harness reopened = Harness::durable(root);
  FDEP_CHECK_EQ(reopened.generation().value(), static_cast<std::uint64_t>(kMutations));
  FDEP_CHECK_EQ(reopened.snapshot().state_digest(), digest);
  FDEP_REQUIRE(reopened.registry().close().ok());
  fdep_test::remove_tree(root);
}

FDEP_TEST(concurrency, close_waits_for_an_in_flight_mutation_and_refuses_the_rest) {
  const std::filesystem::path root = fdep_test::make_temp_directory("concurrency-close");
  Harness harness = Harness::durable(root);

  std::atomic<int> committed{0};
  std::atomic<int> refused{0};
  std::atomic<bool> start{false};
  std::vector<std::thread> workers;
  constexpr int kThreads = 4;
  for (int index = 0; index < kThreads; ++index) {
    workers.emplace_back([&harness, &committed, &refused, &start, index]() {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (int attempt = 0; attempt < 20; ++attempt) {
        RegisterEdgeRequest request;
        request.context.expected_generation = harness.generation();
        request.spec = fdep_test::make_spec(
            asset("node-" + std::to_string(index) + "-" + std::to_string(attempt)), feed("f1"),
            DependencyKind::RequiresPowerFrom);
        const auto outcome = harness.registry().register_edge(request);
        if (outcome) {
          committed.fetch_add(1);
        } else if (outcome.error().code() == ErrorCode::RegistryClosed) {
          refused.fetch_add(1);
        } else if (outcome.error().code() != ErrorCode::StaleGeneration) {
          refused.fetch_add(1);
        }
      }
    });
  }

  start.store(true, std::memory_order_release);
  std::this_thread::yield();
  const Status closed = harness.registry().close();
  for (auto& worker : workers) {
    worker.join();
  }

  FDEP_CHECK(closed.ok());
  FDEP_CHECK(harness.registry().closed());
  // Every mutation either committed before the close or was refused by it;
  // nothing was lost or half applied.
  FDEP_CHECK(committed.load() + refused.load() > 0);
  const RegistrySnapshot final_snapshot = harness.snapshot();
  check_snapshot_invariants(final_snapshot);
  FDEP_CHECK_EQ(final_snapshot.edge_count(), static_cast<std::size_t>(committed.load()));
  FDEP_CHECK_EQ(harness.generation().value(), static_cast<std::uint64_t>(committed.load()));

  // What survived the close is exactly what is on disk.
  Harness reopened = Harness::durable(root);
  FDEP_CHECK_EQ(reopened.snapshot().edge_count(), final_snapshot.edge_count());
  FDEP_CHECK_EQ(reopened.snapshot().state_digest(), final_snapshot.state_digest());
  FDEP_REQUIRE(reopened.registry().close().ok());
  fdep_test::remove_tree(root);
}

FDEP_TEST(concurrency, many_threads_reading_a_large_graph_agree) {
  Harness harness = Harness::ephemeral();
  constexpr int kEdges = 200;
  for (int index = 0; index < kEdges; ++index) {
    harness.add(asset("node-" + std::to_string(index)), feed("f1"), DependencyKind::RequiresPowerFrom);
  }
  const RegistrySnapshot snapshot = harness.snapshot();

  TraversalRequest request;
  request.root = feed("f1");
  request.direction = TraversalDirection::Dependents;
  request.max_depth = 8;
  request.max_nodes = static_cast<std::uint32_t>(kEdges) * 2;

  const auto reference = snapshot.transitive_closure(request);
  FDEP_REQUIRE_OK(reference);
  FDEP_CHECK_EQ(reference.value().entries.size(), static_cast<std::size_t>(kEdges));

  std::atomic<bool> mismatched{false};
  std::vector<std::thread> workers;
  constexpr int kThreads = 6;
  for (int index = 0; index < kThreads; ++index) {
    workers.emplace_back([&]() {
      for (int attempt = 0; attempt < 8; ++attempt) {
        const auto result = snapshot.transitive_closure(request);
        if (!result || result.value().entries.size() != reference.value().entries.size()) {
          mismatched.store(true);
          return;
        }
        for (std::size_t position = 0; position < result.value().entries.size(); ++position) {
          if (!(result.value().entries[position].node == reference.value().entries[position].node)) {
            mismatched.store(true);
            return;
          }
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  FDEP_CHECK(!mismatched.load());
}
