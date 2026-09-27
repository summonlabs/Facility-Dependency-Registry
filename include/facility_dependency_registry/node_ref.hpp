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

#ifndef FACILITY_DEPENDENCY_REGISTRY_NODE_REF_HPP
#define FACILITY_DEPENDENCY_REGISTRY_NODE_REF_HPP

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "facility_dependency_registry/errors.hpp"
#include "facility_dependency_registry/limits.hpp"
#include "facility_dependency_registry/mask.hpp"

namespace facility_dependency_registry {

/// The kind of external object a dependency endpoint refers to.
///
/// This registry never owns any of these objects. Each domain names the
/// adjacent DCCP registry or control layer that does own it, and this
/// repository only ever holds the opaque identity it was given.
/// Values are contiguous and start at 1 so that zero means "no domain".
enum class NodeDomain : std::uint8_t {
  /// Owned by the Asset Registry.
  Asset = 1,
  /// Owned by the Rack Registry.
  Rack = 2,
  /// Owned by the electrical domain of the facility control plane: feeds,
  /// busways, distribution paths and their source equipment.
  ElectricalDomain = 3,
  /// Owned by the cooling domain of the facility control plane: loops,
  /// distribution and their source equipment.
  CoolingDomain = 4,
  /// Owned by the facility service catalogue.
  FacilityService = 5,
  /// A composed Accelerated Systems Infrastructure domain.
  AsiDomain = 6,
  /// A composed Distributed Fabric Infrastructure domain.
  DfiDomain = 7,
};

inline constexpr std::size_t kNodeDomainCount = 7;

using NodeDomainMask = EnumMask<NodeDomain, kNodeDomainCount>;

[[nodiscard]] std::string_view to_token(NodeDomain domain) noexcept;
[[nodiscard]] std::string_view describe(NodeDomain domain) noexcept;
[[nodiscard]] std::optional<NodeDomain> parse_node_domain(std::string_view token) noexcept;

/// Maximum length of a canonical external identifier, in bytes.
inline constexpr std::size_t kMaxExternalIdLength = kHardMaxIdLength;

/// The canonical external identifier syntax.
///
/// An identifier is 1..160 bytes long and contains only `A-Z`, `a-z`, `0-9`,
/// `.`, `_` and `-`. It starts and ends with an alphanumeric character. `.`
/// and `-` are separators and may not be repeated, so `a..b`, `a--b`, `.a` and
/// `a.` are all rejected. `_` is an ordinary character and may repeat.
///
/// The syntax is deliberately narrow and ASCII-only: an identifier that does
/// not match it is rejected, never trimmed, case-folded or otherwise
/// normalized. Identifiers are compared byte for byte.
[[nodiscard]] bool is_valid_external_id(std::string_view id, std::size_t max_length) noexcept;

/// The same check with a machine readable rejection reason. `what` names the
/// field being validated and appears in the detail text.
[[nodiscard]] Status validate_external_id(std::string_view id, std::size_t max_length, std::string_view what);

/// A typed, opaque reference to one external facility object.
///
/// Equality and ordering are structural: two references are the same exactly
/// when they name the same domain and the same identifier bytes. Ordering is
/// by domain ordinal first and identifier bytes second, which is the canonical
/// order used everywhere this repository reports nodes.
class DependencyNodeRef {
 public:
  /// An invalid reference: no domain, no identifier. Values of this shape are
  /// never stored; they exist so that containers and members can be declared.
  DependencyNodeRef() = default;

  /// Creates a reference after validating `id` against the canonical syntax.
  [[nodiscard]] static Result<DependencyNodeRef> create(NodeDomain domain, std::string_view id,
                                                        std::size_t max_length = kMaxExternalIdLength);

  /// Parses the canonical text form `<domain>:<id>`, for example
  /// `asset:row-a-rack-07-node-3`. The first colon separates the domain from
  /// the identifier; identifiers themselves cannot contain a colon.
  [[nodiscard]] static Result<DependencyNodeRef> parse(std::string_view canonical,
                                                       std::size_t max_length = kMaxExternalIdLength);

  [[nodiscard]] NodeDomain domain() const noexcept { return domain_; }
  [[nodiscard]] const std::string& id() const noexcept { return id_; }
  [[nodiscard]] bool valid() const noexcept;

  /// `<domain-token>:<id>`
  [[nodiscard]] std::string to_canonical() const;

  friend bool operator==(const DependencyNodeRef& lhs, const DependencyNodeRef& rhs) noexcept {
    return lhs.domain_ == rhs.domain_ && lhs.id_ == rhs.id_;
  }
  friend bool operator!=(const DependencyNodeRef& lhs, const DependencyNodeRef& rhs) noexcept {
    return !(lhs == rhs);
  }

  /// Canonical order: domain ordinal, then identifier bytes.
  friend std::strong_ordering operator<=>(const DependencyNodeRef& lhs, const DependencyNodeRef& rhs) noexcept {
    if (const auto by_domain = lhs.domain_ <=> rhs.domain_; by_domain != 0) {
      return by_domain;
    }
    return lhs.id_.compare(rhs.id_) <=> 0;
  }

 private:
  friend class NodeRefSet;
  NodeDomain domain_{};
  std::string id_{};
};

/// A bounded, duplicate-free, canonically ordered set of references.
///
/// Batch query inputs and node-list results use this type so that "which
/// nodes" is always answered in one defined order, with a size bound applied
/// before anything is allocated or walked.
class NodeRefSet {
 public:
  NodeRefSet() = default;

  /// Sorts, removes duplicates and applies the size bound. Fails with
  /// RequestLimitExceeded when more references are supplied than allowed,
  /// before sorting anything.
  [[nodiscard]] static Result<NodeRefSet> create(std::vector<DependencyNodeRef> refs, std::uint32_t max_refs);

  [[nodiscard]] std::span<const DependencyNodeRef> refs() const noexcept { return refs_; }
  [[nodiscard]] std::size_t size() const noexcept { return refs_.size(); }
  [[nodiscard]] bool empty() const noexcept { return refs_.empty(); }
  [[nodiscard]] bool contains(const DependencyNodeRef& ref) const noexcept;

  friend bool operator==(const NodeRefSet& lhs, const NodeRefSet& rhs) noexcept { return lhs.refs_ == rhs.refs_; }

 private:
  std::vector<DependencyNodeRef> refs_{};
};

[[nodiscard]] std::string to_text(const DependencyNodeRef& ref);
[[nodiscard]] std::string to_text(const NodeRefSet& set);

}  // namespace facility_dependency_registry

namespace std {

template <>
struct hash<facility_dependency_registry::DependencyNodeRef> {
  [[nodiscard]] size_t operator()(const facility_dependency_registry::DependencyNodeRef& ref) const noexcept {
    const size_t domain_hash = hash<uint8_t>{}(static_cast<uint8_t>(ref.domain()));
    const size_t id_hash = hash<string_view>{}(string_view{ref.id()});
    return domain_hash * 0x9E3779B97F4A7C15ull ^ (id_hash + 0x9E3779B97F4A7C15ull + (domain_hash << 6) + (domain_hash >> 2));
  }
};

}  // namespace std

#endif  // FACILITY_DEPENDENCY_REGISTRY_NODE_REF_HPP
