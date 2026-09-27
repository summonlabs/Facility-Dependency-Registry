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

#include <string>

#include "facility_dependency_registry/facility_dependency_registry.hpp"
#include "test_harness.hpp"

using namespace facility_dependency_registry;

namespace {

Result<ProvenanceRecord> make(ProvenanceSource source, std::string_view source_id, std::string_view principal,
                              std::int64_t when, std::string_view annotation) {
  return ProvenanceRecord::create(source, source_id, principal, when, annotation, 160, 512);
}

}  // namespace

FDEP_TEST(provenance, source_tokens_round_trip) {
  const ProvenanceSource sources[] = {
      ProvenanceSource::OperatorDeclaration,      ProvenanceSource::FacilityInventoryImport,
      ProvenanceSource::AssetRegistryExport,      ProvenanceSource::RackRegistryExport,
      ProvenanceSource::ElectricalDomainExport,   ProvenanceSource::CoolingDomainExport,
      ProvenanceSource::FacilityServiceExport,    ProvenanceSource::AsiDomainExport,
      ProvenanceSource::DfiDomainExport,          ProvenanceSource::ChangeRecord,
      ProvenanceSource::RegistryMigration};
  for (const auto source : sources) {
    const std::string token{to_token(source)};
    FDEP_CHECK(!token.empty());
    FDEP_CHECK(!describe(source).empty());
    const auto parsed = parse_provenance_source(token);
    FDEP_REQUIRE(parsed.has_value());
    FDEP_CHECK(*parsed == source);
  }
  FDEP_CHECK(!parse_provenance_source("").has_value());
  FDEP_CHECK(!parse_provenance_source("operator").has_value());
  FDEP_CHECK_EQ(to_token(static_cast<ProvenanceSource>(0)), std::string{"unknown-provenance-source"});
  FDEP_CHECK_EQ(to_token(static_cast<ProvenanceSource>(200)), std::string{"unknown-provenance-source"});
}

FDEP_TEST(provenance, create_validates_every_field) {
  const auto good = make(ProvenanceSource::OperatorDeclaration, "change-1234", "operator-alice", 1'700'000'000'000,
                         "declared during change window");
  FDEP_REQUIRE_OK(good);
  FDEP_CHECK(good.value().valid());
  FDEP_CHECK_EQ(good.value().source_id(), std::string{"change-1234"});
  FDEP_CHECK_EQ(good.value().principal(), std::string{"operator-alice"});
  FDEP_CHECK_EQ(good.value().recorded_at_unix_ms(), std::int64_t{1'700'000'000'000});
  FDEP_CHECK(good.value().to_text().find("source=operator-declaration") != std::string::npos);
  FDEP_CHECK(good.value().to_text().find("annotation=\"declared during change window\"") != std::string::npos);

  FDEP_CHECK_CODE(make(static_cast<ProvenanceSource>(0), "a", "b", 0, ""), ErrorCode::InvalidProvenanceSource);
  FDEP_CHECK_CODE(make(ProvenanceSource::OperatorDeclaration, "", "b", 0, ""),
                  ErrorCode::InvalidProvenanceIdentifier);
  FDEP_CHECK_CODE(make(ProvenanceSource::OperatorDeclaration, "a", "", 0, ""),
                  ErrorCode::InvalidProvenanceIdentifier);
  FDEP_CHECK_CODE(make(ProvenanceSource::OperatorDeclaration, "a..b", "b", 0, ""),
                  ErrorCode::InvalidProvenanceIdentifier);
  FDEP_CHECK_CODE(make(ProvenanceSource::OperatorDeclaration, "a", "b", -1, ""), ErrorCode::InvalidTimestamp);
  FDEP_CHECK_CODE(make(ProvenanceSource::OperatorDeclaration, "a", "b", kMaxTimestampUnixMs + 1, ""),
                  ErrorCode::InvalidTimestamp);
  FDEP_CHECK(make(ProvenanceSource::OperatorDeclaration, "a", "b", kMaxTimestampUnixMs, "").has_value());
  FDEP_CHECK_CODE(make(ProvenanceSource::OperatorDeclaration, "a", "b", 0, "bad\u0001text"),
                  ErrorCode::InvalidAnnotation);
  FDEP_CHECK_CODE(make(ProvenanceSource::OperatorDeclaration, "a", "b", 0, "line\nbreak"),
                  ErrorCode::InvalidAnnotation);
  FDEP_CHECK_CODE(make(ProvenanceSource::OperatorDeclaration, "a", "b", 0, std::string(513, 'x')),
                  ErrorCode::InvalidAnnotation);
  FDEP_CHECK(make(ProvenanceSource::OperatorDeclaration, "a", "b", 0, std::string(512, 'x')).has_value());
}

FDEP_TEST(provenance, annotations_must_be_well_formed_utf8) {
  // Two byte sequence: valid.
  FDEP_CHECK(make(ProvenanceSource::OperatorDeclaration, "a", "b", 0, "caf\u00e9").has_value());
  // Three byte sequence: valid.
  FDEP_CHECK(make(ProvenanceSource::OperatorDeclaration, "a", "b", 0, "\u20ac").has_value());
  // Four byte sequence: valid.
  FDEP_CHECK(make(ProvenanceSource::OperatorDeclaration, "a", "b", 0, "\U0001F600").has_value());

  const auto rejects = [](const std::string& text) {
    return !make(ProvenanceSource::OperatorDeclaration, "a", "b", 0, text).has_value();
  };
  FDEP_CHECK(rejects(std::string{"\x80"}));                    // stray continuation byte
  FDEP_CHECK(rejects(std::string{"\xC0\x80"}));                // overlong NUL
  FDEP_CHECK(rejects(std::string{"\xC1\xBF"}));                // overlong
  FDEP_CHECK(rejects(std::string{"\xE0\x80\x80"}));            // overlong
  FDEP_CHECK(rejects(std::string{"\xF0\x80\x80\x80"}));        // overlong
  FDEP_CHECK(rejects(std::string{"\xED\xA0\x80"}));            // UTF-16 surrogate
  FDEP_CHECK(rejects(std::string{"\xED\xBF\xBF"}));            // UTF-16 surrogate
  FDEP_CHECK(rejects(std::string{"\xF5\x80\x80\x80"}));        // above U+10FFFF
  FDEP_CHECK(rejects(std::string{"\xF4\x90\x80\x80"}));        // above U+10FFFF
  FDEP_CHECK(rejects(std::string{"\xC3"}));                    // truncated
  FDEP_CHECK(rejects(std::string{"\xE2\x82"}));                // truncated
  FDEP_CHECK(rejects(std::string{"\xF0\x9F\x98"}));            // truncated
  FDEP_CHECK(rejects(std::string{"\xFF"}));                    // never valid
  FDEP_CHECK(rejects(std::string{"\xFE"}));                    // never valid
  FDEP_CHECK(rejects(std::string{"a\x00b", 3}));               // embedded NUL

  FDEP_CHECK(!is_valid_utf8(std::string{"\xE2\x82"}));
  FDEP_CHECK(is_valid_utf8("plain ascii"));
  FDEP_CHECK(is_valid_utf8(""));
}

FDEP_TEST(provenance, equality_and_order_use_every_field) {
  const auto base = make(ProvenanceSource::OperatorDeclaration, "change-1", "alice", 10, "note");
  FDEP_REQUIRE_OK(base);
  const auto same = make(ProvenanceSource::OperatorDeclaration, "change-1", "alice", 10, "note");
  FDEP_REQUIRE_OK(same);
  FDEP_CHECK(base.value() == same.value());
  FDEP_CHECK(!(base.value() < same.value()));
  FDEP_CHECK(!(same.value() < base.value()));

  const auto other_time = make(ProvenanceSource::OperatorDeclaration, "change-1", "alice", 11, "note");
  FDEP_REQUIRE_OK(other_time);
  FDEP_CHECK(base.value() != other_time.value());

  const auto other_source = make(ProvenanceSource::ChangeRecord, "change-1", "alice", 10, "note");
  FDEP_REQUIRE_OK(other_source);
  FDEP_CHECK(base.value() != other_source.value());
  FDEP_CHECK(base.value() < other_source.value());

  const auto other_principal = make(ProvenanceSource::OperatorDeclaration, "change-1", "bob", 10, "note");
  FDEP_REQUIRE_OK(other_principal);
  FDEP_CHECK(base.value() < other_principal.value());

  const auto other_annotation = make(ProvenanceSource::OperatorDeclaration, "change-1", "alice", 10, "note2");
  FDEP_REQUIRE_OK(other_annotation);
  FDEP_CHECK(base.value() < other_annotation.value());

  const auto other_id = make(ProvenanceSource::OperatorDeclaration, "change-2", "alice", 10, "note");
  FDEP_REQUIRE_OK(other_id);
  FDEP_CHECK(base.value() < other_id.value());

  const ProvenanceRecord empty;
  FDEP_CHECK(!empty.valid());
  FDEP_CHECK(empty != base.value());
}
