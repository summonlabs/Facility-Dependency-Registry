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

#ifndef FACILITY_DEPENDENCY_REGISTRY_SRC_GRAPH_ALGORITHMS_HPP
#define FACILITY_DEPENDENCY_REGISTRY_SRC_GRAPH_ALGORITHMS_HPP

#include "facility_dependency_registry/cancel.hpp"
#include "facility_dependency_registry/errors.hpp"
#include "facility_dependency_registry/query.hpp"
#include "graph_state.hpp"

namespace facility_dependency_registry {
namespace detail {

/// Every algorithm here is iterative. The depth of a facility dependency graph
/// is chosen by whoever declares the dependencies, so nothing in this file may
/// recurse on graph structure.

[[nodiscard]] Result<TraversalResult> traverse(const GraphState& state, const TraversalRequest& request);
[[nodiscard]] Result<ImpactCone> impact_cone_of(const GraphState& state, const ImpactConeRequest& request);
[[nodiscard]] Result<SccResult> strongly_connected_components_of(const GraphState& state,
                                                                const ComponentRequest& request);
[[nodiscard]] Result<PathResult> find_path(const GraphState& state, const PathRequest& request);
[[nodiscard]] Result<CycleReport> enumerate_cycles_of(const GraphState& state, const CycleRequest& request);
[[nodiscard]] Result<AcyclicObligationReport> verify_acyclic_obligation_of(const GraphState& state,
                                                                          const CancellationToken& cancellation);

}  // namespace detail
}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_SRC_GRAPH_ALGORITHMS_HPP
