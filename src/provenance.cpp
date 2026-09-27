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

#include "facility_dependency_registry/provenance.hpp"

#include <array>
#include <string>

#include "facility_dependency_registry/node_ref.hpp"

namespace facility_dependency_registry {
namespace {

struct ProvenanceDescriptor {
  ProvenanceSource source;
  std::string_view token;
  std::string_view description;
};

constexpr std::array<ProvenanceDescriptor, kProvenanceSourceCount> kProvenanceTable{{
    {ProvenanceSource::OperatorDeclaration, "operator-declaration", "declared directly by an operator"},
    {ProvenanceSource::FacilityInventoryImport, "facility-inventory-import",
     "imported from a facility inventory export"},
    {ProvenanceSource::AssetRegistryExport, "asset-registry-export", "imported from an Asset Registry export"},
    {ProvenanceSource::RackRegistryExport, "rack-registry-export", "imported from a Rack Registry export"},
    {ProvenanceSource::ElectricalDomainExport, "electrical-domain-export",
     "imported from an electrical domain export"},
    {ProvenanceSource::CoolingDomainExport, "cooling-domain-export", "imported from a cooling domain export"},
    {ProvenanceSource::FacilityServiceExport, "facility-service-export",
     "imported from a facility service catalogue export"},
    {ProvenanceSource::AsiDomainExport, "asi-domain-export", "imported from a composed ASI domain export"},
    {ProvenanceSource::DfiDomainExport, "dfi-domain-export", "imported from a composed DFI domain export"},
    {ProvenanceSource::ChangeRecord, "change-record", "derived from a recorded configuration change"},
    {ProvenanceSource::RegistryMigration, "registry-migration",
     "carried over from an earlier registry generation or store"},
}};

bool source_in_range(ProvenanceSource source) noexcept {
  const auto ordinal = static_cast<unsigned>(source);
  return ordinal >= 1 && ordinal <= kProvenanceSourceCount;
}

}  // namespace

std::string_view to_token(ProvenanceSource source) noexcept {
  if (!source_in_range(source)) {
    return "unknown-provenance-source";
  }
  return kProvenanceTable[static_cast<std::size_t>(source) - 1].token;
}

std::string_view describe(ProvenanceSource source) noexcept {
  if (!source_in_range(source)) {
    return "unrecognised provenance source";
  }
  return kProvenanceTable[static_cast<std::size_t>(source) - 1].description;
}

std::optional<ProvenanceSource> parse_provenance_source(std::string_view token) noexcept {
  for (const auto& entry : kProvenanceTable) {
    if (entry.token == token) {
      return entry.source;
    }
  }
  return std::nullopt;
}

Result<ProvenanceRecord> ProvenanceRecord::create(ProvenanceSource source, std::string_view source_id,
                                                  std::string_view principal, std::int64_t recorded_at_unix_ms,
                                                  std::string_view annotation, std::size_t max_id_length,
                                                  std::size_t max_annotation_length) {
  if (!source_in_range(source)) {
    return Result<ProvenanceRecord>::failure(ErrorCode::InvalidProvenanceSource,
                                             "provenance source is not one of the declared sources");
  }
  if (const auto status = validate_external_id(source_id, max_id_length, "provenance source id"); !status.ok()) {
    return Result<ProvenanceRecord>::failure(ErrorCode::InvalidProvenanceIdentifier, status.error().detail());
  }
  if (const auto status = validate_external_id(principal, max_id_length, "provenance principal"); !status.ok()) {
    return Result<ProvenanceRecord>::failure(ErrorCode::InvalidProvenanceIdentifier, status.error().detail());
  }
  if (recorded_at_unix_ms < 0 || recorded_at_unix_ms > kMaxTimestampUnixMs) {
    return Result<ProvenanceRecord>::failure(
        ErrorCode::InvalidTimestamp,
        "recorded-at timestamp is outside [0, 4102444800000] milliseconds since the Unix epoch");
  }
  if (const auto status = validate_annotation(annotation, max_annotation_length); !status.ok()) {
    return Result<ProvenanceRecord>::failure(status.error());
  }

  ProvenanceRecord record;
  record.source_ = source;
  record.source_id_.assign(source_id);
  record.principal_.assign(principal);
  record.recorded_at_unix_ms_ = recorded_at_unix_ms;
  record.annotation_.assign(annotation);
  return record;
}

bool ProvenanceRecord::valid() const noexcept {
  if (!source_in_range(source_)) {
    return false;
  }
  if (!is_valid_external_id(source_id_, kHardMaxIdLength) || !is_valid_external_id(principal_, kHardMaxIdLength)) {
    return false;
  }
  if (recorded_at_unix_ms_ < 0 || recorded_at_unix_ms_ > kMaxTimestampUnixMs) {
    return false;
  }
  return validate_annotation(annotation_, kHardMaxAnnotationLength).ok();
}

std::string ProvenanceRecord::to_text() const {
  std::string result;
  result.reserve(source_id_.size() + principal_.size() + annotation_.size() + 64);
  result.append("source=");
  result.append(to_token(source_));
  result.append(" source-id=");
  result.append(source_id_);
  result.append(" principal=");
  result.append(principal_);
  result.append(" recorded-at-ms=");
  result.append(std::to_string(recorded_at_unix_ms_));
  if (!annotation_.empty()) {
    result.append(" annotation=\"");
    result.append(annotation_);
    result.push_back('"');
  }
  return result;
}

std::strong_ordering operator<=>(const ProvenanceRecord& lhs, const ProvenanceRecord& rhs) noexcept {
  if (const auto by_source = static_cast<unsigned>(lhs.source_) <=> static_cast<unsigned>(rhs.source_);
      by_source != 0) {
    return by_source;
  }
  if (const auto by_id = lhs.source_id_.compare(rhs.source_id_) <=> 0; by_id != 0) {
    return by_id;
  }
  if (const auto by_principal = lhs.principal_.compare(rhs.principal_) <=> 0; by_principal != 0) {
    return by_principal;
  }
  if (const auto by_time = lhs.recorded_at_unix_ms_ <=> rhs.recorded_at_unix_ms_; by_time != 0) {
    return by_time;
  }
  return lhs.annotation_.compare(rhs.annotation_) <=> 0;
}

}  // namespace facility_dependency_registry
