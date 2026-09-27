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

#include "facility_dependency_registry/edge.hpp"

#include <array>
#include <string>

namespace facility_dependency_registry {
namespace {

constexpr NodeDomainMask all_domains() noexcept { return NodeDomainMask::all(); }

constexpr NodeDomainMask composed_domains() noexcept {
  return NodeDomainMask::of(NodeDomain::AsiDomain).with(NodeDomain::DfiDomain);
}

constexpr ConstraintKindMask constraints_of(std::initializer_list<ConstraintKind> kinds) noexcept {
  ConstraintKindMask mask = ConstraintKindMask::none();
  for (const auto kind : kinds) {
    mask = mask.with(kind);
  }
  return mask;
}

constexpr std::array<DependencyKindDescriptor, kDependencyKindCount> kKindTable{{
    {
        DependencyKind::RequiresPowerFrom,
        "requires-power-from",
        "the source requires electrical supply from the target, which must be an electrical domain object",
        DirectionMask::of(Direction::DependsOn),
        all_domains(),
        NodeDomainMask::of(NodeDomain::ElectricalDomain),
        NodeDomainMask::none(),
        true,
        constraints_of({ConstraintKind::RedundancyClass, ConstraintKind::RedundancyCount,
                        ConstraintKind::FailoverMode}),
    },
    {
        DependencyKind::CooledBy,
        "cooled-by",
        "the source is cooled by the target, which must be a cooling domain object",
        DirectionMask::of(Direction::DependsOn),
        all_domains(),
        NodeDomainMask::of(NodeDomain::CoolingDomain),
        NodeDomainMask::none(),
        true,
        constraints_of({ConstraintKind::MaxLatencyMicros, ConstraintKind::RedundancyClass,
                        ConstraintKind::RedundancyCount, ConstraintKind::FailoverMode}),
    },
    {
        DependencyKind::HousedIn,
        "housed-in",
        "the source is housed in the target, which must be a rack; recorded as a reference only and never "
        "expanded into physical containment",
        DirectionMask::of(Direction::DependsOn),
        NodeDomainMask::of(NodeDomain::Asset).with(NodeDomain::FacilityService).with(NodeDomain::AsiDomain).with(
            NodeDomain::DfiDomain),
        NodeDomainMask::of(NodeDomain::Rack),
        NodeDomainMask::none(),
        true,
        ConstraintKindMask::none(),
    },
    {
        DependencyKind::ServedBy,
        "served-by",
        "the source is served by the target, which must be a facility service",
        DirectionMask::of(Direction::DependsOn),
        all_domains().without(NodeDomain::FacilityService),
        NodeDomainMask::of(NodeDomain::FacilityService),
        NodeDomainMask::none(),
        false,
        constraints_of({ConstraintKind::MaxLatencyMicros, ConstraintKind::MinBandwidthMbps,
                        ConstraintKind::RedundancyClass}),
    },
    {
        DependencyKind::ControlDependsOn,
        "control-depends-on",
        "the control path of the source depends on the target; mutual control dependencies are permitted",
        DirectionMask::of(Direction::DependsOn).with(Direction::Mutual),
        all_domains(),
        all_domains(),
        all_domains(),
        false,
        constraints_of({ConstraintKind::MaxLatencyMicros, ConstraintKind::FailoverMode}),
    },
    {
        DependencyKind::ComposedDomainDependsOn,
        "composed-domain-depends-on",
        "a composed ASI or DFI domain depends on another composed domain or on a facility object; mutual "
        "composition between composed domains is permitted",
        DirectionMask::of(Direction::DependsOn).with(Direction::Mutual),
        composed_domains(),
        all_domains(),
        composed_domains(),
        false,
        ConstraintKindMask::all(),
    },
}};

constexpr DependencyKindDescriptor kUnknownKindDescriptor{
    DependencyKind::RequiresPowerFrom,
    std::string_view{},
    std::string_view{"unrecognised dependency kind"},
    DirectionMask::none(),
    NodeDomainMask::none(),
    NodeDomainMask::none(),
    NodeDomainMask::none(),
    false,
    ConstraintKindMask::none(),
};

bool kind_in_range(DependencyKind kind) noexcept {
  const auto ordinal = static_cast<unsigned>(kind);
  return ordinal >= 1 && ordinal <= kDependencyKindCount;
}

bool strength_in_range(DependencyStrength strength) noexcept {
  const auto ordinal = static_cast<unsigned>(strength);
  return ordinal >= 1 && ordinal <= kDependencyStrengthCount;
}

bool direction_in_range(Direction direction) noexcept {
  const auto ordinal = static_cast<unsigned>(direction);
  return ordinal >= 1 && ordinal <= kDirectionCount;
}

bool lifecycle_in_range(LifecycleState state) noexcept {
  const auto ordinal = static_cast<unsigned>(state);
  return ordinal >= 1 && ordinal <= kLifecycleStateCount;
}

std::string domain_detail(std::string_view endpoint, const DependencyNodeRef& ref) {
  std::string detail{endpoint};
  detail.append(" domain '");
  detail.append(to_token(ref.domain()));
  detail.push_back('\'');
  return detail;
}

}  // namespace

std::string_view to_token(DependencyKind kind) noexcept {
  if (!kind_in_range(kind)) {
    return "unknown-dependency-kind";
  }
  return kKindTable[static_cast<std::size_t>(kind) - 1].token;
}

std::string_view to_token(DependencyStrength strength) noexcept {
  switch (strength) {
    case DependencyStrength::Hard:
      return "hard";
    case DependencyStrength::Soft:
      return "soft";
    case DependencyStrength::Advisory:
      return "advisory";
  }
  return "unknown-dependency-strength";
}

std::string_view to_token(Direction direction) noexcept {
  switch (direction) {
    case Direction::DependsOn:
      return "depends-on";
    case Direction::Mutual:
      return "mutual";
  }
  return "unknown-direction";
}

std::optional<DependencyKind> parse_dependency_kind(std::string_view token) noexcept {
  for (const auto& entry : kKindTable) {
    if (entry.token == token) {
      return entry.kind;
    }
  }
  return std::nullopt;
}

std::optional<DependencyStrength> parse_dependency_strength(std::string_view token) noexcept {
  if (token == "hard") {
    return DependencyStrength::Hard;
  }
  if (token == "soft") {
    return DependencyStrength::Soft;
  }
  if (token == "advisory") {
    return DependencyStrength::Advisory;
  }
  return std::nullopt;
}

std::optional<Direction> parse_direction(std::string_view token) noexcept {
  if (token == "depends-on") {
    return Direction::DependsOn;
  }
  if (token == "mutual") {
    return Direction::Mutual;
  }
  return std::nullopt;
}

const DependencyKindDescriptor& describe(DependencyKind kind) noexcept {
  if (!kind_in_range(kind)) {
    return kUnknownKindDescriptor;
  }
  return kKindTable[static_cast<std::size_t>(kind) - 1];
}

const std::array<DependencyKindDescriptor, kDependencyKindCount>& dependency_kind_table() noexcept {
  return kKindTable;
}

bool requires_acyclic(DependencyKind kind) noexcept { return describe(kind).acyclic_required; }

Status DependencyEdgeSpec::validate(const RegistryLimits& limits) const {
  if (!source.valid()) {
    return Status::failure(ErrorCode::InvalidNodeReference, "the source endpoint is not a valid node reference");
  }
  if (!target.valid()) {
    return Status::failure(ErrorCode::InvalidNodeReference, "the target endpoint is not a valid node reference");
  }
  if (source == target) {
    std::string detail{"the source and target are both "};
    detail.append(source.to_canonical());
    detail.append("; an endpoint may not depend on itself");
    return Status::failure(ErrorCode::SelfDependencyProhibited, std::move(detail));
  }
  if (!kind_in_range(kind)) {
    return Status::failure(ErrorCode::InvalidKindToken, "dependency kind is not one of the declared kinds");
  }
  if (!strength_in_range(strength)) {
    return Status::failure(ErrorCode::InvalidStrengthToken,
                           "dependency strength is not one of the declared strengths");
  }
  if (!direction_in_range(direction)) {
    return Status::failure(ErrorCode::InvalidDirectionToken, "direction is not one of the declared directions");
  }
  if (!lifecycle_in_range(initial_lifecycle)) {
    return Status::failure(ErrorCode::InvalidLifecycleToken,
                           "initial lifecycle is not one of the declared states");
  }
  if (initial_lifecycle != LifecycleState::Proposed && initial_lifecycle != LifecycleState::Active) {
    std::string detail{"initial lifecycle '"};
    detail.append(to_token(initial_lifecycle));
    detail.append("' is not reachable by declaration; use the lifecycle transition command instead");
    return Status::failure(ErrorCode::InvalidArguments, std::move(detail));
  }

  const auto& descriptor = describe(kind);
  if (!descriptor.allowed_directions.contains(direction)) {
    std::string detail{"dependency kind "};
    detail.append(descriptor.token);
    detail.append(" does not allow direction ");
    detail.append(to_token(direction));
    return Status::failure(ErrorCode::DirectionNotAllowedForKind, std::move(detail));
  }

  if (direction == Direction::Mutual) {
    if (!descriptor.mutual_domains.contains(source.domain())) {
      std::string detail = domain_detail("source", source);
      detail.append(" is not allowed for a mutual ");
      detail.append(descriptor.token);
      detail.append(" edge");
      return Status::failure(ErrorCode::EndpointDomainNotAllowed, std::move(detail));
    }
    if (!descriptor.mutual_domains.contains(target.domain())) {
      std::string detail = domain_detail("target", target);
      detail.append(" is not allowed for a mutual ");
      detail.append(descriptor.token);
      detail.append(" edge");
      return Status::failure(ErrorCode::EndpointDomainNotAllowed, std::move(detail));
    }
  } else {
    if (!descriptor.allowed_source_domains.contains(source.domain())) {
      std::string detail = domain_detail("source", source);
      detail.append(" is not an allowed source for ");
      detail.append(descriptor.token);
      return Status::failure(ErrorCode::EndpointDomainNotAllowed, std::move(detail));
    }
    if (!descriptor.allowed_target_domains.contains(target.domain())) {
      std::string detail = domain_detail("target", target);
      detail.append(" is not an allowed target for ");
      detail.append(descriptor.token);
      return Status::failure(ErrorCode::EndpointDomainNotAllowed, std::move(detail));
    }
  }

  if (constraints.size() > limits.max_constraints_per_edge) {
    std::string detail{"the declaration carries "};
    detail.append(std::to_string(constraints.size()));
    detail.append(" constraints; the configured maximum is ");
    detail.append(std::to_string(limits.max_constraints_per_edge));
    return Status::failure(ErrorCode::TooManyConstraints, std::move(detail));
  }

  ConstraintKindMask seen = ConstraintKindMask::none();
  for (const auto& constraint : constraints) {
    const auto ordinal = static_cast<unsigned>(constraint.kind());
    if (ordinal < 1 || ordinal > kConstraintKindCount) {
      return Status::failure(ErrorCode::InvalidConstraintKind, "a constraint has an unrecognised kind");
    }
    if (!descriptor.allowed_constraints.contains(constraint.kind())) {
      std::string detail{"constraint "};
      detail.append(to_token(constraint.kind()));
      detail.append(" may not be attached to dependency kind ");
      detail.append(descriptor.token);
      return Status::failure(ErrorCode::ConstraintNotAllowedForKind, std::move(detail));
    }
    const ConstraintValueType expected = value_type_of(constraint.kind());
    if (expected == ConstraintValueType::Integer && !constraint.value().is_integer()) {
      return Status::failure(ErrorCode::InvalidConstraintValue,
                             "a constraint that takes an integer carries a token");
    }
    if (expected == ConstraintValueType::Token && !constraint.value().is_token()) {
      return Status::failure(ErrorCode::InvalidConstraintValue,
                             "a constraint that takes a token carries an integer");
    }
    if (expected == ConstraintValueType::Integer) {
      const auto [minimum, maximum] = integer_range_of(constraint.kind());
      const std::int64_t value = constraint.value().integer();
      if (value < minimum || value > maximum) {
        std::string detail{"constraint "};
        detail.append(to_token(constraint.kind()));
        detail.append(" value is outside its declared range");
        return Status::failure(ErrorCode::InvalidConstraintValue, std::move(detail));
      }
    } else if (expected == ConstraintValueType::Token) {
      if (const auto status = validate_constraint_token(constraint.kind(), constraint.value().token());
          !status.ok()) {
        return status;
      }
    } else {
      return Status::failure(ErrorCode::InvalidConstraintKind, "a constraint has an unrecognised kind");
    }
    if (seen.contains(constraint.kind())) {
      std::string detail{"constraint "};
      detail.append(to_token(constraint.kind()));
      detail.append(" appears more than once");
      return Status::failure(ErrorCode::DuplicateConstraintKind, std::move(detail));
    }
    seen = seen.with(constraint.kind());
  }

  const auto provenance_ordinal = static_cast<unsigned>(provenance.source());
  if (provenance_ordinal < 1 || provenance_ordinal > kProvenanceSourceCount) {
    return Status::failure(ErrorCode::InvalidProvenanceSource,
                           "the declaration has no recognised provenance source");
  }
  if (const auto status = validate_external_id(provenance.source_id(), limits.max_id_length, "provenance source id");
      !status.ok()) {
    return Status::failure(ErrorCode::InvalidProvenanceIdentifier, status.error().detail());
  }
  if (const auto status = validate_external_id(provenance.principal(), limits.max_id_length, "provenance principal");
      !status.ok()) {
    return Status::failure(ErrorCode::InvalidProvenanceIdentifier, status.error().detail());
  }
  if (provenance.recorded_at_unix_ms() < 0 || provenance.recorded_at_unix_ms() > kMaxTimestampUnixMs) {
    return Status::failure(ErrorCode::InvalidTimestamp,
                           "recorded-at timestamp is outside [0, 4102444800000] milliseconds since the Unix epoch");
  }
  return validate_annotation(provenance.annotation(), limits.max_annotation_length);
}

void order_endpoints_canonically(Direction direction, DependencyNodeRef& source, DependencyNodeRef& target) {
  if (direction != Direction::Mutual) {
    return;
  }
  if (target < source) {
    DependencyNodeRef temporary = std::move(source);
    source = std::move(target);
    target = std::move(temporary);
  }
}

std::string EdgeKey::to_text() const {
  std::string result;
  result.reserve(source_.id().size() + target_.id().size() + 48);
  result.append(source_.to_canonical());
  result.append(" -> ");
  result.append(target_.to_canonical());
  result.append(" [");
  result.append(to_token(kind_));
  result.push_back(']');
  return result;
}

bool operator==(const DependencyEdgeRecord& lhs, const DependencyEdgeRecord& rhs) noexcept {
  return lhs.id_ == rhs.id_ && lhs.revision_ == rhs.revision_ && lhs.kind_ == rhs.kind_ &&
         lhs.strength_ == rhs.strength_ && lhs.direction_ == rhs.direction_ && lhs.lifecycle_ == rhs.lifecycle_ &&
         lhs.source_ == rhs.source_ && lhs.target_ == rhs.target_ && lhs.constraints_ == rhs.constraints_ &&
         lhs.provenance_ == rhs.provenance_ && lhs.registered_generation_ == rhs.registered_generation_ &&
         lhs.last_modified_generation_ == rhs.last_modified_generation_;
}

std::strong_ordering operator<=>(const DependencyEdgeRecord& lhs, const DependencyEdgeRecord& rhs) noexcept {
  if (const auto by_key = lhs.key() <=> rhs.key(); by_key != 0) {
    return by_key;
  }
  return lhs.id_ <=> rhs.id_;
}

std::string DependencyEdgeRecord::to_text() const {
  std::string result;
  result.reserve(source_.id().size() + target_.id().size() + provenance_.annotation().size() + 160);
  result.append("edge id=");
  result.append(facility_dependency_registry::to_text(id_));
  result.append(" revision=");
  result.append(facility_dependency_registry::to_text(revision_));
  result.append(" kind=");
  result.append(to_token(kind_));
  result.append(" strength=");
  result.append(to_token(strength_));
  result.append(" direction=");
  result.append(to_token(direction_));
  result.append(" lifecycle=");
  result.append(to_token(lifecycle_));
  result.append(" source=");
  result.append(source_.to_canonical());
  result.append(" target=");
  result.append(target_.to_canonical());
  result.append(" registered=");
  result.append(facility_dependency_registry::to_text(registered_generation_));
  result.append(" modified=");
  result.append(facility_dependency_registry::to_text(last_modified_generation_));
  result.append(" constraints=");
  if (constraints_.empty()) {
    result.append("none");
  } else {
    bool first = true;
    for (const auto& constraint : constraints_) {
      if (!first) {
        result.push_back(';');
      }
      first = false;
      result.append(constraint.to_text());
    }
  }
  result.append(" provenance={");
  result.append(provenance_.to_text());
  result.push_back('}');
  return result;
}

}  // namespace facility_dependency_registry
