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

#include "facility_dependency_registry/facility_dependency_registry.hpp"
#include "test_harness.hpp"

using namespace facility_dependency_registry;

FDEP_TEST(limits, defaults_are_valid) {
  const RegistryLimits limits;
  FDEP_CHECK(limits.validate().ok());
  FDEP_CHECK(default_limits().validate().ok());
  FDEP_CHECK(limits.max_edges > 0);
  FDEP_CHECK(limits.max_traversal_nodes >= limits.max_traversal_depth);
  FDEP_CHECK(limits.max_cycle_length <= limits.max_analysis_nodes);
}

FDEP_TEST(limits, zero_is_never_a_valid_bound) {
  RegistryLimits limits;
  limits.max_edges = 0;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_declared_refs = 0;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_constraints_per_edge = 0;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_id_length = 0;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_annotation_length = 0;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_direct_results = 0;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_traversal_depth = 0;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_traversal_nodes = 0;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_analysis_nodes = 0;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_cycles = 0;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_query_roots = 0;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_diff_changes = 0;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_journal_entries = 0;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_retained_generations = 0;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_generation = 0;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);
}

FDEP_TEST(limits, hard_ceilings_cannot_be_raised) {
  RegistryLimits limits;
  limits.max_edges = kHardMaxEdges + 1;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_declared_refs = kHardMaxDeclaredRefs + 1;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_constraints_per_edge = static_cast<std::uint16_t>(kHardMaxConstraintsPerEdge + 1);
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_id_length = kHardMaxIdLength + 1;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_annotation_length = kHardMaxAnnotationLength + 1;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_retained_generations = kHardMaxRetainedGenerations + 1;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_persisted_bytes = kHardMaxPersistedBytes + 1;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_persisted_bytes = 1024;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_generation = kHardMaxGeneration + 1;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_traversal_depth = 8;
  limits.max_traversal_nodes = 4;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_cycle_length = 1;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);

  limits = RegistryLimits{};
  limits.max_cycle_length = 64;
  limits.max_analysis_nodes = 16;
  FDEP_CHECK_STATUS(limits.validate(), ErrorCode::InvalidLimits);
}

FDEP_TEST(limits, capacity_helpers_never_overflow) {
  RegistryLimits limits;
  limits.max_edges = 10;
  FDEP_CHECK(limits.has_edge_capacity(0, 10));
  FDEP_CHECK(limits.has_edge_capacity(9, 1));
  FDEP_CHECK(!limits.has_edge_capacity(10, 1));
  FDEP_CHECK(!limits.has_edge_capacity(11, 1));
  FDEP_CHECK(!limits.has_edge_capacity(0, UINT64_MAX));
  FDEP_CHECK(limits.has_edge_capacity(0, 0));

  limits.max_declared_refs = 5;
  FDEP_CHECK(limits.has_declared_ref_capacity(5, 0));
  FDEP_CHECK(!limits.has_declared_ref_capacity(5, 1));
  FDEP_CHECK(!limits.has_declared_ref_capacity(0, UINT64_MAX));

  limits.max_generation = 7;
  FDEP_CHECK(limits.has_generation_capacity(6));
  FDEP_CHECK(!limits.has_generation_capacity(7));
  FDEP_CHECK(!limits.has_generation_capacity(UINT64_MAX));
}
