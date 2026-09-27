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

#ifndef FACILITY_DEPENDENCY_REGISTRY_REQUESTS_HPP
#define FACILITY_DEPENDENCY_REGISTRY_REQUESTS_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "facility_dependency_registry/cancel.hpp"
#include "facility_dependency_registry/edge.hpp"
#include "facility_dependency_registry/errors.hpp"
#include "facility_dependency_registry/ids.hpp"
#include "facility_dependency_registry/lifecycle.hpp"
#include "facility_dependency_registry/node_ref.hpp"
#include "facility_dependency_registry/provenance.hpp"

namespace facility_dependency_registry {

/// The authority context every mutation is addressed with.
///
/// `expected_generation` is mandatory in the sense that a caller must set it
/// deliberately: a default constructed context expects generation zero, the
/// empty graph, and is rejected as stale by any registry that has committed a
/// mutation. There is no "ignore generation" mode.
struct MutationContext {
  /// The generation this mutation believes is current. The registry commits
  /// only when its own generation is exactly this value.
  DependencyGeneration expected_generation = kInitialGeneration;

  /// Optional cooperative cancellation, observed immediately before the
  /// commit boundary. A cancelled mutation publishes nothing.
  CancellationToken cancellation{};
};

/// Declares one new dependency edge.
struct RegisterEdgeRequest {
  MutationContext context{};
  DependencyEdgeSpec spec{};
};

/// The outcome of an accepted registration.
struct RegisterEdgeOutcome {
  DependencyEdgeId id{};
  EdgeRevision revision{};
  /// The generation after the call. When `already_present` is true this is
  /// unchanged and no durable generation was published.
  DependencyGeneration generation{};
  /// True when an identical declaration was already stored. The call was a
  /// successful, idempotent no-op.
  bool already_present = false;
  bool generation_advanced = false;
};

/// Replaces the mutable payload of an existing edge.
///
/// The key of an edge -- its endpoints and its kind -- is immutable. Changing
/// the key is a removal followed by a registration, so that identity,
/// provenance and history stay unambiguous.
struct UpdateEdgeRequest {
  MutationContext context{};
  DependencyEdgeId id{};
  EdgeRevision expected_revision{};
  DependencyStrength strength = DependencyStrength::Hard;
  Direction direction = Direction::DependsOn;
  std::vector<DependencyConstraint> constraints{};
  ProvenanceRecord provenance{};
};

struct UpdateEdgeOutcome {
  DependencyEdgeId id{};
  EdgeRevision revision{};
  DependencyGeneration generation{};
  /// False when the request was accepted but described the stored state
  /// exactly, in which case nothing was published.
  bool changed = false;
  bool generation_advanced = false;
};

/// Moves an edge through its lifecycle transition table.
struct LifecycleTransitionRequest {
  MutationContext context{};
  DependencyEdgeId id{};
  EdgeRevision expected_revision{};
  LifecycleState target = LifecycleState::Active;
  /// Optional bounded free text recorded in the mutation journal.
  std::string reason{};
};

struct LifecycleTransitionOutcome {
  DependencyEdgeId id{};
  EdgeRevision revision{};
  DependencyGeneration generation{};
  /// True when the edge was already in the requested state. Idempotent no-op.
  bool already_in_state = false;
  bool generation_advanced = false;
};

/// Removes an edge outright, releasing its key for a later declaration.
struct RemoveEdgeRequest {
  MutationContext context{};
  DependencyEdgeId id{};
  EdgeRevision expected_revision{};
  /// Optional bounded free text recorded in the mutation journal.
  std::string reason{};
};

struct RemoveEdgeOutcome {
  DependencyEdgeId id{};
  /// The revision the removed edge last had.
  EdgeRevision removed_revision{};
  DependencyGeneration generation{};
  /// True when no edge with that identity was stored. Idempotent no-op.
  bool already_absent = false;
  bool generation_advanced = false;
};

/// Declares that an external identity has been observed to exist.
///
/// This is an observation, not ownership: the registry never creates, mutates
/// or retires the object. A declaration only lets the registry distinguish
/// "this endpoint is a reference to something known to exist" from "this
/// endpoint has never been observed", which is reported as unresolved rather
/// than silently invented.
struct DeclareRefRequest {
  MutationContext context{};
  DependencyNodeRef ref{};
  ProvenanceRecord provenance{};
};

struct DeclareRefOutcome {
  DependencyNodeRef ref{};
  DependencyGeneration generation{};
  bool already_declared = false;
  bool generation_advanced = false;
};

/// Withdraws an external reference declaration. Stored edges are untouched:
/// their endpoints simply become unresolved again.
struct WithdrawRefRequest {
  MutationContext context{};
  DependencyNodeRef ref{};
  /// A declaration that stored edges still reference is refused unless the
  /// caller states, explicitly, that those edges are to be left pointing at an
  /// undeclared identity. There is no default that quietly turns a set of
  /// endpoints into unresolved references.
  bool allow_referenced = false;
};

struct WithdrawRefOutcome {
  DependencyNodeRef ref{};
  DependencyGeneration generation{};
  bool already_absent = false;
  bool generation_advanced = false;
};

/// What the journal records.
enum class JournalOperation : std::uint8_t {
  Open = 1,
  Close = 2,
  RegisterEdge = 3,
  UpdateEdge = 4,
  TransitionEdge = 5,
  RemoveEdge = 6,
  DeclareRef = 7,
  WithdrawRef = 8,
  Publish = 9,
};

[[nodiscard]] std::string_view to_token(JournalOperation operation) noexcept;

/// One bounded journal record. The journal is a diagnostic ring buffer held in
/// memory; it is not authoritative state and does not survive a restart. The
/// authoritative record of a change is the generation it produced.
struct JournalEntry {
  DependencyGeneration generation{};
  JournalOperation operation{JournalOperation::Open};
  DependencyEdgeId edge_id{};
  DependencyNodeRef node{};
  /// Ok for a committed change; otherwise the stable rejection category.
  ErrorCode outcome{ErrorCode::Ok};
  std::string detail{};
  std::int64_t recorded_at_unix_ms = 0;

  [[nodiscard]] std::string to_text() const;
};

}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_REQUESTS_HPP
