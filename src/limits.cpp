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

#include "facility_dependency_registry/limits.hpp"

#include <string>

namespace facility_dependency_registry {
namespace {

Status reject(std::string_view field, std::string_view reason) {
  std::string detail{"limits."};
  detail.append(field);
  detail.append(" ");
  detail.append(reason);
  return Status::failure(ErrorCode::InvalidLimits, std::move(detail));
}

// Every numeric field is checked once: non-zero, at most the hard ceiling.
template <class T>
Status check_field(std::string_view field, T value, T hard_max) {
  if (value == T{0}) {
    return reject(field, "must be greater than zero");
  }
  if (value > hard_max) {
    return reject(field, "exceeds the hard ceiling");
  }
  return Status::success();
}

}  // namespace

Status RegistryLimits::validate() const {
  if (const auto status = check_field("max_edges", max_edges, kHardMaxEdges); !status.ok()) {
    return status;
  }
  if (const auto status = check_field("max_declared_refs", max_declared_refs, kHardMaxDeclaredRefs); !status.ok()) {
    return status;
  }
  if (const auto status = check_field("max_constraints_per_edge", max_constraints_per_edge,
                                      kHardMaxConstraintsPerEdge);
      !status.ok()) {
    return status;
  }
  if (const auto status = check_field("max_id_length", max_id_length, kHardMaxIdLength); !status.ok()) {
    return status;
  }
  if (const auto status = check_field("max_annotation_length", max_annotation_length, kHardMaxAnnotationLength);
      !status.ok()) {
    return status;
  }
  if (const auto status = check_field("max_direct_results", max_direct_results, kHardMaxDirectResults);
      !status.ok()) {
    return status;
  }
  if (const auto status = check_field("max_traversal_depth", max_traversal_depth, kHardMaxTraversalDepth);
      !status.ok()) {
    return status;
  }
  if (const auto status = check_field("max_traversal_nodes", max_traversal_nodes, kHardMaxTraversalNodes);
      !status.ok()) {
    return status;
  }
  if (const auto status = check_field("max_analysis_nodes", max_analysis_nodes, kHardMaxAnalysisNodes);
      !status.ok()) {
    return status;
  }
  if (const auto status = check_field("max_cycles", max_cycles, kHardMaxCycles); !status.ok()) {
    return status;
  }
  if (max_cycle_length < 2) {
    return reject("max_cycle_length", "must be at least 2");
  }
  if (const auto status = check_field("max_cycle_length", max_cycle_length, kHardMaxCycleLength); !status.ok()) {
    return status;
  }
  if (max_cycle_length > max_analysis_nodes) {
    return reject("max_cycle_length", "must not exceed max_analysis_nodes");
  }
  if (const auto status = check_field("max_query_roots", max_query_roots, kHardMaxQueryRoots); !status.ok()) {
    return status;
  }
  if (const auto status = check_field("max_diff_changes", max_diff_changes, kHardMaxDiffChanges); !status.ok()) {
    return status;
  }
  if (const auto status = check_field("max_journal_entries", max_journal_entries, kHardMaxJournalEntries);
      !status.ok()) {
    return status;
  }
  if (const auto status = check_field("max_retained_generations", max_retained_generations,
                                      kHardMaxRetainedGenerations);
      !status.ok()) {
    return status;
  }
  if (max_persisted_bytes < 4096) {
    return reject("max_persisted_bytes", "must be at least 4096 bytes");
  }
  if (max_persisted_bytes > kHardMaxPersistedBytes) {
    return reject("max_persisted_bytes", "exceeds the hard ceiling");
  }
  if (max_generation == 0) {
    return reject("max_generation", "must be greater than zero");
  }
  if (max_generation > kHardMaxGeneration) {
    return reject("max_generation", "exceeds the hard ceiling");
  }
  if (max_traversal_nodes < max_traversal_depth) {
    return reject("max_traversal_nodes", "must be at least max_traversal_depth");
  }
  return Status::success();
}

bool RegistryLimits::has_edge_capacity(std::uint64_t current, std::uint64_t additional) const noexcept {
  return current <= max_edges && additional <= static_cast<std::uint64_t>(max_edges) - current;
}

bool RegistryLimits::has_declared_ref_capacity(std::uint64_t current, std::uint64_t additional) const noexcept {
  return current <= max_declared_refs && additional <= static_cast<std::uint64_t>(max_declared_refs) - current;
}

bool RegistryLimits::has_generation_capacity(std::uint64_t generation) const noexcept {
  return generation < max_generation;
}

RegistryLimits default_limits() noexcept { return RegistryLimits{}; }

}  // namespace facility_dependency_registry
