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

#ifndef FACILITY_DEPENDENCY_REGISTRY_IDS_HPP
#define FACILITY_DEPENDENCY_REGISTRY_IDS_HPP

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace facility_dependency_registry {

/// A strong identifier: a distinct type for every identity kind, so that an
/// edge identity can never be silently used as a revision, a generation or an
/// ordinal. There are no implicit conversions between instantiations, and the
/// constructor is private: the only way to obtain a value is `from_value`.
template <class Tag, class Rep>
class StrongId {
 public:
  using rep_type = Rep;
  using tag_type = Tag;

  /// The zero value. Every identity in this repository documents what its
  /// zero means; none of them is a wildcard or a "match anything" sentinel.
  constexpr StrongId() noexcept = default;

  [[nodiscard]] static constexpr StrongId from_value(Rep value) noexcept { return StrongId{value}; }

  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == Rep{0}; }

  /// Strict total order by numeric value. This is the ordering used whenever
  /// identities are serialized, compared or reported.
  friend constexpr std::strong_ordering operator<=>(const StrongId& lhs, const StrongId& rhs) noexcept {
    return lhs.value_ <=> rhs.value_;
  }
  friend constexpr bool operator==(const StrongId& lhs, const StrongId& rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }

 private:
  explicit constexpr StrongId(Rep value) noexcept : value_(value) {}
  Rep value_{};
};

struct DependencyEdgeIdTag {};
struct EdgeRevisionTag {};
struct DependencyGenerationTag {};

/// Registry assigned identity of one dependency edge.
///
/// Edge identities are allocated from a monotonically increasing registry
/// ordinal. They are never reused, not even after the edge is removed, so an
/// identity observed by a consumer always refers to at most one declaration
/// in the history of a registry generation chain.
using DependencyEdgeId = StrongId<DependencyEdgeIdTag, std::uint64_t>;

/// Per-edge revision. The first revision of an edge is 1; every accepted
/// update, lifecycle transition or provenance replacement advances it by
/// exactly 1. Revision zero is never assigned.
using EdgeRevision = StrongId<EdgeRevisionTag, std::uint64_t>;

/// The graph generation. Generation zero is the empty graph; every committed
/// mutation advances the generation by exactly one. A generation is the unit
/// of authority: mutations are addressed to the generation they expect.
using DependencyGeneration = StrongId<DependencyGenerationTag, std::uint64_t>;

/// The first edge identity ever assigned by a registry.
inline constexpr std::uint64_t kFirstEdgeOrdinal = 1;

/// The generation of an empty graph.
inline constexpr DependencyGeneration kInitialGeneration = DependencyGeneration::from_value(0);

/// The first revision of an edge.
inline constexpr EdgeRevision kInitialRevision = EdgeRevision::from_value(1);

/// True when an edge identity has been assigned. Identity zero is never
/// assigned, so this is exactly "this identity came from a registry".
[[nodiscard]] constexpr bool is_assigned(DependencyEdgeId id) noexcept { return !id.is_zero(); }

/// True when a revision is an assigned revision.
[[nodiscard]] constexpr bool is_assigned(EdgeRevision revision) noexcept { return !revision.is_zero(); }

/// Checked increment helpers. They return false instead of wrapping, so that
/// arithmetic on externally influenced identities can never silently alias.
[[nodiscard]] constexpr bool try_increment(EdgeRevision& revision) noexcept {
  if (revision.value() == UINT64_MAX) {
    return false;
  }
  revision = EdgeRevision::from_value(revision.value() + 1);
  return true;
}

[[nodiscard]] constexpr bool try_increment(DependencyGeneration& generation) noexcept {
  if (generation.value() == UINT64_MAX) {
    return false;
  }
  generation = DependencyGeneration::from_value(generation.value() + 1);
  return true;
}

/// Stable textual form of an identity: the decimal value.
[[nodiscard]] std::string to_text(DependencyEdgeId id);
[[nodiscard]] std::string to_text(EdgeRevision revision);
[[nodiscard]] std::string to_text(DependencyGeneration generation);

/// Strict decimal parsing of an identity. Leading zeros, signs, whitespace and
/// any non-digit are rejected; the value must fit in 64 bits exactly.
[[nodiscard]] bool parse_edge_id(std::string_view text, DependencyEdgeId& out) noexcept;
[[nodiscard]] bool parse_edge_revision(std::string_view text, EdgeRevision& out) noexcept;
[[nodiscard]] bool parse_generation(std::string_view text, DependencyGeneration& out) noexcept;

}  // namespace facility_dependency_registry

namespace std {

template <>
struct hash<facility_dependency_registry::DependencyEdgeId> {
  [[nodiscard]] size_t operator()(const facility_dependency_registry::DependencyEdgeId& id) const noexcept {
    return hash<uint64_t>{}(id.value());
  }
};

template <>
struct hash<facility_dependency_registry::EdgeRevision> {
  [[nodiscard]] size_t operator()(const facility_dependency_registry::EdgeRevision& revision) const noexcept {
    return hash<uint64_t>{}(revision.value());
  }
};

template <>
struct hash<facility_dependency_registry::DependencyGeneration> {
  [[nodiscard]] size_t operator()(const facility_dependency_registry::DependencyGeneration& generation) const noexcept {
    return hash<uint64_t>{}(generation.value());
  }
};

}  // namespace std

#endif  // FACILITY_DEPENDENCY_REGISTRY_IDS_HPP
