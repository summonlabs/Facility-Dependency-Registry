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

#include "facility_dependency_registry/query.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "facility_dependency_registry/snapshot.hpp"

namespace facility_dependency_registry {
namespace {

std::string render_constraints(const std::vector<DependencyConstraint>& constraints) {
  if (constraints.empty()) {
    return "none";
  }
  std::string result;
  for (const auto& constraint : constraints) {
    if (!result.empty()) {
      result.push_back(';');
    }
    result.append(constraint.to_text());
  }
  return result;
}

bool find_in(const std::vector<ClosureEntry>& entries, const DependencyNodeRef& node,
             std::optional<std::uint32_t>& depth) {
  for (const auto& entry : entries) {
    if (entry.node == node) {
      depth = entry.depth;
      return true;
    }
  }
  return false;
}

}  // namespace

std::string_view to_token(TraversalDirection direction) noexcept {
  switch (direction) {
    case TraversalDirection::Dependencies:
      return "dependencies";
    case TraversalDirection::Dependents:
      return "dependents";
  }
  return "unknown-direction";
}

std::string_view to_token(TraversalStop stop) noexcept {
  switch (stop) {
    case TraversalStop::Complete:
      return "complete";
    case TraversalStop::DepthLimit:
      return "depth-limit";
    case TraversalStop::NodeLimit:
      return "node-limit";
    case TraversalStop::Cancelled:
      return "cancelled";
    case TraversalStop::ResultLimit:
      return "result-limit";
  }
  return "unknown-stop";
}

std::string_view to_token(EdgeChangeKind kind) noexcept {
  switch (kind) {
    case EdgeChangeKind::Added:
      return "added";
    case EdgeChangeKind::Removed:
      return "removed";
    case EdgeChangeKind::Modified:
      return "modified";
  }
  return "unknown-change";
}

bool TraversalResult::reached(const DependencyNodeRef& node) const noexcept {
  for (const auto& entry : entries) {
    if (entry.node == node) {
      return true;
    }
  }
  return node == root;
}

std::optional<std::uint32_t> TraversalResult::depth_of(const DependencyNodeRef& node) const noexcept {
  if (node == root) {
    return 0;
  }
  std::optional<std::uint32_t> depth;
  if (find_in(entries, node, depth)) {
    return depth;
  }
  return std::nullopt;
}

bool ImpactCone::reached(const DependencyNodeRef& node) const noexcept {
  for (const auto& entry : entries) {
    if (entry.node == node) {
      return true;
    }
  }
  return node == origin;
}

std::optional<std::uint32_t> ImpactCone::depth_of(const DependencyNodeRef& node) const noexcept {
  if (node == origin) {
    return 0;
  }
  for (const auto& entry : entries) {
    if (entry.node == node) {
      return entry.depth;
    }
  }
  return std::nullopt;
}

bool CycleRecord::contains(const DependencyNodeRef& node) const noexcept {
  return std::find(nodes.begin(), nodes.end(), node) != nodes.end();
}

std::string PathStep::to_text() const {
  std::string result;
  result.reserve(from.id().size() + to.id().size() + 96);
  result.append(from.to_canonical());
  result.append(" -> ");
  result.append(to.to_canonical());
  result.append(" [edge ");
  result.append(facility_dependency_registry::to_text(edge));
  result.append(" kind=");
  result.append(facility_dependency_registry::to_token(kind));
  result.append(" strength=");
  result.append(facility_dependency_registry::to_token(strength));
  result.append(" direction=");
  result.append(facility_dependency_registry::to_token(direction));
  result.append(" lifecycle=");
  result.append(facility_dependency_registry::to_token(lifecycle));
  result.push_back(']');
  return result;
}

std::string ProhibitedCycle::to_text() const {
  std::string result{"prohibited cycle kind="};
  result.append(facility_dependency_registry::to_token(kind));
  result.append(" path=");
  for (std::size_t index = 0; index < cycle.nodes.size(); ++index) {
    if (index > 0) {
      result.append(" -> ");
    }
    result.append(cycle.nodes[index].to_canonical());
  }
  if (!cycle.nodes.empty()) {
    result.append(" -> ");
    result.append(cycle.nodes.front().to_canonical());
  }
  return result;
}

std::string EdgeChange::to_text() const {
  std::string result;
  result.append(facility_dependency_registry::to_token(kind));
  result.append(" edge=");
  result.append(facility_dependency_registry::to_text(id));
  result.append(" key=");
  result.append(key.to_text());
  if (kind == EdgeChangeKind::Modified) {
    result.append(" before-revision=");
    result.append(facility_dependency_registry::to_text(before_revision));
    result.append(" after-revision=");
    result.append(facility_dependency_registry::to_text(after_revision));
    for (const auto& field : fields) {
      result.append(" ");
      result.append(field.field);
      result.append(":");
      result.append(field.before);
      result.append("->");
      result.append(field.after);
    }
  }
  return result;
}

std::string RefChange::to_text() const {
  std::string result{added ? "declared " : "withdrew "};
  result.append(ref.to_canonical());
  return result;
}

std::size_t GenerationDiff::added() const noexcept {
  return static_cast<std::size_t>(
      std::count_if(edge_changes.begin(), edge_changes.end(),
                    [](const EdgeChange& change) { return change.kind == EdgeChangeKind::Added; }));
}

std::size_t GenerationDiff::removed() const noexcept {
  return static_cast<std::size_t>(
      std::count_if(edge_changes.begin(), edge_changes.end(),
                    [](const EdgeChange& change) { return change.kind == EdgeChangeKind::Removed; }));
}

std::size_t GenerationDiff::modified() const noexcept {
  return static_cast<std::size_t>(
      std::count_if(edge_changes.begin(), edge_changes.end(),
                    [](const EdgeChange& change) { return change.kind == EdgeChangeKind::Modified; }));
}

bool GenerationDiff::empty() const noexcept { return edge_changes.empty() && ref_changes.empty(); }

Result<GenerationDiff> diff_snapshots(const RegistrySnapshot& from, const RegistrySnapshot& to,
                                      std::uint32_t max_changes, const CancellationToken& cancellation) {
  GenerationDiff diff;
  diff.from = from.generation();
  diff.to = to.generation();

  const auto from_edges = from.edges();
  const auto to_edges = to.edges();
  std::size_t left = 0;
  std::size_t right = 0;
  bool truncated = false;

  const auto record_change = [&](EdgeChange change) {
    if (diff.edge_changes.size() >= max_changes) {
      truncated = true;
      return false;
    }
    diff.edge_changes.push_back(std::move(change));
    return true;
  };

  while (left < from_edges.size() || right < to_edges.size()) {
    if (cancellation.cancelled()) {
      return Result<GenerationDiff>::failure(ErrorCode::Cancelled, "the generation diff was cancelled");
    }
    if (truncated) {
      break;
    }
    if (left < from_edges.size() && right < to_edges.size() && from_edges[left].key() == to_edges[right].key()) {
      const auto& before = from_edges[left];
      const auto& after = to_edges[right];
      if (before == after) {
        ++diff.unchanged_edges;
      } else {
        EdgeChange change;
        change.kind = EdgeChangeKind::Modified;
        change.id = after.id();
        change.key = after.key();
        change.before_revision = before.revision();
        change.after_revision = after.revision();
        change.before = before;
        change.after = after;
        const auto add_field = [&change](std::string field, std::string before_text, std::string after_text) {
          if (before_text != after_text) {
            EdgeFieldChange entry;
            entry.field = std::move(field);
            entry.before = std::move(before_text);
            entry.after = std::move(after_text);
            change.fields.push_back(std::move(entry));
          }
        };
        add_field("strength", std::string{to_token(before.strength())}, std::string{to_token(after.strength())});
        add_field("direction", std::string{to_token(before.direction())}, std::string{to_token(after.direction())});
        add_field("lifecycle", std::string{to_token(before.lifecycle())}, std::string{to_token(after.lifecycle())});
        add_field("constraints", render_constraints(before.constraints()),
                  render_constraints(after.constraints()));
        add_field("provenance", before.provenance().to_text(), after.provenance().to_text());
        add_field("registered-generation", facility_dependency_registry::to_text(before.registered_generation()),
                  facility_dependency_registry::to_text(after.registered_generation()));
        add_field("last-modified-generation",
                  facility_dependency_registry::to_text(before.last_modified_generation()),
                  facility_dependency_registry::to_text(after.last_modified_generation()));
        if (!record_change(std::move(change))) {
          break;
        }
      }
      ++left;
      ++right;
      continue;
    }
    if (right >= to_edges.size() || (left < from_edges.size() && from_edges[left].key() < to_edges[right].key())) {
      EdgeChange change;
      change.kind = EdgeChangeKind::Removed;
      change.id = from_edges[left].id();
      change.key = from_edges[left].key();
      change.before_revision = from_edges[left].revision();
      change.before = from_edges[left];
      if (!record_change(std::move(change))) {
        break;
      }
      ++left;
      continue;
    }
    EdgeChange change;
    change.kind = EdgeChangeKind::Added;
    change.id = to_edges[right].id();
    change.key = to_edges[right].key();
    change.after_revision = to_edges[right].revision();
    change.after = to_edges[right];
    if (!record_change(std::move(change))) {
      break;
    }
    ++right;
  }

  const auto from_refs = from.declared_refs();
  const auto to_refs = to.declared_refs();
  std::size_t ref_left = 0;
  std::size_t ref_right = 0;
  while (ref_left < from_refs.size() || ref_right < to_refs.size()) {
    if (truncated) {
      break;
    }
    if (ref_left < from_refs.size() && ref_right < to_refs.size() &&
        from_refs[ref_left].ref() == to_refs[ref_right].ref()) {
      ++ref_left;
      ++ref_right;
      continue;
    }
    if (diff.ref_changes.size() >= max_changes) {
      truncated = true;
      break;
    }
    RefChange change;
    if (ref_right >= to_refs.size() ||
        (ref_left < from_refs.size() && from_refs[ref_left].ref() < to_refs[ref_right].ref())) {
      change.added = false;
      change.ref = from_refs[ref_left].ref();
      change.before = from_refs[ref_left].provenance();
      ++ref_left;
    } else {
      change.added = true;
      change.ref = to_refs[ref_right].ref();
      change.after = to_refs[ref_right].provenance();
      ++ref_right;
    }
    diff.ref_changes.push_back(std::move(change));
  }

  diff.truncated = truncated;
  return diff;
}

}  // namespace facility_dependency_registry
