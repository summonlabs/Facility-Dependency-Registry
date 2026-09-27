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

#ifndef FACILITY_DEPENDENCY_REGISTRY_SRC_GRAPH_STATE_HPP
#define FACILITY_DEPENDENCY_REGISTRY_SRC_GRAPH_STATE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "facility_dependency_registry/cancel.hpp"
#include "facility_dependency_registry/digest.hpp"
#include "facility_dependency_registry/edge.hpp"
#include "facility_dependency_registry/errors.hpp"
#include "facility_dependency_registry/ids.hpp"
#include "facility_dependency_registry/limits.hpp"
#include "facility_dependency_registry/node_ref.hpp"
#include "facility_dependency_registry/snapshot.hpp"

namespace facility_dependency_registry {
namespace detail {

/// The only way to construct authoritative records. Records have no public
/// constructor, so no consumer and no other translation unit can fabricate one.
class EdgeRecordFactory {
 public:
  [[nodiscard]] static DependencyEdgeRecord make_edge(DependencyEdgeId id, EdgeRevision revision,
                                                      DependencyEdgeSpec spec, DependencyGeneration registered_generation,
                                                      DependencyGeneration last_modified_generation);

  [[nodiscard]] static DependencyEdgeRecord with_lifecycle(const DependencyEdgeRecord& base, LifecycleState state,
                                                           EdgeRevision revision,
                                                           DependencyGeneration last_modified_generation);

  [[nodiscard]] static DeclaredExternalRef make_declared_ref(DependencyNodeRef ref, ProvenanceRecord provenance,
                                                             DependencyGeneration declared_generation);
};

/// One node of the graph together with its adjacency, as traversable indices
/// into the edge list.
///
/// `out_edges` holds every edge that may be followed away from this node as a
/// source: the edges whose source is this node, plus every mutual edge that
/// touches it. `in_edges` is the mirror image. Both lists are ascending, which
/// is the canonical edge order, so iteration is deterministic without sorting.
struct NodeEntry {
  DependencyNodeRef ref{};
  std::vector<std::uint32_t> out_edges{};
  std::vector<std::uint32_t> in_edges{};
  bool declared = false;
};

/// The immutable authoritative state of one generation.
///
/// A GraphState is built once, validated completely, and never mutated. Every
/// snapshot shares one of these; every mutation builds a new one. This is what
/// makes the concurrency model trivial to reason about: readers can never
/// observe a partially applied mutation, because there is nothing to observe
/// half way.
class GraphState {
 public:
  GraphState() = delete;

  /// Builds and fully validates a state.
  ///
  /// Validation covers: canonical ordering and uniqueness of edges and
  /// declarations, ordinal consistency, every per-edge semantic rule (endpoint
  /// domains, direction, constraints, provenance), capacity limits, and the
  /// acyclic obligation. A state that would violate the acyclic obligation is
  /// rejected with ProhibitedCycle and the offending cycle in the detail.
  [[nodiscard]] static Result<std::shared_ptr<const GraphState>> build(const RegistryLimits& limits,
                                                                       DependencyGeneration generation,
                                                                       std::uint64_t next_edge_ordinal,
                                                                       std::vector<DependencyEdgeRecord> edges,
                                                                       std::vector<DeclaredExternalRef> refs);

  [[nodiscard]] const RegistryLimits& limits() const noexcept { return limits_; }
  [[nodiscard]] DependencyGeneration generation() const noexcept { return generation_; }
  [[nodiscard]] std::uint64_t next_edge_ordinal() const noexcept { return next_edge_ordinal_; }
  [[nodiscard]] std::span<const DependencyEdgeRecord> edges() const noexcept { return edges_; }
  [[nodiscard]] std::span<const DeclaredExternalRef> declared_refs() const noexcept { return refs_; }
  [[nodiscard]] std::span<const DependencyNodeRef> nodes() const noexcept;
  [[nodiscard]] std::span<const NodeEntry> node_entries() const noexcept { return nodes_; }

  [[nodiscard]] const DependencyEdgeRecord* find_edge(DependencyEdgeId id) const noexcept;
  [[nodiscard]] const DependencyEdgeRecord* find_edge(const EdgeKey& key) const noexcept;
  [[nodiscard]] const DeclaredExternalRef* find_declared_ref(const DependencyNodeRef& ref) const noexcept;
  [[nodiscard]] std::optional<std::uint32_t> node_index(const DependencyNodeRef& ref) const noexcept;
  [[nodiscard]] const DependencyNodeRef& node_at(std::uint32_t index) const noexcept;

  /// SHA-256 over the canonical encoding including the generation.
  [[nodiscard]] ContentDigest state_digest() const;
  /// SHA-256 over the canonical encoding with the generation field excluded.
  [[nodiscard]] ContentDigest content_digest() const;

 private:
  explicit GraphState(const RegistryLimits& limits) : limits_(limits) {}

  RegistryLimits limits_{};
  DependencyGeneration generation_{};
  std::uint64_t next_edge_ordinal_ = kFirstEdgeOrdinal;
  std::vector<DependencyEdgeRecord> edges_{};
  std::vector<DeclaredExternalRef> refs_{};
  std::vector<NodeEntry> nodes_{};
  std::vector<DependencyNodeRef> node_refs_{};
  std::unordered_map<EdgeKey, std::uint32_t> key_index_{};
  std::unordered_map<DependencyEdgeId, std::uint32_t> id_index_{};
  mutable std::once_flag digest_once_{};
  mutable ContentDigest state_digest_{};
  mutable ContentDigest content_digest_{};
};

/// Serializes a state into its canonical payload. `include_generation` selects
/// the state form (digest of the authoritative state) or the content form
/// (digest of the graph regardless of generation).
[[nodiscard]] Result<std::vector<std::byte>> encode_state(const GraphState& state, bool include_generation);

/// Decodes a canonical payload. Every field is validated before it is used to
/// allocate, and the decoded graph is put through the same full validation a
/// mutation would have to pass.
[[nodiscard]] Result<std::shared_ptr<const GraphState>> decode_state(std::span<const std::byte> payload,
                                                                     const RegistryLimits& limits);

/// The outcome of searching the acyclic obligation subgraph for a cycle.
///
/// The search stops at the first cycle it finds, exploring start nodes and
/// edges in canonical order, so the reported cycle is deterministic.
struct AcyclicSearchResult {
  bool satisfied = true;
  std::vector<DependencyNodeRef> cycle{};
  std::vector<DependencyEdgeId> cycle_edges{};
  DependencyKind closing_kind = DependencyKind::RequiresPowerFrom;
  bool cancelled = false;
  std::size_t nodes_examined = 0;
  std::size_t edges_examined = 0;
};

[[nodiscard]] AcyclicSearchResult search_acyclic_violation(const GraphState& state,
                                                           const CancellationToken& cancellation = {});

/// Formats a cycle as `a -> b -> c -> a` for diagnostics.
[[nodiscard]] std::string format_cycle(const std::vector<DependencyNodeRef>& nodes);

}  // namespace detail
}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_SRC_GRAPH_STATE_HPP
