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

#include "facility_dependency_registry/snapshot.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "graph_algorithms.hpp"
#include "graph_state.hpp"

namespace facility_dependency_registry {
namespace {

std::shared_ptr<const detail::GraphState> make_empty_state(const RegistryLimits& limits) {
  auto state = detail::GraphState::build(limits, kInitialGeneration, kFirstEdgeOrdinal, {}, {});
  if (!state) {
    // The default limits are valid by construction; if they were not, an empty
    // snapshot would be a lie, so fall back to the hard default rather than
    // pretending.
    auto fallback = detail::GraphState::build(RegistryLimits{}, kInitialGeneration, kFirstEdgeOrdinal, {}, {});
    return fallback.value();
  }
  return state.value();
}

}  // namespace

std::string_view to_token(RefResolution resolution) noexcept {
  switch (resolution) {
    case RefResolution::Declared:
      return "declared";
    case RefResolution::Unresolved:
      return "unresolved";
    case RefResolution::Absent:
      return "absent";
  }
  return "unknown-resolution";
}

std::string DeclaredExternalRef::to_text() const {
  std::string result;
  result.reserve(ref_.id().size() + provenance_.annotation().size() + 96);
  result.append("declaration ref=");
  result.append(ref_.to_canonical());
  result.append(" declared=");
  result.append(facility_dependency_registry::to_text(declared_generation_));
  result.append(" provenance={");
  result.append(provenance_.to_text());
  result.push_back('}');
  return result;
}

RegistrySnapshot::RegistrySnapshot() : state_(make_empty_state(RegistryLimits{})) {}

RegistrySnapshot::~RegistrySnapshot() = default;
RegistrySnapshot::RegistrySnapshot(const RegistrySnapshot&) noexcept = default;
RegistrySnapshot& RegistrySnapshot::operator=(const RegistrySnapshot&) noexcept = default;
RegistrySnapshot::RegistrySnapshot(RegistrySnapshot&&) noexcept = default;
RegistrySnapshot& RegistrySnapshot::operator=(RegistrySnapshot&&) noexcept = default;

RegistrySnapshot RegistrySnapshot::empty(const RegistryLimits& limits) {
  return RegistrySnapshot{make_empty_state(limits)};
}

Result<RegistrySnapshot> RegistrySnapshot::decode(std::span<const std::byte> payload, const RegistryLimits& limits) {
  auto state = detail::decode_state(payload, limits);
  if (!state) {
    return Result<RegistrySnapshot>::failure(state.error());
  }
  return RegistrySnapshot{std::move(state).value()};
}

Result<RegistrySnapshot> make_snapshot(std::shared_ptr<const detail::GraphState> state) {
  if (state == nullptr) {
    return Result<RegistrySnapshot>::failure(ErrorCode::InvalidArguments, "the state handle is empty");
  }
  return RegistrySnapshot{std::move(state)};
}

DependencyGeneration RegistrySnapshot::generation() const noexcept { return state_->generation(); }

const RegistryLimits& RegistrySnapshot::limits() const noexcept { return state_->limits(); }

std::size_t RegistrySnapshot::edge_count() const noexcept { return state_->edges().size(); }

std::size_t RegistrySnapshot::declared_ref_count() const noexcept { return state_->declared_refs().size(); }

std::size_t RegistrySnapshot::node_count() const noexcept { return state_->nodes().size(); }

std::uint64_t RegistrySnapshot::next_edge_ordinal() const noexcept { return state_->next_edge_ordinal(); }

std::span<const DependencyEdgeRecord> RegistrySnapshot::edges() const noexcept { return state_->edges(); }

std::span<const DeclaredExternalRef> RegistrySnapshot::declared_refs() const noexcept {
  return state_->declared_refs();
}

std::span<const DependencyNodeRef> RegistrySnapshot::nodes() const noexcept { return state_->nodes(); }

const DependencyEdgeRecord* RegistrySnapshot::find_edge(DependencyEdgeId id) const noexcept {
  return state_->find_edge(id);
}

const DependencyEdgeRecord* RegistrySnapshot::find_edge(const EdgeKey& key) const noexcept {
  return state_->find_edge(key);
}

const DeclaredExternalRef* RegistrySnapshot::find_declared_ref(const DependencyNodeRef& ref) const noexcept {
  return state_->find_declared_ref(ref);
}

bool RegistrySnapshot::has_node(const DependencyNodeRef& ref) const noexcept {
  return state_->node_index(ref).has_value();
}

RefResolution RegistrySnapshot::resolution_of(const DependencyNodeRef& ref) const noexcept {
  if (state_->find_declared_ref(ref) != nullptr) {
    return RefResolution::Declared;
  }
  if (state_->node_index(ref).has_value()) {
    return RefResolution::Unresolved;
  }
  return RefResolution::Absent;
}

Result<std::vector<DependencyEdgeRecord>> RegistrySnapshot::direct_dependencies(const DependencyNodeRef& node,
                                                                                const EdgeFilter& filter) const {
  return direct_dependents_or_dependencies(node, filter, true);
}

Result<std::vector<DependencyEdgeRecord>> RegistrySnapshot::direct_dependents(const DependencyNodeRef& node,
                                                                              const EdgeFilter& filter) const {
  return direct_dependents_or_dependencies(node, filter, false);
}

Result<std::vector<DependencyEdgeRecord>> RegistrySnapshot::direct_dependents_or_dependencies(
    const DependencyNodeRef& node, const EdgeFilter& filter, bool outgoing) const {
  if (!node.valid()) {
    return Result<std::vector<DependencyEdgeRecord>>::failure(ErrorCode::InvalidNodeReference,
                                                              "the node is not a valid node reference");
  }
  std::vector<DependencyEdgeRecord> result;
  const auto index = state_->node_index(node);
  if (!index.has_value()) {
    return result;
  }
  const auto& entry = state_->node_entries()[*index];
  const auto& list = outgoing ? entry.out_edges : entry.in_edges;
  const auto bound = state_->limits().max_direct_results;
  for (const std::uint32_t edge_index : list) {
    const auto& record = state_->edges()[edge_index];
    if (!filter.matches(record)) {
      continue;
    }
    if (result.size() >= bound) {
      return Result<std::vector<DependencyEdgeRecord>>::failure(
          ErrorCode::QueryResultLimitExceeded,
          "the lookup matches more edges than the configured maximum of " + std::to_string(bound));
    }
    result.push_back(record);
  }
  return result;
}

Result<NodeRefSet> RegistrySnapshot::unresolved_endpoints() const {
  std::vector<DependencyNodeRef> refs;
  const auto bound = state_->limits().max_query_roots;
  for (const auto& node : state_->nodes()) {
    if (state_->find_declared_ref(node) != nullptr) {
      continue;
    }
    if (refs.size() >= bound) {
      return Result<NodeRefSet>::failure(
          ErrorCode::QueryResultLimitExceeded,
          "more unresolved references than the configured maximum of " + std::to_string(bound));
    }
    refs.push_back(node);
  }
  return NodeRefSet::create(std::move(refs), bound);
}

Result<NodeRefSet> RegistrySnapshot::unreferenced_declared_refs() const {
  std::vector<DependencyNodeRef> refs;
  const auto bound = state_->limits().max_query_roots;
  for (const auto& declaration : state_->declared_refs()) {
    const auto index = state_->node_index(declaration.ref());
    if (index.has_value()) {
      const auto& entry = state_->node_entries()[*index];
      if (!entry.out_edges.empty() || !entry.in_edges.empty()) {
        continue;
      }
    }
    if (refs.size() >= bound) {
      return Result<NodeRefSet>::failure(
          ErrorCode::QueryResultLimitExceeded,
          "more unreferenced declarations than the configured maximum of " + std::to_string(bound));
    }
    refs.push_back(declaration.ref());
  }
  return NodeRefSet::create(std::move(refs), bound);
}

ContentDigest RegistrySnapshot::state_digest() const { return state_->state_digest(); }

ContentDigest RegistrySnapshot::content_digest() const { return state_->content_digest(); }

Result<std::vector<std::byte>> RegistrySnapshot::encode() const { return detail::encode_state(*state_, true); }

Result<TraversalResult> RegistrySnapshot::transitive_closure(const TraversalRequest& request) const {
  return detail::traverse(*state_, request);
}

Result<ImpactCone> RegistrySnapshot::impact_cone(const ImpactConeRequest& request) const {
  return detail::impact_cone_of(*state_, request);
}

Result<SccResult> RegistrySnapshot::strongly_connected_components(const ComponentRequest& request) const {
  return detail::strongly_connected_components_of(*state_, request);
}

Result<PathResult> RegistrySnapshot::explain_path(const PathRequest& request) const {
  return detail::find_path(*state_, request);
}

Result<CycleReport> RegistrySnapshot::enumerate_cycles(const CycleRequest& request) const {
  return detail::enumerate_cycles_of(*state_, request);
}

Result<AcyclicObligationReport> RegistrySnapshot::verify_acyclic_obligation(
    const CancellationToken& cancellation) const {
  return detail::verify_acyclic_obligation_of(*state_, cancellation);
}

}  // namespace facility_dependency_registry
