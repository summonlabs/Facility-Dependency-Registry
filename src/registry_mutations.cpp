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
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "registry_internal.hpp"

namespace facility_dependency_registry {
namespace {

using detail::GraphState;
using detail::RegistryImpl;

// -- edit helpers -----------------------------------------------------------
//
// Every mutation rebuilds the authoritative state from a modified copy of the
// previous one. The state is fully re-validated on the way in, so a mutation
// can never introduce a state that the loader would refuse.

std::optional<std::size_t> edge_index_of(const GraphState& state, DependencyEdgeId id) {
  const auto* record = state.find_edge(id);
  if (record == nullptr) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(record - state.edges().data());
}

std::vector<DependencyEdgeRecord> edges_with_insert(const GraphState& state, DependencyEdgeRecord record) {
  std::vector<DependencyEdgeRecord> edges{state.edges().begin(), state.edges().end()};
  const auto position = std::lower_bound(edges.begin(), edges.end(), record);
  edges.insert(position, std::move(record));
  return edges;
}

std::vector<DependencyEdgeRecord> edges_with_replace(const GraphState& state, std::size_t index,
                                                     DependencyEdgeRecord record) {
  std::vector<DependencyEdgeRecord> edges{state.edges().begin(), state.edges().end()};
  edges.erase(edges.begin() + static_cast<std::ptrdiff_t>(index));
  const auto position = std::lower_bound(edges.begin(), edges.end(), record);
  edges.insert(position, std::move(record));
  return edges;
}

std::vector<DependencyEdgeRecord> edges_without(const GraphState& state, std::size_t index) {
  std::vector<DependencyEdgeRecord> edges{state.edges().begin(), state.edges().end()};
  edges.erase(edges.begin() + static_cast<std::ptrdiff_t>(index));
  return edges;
}

std::vector<DeclaredExternalRef> refs_with_insert(const GraphState& state, DeclaredExternalRef declaration) {
  std::vector<DeclaredExternalRef> refs{state.declared_refs().begin(), state.declared_refs().end()};
  const auto position = std::lower_bound(refs.begin(), refs.end(), declaration);
  refs.insert(position, std::move(declaration));
  return refs;
}

std::vector<DeclaredExternalRef> refs_without(const GraphState& state, const DependencyNodeRef& ref) {
  std::vector<DeclaredExternalRef> refs{state.declared_refs().begin(), state.declared_refs().end()};
  const auto position =
      std::lower_bound(refs.begin(), refs.end(), ref,
                       [](const DeclaredExternalRef& entry, const DependencyNodeRef& value) {
                         return entry.ref() < value;
                       });
  if (position != refs.end() && position->ref() == ref) {
    refs.erase(position);
  }
  return refs;
}

/// The payload of a declaration: everything a re-declaration must repeat to be
/// recognized as the same declaration rather than as a conflicting one.
bool same_payload(const DependencyEdgeRecord& record, const DependencyEdgeSpec& spec) {
  return record.strength() == spec.strength && record.direction() == spec.direction &&
         record.lifecycle() == spec.initial_lifecycle && record.constraints() == spec.constraints &&
         record.provenance() == spec.provenance;
}

/// Brings a declaration into canonical form: endpoints ordered for a mutual
/// edge, constraints ordered by kind.
void canonicalize(DependencyEdgeSpec& spec) {
  order_endpoints_canonically(spec.direction, spec.source, spec.target);
  std::sort(spec.constraints.begin(), spec.constraints.end());
}

Status validate_provenance_for_mutation(const ProvenanceRecord& provenance, const RegistryLimits& limits,
                                        std::string_view what) {
  const auto ordinal = static_cast<unsigned>(provenance.source());
  if (ordinal < 1 || ordinal > kProvenanceSourceCount) {
    std::string detail{what};
    detail.append(" has no recognised provenance source");
    return Status::failure(ErrorCode::InvalidProvenanceSource, std::move(detail));
  }
  if (const auto status = validate_external_id(provenance.source_id(), limits.max_id_length, "provenance source id");
      !status.ok()) {
    return Status::failure(ErrorCode::InvalidProvenanceIdentifier, status.error().detail());
  }
  if (const auto status = validate_external_id(provenance.principal(), limits.max_id_length, "provenance principal");
      !status.ok()) {
    return Status::failure(ErrorCode::InvalidProvenanceIdentifier, status.error().detail());
  }
  if (provenance.recorded_at_unix_ms() < 0 || provenance.recorded_at_unix_ms() > kMaxTimestampUnixMs) {
    return Status::failure(ErrorCode::InvalidTimestamp,
                           "recorded-at timestamp is outside [0, 4102444800000] milliseconds since the Unix epoch");
  }
  return validate_annotation(provenance.annotation(), limits.max_annotation_length);
}

bool advances(const RegistryLimits& limits, DependencyGeneration& generation) {
  if (!try_increment(generation)) {
    return false;
  }
  return generation.value() <= limits.max_generation;
}

}  // namespace

Result<RegisterEdgeOutcome> DependencyRegistry::register_edge(const RegisterEdgeRequest& request) {
  if (impl_ == nullptr) {
    return Result<RegisterEdgeOutcome>::failure(ErrorCode::RegistryClosed, "the registry is not open");
  }
  std::lock_guard<std::mutex> guard(impl_->mutation_mutex);
  RegistryImpl& impl = *impl_;
  detail::MutationScope scope{impl, JournalOperation::RegisterEdge};

  if (const auto status = detail::check_authority(impl, request.context); !status.ok()) {
    scope.reject(status);
    return Result<RegisterEdgeOutcome>::failure(status.error());
  }
  const RegistryLimits& limits = impl.limits;
  DependencyEdgeSpec spec = request.spec;
  if (const auto status = spec.validate(limits); !status.ok()) {
    scope.reject(status);
    return Result<RegisterEdgeOutcome>::failure(status.error());
  }
  canonicalize(spec);

  const auto current = impl.state.load(std::memory_order_acquire);
  const EdgeKey key{spec.source, spec.target, spec.kind};

  if (const auto* existing = current->find_edge(key); existing != nullptr) {
    if (existing->lifecycle() == LifecycleState::Retired) {
      std::string detail{"the key "};
      detail.append(key.to_text());
      detail.append(" is held by retired edge ");
      detail.append(facility_dependency_registry::to_text(existing->id()));
      detail.append("; remove that edge to declare this dependency again");
      scope.reject(RegistryError{ErrorCode::DuplicateEdge, detail});
      return Result<RegisterEdgeOutcome>::failure(ErrorCode::DuplicateEdge, std::move(detail));
    }
    if (same_payload(*existing, spec)) {
      RegisterEdgeOutcome outcome;
      outcome.id = existing->id();
      outcome.revision = existing->revision();
      outcome.generation = current->generation();
      outcome.already_present = true;
      outcome.generation_advanced = false;
      scope.accept(existing->id(), "already present; identical declaration");
      return outcome;
    }
    std::string detail{"the key "};
    detail.append(key.to_text());
    detail.append(" is already held by edge ");
    detail.append(facility_dependency_registry::to_text(existing->id()));
    detail.append(" at revision ");
    detail.append(facility_dependency_registry::to_text(existing->revision()));
    detail.append(" with a different payload; use the update command under that revision");
    scope.reject(RegistryError{ErrorCode::DuplicateEdge, detail});
    return Result<RegisterEdgeOutcome>::failure(ErrorCode::DuplicateEdge, std::move(detail));
  }

  if (!limits.has_edge_capacity(current->edges().size(), 1)) {
    Status status = Status::failure(ErrorCode::EdgeCapacityExceeded,
                                    "the graph already holds the configured maximum of " +
                                        std::to_string(limits.max_edges) + " edges");
    scope.reject(status);
    return Result<RegisterEdgeOutcome>::failure(status.error());
  }
  if (current->next_edge_ordinal() > kHardMaxGeneration) {
    Status status = Status::failure(ErrorCode::GenerationExhausted,
                                    "the edge identity space is exhausted");
    scope.reject(status);
    return Result<RegisterEdgeOutcome>::failure(status.error());
  }
  DependencyGeneration next_generation = current->generation();
  if (!advances(limits, next_generation)) {
    Status status = Status::failure(ErrorCode::GenerationExhausted,
                                    "the registry cannot advance beyond generation " +
                                        std::to_string(limits.max_generation));
    scope.reject(status);
    return Result<RegisterEdgeOutcome>::failure(status.error());
  }

  const DependencyEdgeId id = DependencyEdgeId::from_value(current->next_edge_ordinal());
  DependencyEdgeRecord record =
      detail::EdgeRecordFactory::make_edge(id, kInitialRevision, std::move(spec), next_generation, next_generation);
  auto edges = edges_with_insert(*current, std::move(record));
  auto next = detail::build_next_state(*current, next_generation, current->next_edge_ordinal() + 1,
                                       std::move(edges), key.to_text());
  if (!next) {
    scope.reject(next.error());
    return Result<RegisterEdgeOutcome>::failure(next.error());
  }
  if (request.context.cancellation.cancelled()) {
    Status status = Status::failure(ErrorCode::Cancelled, "the mutation was cancelled before it committed");
    scope.reject(status);
    return Result<RegisterEdgeOutcome>::failure(status.error());
  }
  if (const auto status = detail::commit_state(impl, std::move(next).value()); !status.ok()) {
    scope.reject(status);
    return Result<RegisterEdgeOutcome>::failure(status.error());
  }

  RegisterEdgeOutcome outcome;
  outcome.id = id;
  outcome.revision = kInitialRevision;
  outcome.generation = next_generation;
  outcome.already_present = false;
  outcome.generation_advanced = true;
  scope.accept(id, key.to_text());
  scope.set_generation(next_generation);
  return outcome;
}

Result<UpdateEdgeOutcome> DependencyRegistry::update_edge(const UpdateEdgeRequest& request) {
  if (impl_ == nullptr) {
    return Result<UpdateEdgeOutcome>::failure(ErrorCode::RegistryClosed, "the registry is not open");
  }
  std::lock_guard<std::mutex> guard(impl_->mutation_mutex);
  RegistryImpl& impl = *impl_;
  detail::MutationScope scope{impl, JournalOperation::UpdateEdge};

  if (const auto status = detail::check_authority(impl, request.context); !status.ok()) {
    scope.reject(status);
    return Result<UpdateEdgeOutcome>::failure(status.error());
  }
  const RegistryLimits& limits = impl.limits;
  const auto current = impl.state.load(std::memory_order_acquire);

  if (!is_assigned(request.id)) {
    Status status = Status::failure(ErrorCode::InvalidEdgeId, "the request names no edge identity");
    scope.reject(status);
    return Result<UpdateEdgeOutcome>::failure(status.error());
  }
  const auto index = edge_index_of(*current, request.id);
  if (!index.has_value()) {
    std::string detail{"no edge with identity "};
    detail.append(facility_dependency_registry::to_text(request.id));
    detail.append(" is stored");
    scope.reject(RegistryError{ErrorCode::EdgeNotFound, detail});
    return Result<UpdateEdgeOutcome>::failure(ErrorCode::EdgeNotFound, std::move(detail));
  }
  const DependencyEdgeRecord& stored = current->edges()[*index];
  if (stored.revision() != request.expected_revision) {
    std::string detail{"the request expected revision "};
    detail.append(facility_dependency_registry::to_text(request.expected_revision));
    detail.append(" of edge ");
    detail.append(facility_dependency_registry::to_text(request.id));
    detail.append(" but the stored revision is ");
    detail.append(facility_dependency_registry::to_text(stored.revision()));
    scope.reject(RegistryError{ErrorCode::StaleEdgeRevision, detail});
    return Result<UpdateEdgeOutcome>::failure(ErrorCode::StaleEdgeRevision, std::move(detail));
  }
  if (stored.lifecycle() == LifecycleState::Retired) {
    std::string detail{"edge "};
    detail.append(facility_dependency_registry::to_text(request.id));
    detail.append(" is retired and can no longer be changed");
    scope.reject(RegistryError{ErrorCode::EdgeRetired, detail});
    return Result<UpdateEdgeOutcome>::failure(ErrorCode::EdgeRetired, std::move(detail));
  }

  DependencyEdgeSpec spec;
  spec.source = stored.source();
  spec.target = stored.target();
  spec.kind = stored.kind();
  spec.strength = request.strength;
  spec.direction = request.direction;
  // An update never changes the lifecycle, which has its own command. The
  // declaration rule that restricts a *new* edge to proposed or active does not
  // apply here, so validation runs against a legal placeholder and the stored
  // state is carried over unchanged afterwards.
  spec.initial_lifecycle = stored.lifecycle() == LifecycleState::Proposed ? LifecycleState::Proposed
                                                                         : LifecycleState::Active;
  spec.constraints = request.constraints;
  spec.provenance = request.provenance;
  if (const auto status = spec.validate(limits); !status.ok()) {
    scope.reject(status);
    return Result<UpdateEdgeOutcome>::failure(status.error());
  }
  spec.initial_lifecycle = stored.lifecycle();
  canonicalize(spec);

  const EdgeKey previous_key = stored.key();
  const EdgeKey next_key{spec.source, spec.target, spec.kind};
  if (!(next_key == previous_key)) {
    if (const auto* clash = current->find_edge(next_key); clash != nullptr) {
      std::string detail{"reordering this edge would collide with edge "};
      detail.append(facility_dependency_registry::to_text(clash->id()));
      detail.append(" on key ");
      detail.append(next_key.to_text());
      scope.reject(RegistryError{ErrorCode::DuplicateEdge, detail});
      return Result<UpdateEdgeOutcome>::failure(ErrorCode::DuplicateEdge, std::move(detail));
    }
  }

  const bool unchanged = stored.strength() == spec.strength && stored.direction() == spec.direction &&
                         stored.lifecycle() == spec.initial_lifecycle &&
                         stored.constraints() == spec.constraints && stored.provenance() == spec.provenance &&
                         next_key == previous_key;
  if (unchanged) {
    UpdateEdgeOutcome outcome;
    outcome.id = stored.id();
    outcome.revision = stored.revision();
    outcome.generation = current->generation();
    outcome.changed = false;
    outcome.generation_advanced = false;
    scope.accept(stored.id(), "already present; identical payload");
    return outcome;
  }

  DependencyGeneration next_generation = current->generation();
  if (!advances(limits, next_generation)) {
    Status status = Status::failure(ErrorCode::GenerationExhausted,
                                    "the registry cannot advance beyond generation " +
                                        std::to_string(limits.max_generation));
    scope.reject(status);
    return Result<UpdateEdgeOutcome>::failure(status.error());
  }
  EdgeRevision next_revision = stored.revision();
  if (!try_increment(next_revision)) {
    Status status = Status::failure(ErrorCode::InvalidEdgeRevision, "the edge revision cannot advance further");
    scope.reject(status);
    return Result<UpdateEdgeOutcome>::failure(status.error());
  }

  DependencyEdgeRecord record = detail::EdgeRecordFactory::make_edge(
      stored.id(), next_revision, std::move(spec), stored.registered_generation(), next_generation);
  auto edges = edges_with_replace(*current, *index, std::move(record));
  auto next = detail::build_next_state(*current, next_generation, current->next_edge_ordinal(), std::move(edges),
                                       previous_key.to_text());
  if (!next) {
    scope.reject(next.error());
    return Result<UpdateEdgeOutcome>::failure(next.error());
  }
  if (request.context.cancellation.cancelled()) {
    Status status = Status::failure(ErrorCode::Cancelled, "the mutation was cancelled before it committed");
    scope.reject(status);
    return Result<UpdateEdgeOutcome>::failure(status.error());
  }
  if (const auto status = detail::commit_state(impl, std::move(next).value()); !status.ok()) {
    scope.reject(status);
    return Result<UpdateEdgeOutcome>::failure(status.error());
  }

  UpdateEdgeOutcome outcome;
  outcome.id = stored.id();
  outcome.revision = next_revision;
  outcome.generation = next_generation;
  outcome.changed = true;
  outcome.generation_advanced = true;
  scope.accept(stored.id(), next_key.to_text());
  scope.set_generation(next_generation);
  return outcome;
}

Result<LifecycleTransitionOutcome> DependencyRegistry::transition_edge_lifecycle(
    const LifecycleTransitionRequest& request) {
  if (impl_ == nullptr) {
    return Result<LifecycleTransitionOutcome>::failure(ErrorCode::RegistryClosed, "the registry is not open");
  }
  std::lock_guard<std::mutex> guard(impl_->mutation_mutex);
  RegistryImpl& impl = *impl_;
  detail::MutationScope scope{impl, JournalOperation::TransitionEdge};

  if (const auto status = detail::check_authority(impl, request.context); !status.ok()) {
    scope.reject(status);
    return Result<LifecycleTransitionOutcome>::failure(status.error());
  }
  const RegistryLimits& limits = impl.limits;
  const auto current = impl.state.load(std::memory_order_acquire);

  const auto ordinal = static_cast<unsigned>(request.target);
  if (ordinal < 1 || ordinal > kLifecycleStateCount) {
    Status status = Status::failure(ErrorCode::InvalidLifecycleToken,
                                    "the requested lifecycle state is not one of the declared states");
    scope.reject(status);
    return Result<LifecycleTransitionOutcome>::failure(status.error());
  }
  if (!is_assigned(request.id)) {
    Status status = Status::failure(ErrorCode::InvalidEdgeId, "the request names no edge identity");
    scope.reject(status);
    return Result<LifecycleTransitionOutcome>::failure(status.error());
  }
  const auto index = edge_index_of(*current, request.id);
  if (!index.has_value()) {
    std::string detail{"no edge with identity "};
    detail.append(facility_dependency_registry::to_text(request.id));
    detail.append(" is stored");
    scope.reject(RegistryError{ErrorCode::EdgeNotFound, detail});
    return Result<LifecycleTransitionOutcome>::failure(ErrorCode::EdgeNotFound, std::move(detail));
  }
  const DependencyEdgeRecord& stored = current->edges()[*index];
  if (stored.revision() != request.expected_revision) {
    std::string detail{"the request expected revision "};
    detail.append(facility_dependency_registry::to_text(request.expected_revision));
    detail.append(" of edge ");
    detail.append(facility_dependency_registry::to_text(request.id));
    detail.append(" but the stored revision is ");
    detail.append(facility_dependency_registry::to_text(stored.revision()));
    scope.reject(RegistryError{ErrorCode::StaleEdgeRevision, detail});
    return Result<LifecycleTransitionOutcome>::failure(ErrorCode::StaleEdgeRevision, std::move(detail));
  }
  if (stored.lifecycle() == request.target) {
    LifecycleTransitionOutcome outcome;
    outcome.id = stored.id();
    outcome.revision = stored.revision();
    outcome.generation = current->generation();
    outcome.already_in_state = true;
    outcome.generation_advanced = false;
    scope.accept(stored.id(), "already in the requested state");
    return outcome;
  }
  if (stored.lifecycle() == LifecycleState::Retired) {
    std::string detail{"edge "};
    detail.append(facility_dependency_registry::to_text(request.id));
    detail.append(" is retired, which is terminal");
    scope.reject(RegistryError{ErrorCode::EdgeRetired, detail});
    return Result<LifecycleTransitionOutcome>::failure(ErrorCode::EdgeRetired, std::move(detail));
  }
  if (!transition_allowed(stored.lifecycle(), request.target)) {
    std::string detail{"the transition from "};
    detail.append(to_token(stored.lifecycle()));
    detail.append(" to ");
    detail.append(to_token(request.target));
    detail.append(" is not in the lifecycle table");
    scope.reject(RegistryError{ErrorCode::InvalidLifecycleTransition, detail});
    return Result<LifecycleTransitionOutcome>::failure(ErrorCode::InvalidLifecycleTransition, std::move(detail));
  }

  DependencyGeneration next_generation = current->generation();
  if (!advances(limits, next_generation)) {
    Status status = Status::failure(ErrorCode::GenerationExhausted,
                                    "the registry cannot advance beyond generation " +
                                        std::to_string(limits.max_generation));
    scope.reject(status);
    return Result<LifecycleTransitionOutcome>::failure(status.error());
  }
  EdgeRevision next_revision = stored.revision();
  if (!try_increment(next_revision)) {
    Status status = Status::failure(ErrorCode::InvalidEdgeRevision, "the edge revision cannot advance further");
    scope.reject(status);
    return Result<LifecycleTransitionOutcome>::failure(status.error());
  }

  DependencyEdgeRecord record =
      detail::EdgeRecordFactory::with_lifecycle(stored, request.target, next_revision, next_generation);
  auto edges = edges_with_replace(*current, *index, std::move(record));
  auto next = detail::build_next_state(*current, next_generation, current->next_edge_ordinal(), std::move(edges),
                                       stored.key().to_text());
  if (!next) {
    scope.reject(next.error());
    return Result<LifecycleTransitionOutcome>::failure(next.error());
  }
  if (request.context.cancellation.cancelled()) {
    Status status = Status::failure(ErrorCode::Cancelled, "the mutation was cancelled before it committed");
    scope.reject(status);
    return Result<LifecycleTransitionOutcome>::failure(status.error());
  }
  if (const auto status = detail::commit_state(impl, std::move(next).value()); !status.ok()) {
    scope.reject(status);
    return Result<LifecycleTransitionOutcome>::failure(status.error());
  }

  LifecycleTransitionOutcome outcome;
  outcome.id = stored.id();
  outcome.revision = next_revision;
  outcome.generation = next_generation;
  outcome.already_in_state = false;
  outcome.generation_advanced = true;
  scope.accept(stored.id(), std::string{to_token(stored.lifecycle())} + " -> " + std::string{to_token(request.target)});
  scope.set_generation(next_generation);
  return outcome;
}

Result<RemoveEdgeOutcome> DependencyRegistry::remove_edge(const RemoveEdgeRequest& request) {
  if (impl_ == nullptr) {
    return Result<RemoveEdgeOutcome>::failure(ErrorCode::RegistryClosed, "the registry is not open");
  }
  std::lock_guard<std::mutex> guard(impl_->mutation_mutex);
  RegistryImpl& impl = *impl_;
  detail::MutationScope scope{impl, JournalOperation::RemoveEdge};

  if (const auto status = detail::check_authority(impl, request.context); !status.ok()) {
    scope.reject(status);
    return Result<RemoveEdgeOutcome>::failure(status.error());
  }
  const RegistryLimits& limits = impl.limits;
  const auto current = impl.state.load(std::memory_order_acquire);

  if (!is_assigned(request.id)) {
    Status status = Status::failure(ErrorCode::InvalidEdgeId, "the request names no edge identity");
    scope.reject(status);
    return Result<RemoveEdgeOutcome>::failure(status.error());
  }
  const auto index = edge_index_of(*current, request.id);
  if (!index.has_value()) {
    RemoveEdgeOutcome outcome;
    outcome.id = request.id;
    outcome.generation = current->generation();
    outcome.already_absent = true;
    outcome.generation_advanced = false;
    scope.accept(request.id, "already absent");
    return outcome;
  }
  const DependencyEdgeRecord& stored = current->edges()[*index];
  if (stored.revision() != request.expected_revision) {
    std::string detail{"the request expected revision "};
    detail.append(facility_dependency_registry::to_text(request.expected_revision));
    detail.append(" of edge ");
    detail.append(facility_dependency_registry::to_text(request.id));
    detail.append(" but the stored revision is ");
    detail.append(facility_dependency_registry::to_text(stored.revision()));
    scope.reject(RegistryError{ErrorCode::StaleEdgeRevision, detail});
    return Result<RemoveEdgeOutcome>::failure(ErrorCode::StaleEdgeRevision, std::move(detail));
  }

  DependencyGeneration next_generation = current->generation();
  if (!advances(limits, next_generation)) {
    Status status = Status::failure(ErrorCode::GenerationExhausted,
                                    "the registry cannot advance beyond generation " +
                                        std::to_string(limits.max_generation));
    scope.reject(status);
    return Result<RemoveEdgeOutcome>::failure(status.error());
  }

  const EdgeRevision removed_revision = stored.revision();
  auto edges = edges_without(*current, *index);
  auto next = detail::build_next_state(*current, next_generation, current->next_edge_ordinal(), std::move(edges),
                                       stored.key().to_text());
  if (!next) {
    scope.reject(next.error());
    return Result<RemoveEdgeOutcome>::failure(next.error());
  }
  if (request.context.cancellation.cancelled()) {
    Status status = Status::failure(ErrorCode::Cancelled, "the mutation was cancelled before it committed");
    scope.reject(status);
    return Result<RemoveEdgeOutcome>::failure(status.error());
  }
  if (const auto status = detail::commit_state(impl, std::move(next).value()); !status.ok()) {
    scope.reject(status);
    return Result<RemoveEdgeOutcome>::failure(status.error());
  }

  RemoveEdgeOutcome outcome;
  outcome.id = request.id;
  outcome.removed_revision = removed_revision;
  outcome.generation = next_generation;
  outcome.already_absent = false;
  outcome.generation_advanced = true;
  scope.accept(request.id, request.reason);
  scope.set_generation(next_generation);
  return outcome;
}

Result<DeclareRefOutcome> DependencyRegistry::declare_external_ref(const DeclareRefRequest& request) {
  if (impl_ == nullptr) {
    return Result<DeclareRefOutcome>::failure(ErrorCode::RegistryClosed, "the registry is not open");
  }
  std::lock_guard<std::mutex> guard(impl_->mutation_mutex);
  RegistryImpl& impl = *impl_;
  detail::MutationScope scope{impl, JournalOperation::DeclareRef};

  if (const auto status = detail::check_authority(impl, request.context); !status.ok()) {
    scope.reject(status);
    return Result<DeclareRefOutcome>::failure(status.error());
  }
  const RegistryLimits& limits = impl.limits;
  if (!request.ref.valid()) {
    Status status = Status::failure(ErrorCode::InvalidNodeReference,
                                    "the declaration names no valid external reference");
    scope.reject(status);
    return Result<DeclareRefOutcome>::failure(status.error());
  }
  if (const auto status = validate_provenance_for_mutation(request.provenance, limits, "the declaration");
      !status.ok()) {
    scope.reject(status);
    return Result<DeclareRefOutcome>::failure(status.error());
  }

  const auto current = impl.state.load(std::memory_order_acquire);
  if (const auto* existing = current->find_declared_ref(request.ref); existing != nullptr) {
    if (existing->provenance() == request.provenance) {
      DeclareRefOutcome outcome;
      outcome.ref = request.ref;
      outcome.generation = current->generation();
      outcome.already_declared = true;
      outcome.generation_advanced = false;
      scope.accept(request.ref, "already declared with the same provenance");
      return outcome;
    }
    std::string detail{"reference "};
    detail.append(request.ref.to_canonical());
    detail.append(" is already declared with different provenance");
    scope.reject(RegistryError{ErrorCode::DuplicateDeclaration, detail});
    return Result<DeclareRefOutcome>::failure(ErrorCode::DuplicateDeclaration, std::move(detail));
  }
  if (!limits.has_declared_ref_capacity(current->declared_refs().size(), 1)) {
    Status status = Status::failure(ErrorCode::DeclaredRefCapacityExceeded,
                                    "the registry holds the configured maximum of " +
                                        std::to_string(limits.max_declared_refs) + " declarations");
    scope.reject(status);
    return Result<DeclareRefOutcome>::failure(status.error());
  }
  DependencyGeneration next_generation = current->generation();
  if (!advances(limits, next_generation)) {
    Status status = Status::failure(ErrorCode::GenerationExhausted,
                                    "the registry cannot advance beyond generation " +
                                        std::to_string(limits.max_generation));
    scope.reject(status);
    return Result<DeclareRefOutcome>::failure(status.error());
  }

  auto declaration =
      detail::EdgeRecordFactory::make_declared_ref(request.ref, request.provenance, next_generation);
  auto refs = refs_with_insert(*current, std::move(declaration));
  auto next = detail::build_next_state_with_refs(*current, next_generation, std::move(refs),
                                                 request.ref.to_canonical());
  if (!next) {
    scope.reject(next.error());
    return Result<DeclareRefOutcome>::failure(next.error());
  }
  if (request.context.cancellation.cancelled()) {
    Status status = Status::failure(ErrorCode::Cancelled, "the mutation was cancelled before it committed");
    scope.reject(status);
    return Result<DeclareRefOutcome>::failure(status.error());
  }
  if (const auto status = detail::commit_state(impl, std::move(next).value()); !status.ok()) {
    scope.reject(status);
    return Result<DeclareRefOutcome>::failure(status.error());
  }

  DeclareRefOutcome outcome;
  outcome.ref = request.ref;
  outcome.generation = next_generation;
  outcome.already_declared = false;
  outcome.generation_advanced = true;
  scope.accept(request.ref, "declared");
  scope.set_generation(next_generation);
  return outcome;
}

Result<WithdrawRefOutcome> DependencyRegistry::withdraw_external_ref(const WithdrawRefRequest& request) {
  if (impl_ == nullptr) {
    return Result<WithdrawRefOutcome>::failure(ErrorCode::RegistryClosed, "the registry is not open");
  }
  std::lock_guard<std::mutex> guard(impl_->mutation_mutex);
  RegistryImpl& impl = *impl_;
  detail::MutationScope scope{impl, JournalOperation::WithdrawRef};

  if (const auto status = detail::check_authority(impl, request.context); !status.ok()) {
    scope.reject(status);
    return Result<WithdrawRefOutcome>::failure(status.error());
  }
  const RegistryLimits& limits = impl.limits;
  if (!request.ref.valid()) {
    Status status = Status::failure(ErrorCode::InvalidNodeReference,
                                    "the request names no valid external reference");
    scope.reject(status);
    return Result<WithdrawRefOutcome>::failure(status.error());
  }

  const auto current = impl.state.load(std::memory_order_acquire);
  if (current->find_declared_ref(request.ref) == nullptr) {
    WithdrawRefOutcome outcome;
    outcome.ref = request.ref;
    outcome.generation = current->generation();
    outcome.already_absent = true;
    outcome.generation_advanced = false;
    scope.accept(request.ref, "not declared");
    return outcome;
  }
  if (!request.allow_referenced) {
    const auto index = current->node_index(request.ref);
    if (index.has_value()) {
      const auto& entry = current->node_entries()[*index];
      if (!entry.out_edges.empty() || !entry.in_edges.empty()) {
        std::string detail{"reference "};
        detail.append(request.ref.to_canonical());
        detail.append(" is still an endpoint of stored edges; withdrawing it would leave those endpoints "
                      "unresolved, so the request must say so explicitly");
        scope.reject(RegistryError{ErrorCode::DeclaredRefReferenced, detail});
        return Result<WithdrawRefOutcome>::failure(ErrorCode::DeclaredRefReferenced, std::move(detail));
      }
    }
  }

  DependencyGeneration next_generation = current->generation();
  if (!advances(limits, next_generation)) {
    Status status = Status::failure(ErrorCode::GenerationExhausted,
                                    "the registry cannot advance beyond generation " +
                                        std::to_string(limits.max_generation));
    scope.reject(status);
    return Result<WithdrawRefOutcome>::failure(status.error());
  }

  auto refs = refs_without(*current, request.ref);
  auto next = detail::build_next_state_with_refs(*current, next_generation, std::move(refs),
                                                 request.ref.to_canonical());
  if (!next) {
    scope.reject(next.error());
    return Result<WithdrawRefOutcome>::failure(next.error());
  }
  if (request.context.cancellation.cancelled()) {
    Status status = Status::failure(ErrorCode::Cancelled, "the mutation was cancelled before it committed");
    scope.reject(status);
    return Result<WithdrawRefOutcome>::failure(status.error());
  }
  if (const auto status = detail::commit_state(impl, std::move(next).value()); !status.ok()) {
    scope.reject(status);
    return Result<WithdrawRefOutcome>::failure(status.error());
  }

  WithdrawRefOutcome outcome;
  outcome.ref = request.ref;
  outcome.generation = next_generation;
  outcome.already_absent = false;
  outcome.generation_advanced = true;
  scope.accept(request.ref, "withdrawn");
  scope.set_generation(next_generation);
  return outcome;
}

}  // namespace facility_dependency_registry
