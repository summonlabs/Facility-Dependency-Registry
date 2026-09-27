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

/// A graph with enough shape that a traversal has work to do.
void build_graph(Harness& harness, int count) {
  for (int index = 0; index < count; ++index) {
    if (index == 0) {
      harness.add(asset("node-0"), feed("f1"), DependencyKind::RequiresPowerFrom);
    } else {
      harness.add(asset("node-" + std::to_string(index)), asset("node-" + std::to_string(index - 1)),
                  DependencyKind::ControlDependsOn);
    }
  }
}

}  // namespace

FDEP_TEST(cancellation, a_default_token_is_never_cancelled) {
  const CancellationToken token;
  FDEP_CHECK(!token.cancelled());
  FDEP_CHECK(!token.cancellable());

  CancellationSource source;
  const CancellationToken live = source.token();
  FDEP_CHECK(live.cancellable());
  FDEP_CHECK(!live.cancelled());
  FDEP_CHECK(!source.cancelled());
  source.cancel();
  FDEP_CHECK(live.cancelled());
  FDEP_CHECK(source.cancelled());
  source.cancel();  // idempotent
  FDEP_CHECK(live.cancelled());

  // Copies share one flag.
  const CancellationToken copy = live;
  FDEP_CHECK(copy.cancelled());
  FDEP_CHECK(copy.cancellable());
}

FDEP_TEST(cancellation, a_cancelled_traversal_returns_the_cancelled_category) {
  Harness harness = Harness::ephemeral();
  build_graph(harness, 32);

  TraversalRequest request;
  request.root = feed("f1");
  request.direction = TraversalDirection::Dependents;
  request.max_depth = 32;
  request.max_nodes = 256;
  FDEP_REQUIRE_OK(harness.snapshot().transitive_closure(request));

  CancellationSource source;
  source.cancel();
  request.cancellation = source.token();
  const auto cancelled = harness.snapshot().transitive_closure(request);
  FDEP_CHECK_CODE(cancelled, ErrorCode::Cancelled);
  FDEP_CHECK(cancelled.error().detail().find("cancelled") != std::string::npos);
}

FDEP_TEST(cancellation, cancellation_is_observed_during_a_long_walk) {
  Harness harness = Harness::ephemeral();
  constexpr int kNodes = 600;
  build_graph(harness, kNodes);

  TraversalRequest request;
  request.root = feed("f1");
  request.direction = TraversalDirection::Dependents;
  request.max_depth = 64;
  request.max_nodes = 1024;

  CancellationSource source;
  request.cancellation = source.token();
  const RegistrySnapshot snapshot = harness.snapshot();

  // Cancel from another thread while the walk is running. Whether the walk
  // finishes first or observes the flag, the answer must be one of the two
  // honest outcomes: a complete result, or the cancelled category. It must
  // never be a partial result presented as complete.
  std::thread canceller{[&source]() {
    std::this_thread::yield();
    source.cancel();
  }};
  const auto result = snapshot.transitive_closure(request);
  canceller.join();

  if (result) {
    // A returned result is either complete or explicitly truncated at the
    // depth bound; it is never a silently partial answer.
    const bool complete = result.value().stop == TraversalStop::Complete;
    FDEP_CHECK(complete || result.value().stop == TraversalStop::DepthLimit);
    FDEP_CHECK_EQ(result.value().truncated, !complete);
    FDEP_CHECK_EQ(result.value().entries.size(),
                  complete ? static_cast<std::size_t>(kNodes) : std::size_t{64});
  } else {
    FDEP_CHECK(result.error().code() == ErrorCode::Cancelled);
  }
  FDEP_CHECK(source.cancelled());
}

FDEP_TEST(cancellation, every_bounded_query_observes_cancellation) {
  Harness harness = Harness::ephemeral();
  build_graph(harness, 16);
  CancellationSource source;
  source.cancel();
  const CancellationToken token = source.token();

  TraversalRequest traversal;
  traversal.root = feed("f1");
  traversal.direction = TraversalDirection::Dependents;
  traversal.max_depth = 32;
  traversal.max_nodes = 64;
  traversal.cancellation = token;
  FDEP_CHECK_CODE(harness.snapshot().transitive_closure(traversal), ErrorCode::Cancelled);

  ImpactConeRequest cone;
  cone.origin = feed("f1");
  cone.max_depth = 32;
  cone.max_nodes = 64;
  cone.strengths = DependencyStrengthMask::all();
  cone.cancellation = token;
  FDEP_CHECK_CODE(harness.snapshot().impact_cone(cone), ErrorCode::Cancelled);

  PathRequest path;
  path.from = feed("f1");
  path.to = asset("node-15");
  path.direction = TraversalDirection::Dependents;
  path.max_depth = 32;
  path.max_nodes = 64;
  path.cancellation = token;
  FDEP_CHECK_CODE(harness.snapshot().explain_path(path), ErrorCode::Cancelled);

  ComponentRequest components;
  components.max_nodes = 64;
  components.cancellation = token;
  FDEP_CHECK_CODE(harness.snapshot().strongly_connected_components(components), ErrorCode::Cancelled);

  CycleRequest cycles;
  cycles.max_length = 8;
  cycles.max_cycles = 8;
  cycles.max_nodes = 64;
  cycles.cancellation = token;
  FDEP_CHECK_CODE(harness.snapshot().enumerate_cycles(cycles), ErrorCode::Cancelled);

  FDEP_CHECK_CODE(harness.snapshot().verify_acyclic_obligation(token), ErrorCode::Cancelled);

  const auto diff = diff_snapshots(RegistrySnapshot::empty(), harness.snapshot(), 64, token);
  FDEP_CHECK_CODE(diff, ErrorCode::Cancelled);
}

FDEP_TEST(cancellation, a_cancelled_mutation_publishes_nothing) {
  Harness harness = Harness::ephemeral();
  const DependencyGeneration before = harness.generation();
  const ContentDigest digest_before = harness.snapshot().state_digest();

  CancellationSource source;
  source.cancel();
  RegisterEdgeRequest request;
  request.context.expected_generation = harness.generation();
  request.context.cancellation = source.token();
  request.spec = fdep_test::make_spec(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  FDEP_CHECK_CODE(harness.registry().register_edge(request), ErrorCode::Cancelled);
  FDEP_CHECK_EQ(harness.generation().value(), before.value());
  FDEP_CHECK_EQ(harness.snapshot().state_digest(), digest_before);
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{0});
}

FDEP_TEST(cancellation, a_cancelled_mutation_writes_no_generation_file) {
  const std::filesystem::path root = fdep_test::make_temp_directory("cancellation-durable");
  {
    Harness harness = Harness::durable(root);
    harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
    const DependencyGeneration before = harness.generation();

    CancellationSource source;
    source.cancel();
    RegisterEdgeRequest request;
    request.context.expected_generation = harness.generation();
    request.context.cancellation = source.token();
    request.spec = fdep_test::make_spec(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
    FDEP_CHECK_CODE(harness.registry().register_edge(request), ErrorCode::Cancelled);

    // The cancellation left no trace at all: same generation, same state, and
    // no transient file left behind.
    FDEP_CHECK_EQ(harness.generation().value(), before.value());
    FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{1});
    std::size_t transient = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{root, error}) {
      if (!error && entry.path().filename().string().rfind("tmp-", 0) == 0) {
        ++transient;
      }
    }
    FDEP_CHECK_EQ(transient, std::size_t{0});
    FDEP_REQUIRE(harness.registry().close().ok());
  }
  {
    Harness reopened = Harness::durable(root);
    FDEP_CHECK_EQ(reopened.generation().value(), std::uint64_t{1});
    FDEP_CHECK_EQ(reopened.snapshot().edge_count(), std::size_t{1});
    FDEP_REQUIRE(reopened.registry().close().ok());
  }
  fdep_test::remove_tree(root);
}

FDEP_TEST(cancellation, cancellation_after_a_commit_does_not_undo_it) {
  Harness harness = Harness::ephemeral();
  CancellationSource source;
  RegisterEdgeRequest request;
  request.context.expected_generation = harness.generation();
  request.context.cancellation = source.token();
  request.spec = fdep_test::make_spec(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const auto outcome = harness.registry().register_edge(request);
  FDEP_REQUIRE_OK(outcome);

  // Cancelling afterwards cannot retract work that already crossed the commit
  // boundary, and the registry says so by still reporting the committed edge.
  source.cancel();
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{1});
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{1});

  // A later mutation that is cancelled is refused as usual.
  RegisterEdgeRequest second;
  second.context.expected_generation = harness.generation();
  second.context.cancellation = source.token();
  second.spec = fdep_test::make_spec(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
  FDEP_CHECK_CODE(harness.registry().register_edge(second), ErrorCode::Cancelled);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{1});
}

FDEP_TEST(cancellation, every_mutation_command_checks_cancellation_first) {
  Harness harness = Harness::ephemeral();
  const DependencyEdgeId id = harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  harness.declare(asset("a"));
  const DependencyGeneration before = harness.generation();

  CancellationSource source;
  source.cancel();
  const CancellationToken token = source.token();

  UpdateEdgeRequest update;
  update.context.expected_generation = harness.generation();
  update.context.cancellation = token;
  update.id = id;
  update.expected_revision = kInitialRevision;
  update.provenance = fdep_test::provenance();
  FDEP_CHECK_CODE(harness.registry().update_edge(update), ErrorCode::Cancelled);

  LifecycleTransitionRequest transition;
  transition.context.expected_generation = harness.generation();
  transition.context.cancellation = token;
  transition.id = id;
  transition.expected_revision = kInitialRevision;
  transition.target = LifecycleState::Suspended;
  FDEP_CHECK_CODE(harness.registry().transition_edge_lifecycle(transition), ErrorCode::Cancelled);

  RemoveEdgeRequest remove;
  remove.context.expected_generation = harness.generation();
  remove.context.cancellation = token;
  remove.id = id;
  remove.expected_revision = kInitialRevision;
  FDEP_CHECK_CODE(harness.registry().remove_edge(remove), ErrorCode::Cancelled);

  DeclareRefRequest declare;
  declare.context.expected_generation = harness.generation();
  declare.context.cancellation = token;
  declare.ref = feed("f2");
  declare.provenance = fdep_test::provenance();
  FDEP_CHECK_CODE(harness.registry().declare_external_ref(declare), ErrorCode::Cancelled);

  WithdrawRefRequest withdraw;
  withdraw.context.expected_generation = harness.generation();
  withdraw.context.cancellation = token;
  withdraw.ref = asset("a");
  withdraw.allow_referenced = true;
  FDEP_CHECK_CODE(harness.registry().withdraw_external_ref(withdraw), ErrorCode::Cancelled);

  FDEP_CHECK_EQ(harness.generation().value(), before.value());
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{1});
  FDEP_CHECK_EQ(harness.snapshot().declared_ref_count(), std::size_t{1});
}

FDEP_TEST(cancellation, cancellation_is_recorded_in_the_journal) {
  Harness harness = Harness::ephemeral();
  CancellationSource source;
  source.cancel();
  RegisterEdgeRequest request;
  request.context.expected_generation = harness.generation();
  request.context.cancellation = source.token();
  request.spec = fdep_test::make_spec(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  FDEP_CHECK_CODE(harness.registry().register_edge(request), ErrorCode::Cancelled);

  const auto journal = harness.registry().journal();
  FDEP_REQUIRE(!journal.empty());
  FDEP_CHECK_EQ(journal.back().outcome, ErrorCode::Cancelled);
  FDEP_CHECK(journal.back().operation == JournalOperation::RegisterEdge);
  FDEP_CHECK(journal.back().to_text().find("cancelled") != std::string::npos);
}
