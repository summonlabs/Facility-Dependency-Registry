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
using fdep_test::asi;
using fdep_test::dfi;
using fdep_test::feed;
using fdep_test::Harness;
using fdep_test::loop;
using fdep_test::make_spec;
using fdep_test::rack;
using fdep_test::service;
using fdep_test::provenance;

FDEP_TEST(registry_mutation, empty_registry_is_generation_zero) {
  Harness harness = Harness::ephemeral();
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{0});
  FDEP_CHECK(harness.registry().durable() == false);
  FDEP_CHECK(harness.registry().closed() == false);
  const RegistrySnapshot snapshot = harness.snapshot();
  FDEP_CHECK_EQ(snapshot.generation().value(), std::uint64_t{0});
  FDEP_CHECK_EQ(snapshot.edge_count(), std::size_t{0});
  FDEP_CHECK_EQ(snapshot.declared_ref_count(), std::size_t{0});
  FDEP_CHECK_EQ(snapshot.node_count(), std::size_t{0});
  FDEP_CHECK_EQ(snapshot.next_edge_ordinal(), std::uint64_t{1});
  FDEP_CHECK(snapshot.edges().empty());
}

FDEP_TEST(registry_mutation, every_committed_mutation_advances_exactly_one_generation) {
  Harness harness = Harness::ephemeral();
  const auto first = harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{1});
  FDEP_CHECK_EQ(first.value(), std::uint64_t{1});

  const auto second = harness.add(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{2});
  FDEP_CHECK_EQ(second.value(), std::uint64_t{2});

  harness.declare(asset("a"));
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{3});

  const RegistrySnapshot snapshot = harness.snapshot();
  FDEP_CHECK_EQ(snapshot.edge_count(), std::size_t{2});
  FDEP_CHECK_EQ(snapshot.declared_ref_count(), std::size_t{1});
  FDEP_CHECK_EQ(snapshot.next_edge_ordinal(), std::uint64_t{3});

  // Ordinals are never reused: removing the newest edge and adding another
  // gives the new edge a fresh identity.
  FDEP_REQUIRE(harness.remove(second, kInitialRevision).ok());
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{4});
  const auto third = harness.add(asset("c"), feed("f1"), DependencyKind::RequiresPowerFrom);
  FDEP_CHECK_EQ(third.value(), std::uint64_t{3});
  FDEP_CHECK_EQ(harness.snapshot().next_edge_ordinal(), std::uint64_t{4});
}

FDEP_TEST(registry_mutation, registration_records_the_full_record) {
  Harness harness = Harness::ephemeral();
  auto constraint = DependencyConstraint::make(ConstraintKind::RedundancyClass, "N+1");
  FDEP_REQUIRE_OK(constraint);
  const auto spec = make_spec(asset("node-1"), feed("feed-a"), DependencyKind::RequiresPowerFrom,
                              DependencyStrength::Hard, Direction::DependsOn, LifecycleState::Active,
                              {constraint.value()}, provenance("change-1", "operator-alice", 17, "declared"));
  const DependencyEdgeId id = harness.add(spec);

  const auto* record = harness.snapshot().find_edge(id);
  FDEP_REQUIRE(record != nullptr);
  FDEP_CHECK_EQ(record->id().value(), id.value());
  FDEP_CHECK_EQ(record->revision().value(), std::uint64_t{1});
  FDEP_CHECK(record->kind() == DependencyKind::RequiresPowerFrom);
  FDEP_CHECK(record->strength() == DependencyStrength::Hard);
  FDEP_CHECK(record->direction() == Direction::DependsOn);
  FDEP_CHECK(record->lifecycle() == LifecycleState::Active);
  FDEP_CHECK(record->source() == asset("node-1"));
  FDEP_CHECK(record->target() == feed("feed-a"));
  FDEP_CHECK_EQ(record->constraints().size(), std::size_t{1});
  FDEP_CHECK_EQ(record->registered_generation().value(), std::uint64_t{1});
  FDEP_CHECK_EQ(record->last_modified_generation().value(), std::uint64_t{1});
  FDEP_CHECK_EQ(record->provenance().principal(), std::string{"operator-alice"});
  FDEP_CHECK(record->is_in_force());
  FDEP_CHECK(record->requires_acyclic());

  const auto* by_key = harness.snapshot().find_edge(EdgeKey{asset("node-1"), feed("feed-a"),
                                                            DependencyKind::RequiresPowerFrom});
  FDEP_REQUIRE(by_key != nullptr);
  FDEP_CHECK(*by_key == *record);
}

FDEP_TEST(registry_mutation, spec_validation_rejects_before_any_change) {
  Harness harness = Harness::ephemeral();

  // Self dependency.
  FDEP_CHECK_CODE(harness.try_add(make_spec(asset("a"), asset("a"), DependencyKind::HousedIn),
                                  harness.generation()),
                  ErrorCode::SelfDependencyProhibited);

  // Wrong endpoint domain for the kind.
  FDEP_CHECK_CODE(harness.try_add(make_spec(asset("a"), rack("r1"), DependencyKind::RequiresPowerFrom),
                                  harness.generation()),
                  ErrorCode::EndpointDomainNotAllowed);
  FDEP_CHECK_CODE(harness.try_add(make_spec(asset("a"), feed("f1"), DependencyKind::CooledBy),
                                  harness.generation()),
                  ErrorCode::EndpointDomainNotAllowed);
  FDEP_CHECK_CODE(harness.try_add(make_spec(asset("a"), rack("r1"), DependencyKind::ServedBy),
                                  harness.generation()),
                  ErrorCode::EndpointDomainNotAllowed);
  FDEP_CHECK_CODE(harness.try_add(make_spec(rack("r1"), rack("r2"), DependencyKind::HousedIn),
                                  harness.generation()),
                  ErrorCode::EndpointDomainNotAllowed);
  FDEP_CHECK_CODE(harness.try_add(make_spec(asset("a"), rack("r1"), DependencyKind::ComposedDomainDependsOn),
                                  harness.generation()),
                  ErrorCode::EndpointDomainNotAllowed);

  // Invalid node reference.
  DependencyEdgeSpec invalid_ref = make_spec(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  invalid_ref.source = DependencyNodeRef{};
  FDEP_CHECK_CODE(harness.try_add(invalid_ref, harness.generation()), ErrorCode::InvalidNodeReference);

  // Invalid provenance.
  DependencyEdgeSpec invalid_provenance = make_spec(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  invalid_provenance.provenance = ProvenanceRecord{};
  FDEP_CHECK_CODE(harness.try_add(invalid_provenance, harness.generation()),
                  ErrorCode::InvalidProvenanceSource);

  // Initial lifecycle must be proposed or active.
  FDEP_CHECK_CODE(harness.try_add(make_spec(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom,
                                            DependencyStrength::Hard, Direction::DependsOn,
                                            LifecycleState::Suspended),
                                  harness.generation()),
                  ErrorCode::InvalidArguments);
  FDEP_CHECK_CODE(harness.try_add(make_spec(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom,
                                            DependencyStrength::Hard, Direction::DependsOn,
                                            LifecycleState::Retired),
                                  harness.generation()),
                  ErrorCode::InvalidArguments);
  FDEP_CHECK(harness.try_add(make_spec(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom,
                                       DependencyStrength::Hard, Direction::DependsOn, LifecycleState::Proposed),
                             harness.generation())
                 .has_value());

  // Constraints that do not belong to the kind.
  const auto latency = DependencyConstraint::make(ConstraintKind::MaxLatencyMicros, 100);
  FDEP_REQUIRE_OK(latency);
  FDEP_CHECK_CODE(harness.try_add(make_spec(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom,
                                            DependencyStrength::Hard, Direction::DependsOn,
                                            LifecycleState::Active, {latency.value()}),
                                  harness.generation()),
                  ErrorCode::ConstraintNotAllowedForKind);
  FDEP_CHECK_CODE(harness.try_add(make_spec(asset("c"), rack("r1"), DependencyKind::HousedIn,
                                            DependencyStrength::Hard, Direction::DependsOn,
                                            LifecycleState::Active, {latency.value()}),
                                  harness.generation()),
                  ErrorCode::ConstraintNotAllowedForKind);

  // Duplicate constraint kinds.
  const auto redundancy = DependencyConstraint::make(ConstraintKind::RedundancyClass, "N");
  FDEP_REQUIRE_OK(redundancy);
  FDEP_CHECK_CODE(harness.try_add(make_spec(asset("d"), feed("f1"), DependencyKind::RequiresPowerFrom,
                                            DependencyStrength::Hard, Direction::DependsOn, LifecycleState::Active,
                                            {redundancy.value(), redundancy.value()}),
                                  harness.generation()),
                  ErrorCode::DuplicateConstraintKind);

  // Direction that the kind does not allow.
  FDEP_CHECK_CODE(harness.try_add(make_spec(asset("e"), feed("f2"), DependencyKind::RequiresPowerFrom,
                                            DependencyStrength::Hard, Direction::Mutual,
                                            LifecycleState::Active),
                                  harness.generation()),
                  ErrorCode::DirectionNotAllowedForKind);

  // Out of domain enumerations.
  DependencyEdgeSpec bad_kind = make_spec(asset("f"), feed("f1"), DependencyKind::RequiresPowerFrom);
  bad_kind.kind = static_cast<DependencyKind>(0);
  FDEP_CHECK_CODE(harness.try_add(bad_kind, harness.generation()), ErrorCode::InvalidKindToken);
  DependencyEdgeSpec bad_strength = make_spec(asset("g"), feed("f1"), DependencyKind::RequiresPowerFrom);
  bad_strength.strength = static_cast<DependencyStrength>(0);
  FDEP_CHECK_CODE(harness.try_add(bad_strength, harness.generation()), ErrorCode::InvalidStrengthToken);
  DependencyEdgeSpec bad_direction = make_spec(asset("h"), feed("f1"), DependencyKind::RequiresPowerFrom);
  bad_direction.direction = static_cast<Direction>(0);
  FDEP_CHECK_CODE(harness.try_add(bad_direction, harness.generation()), ErrorCode::InvalidDirectionToken);
  DependencyEdgeSpec bad_lifecycle = make_spec(asset("i"), feed("f1"), DependencyKind::RequiresPowerFrom);
  bad_lifecycle.initial_lifecycle = static_cast<LifecycleState>(0);
  FDEP_CHECK_CODE(harness.try_add(bad_lifecycle, harness.generation()), ErrorCode::InvalidLifecycleToken);

  // Nothing above changed the graph.
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{1});
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{1});
}

FDEP_TEST(registry_mutation, mutual_edges_are_stored_once_in_canonical_order) {
  Harness harness = Harness::ephemeral();
  // A mutual control dependency between two composed domains. Declaring it from
  // either side must produce the same edge.
  const DependencyEdgeId forward = harness.add(asi("a"), dfi("b"), DependencyKind::ControlDependsOn,
                                               DependencyStrength::Soft, Direction::Mutual);
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{1});

  const auto reversed = harness.try_add(make_spec(dfi("b"), asi("a"), DependencyKind::ControlDependsOn,
                                                  DependencyStrength::Soft, Direction::Mutual),
                                        harness.generation());
  FDEP_REQUIRE_OK(reversed);
  FDEP_CHECK(reversed.value().already_present);
  FDEP_CHECK_EQ(reversed.value().id.value(), forward.value());
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{1});

  const auto* record = harness.snapshot().find_edge(forward);
  FDEP_REQUIRE(record != nullptr);
  // asi < dfi in canonical order, so the stored source is the asi domain even
  // though the first declaration named it second.
  FDEP_CHECK(record->source() == asi("a"));
  FDEP_CHECK(record->target() == dfi("b"));

  // Both endpoints see each other as a dependency and as a dependent.
  const auto dependencies = harness.snapshot().direct_dependencies(dfi("b"));
  FDEP_REQUIRE_OK(dependencies);
  FDEP_CHECK_EQ(dependencies.value().size(), std::size_t{1});
  const auto dependents = harness.snapshot().direct_dependents(asi("a"));
  FDEP_REQUIRE_OK(dependents);
  FDEP_CHECK_EQ(dependents.value().size(), std::size_t{1});

  // A mutual edge whose endpoint domains are not both composed domains is
  // refused.
  FDEP_CHECK_CODE(harness.try_add(make_spec(asi("a"), rack("r1"), DependencyKind::ComposedDomainDependsOn,
                                            DependencyStrength::Soft, Direction::Mutual),
                                  harness.generation()),
                  ErrorCode::EndpointDomainNotAllowed);
}
