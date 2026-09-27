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
using fdep_test::make_spec;
using fdep_test::provenance;
using fdep_test::rack;
using fdep_test::service;

namespace {

MutationContext context_for(const Harness& harness) {
  MutationContext context;
  context.expected_generation = harness.generation();
  return context;
}

}  // namespace

FDEP_TEST(authority, stale_generation_is_rejected_on_every_mutation) {
  Harness harness = Harness::ephemeral();
  const DependencyEdgeId first = harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const DependencyGeneration stale = DependencyGeneration::from_value(0);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{1});

  RegisterEdgeRequest register_request;
  register_request.context.expected_generation = stale;
  register_request.spec = make_spec(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
  FDEP_CHECK_CODE(harness.registry().register_edge(register_request), ErrorCode::StaleGeneration);

  UpdateEdgeRequest update_request;
  update_request.context.expected_generation = stale;
  update_request.id = first;
  update_request.expected_revision = kInitialRevision;
  update_request.strength = DependencyStrength::Soft;
  update_request.provenance = provenance();
  FDEP_CHECK_CODE(harness.registry().update_edge(update_request), ErrorCode::StaleGeneration);

  LifecycleTransitionRequest transition_request;
  transition_request.context.expected_generation = stale;
  transition_request.id = first;
  transition_request.expected_revision = kInitialRevision;
  transition_request.target = LifecycleState::Suspended;
  FDEP_CHECK_CODE(harness.registry().transition_edge_lifecycle(transition_request), ErrorCode::StaleGeneration);

  RemoveEdgeRequest remove_request;
  remove_request.context.expected_generation = stale;
  remove_request.id = first;
  remove_request.expected_revision = kInitialRevision;
  FDEP_CHECK_CODE(harness.registry().remove_edge(remove_request), ErrorCode::StaleGeneration);

  DeclareRefRequest declare_request;
  declare_request.context.expected_generation = stale;
  declare_request.ref = asset("a");
  declare_request.provenance = provenance();
  FDEP_CHECK_CODE(harness.registry().declare_external_ref(declare_request), ErrorCode::StaleGeneration);

  WithdrawRefRequest withdraw_request;
  withdraw_request.context.expected_generation = stale;
  withdraw_request.ref = asset("a");
  FDEP_CHECK_CODE(harness.registry().withdraw_external_ref(withdraw_request), ErrorCode::StaleGeneration);

  // Nothing changed.
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{1});
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{1});
}

FDEP_TEST(authority, generation_zero_context_is_only_valid_for_the_empty_graph) {
  Harness harness = Harness::ephemeral();
  RegisterEdgeRequest request;
  // A default constructed context expects generation zero.
  FDEP_CHECK(request.context.expected_generation == kInitialGeneration);
  request.spec = make_spec(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  FDEP_REQUIRE_OK(harness.registry().register_edge(request));

  // The same default is now stale.
  RegisterEdgeRequest second;
  second.spec = make_spec(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
  FDEP_CHECK_CODE(harness.registry().register_edge(second), ErrorCode::StaleGeneration);
}

FDEP_TEST(authority, stale_edge_revision_is_rejected) {
  Harness harness = Harness::ephemeral();
  const DependencyEdgeId id = harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);

  UpdateEdgeRequest update;
  update.context = context_for(harness);
  update.id = id;
  update.expected_revision = EdgeRevision::from_value(7);
  update.strength = DependencyStrength::Soft;
  update.provenance = provenance();
  FDEP_CHECK_CODE(harness.registry().update_edge(update), ErrorCode::StaleEdgeRevision);

  RemoveEdgeRequest remove;
  remove.context = context_for(harness);
  remove.id = id;
  remove.expected_revision = EdgeRevision::from_value(0);
  FDEP_CHECK_CODE(harness.registry().remove_edge(remove), ErrorCode::StaleEdgeRevision);

  LifecycleTransitionRequest transition;
  transition.context = context_for(harness);
  transition.id = id;
  transition.expected_revision = EdgeRevision::from_value(99);
  transition.target = LifecycleState::Suspended;
  FDEP_CHECK_CODE(harness.registry().transition_edge_lifecycle(transition), ErrorCode::StaleEdgeRevision);

  // The revision advances by exactly one per accepted change.
  FDEP_REQUIRE(harness.transition(id, kInitialRevision, LifecycleState::Suspended).ok());
  FDEP_CHECK_EQ(harness.snapshot().find_edge(id)->revision().value(), std::uint64_t{2});
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{2});
  FDEP_CHECK_EQ(harness.snapshot().find_edge(id)->last_modified_generation().value(), std::uint64_t{2});
  FDEP_CHECK_EQ(harness.snapshot().find_edge(id)->registered_generation().value(), std::uint64_t{1});
}

FDEP_TEST(authority, unknown_edge_identity_is_reported_not_ignored) {
  Harness harness = Harness::ephemeral();
  UpdateEdgeRequest update;
  update.context = context_for(harness);
  update.id = DependencyEdgeId::from_value(42);
  update.expected_revision = kInitialRevision;
  update.provenance = provenance();
  FDEP_CHECK_CODE(harness.registry().update_edge(update), ErrorCode::EdgeNotFound);

  LifecycleTransitionRequest transition;
  transition.context = context_for(harness);
  transition.id = DependencyEdgeId::from_value(42);
  transition.expected_revision = kInitialRevision;
  transition.target = LifecycleState::Active;
  FDEP_CHECK_CODE(harness.registry().transition_edge_lifecycle(transition), ErrorCode::EdgeNotFound);

  UpdateEdgeRequest unassigned;
  unassigned.context = context_for(harness);
  unassigned.id = DependencyEdgeId{};
  unassigned.expected_revision = kInitialRevision;
  unassigned.provenance = provenance();
  FDEP_CHECK_CODE(harness.registry().update_edge(unassigned), ErrorCode::InvalidEdgeId);

  // Removing an edge that is not there is an idempotent no-op, not an error:
  // a retried removal must not fail, but it must not advance anything either.
  RemoveEdgeRequest remove;
  remove.context = context_for(harness);
  remove.id = DependencyEdgeId::from_value(42);
  remove.expected_revision = kInitialRevision;
  const auto outcome = harness.registry().remove_edge(remove);
  FDEP_REQUIRE_OK(outcome);
  FDEP_CHECK(outcome.value().already_absent);
  FDEP_CHECK(!outcome.value().generation_advanced);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{0});

  const auto remove_again = harness.registry().remove_edge(remove);
  FDEP_REQUIRE_OK(remove_again);
  FDEP_CHECK(remove_again.value().already_absent);
}

FDEP_TEST(authority, lifecycle_transitions_follow_the_table) {
  Harness harness = Harness::ephemeral();
  const DependencyEdgeId id = harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom,
                                          DependencyStrength::Hard, Direction::DependsOn,
                                          LifecycleState::Proposed);
  FDEP_CHECK(harness.snapshot().find_edge(id)->lifecycle() == LifecycleState::Proposed);

  LifecycleTransitionRequest illegal;
  illegal.context = context_for(harness);
  illegal.id = id;
  illegal.expected_revision = kInitialRevision;
  illegal.target = LifecycleState::Suspended;
  FDEP_CHECK_CODE(harness.registry().transition_edge_lifecycle(illegal),
                  ErrorCode::InvalidLifecycleTransition);

  LifecycleTransitionRequest activate;
  activate.context = context_for(harness);
  activate.id = id;
  activate.expected_revision = kInitialRevision;
  activate.target = LifecycleState::Active;
  const auto activated = harness.registry().transition_edge_lifecycle(activate);
  FDEP_REQUIRE_OK(activated);
  FDEP_CHECK(!activated.value().already_in_state);
  FDEP_CHECK(activated.value().generation_advanced);
  FDEP_CHECK_EQ(activated.value().revision.value(), std::uint64_t{2});

  // Repeating the transition is an idempotent no-op.
  LifecycleTransitionRequest repeat = activate;
  repeat.context = context_for(harness);
  repeat.expected_revision = EdgeRevision::from_value(2);
  const auto again = harness.registry().transition_edge_lifecycle(repeat);
  FDEP_REQUIRE_OK(again);
  FDEP_CHECK(again.value().already_in_state);
  FDEP_CHECK(!again.value().generation_advanced);
  FDEP_CHECK_EQ(again.value().revision.value(), std::uint64_t{2});
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{2});

  // Retired is terminal.
  LifecycleTransitionRequest retire;
  retire.context = context_for(harness);
  retire.id = id;
  retire.expected_revision = EdgeRevision::from_value(2);
  retire.target = LifecycleState::Retired;
  FDEP_REQUIRE_OK(harness.registry().transition_edge_lifecycle(retire));

  LifecycleTransitionRequest revive;
  revive.context = context_for(harness);
  revive.id = id;
  revive.expected_revision = EdgeRevision::from_value(3);
  revive.target = LifecycleState::Active;
  FDEP_CHECK_CODE(harness.registry().transition_edge_lifecycle(revive), ErrorCode::EdgeRetired);

  UpdateEdgeRequest update;
  update.context = context_for(harness);
  update.id = id;
  update.expected_revision = EdgeRevision::from_value(3);
  update.strength = DependencyStrength::Soft;
  update.provenance = provenance();
  FDEP_CHECK_CODE(harness.registry().update_edge(update), ErrorCode::EdgeRetired);

  // Out of domain lifecycle tokens are refused before anything else happens.
  LifecycleTransitionRequest bad_target;
  bad_target.context = context_for(harness);
  bad_target.id = id;
  bad_target.expected_revision = EdgeRevision::from_value(3);
  bad_target.target = static_cast<LifecycleState>(0);
  FDEP_CHECK_CODE(harness.registry().transition_edge_lifecycle(bad_target),
                  ErrorCode::InvalidLifecycleToken);
}

FDEP_TEST(authority, update_changes_the_payload_and_only_that) {
  Harness harness = Harness::ephemeral();
  const DependencyEdgeId id = harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const auto constraint = DependencyConstraint::make(ConstraintKind::FailoverMode, "automatic");
  FDEP_REQUIRE_OK(constraint);

  UpdateEdgeRequest update;
  update.context = context_for(harness);
  update.id = id;
  update.expected_revision = kInitialRevision;
  update.strength = DependencyStrength::Soft;
  update.direction = Direction::DependsOn;
  update.constraints = {constraint.value()};
  update.provenance = provenance("change-2", "bob", 9, "revised");
  const auto outcome = harness.registry().update_edge(update);
  FDEP_REQUIRE_OK(outcome);
  FDEP_CHECK(outcome.value().changed);
  FDEP_CHECK(harness.snapshot().find_edge(id)->strength() == DependencyStrength::Soft);
  FDEP_CHECK_EQ(harness.snapshot().find_edge(id)->constraints().size(), std::size_t{1});
  FDEP_CHECK_EQ(harness.snapshot().find_edge(id)->provenance().principal(), std::string{"bob"});
  FDEP_CHECK_EQ(harness.snapshot().find_edge(id)->registered_generation().value(), std::uint64_t{1});
  FDEP_CHECK_EQ(harness.snapshot().find_edge(id)->last_modified_generation().value(), std::uint64_t{2});

  // An update that describes the stored state exactly publishes nothing.
  UpdateEdgeRequest repeat = update;
  repeat.context = context_for(harness);
  repeat.expected_revision = EdgeRevision::from_value(2);
  const auto repeated = harness.registry().update_edge(repeat);
  FDEP_REQUIRE_OK(repeated);
  FDEP_CHECK(!repeated.value().changed);
  FDEP_CHECK(!repeated.value().generation_advanced);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{2});

  // An update may not change the key: that is a removal plus a registration.
  UpdateEdgeRequest rekey;
  rekey.context = context_for(harness);
  rekey.id = id;
  rekey.expected_revision = EdgeRevision::from_value(2);
  rekey.strength = DependencyStrength::Soft;
  rekey.provenance = provenance("change-2", "bob", 9, "revised");
  rekey.constraints = {};
  UpdateEdgeRequest applied = rekey;
  FDEP_REQUIRE_OK(harness.registry().update_edge(applied));
  FDEP_CHECK(harness.snapshot().find_edge(id)->source() == asset("a"));
  FDEP_CHECK(harness.snapshot().find_edge(id)->target() == feed("f1"));

  // Updating a control dependency into a mutual relation reorders the key only
  // when the endpoints are not already canonical.
  Harness other = Harness::ephemeral();
  const DependencyEdgeId control = other.add(service("s1"), asset("a"), DependencyKind::ControlDependsOn,
                                            DependencyStrength::Soft, Direction::DependsOn);
  UpdateEdgeRequest to_mutual;
  to_mutual.context = context_for(other);
  to_mutual.id = control;
  to_mutual.expected_revision = kInitialRevision;
  to_mutual.strength = DependencyStrength::Soft;
  to_mutual.direction = Direction::Mutual;
  to_mutual.provenance = provenance();
  const auto mutual_outcome = other.registry().update_edge(to_mutual);
  FDEP_REQUIRE_OK(mutual_outcome);
  const auto* record = other.snapshot().find_edge(control);
  FDEP_REQUIRE(record != nullptr);
  FDEP_CHECK(record->direction() == Direction::Mutual);
  FDEP_CHECK(record->source() == asset("a"));
  FDEP_CHECK(record->target() == service("s1"));
}

FDEP_TEST(authority, closed_registry_refuses_every_mutation_but_still_answers_queries) {
  Harness harness = Harness::ephemeral();
  const DependencyEdgeId id = harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const RegistrySnapshot before_close = harness.snapshot();
  FDEP_REQUIRE(harness.registry().close().ok());
  FDEP_CHECK(harness.registry().closed());
  FDEP_REQUIRE(harness.registry().close().ok());  // idempotent

  MutationContext context;
  context.expected_generation = harness.generation();
  RegisterEdgeRequest request;
  request.context = context;
  request.spec = make_spec(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
  FDEP_CHECK_CODE(harness.registry().register_edge(request), ErrorCode::RegistryClosed);

  UpdateEdgeRequest update;
  update.context = context;
  update.id = id;
  update.expected_revision = kInitialRevision;
  update.provenance = provenance();
  FDEP_CHECK_CODE(harness.registry().update_edge(update), ErrorCode::RegistryClosed);

  RemoveEdgeRequest remove;
  remove.context = context;
  remove.id = id;
  remove.expected_revision = kInitialRevision;
  FDEP_CHECK_CODE(harness.registry().remove_edge(remove), ErrorCode::RegistryClosed);

  LifecycleTransitionRequest transition;
  transition.context = context;
  transition.id = id;
  transition.expected_revision = kInitialRevision;
  transition.target = LifecycleState::Suspended;
  FDEP_CHECK_CODE(harness.registry().transition_edge_lifecycle(transition), ErrorCode::RegistryClosed);

  DeclareRefRequest declare;
  declare.context = context;
  declare.ref = asset("a");
  declare.provenance = provenance();
  FDEP_CHECK_CODE(harness.registry().declare_external_ref(declare), ErrorCode::RegistryClosed);

  WithdrawRefRequest withdraw;
  withdraw.context = context;
  withdraw.ref = asset("a");
  FDEP_CHECK_CODE(harness.registry().withdraw_external_ref(withdraw), ErrorCode::RegistryClosed);

  // A snapshot taken before the close stays valid and unchanged.
  FDEP_CHECK_EQ(before_close.edge_count(), std::size_t{1});
  FDEP_CHECK_EQ(before_close.state_digest(), harness.snapshot().state_digest());
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{1});
}

FDEP_TEST(authority, journal_records_accepted_and_rejected_attempts) {
  Harness harness = Harness::ephemeral();
  const DependencyEdgeId id = harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  FDEP_CHECK_CODE(harness.try_add(make_spec(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom,
                                           DependencyStrength::Soft),
                                  harness.generation()),
                  ErrorCode::DuplicateEdge);
  FDEP_REQUIRE(harness.remove(id, kInitialRevision).ok());

  const auto journal = harness.registry().journal();
  FDEP_REQUIRE(journal.size() >= 4);
  FDEP_CHECK(journal.front().operation == JournalOperation::Open);

  std::size_t accepted = 0;
  std::size_t rejected = 0;
  for (const auto& entry : journal) {
    if (entry.outcome == ErrorCode::Ok) {
      ++accepted;
    } else {
      ++rejected;
    }
    FDEP_CHECK_EQ(entry.recorded_at_unix_ms, std::int64_t{1'700'000'000'000});
  }
  FDEP_CHECK_EQ(accepted, std::size_t{3});
  FDEP_CHECK_EQ(rejected, std::size_t{1});

  const JournalEntry& last = journal.back();
  FDEP_CHECK(last.operation == JournalOperation::RemoveEdge);
  FDEP_CHECK_EQ(last.generation.value(), std::uint64_t{2});
  FDEP_CHECK(!last.to_text().empty());
}

FDEP_TEST(authority, journal_is_bounded) {
  RegistryLimits limits;
  limits.max_journal_entries = 4;
  Harness harness = Harness::ephemeral(limits);
  for (int index = 0; index < 20; ++index) {
    harness.add(asset("node-" + std::to_string(index)), feed("f1"), DependencyKind::RequiresPowerFrom);
  }
  const auto journal = harness.registry().journal();
  FDEP_CHECK_EQ(journal.size(), std::size_t{4});
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{20});
}

FDEP_TEST(authority, ephemeral_registry_reports_not_durable) {
  Harness harness = Harness::ephemeral();
  FDEP_CHECK(!harness.registry().durable());
  FDEP_CHECK(!harness.registry().writer_incarnation().valid());
  const auto status = harness.registry().store_status();
  FDEP_CHECK_CODE(status, ErrorCode::NotDurable);
  FDEP_CHECK(harness.registry().clock() != nullptr);
}

FDEP_TEST(authority, edge_capacity_is_enforced_before_allocating) {
  RegistryLimits limits;
  limits.max_edges = 2;
  Harness harness = Harness::ephemeral(limits);
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  harness.add(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
  FDEP_CHECK_CODE(harness.try_add(make_spec(asset("c"), feed("f1"), DependencyKind::RequiresPowerFrom),
                                  harness.generation()),
                  ErrorCode::EdgeCapacityExceeded);
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{2});

  RegistryLimits ref_limits;
  ref_limits.max_declared_refs = 1;
  Harness refs = Harness::ephemeral(ref_limits);
  refs.declare(asset("a"));
  DeclareRefRequest second;
  second.context.expected_generation = refs.generation();
  second.ref = asset("b");
  second.provenance = provenance();
  FDEP_CHECK_CODE(refs.registry().declare_external_ref(second), ErrorCode::DeclaredRefCapacityExceeded);
}

FDEP_TEST(authority, declared_reference_withdrawal_is_explicit_about_unresolved_endpoints) {
  Harness harness = Harness::ephemeral();
  harness.declare(asset("a"));
  harness.declare(feed("f1"));
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);

  WithdrawRefRequest blocked;
  blocked.context = context_for(harness);
  blocked.ref = asset("a");
  FDEP_CHECK_CODE(harness.registry().withdraw_external_ref(blocked), ErrorCode::DeclaredRefReferenced);

  WithdrawRefRequest allowed;
  allowed.context = context_for(harness);
  allowed.ref = asset("a");
  allowed.allow_referenced = true;
  const auto outcome = harness.registry().withdraw_external_ref(allowed);
  FDEP_REQUIRE_OK(outcome);
  FDEP_CHECK(!outcome.value().already_absent);
  FDEP_CHECK(outcome.value().generation_advanced);
  FDEP_CHECK(harness.snapshot().resolution_of(asset("a")) == RefResolution::Unresolved);
  FDEP_CHECK(harness.snapshot().resolution_of(feed("f1")) == RefResolution::Declared);
  FDEP_CHECK(harness.snapshot().resolution_of(rack("r9")) == RefResolution::Absent);

  const auto repeat_request = [&]() {
    WithdrawRefRequest again = allowed;
    again.context.expected_generation = harness.generation();
    return harness.registry().withdraw_external_ref(again);
  };
  const auto repeat = repeat_request();
  FDEP_REQUIRE_OK(repeat);
  FDEP_CHECK(repeat.value().already_absent);
}
