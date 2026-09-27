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

#ifndef FACILITY_DEPENDENCY_REGISTRY_SNAPSHOT_HPP
#define FACILITY_DEPENDENCY_REGISTRY_SNAPSHOT_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "facility_dependency_registry/digest.hpp"
#include "facility_dependency_registry/edge.hpp"
#include "facility_dependency_registry/errors.hpp"
#include "facility_dependency_registry/ids.hpp"
#include "facility_dependency_registry/limits.hpp"
#include "facility_dependency_registry/node_ref.hpp"
#include "facility_dependency_registry/provenance.hpp"
#include "facility_dependency_registry/query.hpp"

namespace facility_dependency_registry {

namespace detail {
class GraphState;
}  // namespace detail

/// One observed external identity.
///
/// A declaration records that some source reported this identity as existing.
/// It carries provenance and the generation in which it was declared, and
/// nothing else: the registry makes no claim about the object behind it.
class DeclaredExternalRef {
 public:
  DeclaredExternalRef() = default;

  [[nodiscard]] const DependencyNodeRef& ref() const noexcept { return ref_; }
  [[nodiscard]] const ProvenanceRecord& provenance() const noexcept { return provenance_; }
  [[nodiscard]] DependencyGeneration declared_generation() const noexcept { return declared_generation_; }
  [[nodiscard]] std::string to_text() const;

  friend bool operator==(const DeclaredExternalRef& lhs, const DeclaredExternalRef& rhs) noexcept {
    return lhs.ref_ == rhs.ref_ && lhs.provenance_ == rhs.provenance_ &&
           lhs.declared_generation_ == rhs.declared_generation_;
  }
  friend bool operator!=(const DeclaredExternalRef& lhs, const DeclaredExternalRef& rhs) noexcept {
    return !(lhs == rhs);
  }
  friend std::strong_ordering operator<=>(const DeclaredExternalRef& lhs, const DeclaredExternalRef& rhs) noexcept {
    return lhs.ref_ <=> rhs.ref_;
  }

 private:
  friend class detail::GraphState;
  friend class detail::EdgeRecordFactory;
  DependencyNodeRef ref_{};
  ProvenanceRecord provenance_{};
  DependencyGeneration declared_generation_{};
};

/// Whether an endpoint is known to exist.
enum class RefResolution : std::uint8_t {
  /// An explicit declaration for this identity is stored.
  Declared = 1,
  /// The identity is referenced by at least one stored edge but has never been
  /// declared. It stays an unresolved reference; the registry never invents a
  /// node definition to make the graph look complete.
  Unresolved = 2,
  /// Nothing references or declares this identity.
  Absent = 3,
};

[[nodiscard]] std::string_view to_token(RefResolution resolution) noexcept;

/// An immutable, self-contained view of one authoritative generation.
///
/// A snapshot owns its state, is cheap to copy (it shares one immutable state
/// block) and never changes. Every read-only query in this repository is a
/// method of this type, which is what keeps mutation commands and inspection
/// cleanly separated: a consumer that holds a snapshot cannot mutate anything,
/// and a mutation can never invalidate a query that is already running.
///
/// Lifetimes: the spans and pointers returned by this type point into the
/// snapshot's own state and stay valid exactly as long as the snapshot does.
/// Copying the snapshot extends the lifetime of the shared state, not of the
/// individual views borrowed from one snapshot object.
class RegistrySnapshot {
 public:
  /// An empty snapshot at generation zero.
  RegistrySnapshot();

  ~RegistrySnapshot();
  RegistrySnapshot(const RegistrySnapshot&) noexcept;
  RegistrySnapshot& operator=(const RegistrySnapshot&) noexcept;
  RegistrySnapshot(RegistrySnapshot&&) noexcept;
  RegistrySnapshot& operator=(RegistrySnapshot&&) noexcept;

  /// The empty graph at generation zero under the given limits.
  [[nodiscard]] static RegistrySnapshot empty(const RegistryLimits& limits = {});

  /// Decodes a canonical state payload.
  ///
  /// Every field is validated before it is used to allocate: counts are
  /// checked against `limits`, lengths against the configured maxima, enums
  /// against their declared domains, ordering against the canonical order, and
  /// the result against the semantic rules a mutation would have enforced.
  /// A payload that the mutation API could not have produced is rejected.
  [[nodiscard]] static Result<RegistrySnapshot> decode(std::span<const std::byte> payload,
                                                       const RegistryLimits& limits);

  [[nodiscard]] DependencyGeneration generation() const noexcept;
  [[nodiscard]] const RegistryLimits& limits() const noexcept;
  [[nodiscard]] std::size_t edge_count() const noexcept;
  [[nodiscard]] std::size_t declared_ref_count() const noexcept;
  [[nodiscard]] std::size_t node_count() const noexcept;
  /// The ordinal the next edge will receive. Strictly greater than every
  /// stored edge identity.
  [[nodiscard]] std::uint64_t next_edge_ordinal() const noexcept;

  /// Every stored edge, in canonical order: by key, then by identity.
  [[nodiscard]] std::span<const DependencyEdgeRecord> edges() const noexcept;
  /// Every declaration, in canonical node order.
  [[nodiscard]] std::span<const DeclaredExternalRef> declared_refs() const noexcept;
  /// Every node that is either an endpoint of a stored edge or a declaration,
  /// in canonical order.
  [[nodiscard]] std::span<const DependencyNodeRef> nodes() const noexcept;

  /// Null when absent. The pointer is valid as long as this snapshot.
  [[nodiscard]] const DependencyEdgeRecord* find_edge(DependencyEdgeId id) const noexcept;
  [[nodiscard]] const DependencyEdgeRecord* find_edge(const EdgeKey& key) const noexcept;
  [[nodiscard]] const DeclaredExternalRef* find_declared_ref(const DependencyNodeRef& ref) const noexcept;
  [[nodiscard]] bool has_node(const DependencyNodeRef& ref) const noexcept;
  [[nodiscard]] RefResolution resolution_of(const DependencyNodeRef& ref) const noexcept;

  /// Bounded direct lookups, in canonical edge order. The result bound is
  /// RegistryLimits::max_direct_results and is applied while collecting, so a
  /// node with an enormous fan-in is rejected rather than materialized.
  [[nodiscard]] Result<std::vector<DependencyEdgeRecord>> direct_dependencies(const DependencyNodeRef& node,
                                                                             const EdgeFilter& filter = {}) const;
  [[nodiscard]] Result<std::vector<DependencyEdgeRecord>> direct_dependents(const DependencyNodeRef& node,
                                                                           const EdgeFilter& filter = {}) const;

  /// Endpoint identities that are referenced but never declared, in canonical
  /// order.
  [[nodiscard]] Result<NodeRefSet> unresolved_endpoints() const;
  /// Declared identities that no stored edge references, in canonical order.
  [[nodiscard]] Result<NodeRefSet> unreferenced_declared_refs() const;

  /// SHA-256 over the canonical encoding of this state, including its
  /// generation. Two snapshots have equal state digests exactly when they are
  /// byte identical canonical states.
  [[nodiscard]] ContentDigest state_digest() const;
  /// SHA-256 over the canonical encoding of the graph content with the
  /// generation field excluded, so that "the same graph at a different
  /// generation" is recognizable.
  [[nodiscard]] ContentDigest content_digest() const;

  /// The canonical byte encoding, the same bytes a durable generation file
  /// carries as its payload.
  [[nodiscard]] Result<std::vector<std::byte>> encode() const;

  // -- graph algorithms ------------------------------------------------
  [[nodiscard]] Result<TraversalResult> transitive_closure(const TraversalRequest& request) const;
  [[nodiscard]] Result<ImpactCone> impact_cone(const ImpactConeRequest& request) const;
  [[nodiscard]] Result<SccResult> strongly_connected_components(const ComponentRequest& request) const;
  [[nodiscard]] Result<PathResult> explain_path(const PathRequest& request) const;
  [[nodiscard]] Result<CycleReport> enumerate_cycles(const CycleRequest& request) const;
  /// Re-checks that no directed edge of an acyclic-required kind takes part in
  /// a cycle. Bounded by RegistryLimits::max_analysis_nodes.
  [[nodiscard]] Result<AcyclicObligationReport> verify_acyclic_obligation(
      const CancellationToken& cancellation = {}) const;

  /// Internal state handle. Not part of the supported surface: the type is
  /// incomplete outside this library, so a consumer can hold it but not
  /// inspect it.
  [[nodiscard]] const std::shared_ptr<const detail::GraphState>& detail_state() const noexcept { return state_; }

 private:
  friend class DependencyRegistry;
  friend Result<RegistrySnapshot> make_snapshot(std::shared_ptr<const detail::GraphState> state);
  explicit RegistrySnapshot(std::shared_ptr<const detail::GraphState> state) noexcept : state_(std::move(state)) {}

  [[nodiscard]] Result<std::vector<DependencyEdgeRecord>> direct_dependents_or_dependencies(
      const DependencyNodeRef& node, const EdgeFilter& filter, bool outgoing) const;

  std::shared_ptr<const detail::GraphState> state_;
};

/// Wraps an already constructed internal state. Used by the registry and by
/// the durable store; not part of the consumer surface.
[[nodiscard]] Result<RegistrySnapshot> make_snapshot(std::shared_ptr<const detail::GraphState> state);

}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_SNAPSHOT_HPP
