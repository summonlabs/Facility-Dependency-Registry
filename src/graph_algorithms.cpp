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

#include "graph_algorithms.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace facility_dependency_registry {
namespace detail {
namespace {

constexpr std::uint32_t kNoNode = 0xFFFFFFFFu;
constexpr std::size_t kCancellationCheckInterval = 4096;

/// The other end of an edge from `node`'s point of view. Correct for directed
/// edges (which appear in exactly one of the two adjacency lists) and for
/// mutual edges (which appear in both, so the answer is the opposite
/// endpoint).
const DependencyNodeRef& opposite_endpoint(const NodeEntry& entry, const DependencyEdgeRecord& record) {
  return record.source() == entry.ref ? record.target() : record.source();
}

struct Candidate {
  std::uint32_t node = kNoNode;
  ClosureEntry entry{};
};

bool validate_traversal_bounds(const RegistryLimits& limits, std::uint32_t max_depth, std::uint32_t max_nodes,
                               std::string_view what, std::string& detail) {
  if (max_depth == 0 || max_depth > limits.max_traversal_depth) {
    detail.assign(what);
    detail.append(" depth must be between 1 and ");
    detail.append(std::to_string(limits.max_traversal_depth));
    return false;
  }
  if (max_nodes == 0 || max_nodes > limits.max_traversal_nodes) {
    detail.assign(what);
    detail.append(" node bound must be between 1 and ");
    detail.append(std::to_string(limits.max_traversal_nodes));
    return false;
  }
  return true;
}

const std::vector<std::uint32_t>& adjacency_for(const NodeEntry& entry, TraversalDirection direction) {
  return direction == TraversalDirection::Dependencies ? entry.out_edges : entry.in_edges;
}

bool direction_is_valid(TraversalDirection direction) noexcept {
  return direction == TraversalDirection::Dependencies || direction == TraversalDirection::Dependents;
}

/// True when at least one node of `frontier` still has a matching edge to a
/// node that has not been visited. Used so that "truncated by depth" is
/// reported exactly, rather than guessed.
bool frontier_has_unvisited_neighbour(const GraphState& state, const std::vector<std::uint32_t>& frontier,
                                      const std::vector<std::uint8_t>& visited, const TraversalRequest& request) {
  for (const std::uint32_t node : frontier) {
    const auto& entry = state.node_entries()[node];
    for (const std::uint32_t edge_index : adjacency_for(entry, request.direction)) {
      const auto& record = state.edges()[edge_index];
      if (!request.filter.matches(record)) {
        continue;
      }
      const auto neighbour = state.node_index(opposite_endpoint(entry, record));
      if (neighbour.has_value() && visited[*neighbour] == 0) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

Result<TraversalResult> traverse(const GraphState& state, const TraversalRequest& request) {
  const auto& limits = state.limits();
  if (!request.root.valid()) {
    return Result<TraversalResult>::failure(ErrorCode::InvalidNodeReference,
                                            "the traversal root is not a valid node reference");
  }
  if (!direction_is_valid(request.direction)) {
    return Result<TraversalResult>::failure(ErrorCode::InvalidArguments,
                                            "the traversal direction is not one of the declared directions");
  }
  std::string detail;
  if (!validate_traversal_bounds(limits, request.max_depth, request.max_nodes, "traversal", detail)) {
    return Result<TraversalResult>::failure(ErrorCode::RequestLimitExceeded, std::move(detail));
  }

  TraversalResult result;
  result.root = request.root;
  result.direction = request.direction;
  result.generation = state.generation();

  const auto start = state.node_index(request.root);
  if (!start.has_value()) {
    return result;  // a node the graph has never seen has no closure
  }

  const std::size_t node_count = state.node_entries().size();
  std::vector<std::uint8_t> visited(node_count, 0);
  std::vector<std::uint32_t> stamp(node_count, 0);
  std::uint32_t current_stamp = 0;

  std::vector<std::uint32_t> frontier;
  frontier.push_back(*start);
  visited[*start] = 1;
  result.nodes_examined = 1;

  std::uint32_t depth = 0;
  std::size_t since_cancel_check = 0;

  while (!frontier.empty() && depth < request.max_depth) {
    if (request.cancellation.cancelled()) {
      return Result<TraversalResult>::failure(ErrorCode::Cancelled,
                                              "the traversal was cancelled before it completed");
    }
    ++depth;
    ++current_stamp;

    std::vector<Candidate> candidates;
    for (const std::uint32_t node : frontier) {
      const auto& entry = state.node_entries()[node];
      for (const std::uint32_t edge_index : adjacency_for(entry, request.direction)) {
        const auto& record = state.edges()[edge_index];
        ++result.edges_examined;
        if (++since_cancel_check >= kCancellationCheckInterval) {
          since_cancel_check = 0;
          if (request.cancellation.cancelled()) {
            return Result<TraversalResult>::failure(ErrorCode::Cancelled,
                                                    "the traversal was cancelled before it completed");
          }
        }
        if (!request.filter.matches(record)) {
          continue;
        }
        const auto neighbour = state.node_index(opposite_endpoint(entry, record));
        if (!neighbour.has_value() || visited[*neighbour] != 0 || stamp[*neighbour] == current_stamp) {
          continue;
        }
        stamp[*neighbour] = current_stamp;
        Candidate candidate;
        candidate.node = *neighbour;
        candidate.entry.node = state.node_at(*neighbour);
        candidate.entry.depth = depth;
        candidate.entry.via_edge = record.id();
        candidate.entry.via_node = entry.ref;
        candidates.push_back(std::move(candidate));
      }
    }

    // Candidates are ordered canonically before the node bound is applied, so
    // a truncated result is deterministic and always the canonically first
    // part of the reachable set.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& lhs, const Candidate& rhs) { return lhs.node < rhs.node; });

    const std::size_t budget = request.max_nodes - result.entries.size();
    if (candidates.size() > budget) {
      candidates.resize(budget);
      for (auto& candidate : candidates) {
        visited[candidate.node] = 1;
        result.entries.push_back(std::move(candidate.entry));
      }
      result.nodes_examined = result.entries.size() + 1;
      result.stop = TraversalStop::NodeLimit;
      result.truncated = true;
      return result;
    }

    frontier.clear();
    for (auto& candidate : candidates) {
      visited[candidate.node] = 1;
      frontier.push_back(candidate.node);
      result.entries.push_back(std::move(candidate.entry));
    }
    result.nodes_examined = result.entries.size() + 1;
  }

  if (!frontier.empty() && frontier_has_unvisited_neighbour(state, frontier, visited, request)) {
    result.stop = TraversalStop::DepthLimit;
    result.truncated = true;
  }
  return result;
}

Result<ImpactCone> impact_cone_of(const GraphState& state, const ImpactConeRequest& request) {
  if (!request.origin.valid()) {
    return Result<ImpactCone>::failure(ErrorCode::InvalidNodeReference,
                                       "the impact origin is not a valid node reference");
  }
  EdgeFilter filter;
  filter.kinds = request.kinds;
  filter.strengths = request.strengths;
  filter.lifecycles = request.lifecycles;

  TraversalRequest traversal;
  traversal.root = request.origin;
  traversal.direction = TraversalDirection::Dependents;
  traversal.filter = filter;
  traversal.max_depth = request.max_depth;
  traversal.max_nodes = request.max_nodes;
  traversal.cancellation = request.cancellation;

  auto closure = traverse(state, traversal);
  if (!closure) {
    return Result<ImpactCone>::failure(closure.error());
  }

  ImpactCone cone;
  cone.origin = request.origin;
  cone.generation = closure.value().generation;
  cone.stop = closure.value().stop;
  cone.truncated = closure.value().truncated;
  cone.edges_examined = closure.value().edges_examined;
  cone.nodes_examined = closure.value().nodes_examined;
  cone.entries.reserve(closure.value().entries.size());
  for (const auto& entry : closure.value().entries) {
    ImpactEntry impacted;
    impacted.node = entry.node;
    impacted.depth = entry.depth;
    impacted.via_edge = entry.via_edge;
    impacted.via_node = entry.via_node;
    cone.entries.push_back(std::move(impacted));
  }
  return cone;
}

Result<SccResult> strongly_connected_components_of(const GraphState& state, const ComponentRequest& request) {
  const auto& limits = state.limits();
  if (request.max_nodes == 0 || request.max_nodes > limits.max_analysis_nodes) {
    return Result<SccResult>::failure(
        ErrorCode::RequestLimitExceeded,
        "the analysis node bound must be between 1 and " + std::to_string(limits.max_analysis_nodes));
  }

  const auto node_count = state.node_entries().size();
  std::vector<std::uint32_t> compact(node_count, kNoNode);
  std::vector<std::uint32_t> members;
  members.reserve(node_count);
  std::size_t edges_examined = 0;

  for (const auto& record : state.edges()) {
    if (!request.filter.matches(record)) {
      continue;
    }
    ++edges_examined;
    const auto source = state.node_index(record.source());
    const auto target = state.node_index(record.target());
    if (!source.has_value() || !target.has_value()) {
      continue;
    }
    for (const std::uint32_t node : {*source, *target}) {
      if (compact[node] == kNoNode) {
        compact[node] = static_cast<std::uint32_t>(members.size());
        members.push_back(node);
      }
    }
  }

  std::sort(members.begin(), members.end());
  for (std::size_t position = 0; position < members.size(); ++position) {
    compact[members[position]] = static_cast<std::uint32_t>(position);
  }

  SccResult result;
  result.generation = state.generation();
  result.nodes_examined = members.size();
  result.edges_examined = edges_examined;

  if (members.empty()) {
    return result;
  }
  if (members.size() > request.max_nodes) {
    return Result<SccResult>::failure(
        ErrorCode::AnalysisLimitExceeded,
        "the analysis would visit " + std::to_string(members.size()) +
            " nodes, above the configured bound of " + std::to_string(request.max_nodes));
  }

  const std::size_t count = members.size();
  std::vector<std::uint32_t> forward_begin(count + 1, 0);
  std::vector<std::uint32_t> reverse_begin(count + 1, 0);
  std::vector<std::uint32_t> forward_edges;
  std::vector<std::uint32_t> reverse_edges;
  forward_edges.reserve(edges_examined);
  reverse_edges.reserve(edges_examined);
  // A mutual edge is a traversable arc in both directions, so it contributes
  // one arc to each endpoint in each adjacency. Counting them here and filling
  // them below use the same rule, which is what keeps the two passes
  // consistent.
  for (const auto& record : state.edges()) {
    if (!request.filter.matches(record)) {
      continue;
    }
    const auto source = state.node_index(record.source());
    const auto target = state.node_index(record.target());
    if (!source.has_value() || !target.has_value()) {
      continue;
    }
    forward_begin[compact[*source] + 1] += 1;
    reverse_begin[compact[*target] + 1] += 1;
    if (record.direction() == Direction::Mutual) {
      forward_begin[compact[*target] + 1] += 1;
      reverse_begin[compact[*source] + 1] += 1;
    }
  }
  for (std::size_t index = 0; index < count; ++index) {
    forward_begin[index + 1] += forward_begin[index];
    reverse_begin[index + 1] += reverse_begin[index];
  }
  forward_edges.resize(forward_begin[count]);
  reverse_edges.resize(reverse_begin[count]);
  {
    std::vector<std::uint32_t> forward_cursor(forward_begin.begin(), forward_begin.end() - 1);
    std::vector<std::uint32_t> reverse_cursor(reverse_begin.begin(), reverse_begin.end() - 1);
    for (const auto& record : state.edges()) {
      if (!request.filter.matches(record)) {
        continue;
      }
      const auto source = state.node_index(record.source());
      const auto target = state.node_index(record.target());
      if (!source.has_value() || !target.has_value()) {
        continue;
      }
      forward_edges[forward_cursor[compact[*source]]++] = compact[*target];
      reverse_edges[reverse_cursor[compact[*target]]++] = compact[*source];
      if (record.direction() == Direction::Mutual) {
        forward_edges[forward_cursor[compact[*target]]++] = compact[*source];
        reverse_edges[reverse_cursor[compact[*source]]++] = compact[*target];
      }
    }
  }

  // Kosaraju, both passes iterative.
  std::vector<std::uint8_t> visited(count, 0);
  std::vector<std::uint32_t> order;
  order.reserve(count);
  std::vector<std::uint32_t> stack;
  std::vector<std::uint32_t> cursor;
  for (std::size_t start = 0; start < count; ++start) {
    if (visited[start] != 0) {
      continue;
    }
    stack.clear();
    cursor.clear();
    stack.push_back(static_cast<std::uint32_t>(start));
    cursor.push_back(forward_begin[start]);
    visited[start] = 1;
    while (!stack.empty()) {
      if (request.cancellation.cancelled()) {
        return Result<SccResult>::failure(ErrorCode::Cancelled,
                                          "the component analysis was cancelled before it completed");
      }
      const std::uint32_t node = stack.back();
      if (cursor.back() >= forward_begin[node + 1]) {
        order.push_back(node);
        stack.pop_back();
        cursor.pop_back();
        continue;
      }
      const std::uint32_t next = forward_edges[cursor.back()];
      ++cursor.back();
      if (visited[next] == 0) {
        visited[next] = 1;
        stack.push_back(next);
        cursor.push_back(forward_begin[next]);
      }
    }
  }

  std::vector<std::uint32_t> component(count, kNoNode);
  std::vector<StronglyConnectedComponent> components;
  for (auto position = order.size(); position > 0; --position) {
    const std::uint32_t start = order[position - 1];
    if (component[start] != kNoNode) {
      continue;
    }
    const std::uint32_t identifier = static_cast<std::uint32_t>(components.size());
    StronglyConnectedComponent bucket;
    stack.clear();
    cursor.clear();
    stack.push_back(start);
    cursor.push_back(reverse_begin[start]);
    component[start] = identifier;
    bucket.members.push_back(state.node_at(members[start]));
    while (!stack.empty()) {
      if (request.cancellation.cancelled()) {
        return Result<SccResult>::failure(ErrorCode::Cancelled,
                                          "the component analysis was cancelled before it completed");
      }
      const std::uint32_t node = stack.back();
      if (cursor.back() >= reverse_begin[node + 1]) {
        stack.pop_back();
        cursor.pop_back();
        continue;
      }
      const std::uint32_t next = reverse_edges[cursor.back()];
      ++cursor.back();
      if (component[next] == kNoNode) {
        component[next] = identifier;
        bucket.members.push_back(state.node_at(members[next]));
        stack.push_back(next);
        cursor.push_back(reverse_begin[next]);
      }
    }
    std::sort(bucket.members.begin(), bucket.members.end());
    components.push_back(std::move(bucket));
  }

  for (const auto& record : state.edges()) {
    if (!request.filter.matches(record)) {
      continue;
    }
    const auto source = state.node_index(record.source());
    const auto target = state.node_index(record.target());
    if (!source.has_value() || !target.has_value()) {
      continue;
    }
    if (component[compact[*source]] == component[compact[*target]]) {
      components[component[compact[*source]]].edges.push_back(record.id());
    }
  }

  for (auto& bucket : components) {
    bucket.cyclic = bucket.members.size() > 1;
  }
  std::sort(components.begin(), components.end(),
            [](const StronglyConnectedComponent& lhs, const StronglyConnectedComponent& rhs) {
              return lhs.members.front() < rhs.members.front();
            });

  for (auto& bucket : components) {
    if (bucket.cyclic) {
      ++result.cyclic_components;
    }
    if (request.include_singletons || bucket.cyclic) {
      result.components.push_back(std::move(bucket));
    }
  }
  return result;
}

Result<PathResult> find_path(const GraphState& state, const PathRequest& request) {
  const auto& limits = state.limits();
  if (!request.from.valid() || !request.to.valid()) {
    return Result<PathResult>::failure(ErrorCode::InvalidNodeReference,
                                       "a path endpoint is not a valid node reference");
  }
  if (!direction_is_valid(request.direction)) {
    return Result<PathResult>::failure(ErrorCode::InvalidArguments,
                                       "the path direction is not one of the declared directions");
  }
  std::string detail;
  if (!validate_traversal_bounds(limits, request.max_depth, request.max_nodes, "path", detail)) {
    return Result<PathResult>::failure(ErrorCode::RequestLimitExceeded, std::move(detail));
  }

  PathResult result;
  result.from = request.from;
  result.to = request.to;
  result.generation = state.generation();

  if (request.from == request.to) {
    result.found = true;
    return result;
  }
  const auto start = state.node_index(request.from);
  const auto goal = state.node_index(request.to);
  if (!start.has_value() || !goal.has_value()) {
    return result;
  }

  const std::size_t node_count = state.node_entries().size();
  std::vector<std::uint8_t> visited(node_count, 0);
  std::vector<std::uint32_t> stamp(node_count, 0);
  std::vector<std::uint32_t> predecessor_node(node_count, kNoNode);
  std::vector<std::uint32_t> predecessor_edge(node_count, kNoNode);
  std::uint32_t current_stamp = 0;

  std::vector<std::uint32_t> frontier;
  frontier.push_back(*start);
  visited[*start] = 1;
  result.nodes_examined = 1;

  std::uint32_t depth = 0;
  while (!frontier.empty() && depth < request.max_depth) {
    if (request.cancellation.cancelled()) {
      return Result<PathResult>::failure(ErrorCode::Cancelled, "the path search was cancelled");
    }
    ++depth;
    ++current_stamp;
    std::vector<std::uint32_t> candidates;
    for (const std::uint32_t node : frontier) {
      const auto& entry = state.node_entries()[node];
      for (const std::uint32_t edge_index : adjacency_for(entry, request.direction)) {
        const auto& record = state.edges()[edge_index];
        ++result.edges_examined;
        if (!request.filter.matches(record)) {
          continue;
        }
        const auto neighbour = state.node_index(opposite_endpoint(entry, record));
        if (!neighbour.has_value() || visited[*neighbour] != 0 || stamp[*neighbour] == current_stamp) {
          continue;
        }
        stamp[*neighbour] = current_stamp;
        predecessor_node[*neighbour] = node;
        predecessor_edge[*neighbour] = edge_index;
        candidates.push_back(*neighbour);
      }
    }
    std::sort(candidates.begin(), candidates.end());
    if (result.nodes_examined + candidates.size() > request.max_nodes) {
      result.stop = TraversalStop::NodeLimit;
      return result;
    }
    bool reached = false;
    for (const std::uint32_t node : candidates) {
      visited[node] = 1;
      ++result.nodes_examined;
      if (node == *goal) {
        reached = true;
      }
    }
    if (reached) {
      // Reconstruct from the goal backwards, then reverse.
      std::vector<PathStep> reversed;
      std::uint32_t current = *goal;
      while (current != *start) {
        const std::uint32_t edge_index = predecessor_edge[current];
        const auto& record = state.edges()[edge_index];
        PathStep step;
        step.edge = record.id();
        step.from = state.node_at(predecessor_node[current]);
        step.to = state.node_at(current);
        step.kind = record.kind();
        step.strength = record.strength();
        step.direction = record.direction();
        step.lifecycle = record.lifecycle();
        reversed.push_back(std::move(step));
        current = predecessor_node[current];
      }
      result.steps.assign(reversed.rbegin(), reversed.rend());
      result.found = true;
      return result;
    }
    frontier = std::move(candidates);
  }

  if (!frontier.empty()) {
    result.stop = TraversalStop::DepthLimit;
  }
  return result;
}

Result<CycleReport> enumerate_cycles_of(const GraphState& state, const CycleRequest& request) {
  const auto& limits = state.limits();
  if (request.max_length < 2 || request.max_length > limits.max_cycle_length) {
    return Result<CycleReport>::failure(
        ErrorCode::RequestLimitExceeded,
        "the cycle length bound must be between 2 and " + std::to_string(limits.max_cycle_length));
  }
  if (request.max_cycles == 0 || request.max_cycles > limits.max_cycles) {
    return Result<CycleReport>::failure(
        ErrorCode::RequestLimitExceeded,
        "the cycle count bound must be between 1 and " + std::to_string(limits.max_cycles));
  }
  if (request.max_nodes == 0 || request.max_nodes > limits.max_analysis_nodes) {
    return Result<CycleReport>::failure(
        ErrorCode::RequestLimitExceeded,
        "the cycle analysis node bound must be between 1 and " + std::to_string(limits.max_analysis_nodes));
  }

  CycleReport report;
  report.generation = state.generation();

  const auto node_count = state.node_entries().size();
  std::vector<std::uint32_t> compact(node_count, kNoNode);
  std::vector<std::uint32_t> members;
  for (const auto& record : state.edges()) {
    if (!request.filter.matches(record)) {
      continue;
    }
    ++report.edges_examined;
    for (const auto& endpoint : {&record.source(), &record.target()}) {
      const auto index = state.node_index(*endpoint);
      if (index.has_value() && compact[*index] == kNoNode) {
        compact[*index] = 1;
        members.push_back(*index);
      }
    }
  }
  std::sort(members.begin(), members.end());
  for (std::size_t position = 0; position < members.size(); ++position) {
    compact[members[position]] = static_cast<std::uint32_t>(position);
  }
  report.nodes_examined = members.size();
  if (members.empty()) {
    return report;
  }
  if (members.size() > request.max_nodes) {
    return Result<CycleReport>::failure(
        ErrorCode::AnalysisLimitExceeded,
        "the analysis would visit " + std::to_string(members.size()) +
            " nodes, above the configured bound of " + std::to_string(request.max_nodes));
  }

  struct Frame {
    std::uint32_t node = kNoNode;
    std::uint32_t cursor = 0;
  };

  std::vector<std::uint8_t> on_path(members.size(), 0);
  std::vector<std::uint32_t> path;
  std::vector<std::uint32_t> path_edge;
  std::vector<Frame> stack;

  for (std::size_t start = 0; start < members.size(); ++start) {
    if (request.cancellation.cancelled()) {
      return Result<CycleReport>::failure(ErrorCode::Cancelled, "the cycle analysis was cancelled");
    }
    on_path.assign(members.size(), 0);
    path.clear();
    path_edge.clear();
    stack.clear();
    stack.push_back(Frame{static_cast<std::uint32_t>(start), 0});
    path.push_back(static_cast<std::uint32_t>(start));
    path_edge.push_back(kNoNode);
    on_path[start] = 1;

    while (!stack.empty()) {
      const std::size_t top = stack.size() - 1;
      const std::uint32_t top_node = stack[top].node;
      const auto& entry = state.node_entries()[members[top_node]];
      if (stack[top].cursor >= entry.out_edges.size()) {
        on_path[top_node] = 0;
        path.pop_back();
        path_edge.pop_back();
        stack.pop_back();
        continue;
      }
      const std::uint32_t edge_index = entry.out_edges[stack[top].cursor];
      ++stack[top].cursor;
      const auto& record = state.edges()[edge_index];
      if (!request.filter.matches(record)) {
        continue;
      }
      const auto neighbour = state.node_index(opposite_endpoint(entry, record));
      if (!neighbour.has_value() || compact[*neighbour] == kNoNode) {
        continue;
      }
      const std::uint32_t neighbour_compact = compact[*neighbour];
      if (neighbour_compact == start) {
        if (path.size() < 2) {
          continue;  // a self relation is not an elementary cycle of length 1
        }
        CycleRecord cycle;
        cycle.nodes.reserve(path.size());
        cycle.edges.reserve(path.size());
        for (std::size_t index = 0; index < path.size(); ++index) {
          cycle.nodes.push_back(state.node_at(members[path[index]]));
          if (index > 0) {
            cycle.edges.push_back(state.edges()[path_edge[index]].id());
          }
        }
        cycle.edges.push_back(record.id());
        report.cycles.push_back(std::move(cycle));
        if (report.cycles.size() >= request.max_cycles) {
          report.truncated = true;
          report.stop = TraversalStop::ResultLimit;
          std::sort(report.cycles.begin(), report.cycles.end(),
                    [](const CycleRecord& lhs, const CycleRecord& rhs) {
                      if (lhs.length() != rhs.length()) {
                        return lhs.length() < rhs.length();
                      }
                      return lhs.nodes < rhs.nodes;
                    });
          return report;
        }
        continue;
      }
      if (neighbour_compact < start || on_path[neighbour_compact] != 0) {
        continue;  // keeps every cycle to exactly one starting member
      }
      if (path.size() >= request.max_length) {
        continue;
      }
      on_path[neighbour_compact] = 1;
      path.push_back(neighbour_compact);
      path_edge.push_back(edge_index);
      stack.push_back(Frame{neighbour_compact, 0});
    }
  }

  std::sort(report.cycles.begin(), report.cycles.end(), [](const CycleRecord& lhs, const CycleRecord& rhs) {
    if (lhs.length() != rhs.length()) {
      return lhs.length() < rhs.length();
    }
    return lhs.nodes < rhs.nodes;
  });
  return report;
}

Result<AcyclicObligationReport> verify_acyclic_obligation_of(const GraphState& state,
                                                             const CancellationToken& cancellation) {
  AcyclicObligationReport report;
  report.generation = state.generation();
  const auto search = search_acyclic_violation(state, cancellation);
  if (search.cancelled) {
    return Result<AcyclicObligationReport>::failure(ErrorCode::Cancelled,
                                                    "the acyclic obligation check was cancelled");
  }
  report.satisfied = search.satisfied;
  report.nodes_examined = search.nodes_examined;
  report.edges_examined = search.edges_examined;
  if (!search.satisfied) {
    ProhibitedCycle violation;
    violation.cycle.nodes = search.cycle;
    violation.cycle.edges = search.cycle_edges;
    violation.kind = search.closing_kind;
    report.violations.push_back(std::move(violation));
  }
  return report;
}

}  // namespace detail
}  // namespace facility_dependency_registry
