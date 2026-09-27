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

#ifndef FACILITY_DEPENDENCY_REGISTRY_EDGE_HPP
#define FACILITY_DEPENDENCY_REGISTRY_EDGE_HPP

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "facility_dependency_registry/constraint.hpp"
#include "facility_dependency_registry/errors.hpp"
#include "facility_dependency_registry/ids.hpp"
#include "facility_dependency_registry/lifecycle.hpp"
#include "facility_dependency_registry/limits.hpp"
#include "facility_dependency_registry/mask.hpp"
#include "facility_dependency_registry/node_ref.hpp"
#include "facility_dependency_registry/provenance.hpp"

namespace facility_dependency_registry {

namespace detail {
class EdgeRecordFactory;
}  // namespace detail

/// The narrow set of dependency relationships this registry understands.
///
/// There is deliberately no "other" or "custom" member: an untyped edge bag is
/// exactly what this repository exists to avoid. Each kind fixes what its
/// endpoints may be, which directions it accepts, and whether it must stay
/// acyclic. Values are contiguous and start at 1.
enum class DependencyKind : std::uint8_t {
  /// The source needs electrical supply from the target, which must be an
  /// electrical domain object. Required to be acyclic.
  RequiresPowerFrom = 1,
  /// The source is cooled by the target, which must be a cooling domain
  /// object. Required to be acyclic.
  CooledBy = 2,
  /// The source is housed in the target, which must be a rack. This is the
  /// reference-only form of housing: this registry records the declaration and
  /// never derives physical containment, slot occupancy or topology from it.
  /// Required to be acyclic.
  HousedIn = 3,
  /// The source is served by the target, which must be a facility service.
  /// A service dependency is a real operational relationship and may legally
  /// be mutual, so cycles are permitted here.
  ServedBy = 4,
  /// The source's control path depends on the target. Control dependencies
  /// between control planes are frequently mutual, so cycles are permitted.
  ControlDependsOn = 5,
  /// A composed ASI or DFI domain depends on another composed domain or on a
  /// facility object. Composition is frequently mutual, so cycles are
  /// permitted.
  ComposedDomainDependsOn = 6,
};

inline constexpr std::size_t kDependencyKindCount = 6;

using DependencyKindMask = EnumMask<DependencyKind, kDependencyKindCount>;

/// How much authority the declared dependency carries.
///
/// Strength is a declaration about consequences, not a computation of them.
/// This repository never derives recovery, failover or capacity behaviour from
/// it; it records what the declaring party asserted. Values start at 1.
enum class DependencyStrength : std::uint8_t {
  /// Loss of the target is declared to be an outage of the source.
  Hard = 1,
  /// Loss of the target is declared to degrade the source.
  Soft = 2,
  /// Recorded for information; no declared operational consequence.
  Advisory = 3,
};

inline constexpr std::size_t kDependencyStrengthCount = 3;

using DependencyStrengthMask = EnumMask<DependencyStrength, kDependencyStrengthCount>;

/// How an edge is read.
///
/// Values start at 1.
enum class Direction : std::uint8_t {
  /// The source depends on the target. The relation is directed and the
  /// endpoints are ordered.
  DependsOn = 1,
  /// The relation is symmetric: each endpoint depends on the other. A mutual
  /// edge is stored once, in canonical endpoint order, and is traversed in
  /// both directions. Mutual edges never participate in the acyclic
  /// obligation, because a symmetric relation is a two-cycle by definition.
  Mutual = 2,
};

inline constexpr std::size_t kDirectionCount = 2;

using DirectionMask = EnumMask<Direction, kDirectionCount>;

[[nodiscard]] std::string_view to_token(DependencyKind kind) noexcept;
[[nodiscard]] std::string_view to_token(DependencyStrength strength) noexcept;
[[nodiscard]] std::string_view to_token(Direction direction) noexcept;
[[nodiscard]] std::optional<DependencyKind> parse_dependency_kind(std::string_view token) noexcept;
[[nodiscard]] std::optional<DependencyStrength> parse_dependency_strength(std::string_view token) noexcept;
[[nodiscard]] std::optional<Direction> parse_direction(std::string_view token) noexcept;

/// Everything the registry needs to know about one kind of dependency.
struct DependencyKindDescriptor {
  DependencyKind kind;
  std::string_view token;
  std::string_view semantics;
  DirectionMask allowed_directions;
  NodeDomainMask allowed_source_domains;
  NodeDomainMask allowed_target_domains;
  /// Domains both endpoints must belong to when the direction is Mutual.
  /// Empty means Mutual is not allowed for this kind at all.
  NodeDomainMask mutual_domains;
  /// True when edges of this kind take part in the acyclic obligation.
  bool acyclic_required;
  /// Constraint kinds this dependency kind may carry. An empty mask means the
  /// kind carries no requirement metadata.
  ConstraintKindMask allowed_constraints;
};

/// The descriptor of a kind. Out-of-range values are reported as a descriptor
/// with an empty token rather than by undefined behaviour.
[[nodiscard]] const DependencyKindDescriptor& describe(DependencyKind kind) noexcept;

[[nodiscard]] const std::array<DependencyKindDescriptor, kDependencyKindCount>& dependency_kind_table() noexcept;

/// True when edges of this kind must not form a directed cycle.
[[nodiscard]] bool requires_acyclic(DependencyKind kind) noexcept;

/// The natural key of a dependency edge: the two endpoints and the kind.
///
/// The key, not the registry assigned identity, is what makes an edge unique.
/// `(source, target, kind)` may appear at most once among the edges a registry
/// stores: re-declaring an existing key is either an idempotent no-op (when the
/// payload is identical) or a DuplicateEdge rejection.
class EdgeKey {
 public:
  EdgeKey() = default;
  EdgeKey(DependencyNodeRef source, DependencyNodeRef target, DependencyKind kind)
      : source_(std::move(source)), target_(std::move(target)), kind_(kind) {}

  [[nodiscard]] const DependencyNodeRef& source() const noexcept { return source_; }
  [[nodiscard]] const DependencyNodeRef& target() const noexcept { return target_; }
  [[nodiscard]] DependencyKind kind() const noexcept { return kind_; }

  [[nodiscard]] std::string to_text() const;

  friend bool operator==(const EdgeKey& lhs, const EdgeKey& rhs) noexcept {
    return lhs.source_ == rhs.source_ && lhs.target_ == rhs.target_ && lhs.kind_ == rhs.kind_;
  }
  friend bool operator!=(const EdgeKey& lhs, const EdgeKey& rhs) noexcept { return !(lhs == rhs); }

  /// Canonical order: source, then target, then kind ordinal.
  friend std::strong_ordering operator<=>(const EdgeKey& lhs, const EdgeKey& rhs) noexcept {
    if (const auto by_source = lhs.source_ <=> rhs.source_; by_source != 0) {
      return by_source;
    }
    if (const auto by_target = lhs.target_ <=> rhs.target_; by_target != 0) {
      return by_target;
    }
    return static_cast<unsigned>(lhs.kind_) <=> static_cast<unsigned>(rhs.kind_);
  }

 private:
  DependencyNodeRef source_{};
  DependencyNodeRef target_{};
  DependencyKind kind_{};
};

/// A caller's declaration of one dependency edge, before the registry assigns
/// it an identity, a revision and a generation.
///
/// This is a plain value type; it carries no authority. `validate()` is the
/// single validation entry point and is applied by the registry to every
/// declaration, including declarations that arrive through the API, the CLI or
/// a decoded store. A spec that fails validation is rejected, never repaired.
struct DependencyEdgeSpec {
  DependencyNodeRef source{};
  DependencyNodeRef target{};
  DependencyKind kind = DependencyKind::RequiresPowerFrom;
  DependencyStrength strength = DependencyStrength::Hard;
  Direction direction = Direction::DependsOn;
  /// An edge may only start life Proposed or Active. Suspended and Retired are
  /// reached through the lifecycle transition API, which is itself audited.
  LifecycleState initial_lifecycle = LifecycleState::Active;
  std::vector<DependencyConstraint> constraints{};
  ProvenanceRecord provenance{};

  [[nodiscard]] Status validate(const RegistryLimits& limits) const;
};

/// Puts a mutual edge's endpoints into canonical order. Directed edges are
/// left untouched. Applying this to a declaration and to its mirror image
/// yields the same key, which is what makes the second declaration a duplicate.
void order_endpoints_canonically(Direction direction, DependencyNodeRef& source, DependencyNodeRef& target);

/// The authoritative record of one dependency edge.
///
/// Records are produced only by the registry and by the canonical decoder, and
/// are immutable once produced. A record's identity never changes; its
/// revision advances by exactly one per accepted change.
class DependencyEdgeRecord {
 public:
  DependencyEdgeRecord() = default;

  [[nodiscard]] DependencyEdgeId id() const noexcept { return id_; }
  [[nodiscard]] EdgeRevision revision() const noexcept { return revision_; }
  [[nodiscard]] DependencyKind kind() const noexcept { return kind_; }
  [[nodiscard]] DependencyStrength strength() const noexcept { return strength_; }
  [[nodiscard]] Direction direction() const noexcept { return direction_; }
  [[nodiscard]] LifecycleState lifecycle() const noexcept { return lifecycle_; }
  [[nodiscard]] const DependencyNodeRef& source() const noexcept { return source_; }
  [[nodiscard]] const DependencyNodeRef& target() const noexcept { return target_; }
  [[nodiscard]] const std::vector<DependencyConstraint>& constraints() const noexcept { return constraints_; }
  [[nodiscard]] const ProvenanceRecord& provenance() const noexcept { return provenance_; }

  /// The generation in which this edge was first declared.
  [[nodiscard]] DependencyGeneration registered_generation() const noexcept { return registered_generation_; }
  /// The generation of the most recent accepted change to this edge.
  [[nodiscard]] DependencyGeneration last_modified_generation() const noexcept {
    return last_modified_generation_;
  }

  [[nodiscard]] EdgeKey key() const { return EdgeKey{source_, target_, kind_}; }
  [[nodiscard]] bool is_in_force() const noexcept {
    return facility_dependency_registry::is_in_force(lifecycle_);
  }
  [[nodiscard]] bool requires_acyclic() const noexcept { return facility_dependency_registry::requires_acyclic(kind_); }

  /// One line, deterministic, suitable for logs and inspection output.
  [[nodiscard]] std::string to_text() const;

  /// Full structural equality over every field, including identity, revision
  /// and the generations in which the record was written.
  friend bool operator==(const DependencyEdgeRecord& lhs, const DependencyEdgeRecord& rhs) noexcept;
  friend bool operator!=(const DependencyEdgeRecord& lhs, const DependencyEdgeRecord& rhs) noexcept {
    return !(lhs == rhs);
  }

  /// Canonical order: by key, then by identity.
  friend std::strong_ordering operator<=>(const DependencyEdgeRecord& lhs, const DependencyEdgeRecord& rhs) noexcept;

 private:
  friend class detail::EdgeRecordFactory;

  DependencyEdgeId id_{};
  EdgeRevision revision_{};
  DependencyKind kind_{};
  DependencyStrength strength_{};
  Direction direction_{};
  LifecycleState lifecycle_{};
  DependencyNodeRef source_{};
  DependencyNodeRef target_{};
  std::vector<DependencyConstraint> constraints_{};
  ProvenanceRecord provenance_{};
  DependencyGeneration registered_generation_{};
  DependencyGeneration last_modified_generation_{};
};

/// Which edges a query or a traversal considers.
///
/// The defaults are the authoritative reading: all kinds, all strengths, and
/// only edges that are actually in force.
struct EdgeFilter {
  DependencyKindMask kinds{DependencyKindMask::all()};
  DependencyStrengthMask strengths{DependencyStrengthMask::all()};
  LifecycleMask lifecycles{LifecycleMask::of(LifecycleState::Active)};

  [[nodiscard]] bool matches(const DependencyEdgeRecord& record) const noexcept {
    return kinds.contains(record.kind()) && strengths.contains(record.strength()) &&
           lifecycles.contains(record.lifecycle());
  }

  /// A filter that matches every stored edge, whatever its state.
  [[nodiscard]] static EdgeFilter any() noexcept {
    return EdgeFilter{DependencyKindMask::all(), DependencyStrengthMask::all(), LifecycleMask::all()};
  }

  /// A filter that matches only edges in force.
  [[nodiscard]] static EdgeFilter in_force() noexcept { return EdgeFilter{}; }
};

}  // namespace facility_dependency_registry

namespace std {

template <>
struct hash<facility_dependency_registry::EdgeKey> {
  [[nodiscard]] size_t operator()(const facility_dependency_registry::EdgeKey& key) const noexcept {
    const size_t source_hash = hash<facility_dependency_registry::DependencyNodeRef>{}(key.source());
    const size_t target_hash = hash<facility_dependency_registry::DependencyNodeRef>{}(key.target());
    const size_t kind_hash = hash<uint8_t>{}(static_cast<uint8_t>(key.kind()));
    size_t combined = source_hash;
    combined ^= target_hash + 0x9E3779B97F4A7C15ull + (combined << 6) + (combined >> 2);
    combined ^= kind_hash + 0x9E3779B97F4A7C15ull + (combined << 6) + (combined >> 2);
    return combined;
  }
};

}  // namespace std

#endif  // FACILITY_DEPENDENCY_REGISTRY_EDGE_HPP
