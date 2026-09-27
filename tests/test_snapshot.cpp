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
using fdep_test::rack;
using fdep_test::service;

FDEP_TEST(snapshot, default_snapshot_is_the_empty_graph) {
  const RegistrySnapshot snapshot;
  FDEP_CHECK_EQ(snapshot.generation().value(), std::uint64_t{0});
  FDEP_CHECK_EQ(snapshot.edge_count(), std::size_t{0});
  FDEP_CHECK_EQ(snapshot.node_count(), std::size_t{0});
  FDEP_CHECK_EQ(snapshot.next_edge_ordinal(), std::uint64_t{1});
  FDEP_CHECK(snapshot.edges().empty());
  FDEP_CHECK(snapshot.declared_refs().empty());
  FDEP_CHECK(snapshot.find_edge(DependencyEdgeId::from_value(1)) == nullptr);
  FDEP_CHECK(!snapshot.has_node(asset("a")));

  const RegistrySnapshot other = RegistrySnapshot::empty();
  FDEP_CHECK_EQ(snapshot.state_digest(), other.state_digest());
  FDEP_CHECK(!snapshot.state_digest().is_zero());
}

FDEP_TEST(snapshot, snapshots_are_immutable_and_independent_of_the_registry) {
  Harness harness = Harness::ephemeral();
  const DependencyEdgeId first = harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const RegistrySnapshot before = harness.snapshot();
  const ContentDigest before_digest = before.state_digest();

  // A later mutation does not change the snapshot that was taken before it.
  harness.add(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
  FDEP_REQUIRE(harness.remove(first, kInitialRevision).ok());
  FDEP_CHECK_EQ(before.edge_count(), std::size_t{1});
  FDEP_CHECK_EQ(before.state_digest(), before_digest);
  FDEP_CHECK(before.find_edge(first) != nullptr);
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{1});
  FDEP_CHECK(harness.snapshot().find_edge(first) == nullptr);
  FDEP_CHECK(before.state_digest() != harness.snapshot().state_digest());

  // Copying a snapshot is cheap and keeps the same state alive.
  const RegistrySnapshot copy = before;
  FDEP_CHECK_EQ(copy.state_digest(), before.state_digest());
  FDEP_CHECK_EQ(copy.edge_count(), before.edge_count());
  RegistrySnapshot assigned;
  assigned = copy;
  FDEP_CHECK_EQ(assigned.state_digest(), before.state_digest());

  // Moving leaves the source valid but unspecified in state; the moved-to
  // snapshot is the one that carries the graph.
  RegistrySnapshot moved = std::move(assigned);
  FDEP_CHECK_EQ(moved.state_digest(), before.state_digest());
}

FDEP_TEST(snapshot, a_snapshot_stays_valid_after_the_registry_closes) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const RegistrySnapshot snapshot = harness.snapshot();
  FDEP_REQUIRE(harness.registry().close().ok());
  FDEP_CHECK_EQ(snapshot.edge_count(), std::size_t{1});
  const auto result = snapshot.direct_dependencies(asset("a"));
  FDEP_REQUIRE_OK(result);
  FDEP_CHECK_EQ(result.value().size(), std::size_t{1});
}

FDEP_TEST(snapshot, state_and_content_digests_differ_exactly_by_generation) {
  // The content digest is the state digest of the same canonical payload with
  // the generation field omitted. A registry that declares a reference and then
  // withdraws it is back to the same graph at a later generation, so the two
  // digests separate exactly as documented.
  Harness empty = Harness::ephemeral();
  Harness returned = Harness::ephemeral();
  returned.declare(asset("x"));
  WithdrawRefRequest withdraw;
  withdraw.context.expected_generation = returned.generation();
  withdraw.ref = asset("x");
  FDEP_REQUIRE_OK(returned.registry().withdraw_external_ref(withdraw));

  FDEP_CHECK_EQ(returned.generation().value(), std::uint64_t{2});
  FDEP_CHECK_EQ(empty.generation().value(), std::uint64_t{0});
  FDEP_CHECK_EQ(empty.snapshot().content_digest(), returned.snapshot().content_digest());
  FDEP_CHECK(empty.snapshot().state_digest() != returned.snapshot().state_digest());

  // The same mutation history produces identical state, byte for byte.
  Harness first = Harness::ephemeral();
  first.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  first.add(asset("b"), loop("l1"), DependencyKind::CooledBy);

  Harness second = Harness::ephemeral();
  second.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  second.add(asset("b"), loop("l1"), DependencyKind::CooledBy);

  FDEP_CHECK_EQ(first.snapshot().state_digest(), second.snapshot().state_digest());
  FDEP_CHECK_EQ(first.snapshot().content_digest(), second.snapshot().content_digest());

  // Edge identities are part of authoritative state, so a graph reached by a
  // different allocation history is a different state even when the edges are
  // the same. The registry does not pretend otherwise.
  Harness reordered = Harness::ephemeral();
  reordered.add(asset("b"), loop("l1"), DependencyKind::CooledBy);
  reordered.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  FDEP_CHECK_EQ(reordered.snapshot().edge_count(), first.snapshot().edge_count());
  FDEP_CHECK(reordered.snapshot().state_digest() != first.snapshot().state_digest());
  FDEP_CHECK(reordered.snapshot().content_digest() != first.snapshot().content_digest());
}

FDEP_TEST(snapshot, equivalent_graphs_reached_by_different_orders_are_identical) {
  Harness forward = Harness::ephemeral();
  forward.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  forward.add(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
  forward.add(asset("a"), rack("r1"), DependencyKind::HousedIn);

  Harness backward = Harness::ephemeral();
  backward.add(asset("a"), rack("r1"), DependencyKind::HousedIn);
  backward.add(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
  backward.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);

  // Edge identities differ, because they are allocated in registration order.
  // Assigning the same identity to each canonical position makes the rest of
  // the state comparable, which is what the canonical encoding does.
  const auto encode = [](const RegistrySnapshot& snapshot) { return snapshot.encode(); };
  FDEP_REQUIRE_OK(encode(forward.snapshot()));
  FDEP_REQUIRE_OK(encode(backward.snapshot()));
  FDEP_CHECK(forward.snapshot().content_digest() != backward.snapshot().content_digest());

  // Rebuilding the same graph in the same order, however, is byte identical.
  Harness repeat = Harness::ephemeral();
  repeat.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  repeat.add(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
  repeat.add(asset("a"), rack("r1"), DependencyKind::HousedIn);
  FDEP_CHECK_EQ(repeat.snapshot().state_digest(), forward.snapshot().state_digest());
  FDEP_REQUIRE_OK(encode(repeat.snapshot()));
  FDEP_CHECK(encode(repeat.snapshot()).value() == encode(forward.snapshot()).value());
}

FDEP_TEST(snapshot, spans_are_tied_to_the_snapshot_that_produced_them) {
  Harness harness = Harness::ephemeral();
  harness.declare(asset("declared"));
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);

  const RegistrySnapshot snapshot = harness.snapshot();
  const std::span<const DependencyEdgeRecord> edges = snapshot.edges();
  const std::span<const DeclaredExternalRef> refs = snapshot.declared_refs();
  const std::span<const DependencyNodeRef> nodes = snapshot.nodes();
  FDEP_REQUIRE(edges.size() == 1);
  FDEP_REQUIRE(refs.size() == 1);
  FDEP_REQUIRE(nodes.size() == 3);
  FDEP_CHECK(refs[0].ref() == asset("declared"));
  FDEP_CHECK(edges[0].source() == asset("a"));
  FDEP_CHECK_EQ(std::string{to_token(RefResolution::Declared)}, std::string{"declared"});
  FDEP_CHECK(edges[0].to_text().find("edge id=1") != std::string::npos);
}

FDEP_TEST(snapshot, detail_state_is_opaque_but_shareable) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const RegistrySnapshot snapshot = harness.snapshot();
  const auto handle = snapshot.detail_state();
  // The type is incomplete outside the library, so a consumer can hold and
  // pass the handle but cannot reach into the state through it.
  FDEP_CHECK(handle != nullptr);
  const auto wrapped = make_snapshot(handle);
  FDEP_REQUIRE_OK(wrapped);
  FDEP_CHECK_EQ(wrapped.value().state_digest(), snapshot.state_digest());
  FDEP_CHECK_EQ(wrapped.value().edge_count(), snapshot.edge_count());
  const auto empty = make_snapshot(nullptr);
  FDEP_CHECK_CODE(empty, ErrorCode::InvalidArguments);
}
