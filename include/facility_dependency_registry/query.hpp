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

#ifndef FACILITY_DEPENDENCY_REGISTRY_QUERY_HPP
#define FACILITY_DEPENDENCY_REGISTRY_QUERY_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "facility_dependency_registry/cancel.hpp"
#include "facility_dependency_registry/edge.hpp"
#include "facility_dependency_registry/errors.hpp"
#include "facility_dependency_registry/ids.hpp"
#include "facility_dependency_registry/lifecycle.hpp"
#include "facility_dependency_registry/limits.hpp"
#include "facility_dependency_registry/mask.hpp"
#include "facility_dependency_registry/node_ref.hpp"
#include "facility_dependency_registry/provenance.hpp"

namespace facility_dependency_registry {

class RegistrySnapshot;

/// Which way a traversal reads the graph.
enum class TraversalDirection : std::uint8_t {
  /// Follow edges from source to target: what does this node depend on?
  Dependencies = 1,
  /// Follow edges from target to source: what depends on this node?
  Dependents = 2,
};

[[nodiscard]] std::string_view to_token(TraversalDirection direction) noexcept;

/// Why a traversal stopped.
enum class TraversalStop : std::uint8_t {
  /// The whole reachable set inside the request bounds was visited.
  Complete = 1,
  /// The request's depth bound stopped the walk. The result is partial.
  DepthLimit = 2,
  /// The request's node bound stopped the walk. The result is partial.
  NodeLimit = 3,
  /// Cancellation was observed. The partial result is discarded by the caller.
  Cancelled = 4,
  /// The request's result count bound stopped the walk. The result is partial.
  ResultLimit = 5,
};

[[nodiscard]] std::string_view to_token(TraversalStop stop) noexcept;

/// One visited node of a traversal.
struct ClosureEntry {
  DependencyNodeRef node{};
  /// Number of edges between the root and this node. Always at least 1.
  std::uint32_t depth = 0;
  /// The edge this node was first reached through.
  DependencyEdgeId via_edge{};
  /// The node the discovering edge led from.
  DependencyNodeRef via_node{};
};

/// A bounded transitive closure.
///
/// The result is deterministic: nodes are reported in breadth-first layer
/// order, and within one layer in the canonical node order (domain ordinal,
/// then identifier bytes). A node is reported exactly once, at its minimal
/// depth. When a node is reachable by several edges of the same layer, the
/// recorded discovering edge is the one found first while expanding that layer
/// in canonical node order and each node's edges in canonical edge order. The
/// node bound is applied after each layer is ordered canonically, so a
/// truncated result is always the canonically first part of the reachable set.
struct TraversalResult {
  DependencyNodeRef root{};
  TraversalDirection direction = TraversalDirection::Dependencies;
  DependencyGeneration generation{};
  std::vector<ClosureEntry> entries{};
  TraversalStop stop = TraversalStop::Complete;
  bool truncated = false;
  std::size_t edges_examined = 0;
  std::size_t nodes_examined = 0;

  [[nodiscard]] bool reached(const DependencyNodeRef& node) const noexcept;
  [[nodiscard]] std::optional<std::uint32_t> depth_of(const DependencyNodeRef& node) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
};

struct TraversalRequest {
  DependencyNodeRef root{};
  TraversalDirection direction = TraversalDirection::Dependencies;
  EdgeFilter filter{};
  /// At least 1 and at most RegistryLimits::max_traversal_depth.
  std::uint32_t max_depth = 8;
  /// At least 1 and at most RegistryLimits::max_traversal_nodes.
  std::uint32_t max_nodes = 4096;
  CancellationToken cancellation{};
};

/// A bounded impact cone: every node that transitively depends on the origin
/// through chains whose edges are all within the requested strength set.
struct ImpactEntry {
  DependencyNodeRef node{};
  std::uint32_t depth = 0;
  DependencyEdgeId via_edge{};
  DependencyNodeRef via_node{};
};

struct ImpactCone {
  DependencyNodeRef origin{};
  DependencyGeneration generation{};
  std::vector<ImpactEntry> entries{};
  TraversalStop stop = TraversalStop::Complete;
  bool truncated = false;
  std::size_t edges_examined = 0;
  std::size_t nodes_examined = 0;

  [[nodiscard]] std::size_t size() const noexcept { return entries.size(); }
  [[nodiscard]] bool reached(const DependencyNodeRef& node) const noexcept;
  [[nodiscard]] std::optional<std::uint32_t> depth_of(const DependencyNodeRef& node) const noexcept;
};

struct ImpactConeRequest {
  DependencyNodeRef origin{};
  /// Strength classes that propagate impact. Defaults to hard dependencies
  /// only, because that is the only reading this repository can state without
  /// inventing operational semantics it does not own.
  DependencyStrengthMask strengths{DependencyStrengthMask::of(DependencyStrength::Hard)};
  DependencyKindMask kinds{DependencyKindMask::all()};
  LifecycleMask lifecycles{LifecycleMask::of(LifecycleState::Active)};
  std::uint32_t max_depth = 8;
  std::uint32_t max_nodes = 4096;
  CancellationToken cancellation{};
};

/// One strongly connected component.
struct StronglyConnectedComponent {
  /// Members in canonical node order.
  std::vector<DependencyNodeRef> members{};
  /// Edges with both endpoints in this component, in canonical edge order.
  std::vector<DependencyEdgeId> edges{};
  /// True when the component contains more than one node, or one node with a
  /// mutual self-relation. These are exactly the legal cycles.
  bool cyclic = false;

  [[nodiscard]] std::size_t size() const noexcept { return members.size(); }
};

struct SccResult {
  DependencyGeneration generation{};
  /// Components ordered by their smallest member in canonical node order.
  /// A node belongs to at most one component.
  std::vector<StronglyConnectedComponent> components{};
  std::size_t nodes_examined = 0;
  std::size_t edges_examined = 0;
  std::size_t cyclic_components = 0;
  bool truncated = false;
  TraversalStop stop = TraversalStop::Complete;
};

struct ComponentRequest {
  EdgeFilter filter{};
  /// When false, only components of more than one member are reported. This is
  /// the default because a singleton component is not a cycle.
  bool include_singletons = false;
  /// At most RegistryLimits::max_analysis_nodes.
  std::uint32_t max_nodes = 250'000;
  CancellationToken cancellation{};
};

/// One hop of an explained path.
struct PathStep {
  DependencyEdgeId edge{};
  DependencyNodeRef from{};
  DependencyNodeRef to{};
  DependencyKind kind = DependencyKind::RequiresPowerFrom;
  DependencyStrength strength = DependencyStrength::Hard;
  Direction direction = Direction::DependsOn;
  LifecycleState lifecycle = LifecycleState::Active;

  [[nodiscard]] std::string to_text() const;
};

/// The deterministic shortest explanation of how one node reaches another.
struct PathResult {
  DependencyNodeRef from{};
  DependencyNodeRef to{};
  DependencyGeneration generation{};
  std::vector<PathStep> steps{};
  bool found = false;
  std::size_t edges_examined = 0;
  std::size_t nodes_examined = 0;
  TraversalStop stop = TraversalStop::Complete;

  [[nodiscard]] std::size_t hops() const noexcept { return steps.size(); }
};

struct PathRequest {
  DependencyNodeRef from{};
  DependencyNodeRef to{};
  TraversalDirection direction = TraversalDirection::Dependencies;
  EdgeFilter filter{};
  std::uint32_t max_depth = 8;
  std::uint32_t max_nodes = 4096;
  CancellationToken cancellation{};
};

/// One elementary cycle.
struct CycleRecord {
  /// Cycle members, rotated so that the canonically smallest member is first.
  std::vector<DependencyNodeRef> nodes{};
  /// `edges[i]` runs from `nodes[i]` to `nodes[i + 1]`, wrapping at the end.
  std::vector<DependencyEdgeId> edges{};

  [[nodiscard]] std::size_t length() const noexcept { return nodes.size(); }
  [[nodiscard]] bool contains(const DependencyNodeRef& node) const noexcept;
};

struct CycleReport {
  DependencyGeneration generation{};
  /// Cycles in a deterministic order: by length, then by member list.
  std::vector<CycleRecord> cycles{};
  std::size_t nodes_examined = 0;
  std::size_t edges_examined = 0;
  bool truncated = false;
  TraversalStop stop = TraversalStop::Complete;
};

struct CycleRequest {
  EdgeFilter filter{};
  /// At most RegistryLimits::max_cycle_length.
  std::uint32_t max_length = 8;
  /// At most RegistryLimits::max_cycles.
  std::uint32_t max_cycles = 64;
  /// At most RegistryLimits::max_analysis_nodes.
  std::uint32_t max_nodes = 250'000;
  CancellationToken cancellation{};
};

/// A cycle that violates the acyclic obligation.
struct ProhibitedCycle {
  CycleRecord cycle{};
  /// The kind that makes this cycle illegal.
  DependencyKind kind = DependencyKind::RequiresPowerFrom;
  [[nodiscard]] std::string to_text() const;
};

/// The result of re-checking the acyclic obligation over a decoded or mutated
/// graph. A well formed registry always reports `satisfied == true`; the check
/// exists so that a corrupted or hand-edited store is rejected instead of
/// silently trusted.
struct AcyclicObligationReport {
  DependencyGeneration generation{};
  bool satisfied = false;
  std::vector<ProhibitedCycle> violations{};
  std::size_t nodes_examined = 0;
  std::size_t edges_examined = 0;
  bool truncated = false;
};

/// What changed about one edge between two generations.
enum class EdgeChangeKind : std::uint8_t {
  Added = 1,
  Removed = 2,
  Modified = 3,
};

[[nodiscard]] std::string_view to_token(EdgeChangeKind kind) noexcept;

/// One changed field of one modified edge.
struct EdgeFieldChange {
  /// Stable field name: "strength", "direction", "lifecycle", "constraints",
  /// "provenance", "registered-generation" or "last-modified-generation".
  std::string field{};
  std::string before{};
  std::string after{};
};

struct EdgeChange {
  EdgeChangeKind kind = EdgeChangeKind::Added;
  DependencyEdgeId id{};
  EdgeKey key{};
  EdgeRevision before_revision{};
  EdgeRevision after_revision{};
  /// The record before the change. Id zero when the edge was added.
  DependencyEdgeRecord before{};
  /// The record after the change. Id zero when the edge was removed.
  DependencyEdgeRecord after{};
  /// Populated for Modified changes, in a stable field order.
  std::vector<EdgeFieldChange> fields{};

  [[nodiscard]] std::string to_text() const;
};

/// A change to the set of declared external references.
struct RefChange {
  bool added = false;
  DependencyNodeRef ref{};
  ProvenanceRecord before{};
  ProvenanceRecord after{};

  [[nodiscard]] std::string to_text() const;
};

/// The difference between two authoritative states, computed by a merge join
/// over the canonical edge order, so the cost is linear in the two graphs and
/// the output order does not depend on insertion history.
struct GenerationDiff {
  DependencyGeneration from{};
  DependencyGeneration to{};
  std::vector<EdgeChange> edge_changes{};
  std::vector<RefChange> ref_changes{};
  std::size_t unchanged_edges = 0;
  bool truncated = false;

  [[nodiscard]] std::size_t added() const noexcept;
  [[nodiscard]] std::size_t removed() const noexcept;
  [[nodiscard]] std::size_t modified() const noexcept;
  [[nodiscard]] bool empty() const noexcept;
};

/// Diffs two snapshots. `max_changes` bounds the number of reported changes;
/// when the bound is reached the diff stops, is marked truncated, and the
/// unexamined remainder is reported only through that flag.
[[nodiscard]] Result<GenerationDiff> diff_snapshots(const RegistrySnapshot& from, const RegistrySnapshot& to,
                                                    std::uint32_t max_changes, const CancellationToken& cancellation = {});

}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_QUERY_HPP
