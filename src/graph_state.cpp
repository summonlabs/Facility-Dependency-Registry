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

#include "graph_state.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace facility_dependency_registry {
namespace detail {
namespace {

std::string ordinal_detail(std::string_view what, std::uint64_t value) {
  std::string detail{what};
  detail.append(" ");
  detail.append(std::to_string(value));
  return detail;
}

}  // namespace

AcyclicSearchResult search_acyclic_violation(const GraphState& state, const CancellationToken& cancellation) {
  AcyclicSearchResult result;
  const auto& nodes = state.node_entries();
  const auto& edges = state.edges();
  constexpr std::uint8_t kWhite = 0;
  constexpr std::uint8_t kGrey = 1;
  constexpr std::uint8_t kBlack = 2;
  constexpr std::uint32_t kNoEdge = 0xFFFFFFFFu;

  std::vector<std::uint8_t> colour(nodes.size(), kWhite);
  std::vector<std::uint32_t> stack;
  std::vector<std::uint32_t> stack_cursor;
  std::vector<std::uint32_t> stack_edge;
  stack.reserve(nodes.size());
  stack_cursor.reserve(nodes.size());
  stack_edge.reserve(nodes.size());

  for (std::size_t start = 0; start < nodes.size(); ++start) {
    if (colour[start] != kWhite) {
      continue;
    }
    stack.clear();
    stack_cursor.clear();
    stack_edge.clear();
    stack.push_back(static_cast<std::uint32_t>(start));
    stack_cursor.push_back(0);
    stack_edge.push_back(kNoEdge);
    colour[start] = kGrey;
    ++result.nodes_examined;

    while (!stack.empty()) {
      if (cancellation.cancelled()) {
        result.cancelled = true;
        return result;
      }
      const std::uint32_t node = stack.back();
      const auto& entry = nodes[node];
      if (stack_cursor.back() >= entry.out_edges.size()) {
        colour[node] = kBlack;
        stack.pop_back();
        stack_cursor.pop_back();
        stack_edge.pop_back();
        continue;
      }
      const std::uint32_t edge_index = entry.out_edges[stack_cursor.back()];
      ++stack_cursor.back();
      ++result.edges_examined;
      const auto& record = edges[edge_index];
      if (record.direction() != Direction::DependsOn || !record.requires_acyclic()) {
        continue;  // not part of the acyclic obligation
      }
      const auto target_index = state.node_index(record.target());
      if (!target_index.has_value()) {
        continue;
      }
      if (colour[*target_index] == kGrey) {
        // The grey nodes on the stack are exactly the current search path, so
        // the slice from the target to the top is the cycle, in path order.
        std::size_t first = 0;
        for (std::size_t position = 0; position < stack.size(); ++position) {
          if (nodes[stack[position]].ref == record.target()) {
            first = position;
            break;
          }
        }
        result.satisfied = false;
        result.closing_kind = record.kind();
        for (std::size_t position = first; position < stack.size(); ++position) {
          result.cycle.push_back(nodes[stack[position]].ref);
          if (position > first) {
            result.cycle_edges.push_back(edges[stack_edge[position]].id());
          }
        }
        result.cycle_edges.push_back(record.id());
        return result;
      }
      if (colour[*target_index] == kWhite) {
        colour[*target_index] = kGrey;
        ++result.nodes_examined;
        stack.push_back(*target_index);
        stack_cursor.push_back(0);
        stack_edge.push_back(edge_index);
      }
    }
  }
  return result;
}

DependencyEdgeRecord EdgeRecordFactory::make_edge(DependencyEdgeId id, EdgeRevision revision,
                                                  DependencyEdgeSpec spec,
                                                  DependencyGeneration registered_generation,
                                                  DependencyGeneration last_modified_generation) {
  DependencyEdgeRecord record;
  record.id_ = id;
  record.revision_ = revision;
  record.kind_ = spec.kind;
  record.strength_ = spec.strength;
  record.direction_ = spec.direction;
  record.lifecycle_ = spec.initial_lifecycle;
  record.source_ = std::move(spec.source);
  record.target_ = std::move(spec.target);
  record.constraints_ = std::move(spec.constraints);
  record.provenance_ = std::move(spec.provenance);
  record.registered_generation_ = registered_generation;
  record.last_modified_generation_ = last_modified_generation;
  return record;
}

DependencyEdgeRecord EdgeRecordFactory::with_lifecycle(const DependencyEdgeRecord& base, LifecycleState state,
                                                       EdgeRevision revision,
                                                       DependencyGeneration last_modified_generation) {
  DependencyEdgeRecord record = base;
  record.lifecycle_ = state;
  record.revision_ = revision;
  record.last_modified_generation_ = last_modified_generation;
  return record;
}

DeclaredExternalRef EdgeRecordFactory::make_declared_ref(DependencyNodeRef ref, ProvenanceRecord provenance,
                                                         DependencyGeneration declared_generation) {
  DeclaredExternalRef record;
  record.ref_ = std::move(ref);
  record.provenance_ = std::move(provenance);
  record.declared_generation_ = declared_generation;
  return record;
}

Result<std::shared_ptr<const GraphState>> GraphState::build(const RegistryLimits& limits,
                                                            DependencyGeneration generation,
                                                            std::uint64_t next_edge_ordinal,
                                                            std::vector<DependencyEdgeRecord> edges,
                                                            std::vector<DeclaredExternalRef> refs) {
  if (const auto status = limits.validate(); !status.ok()) {
    return Result<std::shared_ptr<const GraphState>>::failure(status.error());
  }
  if (generation.value() > limits.max_generation) {
    return Result<std::shared_ptr<const GraphState>>::failure(
        ErrorCode::InvalidGeneration, ordinal_detail("generation", generation.value()));
  }
  if (edges.size() > limits.max_edges) {
    return Result<std::shared_ptr<const GraphState>>::failure(
        ErrorCode::EdgeCapacityExceeded,
        ordinal_detail("edge count", edges.size()) + " exceeds the configured maximum");
  }
  if (refs.size() > limits.max_declared_refs) {
    return Result<std::shared_ptr<const GraphState>>::failure(
        ErrorCode::DeclaredRefCapacityExceeded,
        ordinal_detail("declaration count", refs.size()) + " exceeds the configured maximum");
  }
  if (next_edge_ordinal < kFirstEdgeOrdinal || next_edge_ordinal > kHardMaxGeneration) {
    return Result<std::shared_ptr<const GraphState>>::failure(
        ErrorCode::InvalidEdgeId, ordinal_detail("next edge ordinal", next_edge_ordinal));
  }

  for (std::size_t index = 0; index < refs.size(); ++index) {
    const auto& declaration = refs[index];
    if (!declaration.ref().valid()) {
      return Result<std::shared_ptr<const GraphState>>::failure(
          ErrorCode::InvalidNodeReference, "a declaration carries an invalid node reference");
    }
    if (!declaration.provenance().valid()) {
      return Result<std::shared_ptr<const GraphState>>::failure(
          ErrorCode::InvalidProvenanceIdentifier, "a declaration carries an invalid provenance record");
    }
    if (declaration.declared_generation().value() > generation.value()) {
      return Result<std::shared_ptr<const GraphState>>::failure(
          ErrorCode::InvalidGeneration, "a declaration names a generation that has not been reached");
    }
    if (index > 0 && !(refs[index - 1].ref() < declaration.ref())) {
      return Result<std::shared_ptr<const GraphState>>::failure(
          ErrorCode::InvalidArguments, "declarations are not in canonical order, or repeat a reference");
    }
  }

  for (std::size_t index = 0; index < edges.size(); ++index) {
    const auto& record = edges[index];
    if (!is_assigned(record.id())) {
      return Result<std::shared_ptr<const GraphState>>::failure(ErrorCode::InvalidEdgeId,
                                                                "an edge has no identity");
    }
    if (record.id().value() >= next_edge_ordinal) {
      return Result<std::shared_ptr<const GraphState>>::failure(
          ErrorCode::InvalidEdgeId, "an edge identity is not below the next edge ordinal");
    }
    if (!is_assigned(record.revision())) {
      return Result<std::shared_ptr<const GraphState>>::failure(ErrorCode::InvalidEdgeRevision,
                                                                "an edge has no revision");
    }
    if (record.registered_generation().value() > generation.value() ||
        record.last_modified_generation().value() > generation.value()) {
      return Result<std::shared_ptr<const GraphState>>::failure(
          ErrorCode::InvalidGeneration, "an edge names a generation that has not been reached");
    }
    if (record.last_modified_generation() < record.registered_generation()) {
      return Result<std::shared_ptr<const GraphState>>::failure(
          ErrorCode::InvalidGeneration, "an edge was modified before it was registered");
    }

    DependencyEdgeSpec spec;
    spec.source = record.source();
    spec.target = record.target();
    spec.kind = record.kind();
    spec.strength = record.strength();
    spec.direction = record.direction();
    // A stored edge may be in any of the four lifecycle states; only a fresh
    // declaration is restricted to proposed or active. The stored state is
    // therefore checked separately from the declaration rules below.
    spec.initial_lifecycle = LifecycleState::Active;
    spec.constraints = record.constraints();
    spec.provenance = record.provenance();
    if (const auto status = spec.validate(limits); !status.ok()) {
      return Result<std::shared_ptr<const GraphState>>::failure(status.error());
    }
    const auto lifecycle_ordinal = static_cast<unsigned>(record.lifecycle());
    if (lifecycle_ordinal < 1 || lifecycle_ordinal > kLifecycleStateCount) {
      return Result<std::shared_ptr<const GraphState>>::failure(ErrorCode::InvalidLifecycleToken,
                                                                "an edge is in an unrecognised lifecycle state");
    }

    if (index > 0) {
      const auto& previous = edges[index - 1];
      if (previous.key() == record.key()) {
        return Result<std::shared_ptr<const GraphState>>::failure(
            ErrorCode::DuplicateEdge,
            std::string{"two stored edges share the key "} + record.key().to_text());
      }
      if (record < previous) {
        return Result<std::shared_ptr<const GraphState>>::failure(
            ErrorCode::InvalidArguments, "edges are not in canonical order");
      }
    }
  }

  auto state = std::shared_ptr<GraphState>{new GraphState{limits}};
  state->generation_ = generation;
  state->next_edge_ordinal_ = next_edge_ordinal;
  state->edges_ = std::move(edges);
  state->refs_ = std::move(refs);

  // Node table: every endpoint and every declaration, in canonical order.
  state->node_refs_.reserve(state->edges_.size() * 2 + state->refs_.size());
  for (const auto& record : state->edges_) {
    state->node_refs_.push_back(record.source());
    state->node_refs_.push_back(record.target());
  }
  for (const auto& declaration : state->refs_) {
    state->node_refs_.push_back(declaration.ref());
  }
  std::sort(state->node_refs_.begin(), state->node_refs_.end());
  state->node_refs_.erase(std::unique(state->node_refs_.begin(), state->node_refs_.end()), state->node_refs_.end());

  state->nodes_.resize(state->node_refs_.size());
  for (std::size_t index = 0; index < state->node_refs_.size(); ++index) {
    state->nodes_[index].ref = state->node_refs_[index];
  }
  state->key_index_.reserve(state->edges_.size());
  state->id_index_.reserve(state->edges_.size());

  for (std::size_t index = 0; index < state->edges_.size(); ++index) {
    const auto edge_index = static_cast<std::uint32_t>(index);
    const auto& record = state->edges_[index];
    const auto source_index = static_cast<std::size_t>(
        std::lower_bound(state->node_refs_.begin(), state->node_refs_.end(), record.source()) -
        state->node_refs_.begin());
    const auto target_index = static_cast<std::size_t>(
        std::lower_bound(state->node_refs_.begin(), state->node_refs_.end(), record.target()) -
        state->node_refs_.begin());
    state->nodes_[source_index].out_edges.push_back(edge_index);
    state->nodes_[target_index].in_edges.push_back(edge_index);
    if (record.direction() == Direction::Mutual) {
      state->nodes_[target_index].out_edges.push_back(edge_index);
      state->nodes_[source_index].in_edges.push_back(edge_index);
    }
    state->key_index_.emplace(record.key(), edge_index);
    state->id_index_.emplace(record.id(), edge_index);
  }

  for (const auto& declaration : state->refs_) {
    const auto position = static_cast<std::size_t>(
        std::lower_bound(state->node_refs_.begin(), state->node_refs_.end(), declaration.ref()) -
        state->node_refs_.begin());
    state->nodes_[position].declared = true;
  }

  // Mutual edges push onto the target's out list and the source's in list after
  // the directed pass, so both lists are sorted once here.
  for (auto& node : state->nodes_) {
    std::sort(node.out_edges.begin(), node.out_edges.end());
    std::sort(node.in_edges.begin(), node.in_edges.end());
  }

  if (const auto search = search_acyclic_violation(*state); !search.satisfied) {
    std::string detail{"the acyclic obligation is violated by the cycle "};
    detail.append(format_cycle(search.cycle));
    return Result<std::shared_ptr<const GraphState>>::failure(ErrorCode::ProhibitedCycle, std::move(detail));
  }

  return std::shared_ptr<const GraphState>{std::move(state)};
}

std::span<const DependencyNodeRef> GraphState::nodes() const noexcept { return node_refs_; }

const DependencyEdgeRecord* GraphState::find_edge(DependencyEdgeId id) const noexcept {
  const auto position = id_index_.find(id);
  if (position == id_index_.end()) {
    return nullptr;
  }
  return &edges_[position->second];
}

const DependencyEdgeRecord* GraphState::find_edge(const EdgeKey& key) const noexcept {
  const auto position = key_index_.find(key);
  if (position == key_index_.end()) {
    return nullptr;
  }
  return &edges_[position->second];
}

const DeclaredExternalRef* GraphState::find_declared_ref(const DependencyNodeRef& ref) const noexcept {
  const auto position =
      std::lower_bound(refs_.begin(), refs_.end(), ref, [](const DeclaredExternalRef& entry, const DependencyNodeRef& value) {
        return entry.ref() < value;
      });
  if (position == refs_.end() || !(position->ref() == ref)) {
    return nullptr;
  }
  return &*position;
}

std::optional<std::uint32_t> GraphState::node_index(const DependencyNodeRef& ref) const noexcept {
  const auto position = std::lower_bound(node_refs_.begin(), node_refs_.end(), ref);
  if (position == node_refs_.end() || !(*position == ref)) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(position - node_refs_.begin());
}

const DependencyNodeRef& GraphState::node_at(std::uint32_t index) const noexcept { return node_refs_[index]; }

ContentDigest GraphState::state_digest() const {
  std::call_once(digest_once_, [this]() {
    const auto payload = encode_state(*this, true);
    state_digest_ = payload ? sha256(std::span<const std::byte>{payload.value()}) : ContentDigest{};
    const auto content = encode_state(*this, false);
    content_digest_ = content ? sha256(std::span<const std::byte>{content.value()}) : ContentDigest{};
  });
  return state_digest_;
}

ContentDigest GraphState::content_digest() const {
  std::call_once(digest_once_, [this]() {
    const auto payload = encode_state(*this, true);
    state_digest_ = payload ? sha256(std::span<const std::byte>{payload.value()}) : ContentDigest{};
    const auto content = encode_state(*this, false);
    content_digest_ = content ? sha256(std::span<const std::byte>{content.value()}) : ContentDigest{};
  });
  return content_digest_;
}

std::string format_cycle(const std::vector<DependencyNodeRef>& nodes) {
  std::string result;
  for (const auto& node : nodes) {
    if (!result.empty()) {
      result.append(" -> ");
    }
    result.append(node.to_canonical());
  }
  if (!nodes.empty()) {
    result.append(" -> ");
    result.append(nodes.front().to_canonical());
  }
  return result;
}

}  // namespace detail
}  // namespace facility_dependency_registry
