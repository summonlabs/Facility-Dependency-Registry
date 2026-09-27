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

#include "facility_dependency_registry/node_ref.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace facility_dependency_registry {
namespace {

struct DomainDescriptor {
  NodeDomain domain;
  std::string_view token;
  std::string_view description;
};

constexpr std::array<DomainDescriptor, kNodeDomainCount> kDomainTable{{
    {NodeDomain::Asset, "asset", "an asset owned by the Asset Registry"},
    {NodeDomain::Rack, "rack", "a rack owned by the Rack Registry"},
    {NodeDomain::ElectricalDomain, "electrical-domain",
     "an electrical domain object owned by the facility control plane"},
    {NodeDomain::CoolingDomain, "cooling-domain",
     "a cooling domain object owned by the facility control plane"},
    {NodeDomain::FacilityService, "facility-service", "a facility service from the facility service catalogue"},
    {NodeDomain::AsiDomain, "asi-domain", "a composed Accelerated Systems Infrastructure domain"},
    {NodeDomain::DfiDomain, "dfi-domain", "a composed Distributed Fabric Infrastructure domain"},
}};

bool domain_in_range(NodeDomain domain) noexcept {
  const auto ordinal = static_cast<unsigned>(domain);
  return ordinal >= 1 && ordinal <= kNodeDomainCount;
}

bool is_alphanumeric(char character) noexcept {
  return (character >= '0' && character <= '9') || (character >= 'A' && character <= 'Z') ||
         (character >= 'a' && character <= 'z');
}

bool is_identifier_character(char character) noexcept {
  return is_alphanumeric(character) || character == '.' || character == '_' || character == '-';
}

bool is_separator(char character) noexcept { return character == '.' || character == '-'; }

}  // namespace

std::string_view to_token(NodeDomain domain) noexcept {
  if (!domain_in_range(domain)) {
    return "unknown-node-domain";
  }
  return kDomainTable[static_cast<std::size_t>(domain) - 1].token;
}

std::string_view describe(NodeDomain domain) noexcept {
  if (!domain_in_range(domain)) {
    return "unrecognised node domain";
  }
  return kDomainTable[static_cast<std::size_t>(domain) - 1].description;
}

std::optional<NodeDomain> parse_node_domain(std::string_view token) noexcept {
  for (const auto& entry : kDomainTable) {
    if (entry.token == token) {
      return entry.domain;
    }
  }
  return std::nullopt;
}

bool is_valid_external_id(std::string_view id, std::size_t max_length) noexcept {
  if (id.empty() || id.size() > max_length) {
    return false;
  }
  if (!is_alphanumeric(id.front()) || !is_alphanumeric(id.back())) {
    return false;
  }
  for (std::size_t index = 0; index < id.size(); ++index) {
    const char character = id[index];
    if (!is_identifier_character(character)) {
      return false;
    }
    if (is_separator(character) && index + 1 < id.size() && is_separator(id[index + 1])) {
      return false;
    }
  }
  return true;
}

Status validate_external_id(std::string_view id, std::size_t max_length, std::string_view what) {
  std::string detail{what};
  if (id.empty()) {
    detail.append(" is empty");
    return Status::failure(ErrorCode::InvalidNodeIdSyntax, std::move(detail));
  }
  if (id.size() > max_length) {
    detail.append(" is longer than the configured maximum of ");
    detail.append(std::to_string(max_length));
    detail.append(" bytes");
    return Status::failure(ErrorCode::InvalidNodeIdSyntax, std::move(detail));
  }
  if (!is_valid_external_id(id, max_length)) {
    detail.append(" is not a canonical external identifier: it must start and end with an alphanumeric "
                  "character and contain only A-Z, a-z, 0-9, '.', '_' and '-', with '.' and '-' used as single "
                  "separators");
    return Status::failure(ErrorCode::InvalidNodeIdSyntax, std::move(detail));
  }
  return Status::success();
}

Result<DependencyNodeRef> DependencyNodeRef::create(NodeDomain domain, std::string_view id, std::size_t max_length) {
  if (!domain_in_range(domain)) {
    return Result<DependencyNodeRef>::failure(ErrorCode::InvalidNodeDomain,
                                              "node domain is not one of the declared domains");
  }
  if (const auto status = validate_external_id(id, max_length, "node identifier"); !status.ok()) {
    return Result<DependencyNodeRef>::failure(status.error());
  }
  DependencyNodeRef result;
  result.domain_ = domain;
  result.id_.assign(id);
  return result;
}

Result<DependencyNodeRef> DependencyNodeRef::parse(std::string_view canonical, std::size_t max_length) {
  const std::size_t separator = canonical.find(':');
  if (separator == std::string_view::npos) {
    return Result<DependencyNodeRef>::failure(
        ErrorCode::InvalidNodeReference, "expected the canonical form <domain>:<identifier>");
  }
  const std::string_view domain_token = canonical.substr(0, separator);
  const std::string_view identifier = canonical.substr(separator + 1);
  const auto domain = parse_node_domain(domain_token);
  if (!domain.has_value()) {
    std::string detail{"unknown node domain '"};
    detail.append(domain_token);
    detail.push_back('\'');
    return Result<DependencyNodeRef>::failure(ErrorCode::InvalidNodeDomain, std::move(detail));
  }
  return create(*domain, identifier, max_length);
}

bool DependencyNodeRef::valid() const noexcept {
  return domain_in_range(domain_) && is_valid_external_id(id_, kHardMaxIdLength);
}

std::string DependencyNodeRef::to_canonical() const {
  std::string result;
  result.reserve(id_.size() + 20);
  result.append(to_token(domain_));
  result.push_back(':');
  result.append(id_);
  return result;
}

Result<NodeRefSet> NodeRefSet::create(std::vector<DependencyNodeRef> refs, std::uint32_t max_refs) {
  if (refs.size() > max_refs) {
    return Result<NodeRefSet>::failure(ErrorCode::RequestLimitExceeded,
                                       "more node references than the configured maximum of " +
                                           std::to_string(max_refs));
  }
  for (const auto& ref : refs) {
    if (!ref.valid()) {
      return Result<NodeRefSet>::failure(ErrorCode::InvalidNodeReference,
                                         "a node reference in the set is not a valid external reference");
    }
  }
  std::sort(refs.begin(), refs.end());
  refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
  NodeRefSet result;
  result.refs_ = std::move(refs);
  return result;
}

bool NodeRefSet::contains(const DependencyNodeRef& ref) const noexcept {
  return std::binary_search(refs_.begin(), refs_.end(), ref);
}

std::string to_text(const DependencyNodeRef& ref) { return ref.to_canonical(); }

std::string to_text(const NodeRefSet& set) {
  std::string result;
  for (const auto& ref : set.refs()) {
    if (!result.empty()) {
      result.push_back('\n');
    }
    result.append(ref.to_canonical());
  }
  return result;
}

}  // namespace facility_dependency_registry
