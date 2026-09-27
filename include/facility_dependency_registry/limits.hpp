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

#ifndef FACILITY_DEPENDENCY_REGISTRY_LIMITS_HPP
#define FACILITY_DEPENDENCY_REGISTRY_LIMITS_HPP

#include <cstdint>

#include "facility_dependency_registry/errors.hpp"

namespace facility_dependency_registry {

/// Hard ceilings. A configured limit may never exceed these, whatever a
/// configuration file, a CLI argument or an embedded store header claims.
inline constexpr std::uint32_t kHardMaxEdges = 8'388'608;
inline constexpr std::uint32_t kHardMaxDeclaredRefs = 8'388'608;
inline constexpr std::uint16_t kHardMaxConstraintsPerEdge = 16;
inline constexpr std::uint32_t kHardMaxIdLength = 160;
inline constexpr std::uint32_t kHardMaxAnnotationLength = 4096;
inline constexpr std::uint32_t kHardMaxDirectResults = 1'048'576;
inline constexpr std::uint32_t kHardMaxTraversalDepth = 4096;
inline constexpr std::uint32_t kHardMaxTraversalNodes = 8'388'608;
inline constexpr std::uint32_t kHardMaxAnalysisNodes = 8'388'608;
inline constexpr std::uint32_t kHardMaxCycles = 65'536;
inline constexpr std::uint32_t kHardMaxCycleLength = 256;
inline constexpr std::uint32_t kHardMaxQueryRoots = 65'536;
inline constexpr std::uint32_t kHardMaxDiffChanges = 8'388'608;
inline constexpr std::uint32_t kHardMaxJournalEntries = 1'048'576;
inline constexpr std::uint32_t kHardMaxRetainedGenerations = 8;
inline constexpr std::uint64_t kHardMaxPersistedBytes = 4'294'967'296ull;  // 4 GiB
inline constexpr std::uint64_t kHardMaxGeneration = (1ull << 48) - 1;

/// Bounded resource configuration of one registry.
///
/// Every bound below is enforced before allocation or before a traversal
/// expands, never after. The defaults are sized for a single large facility;
/// a consumer that manages a very large estate raises them explicitly, and
/// `validate()` refuses a configuration whose values are incoherent.
struct RegistryLimits {
  /// Maximum number of edges in the graph.
  std::uint32_t max_edges = 200'000;
  /// Maximum number of explicitly declared external references.
  std::uint32_t max_declared_refs = 200'000;
  /// Maximum number of constraint records carried by one edge.
  std::uint16_t max_constraints_per_edge = 8;
  /// Maximum length in bytes of an external identifier.
  std::uint32_t max_id_length = kHardMaxIdLength;
  /// Maximum length in bytes of a provenance annotation.
  std::uint32_t max_annotation_length = 512;
  /// Maximum number of edges returned by one direct lookup.
  std::uint32_t max_direct_results = 4096;
  /// Maximum traversal depth a caller may request.
  std::uint32_t max_traversal_depth = 64;
  /// Maximum number of nodes any single traversal may visit.
  std::uint32_t max_traversal_nodes = 250'000;
  /// Maximum number of nodes any single whole-graph analysis may visit.
  std::uint32_t max_analysis_nodes = 250'000;
  /// Maximum number of elementary cycles enumerated by one request.
  std::uint32_t max_cycles = 1'024;
  /// Maximum length of an enumerated elementary cycle.
  std::uint32_t max_cycle_length = 32;
  /// Maximum number of root references in one batch query.
  std::uint32_t max_query_roots = 1'024;
  /// Maximum number of changes reported by one generation diff.
  std::uint32_t max_diff_changes = 262'144;
  /// Maximum number of in-memory mutation journal entries retained.
  std::uint32_t max_journal_entries = 4'096;
  /// Maximum number of generation files a durable store retains, including the
  /// one its pointer names. A value of 1 keeps only the generation the pointer
  /// names, which is a valid choice but removes the ability to fall back to an
  /// earlier generation when the newest one fails its integrity check.
  std::uint32_t max_retained_generations = 2;
  /// Maximum size in bytes of one durable generation file.
  std::uint64_t max_persisted_bytes = 134'217'728ull;  // 128 MiB
  /// Maximum generation a registry will advance to.
  std::uint64_t max_generation = kHardMaxGeneration;

  /// Rejects incoherent configurations. Returns the first violation found, in
  /// field declaration order, so that the outcome is deterministic.
  [[nodiscard]] Status validate() const;

  /// True when `count` further edges still fit inside `max_edges`.
  [[nodiscard]] bool has_edge_capacity(std::uint64_t current, std::uint64_t additional) const noexcept;

  /// True when `count` further declarations still fit inside `max_declared_refs`.
  [[nodiscard]] bool has_declared_ref_capacity(std::uint64_t current, std::uint64_t additional) const noexcept;

  /// True when `generation` is still strictly below `max_generation`, so that
  /// publishing the next generation cannot overflow.
  [[nodiscard]] bool has_generation_capacity(std::uint64_t generation) const noexcept;
};

/// The limits a registry uses unless the caller configures otherwise.
[[nodiscard]] RegistryLimits default_limits() noexcept;

}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_LIMITS_HPP
