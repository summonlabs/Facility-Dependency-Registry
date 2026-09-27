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

#ifndef FACILITY_DEPENDENCY_REGISTRY_PROVENANCE_HPP
#define FACILITY_DEPENDENCY_REGISTRY_PROVENANCE_HPP

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "facility_dependency_registry/errors.hpp"

namespace facility_dependency_registry {

/// Where an authoritative declaration came from.
///
/// Provenance records the origin of a declaration; it never grants authority.
/// Values are contiguous and start at 1.
enum class ProvenanceSource : std::uint8_t {
  /// Declared directly by an operator through the API or the inspection tool.
  OperatorDeclaration = 1,
  /// Imported from a facility inventory export.
  FacilityInventoryImport = 2,
  /// Imported from an Asset Registry export.
  AssetRegistryExport = 3,
  /// Imported from a Rack Registry export.
  RackRegistryExport = 4,
  /// Imported from an electrical domain export.
  ElectricalDomainExport = 5,
  /// Imported from a cooling domain export.
  CoolingDomainExport = 6,
  /// Imported from a facility service catalogue export.
  FacilityServiceExport = 7,
  /// Imported from a composed ASI domain export.
  AsiDomainExport = 8,
  /// Imported from a composed DFI domain export.
  DfiDomainExport = 9,
  /// Derived from a recorded configuration change.
  ChangeRecord = 10,
  /// Carried over from an earlier registry generation or an earlier store.
  RegistryMigration = 11,
};

inline constexpr std::size_t kProvenanceSourceCount = 11;

[[nodiscard]] std::string_view to_token(ProvenanceSource source) noexcept;
[[nodiscard]] std::string_view describe(ProvenanceSource source) noexcept;
[[nodiscard]] std::optional<ProvenanceSource> parse_provenance_source(std::string_view token) noexcept;

/// Latest timestamp this repository accepts, in milliseconds since the Unix
/// epoch: 2100-01-01T00:00:00Z. Timestamps are supplied by the caller and are
/// recorded, never used as authority.
inline constexpr std::int64_t kMaxTimestampUnixMs = 4'102'444'800'000;

/// True when `text` is well formed UTF-8: no overlong encodings, no UTF-16
/// surrogate code points, no code points above U+10FFFF, no truncated
/// sequences, no stray continuation bytes and no embedded NUL.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

/// Validates a human readable annotation: well formed UTF-8, at most
/// `max_length` bytes, and no C0 or C1 control characters other than the
/// printable ASCII range. Rejected text is never repaired or truncated.
[[nodiscard]] Status validate_annotation(std::string_view text, std::size_t max_length);

/// Who declared an edge, and when they said they recorded it.
///
/// A provenance record is mandatory for every authoritative declaration. It is
/// part of the edge payload for duplicate detection and canonical serialization
/// and therefore participates in equality exactly as its fields do.
class ProvenanceRecord {
 public:
  ProvenanceRecord() = default;

  /// `source_id` names the originating system or record, `principal` names the
  /// acting principal, and `annotation` is optional free text. Both
  /// identifiers must match the canonical external identifier syntax.
  [[nodiscard]] static Result<ProvenanceRecord> create(ProvenanceSource source, std::string_view source_id,
                                                       std::string_view principal, std::int64_t recorded_at_unix_ms,
                                                       std::string_view annotation, std::size_t max_id_length,
                                                       std::size_t max_annotation_length);

  [[nodiscard]] ProvenanceSource source() const noexcept { return source_; }
  [[nodiscard]] const std::string& source_id() const noexcept { return source_id_; }
  [[nodiscard]] const std::string& principal() const noexcept { return principal_; }
  [[nodiscard]] std::int64_t recorded_at_unix_ms() const noexcept { return recorded_at_unix_ms_; }
  [[nodiscard]] const std::string& annotation() const noexcept { return annotation_; }
  [[nodiscard]] bool valid() const noexcept;

  /// `source=<token> source-id=<id> principal=<id> recorded-at-ms=<n>`
  [[nodiscard]] std::string to_text() const;

  friend bool operator==(const ProvenanceRecord& lhs, const ProvenanceRecord& rhs) noexcept {
    return lhs.source_ == rhs.source_ && lhs.source_id_ == rhs.source_id_ && lhs.principal_ == rhs.principal_ &&
           lhs.recorded_at_unix_ms_ == rhs.recorded_at_unix_ms_ && lhs.annotation_ == rhs.annotation_;
  }
  friend bool operator!=(const ProvenanceRecord& lhs, const ProvenanceRecord& rhs) noexcept { return !(lhs == rhs); }

  /// Canonical order over the serialized fields, used by canonical encodings.
  friend std::strong_ordering operator<=>(const ProvenanceRecord& lhs, const ProvenanceRecord& rhs) noexcept;

 private:
  ProvenanceSource source_{};
  std::string source_id_{};
  std::string principal_{};
  std::int64_t recorded_at_unix_ms_{0};
  std::string annotation_{};
};

}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_PROVENANCE_HPP
