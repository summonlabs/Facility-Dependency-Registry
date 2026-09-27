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

#include "facility_dependency_registry/facility_dependency_registry.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

using namespace facility_dependency_registry;
using fdep_test::asset;
using fdep_test::feed;
using fdep_test::Harness;
using fdep_test::make_spec;
using fdep_test::provenance;
using fdep_test::rack;

FDEP_TEST(duplicate_semantics, identical_declaration_is_an_idempotent_no_op) {
  Harness harness = Harness::ephemeral();
  const auto spec = make_spec(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const DependencyEdgeId first = harness.add(spec);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{1});
  const DependencyGeneration after_first = harness.generation();

  const auto retry = harness.try_add(spec, harness.generation());
  FDEP_REQUIRE_OK(retry);
  FDEP_CHECK(retry.value().already_present);
  FDEP_CHECK(!retry.value().generation_advanced);
  FDEP_CHECK_EQ(retry.value().id.value(), first.value());
  FDEP_CHECK_EQ(retry.value().revision.value(), std::uint64_t{1});
  // The retry published nothing: the generation is unchanged, so a durable
  // store did not write a generation either.
  FDEP_CHECK_EQ(harness.generation().value(), after_first.value());
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{1});
  FDEP_CHECK_EQ(harness.snapshot().next_edge_ordinal(), std::uint64_t{2});
}

FDEP_TEST(duplicate_semantics, identical_declaration_with_reordered_constraints_is_the_same_declaration) {
  Harness harness = Harness::ephemeral();
  const auto latency = DependencyConstraint::make(ConstraintKind::MaxLatencyMicros, 500);
  FDEP_REQUIRE_OK(latency);
  const auto redundancy = DependencyConstraint::make(ConstraintKind::RedundancyClass, "2N");
  FDEP_REQUIRE_OK(redundancy);

  harness.add(make_spec(asset("a"), fdep_test::loop("l1"), DependencyKind::CooledBy, DependencyStrength::Hard,
                        Direction::DependsOn, LifecycleState::Active, {latency.value(), redundancy.value()}));

  const auto reordered = harness.try_add(
      make_spec(asset("a"), fdep_test::loop("l1"), DependencyKind::CooledBy, DependencyStrength::Hard,
                Direction::DependsOn, LifecycleState::Active, {redundancy.value(), latency.value()}),
      harness.generation());
  FDEP_REQUIRE_OK(reordered);
  FDEP_CHECK(reordered.value().already_present);
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{1});

  // The stored constraints are in canonical order, so the rendering is stable.
  const auto* record = harness.snapshot().find_edge(reordered.value().id);
  FDEP_REQUIRE(record != nullptr);
  FDEP_CHECK_EQ(record->constraints().size(), std::size_t{2});
  FDEP_CHECK(record->constraints()[0].kind() == ConstraintKind::MaxLatencyMicros);
  FDEP_CHECK(record->constraints()[1].kind() == ConstraintKind::RedundancyClass);
}

FDEP_TEST(duplicate_semantics, conflicting_declaration_is_rejected_and_names_the_holder) {
  Harness harness = Harness::ephemeral();
  const DependencyEdgeId first = harness.add(make_spec(asset("a"), feed("f1"),
                                                       DependencyKind::RequiresPowerFrom, DependencyStrength::Hard));
  const DependencyGeneration after_first = harness.generation();

  const auto conflict = harness.try_add(
      make_spec(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom, DependencyStrength::Soft),
      harness.generation());
  FDEP_CHECK_CODE(conflict, ErrorCode::DuplicateEdge);
  FDEP_CHECK(conflict.error().detail().find("1") != std::string::npos);
  FDEP_CHECK_EQ(harness.generation().value(), after_first.value());
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{1});
  FDEP_CHECK_EQ(harness.snapshot().find_edge(first)->strength(), DependencyStrength::Hard);

  // A different provenance is a different payload too: the registry never
  // silently adopts the newer declaration.
  const auto different_provenance = harness.try_add(
      make_spec(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom, DependencyStrength::Hard,
                Direction::DependsOn, LifecycleState::Active, {},
                provenance("other-source", "other-principal", 5)),
      harness.generation());
  FDEP_CHECK_CODE(different_provenance, ErrorCode::DuplicateEdge);
  FDEP_CHECK_EQ(harness.snapshot().find_edge(first)->provenance().principal(), std::string{"test-principal"});
}

FDEP_TEST(duplicate_semantics, different_kind_or_endpoints_is_a_different_edge) {
  Harness harness = Harness::ephemeral();
  const DependencyEdgeId power = harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const DependencyEdgeId other_feed = harness.add(asset("a"), feed("f2"), DependencyKind::RequiresPowerFrom);
  const DependencyEdgeId other_asset = harness.add(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const DependencyEdgeId housing = harness.add(asset("a"), rack("r1"), DependencyKind::HousedIn);
  const DependencyEdgeId served = harness.add(asset("a"), fdep_test::service("s1"), DependencyKind::ServedBy);

  FDEP_CHECK(power != other_feed);
  FDEP_CHECK(power != other_asset);
  FDEP_CHECK(power != housing);
  FDEP_CHECK(power != served);
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{5});
}

FDEP_TEST(duplicate_semantics, retired_edges_reserve_their_key_until_removed) {
  Harness harness = Harness::ephemeral();
  const DependencyEdgeId id = harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  FDEP_REQUIRE(harness.transition(id, kInitialRevision, LifecycleState::Retired).ok());
  FDEP_CHECK_EQ(harness.snapshot().find_edge(id)->lifecycle(), LifecycleState::Retired);

  const auto blocked = harness.try_add(make_spec(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom),
                                       harness.generation());
  FDEP_CHECK_CODE(blocked, ErrorCode::DuplicateEdge);
  FDEP_CHECK(blocked.error().detail().find("retired") != std::string::npos);

  // Removing the retired edge releases the key, and the re-declaration gets a
  // fresh identity rather than resurrecting the old one.
  FDEP_REQUIRE(harness.remove(id, EdgeRevision::from_value(2)).ok());
  const DependencyEdgeId again = harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  FDEP_CHECK(again != id);
  FDEP_CHECK(again.value() > id.value());
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{1});
}

FDEP_TEST(duplicate_semantics, declared_reference_duplicate_rules) {
  Harness harness = Harness::ephemeral();
  const auto record = provenance("change-1", "alice", 1, "observed");
  harness.declare(asset("a"), record);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{1});

  DeclareRefRequest retry;
  retry.context.expected_generation = harness.generation();
  retry.ref = asset("a");
  retry.provenance = record;
  const auto repeated = harness.registry().declare_external_ref(retry);
  FDEP_REQUIRE_OK(repeated);
  FDEP_CHECK(repeated.value().already_declared);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{1});

  DeclareRefRequest conflict;
  conflict.context.expected_generation = harness.generation();
  conflict.ref = asset("a");
  conflict.provenance = provenance("change-1", "alice", 2, "observed again");
  FDEP_CHECK_CODE(harness.registry().declare_external_ref(conflict), ErrorCode::DuplicateDeclaration);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{1});
}
