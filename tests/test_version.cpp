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

#include <set>
#include <string>

#include "facility_dependency_registry/facility_dependency_registry.hpp"
#include "test_harness.hpp"

using namespace facility_dependency_registry;

// Compile time facts about the version and format constants. These belong in
// static_assert rather than a runtime check, because they are properties of the
// build, not of a run.
static_assert(kStateFormatVersion >= 1);
static_assert(kContainerFormatVersion >= 1);
static_assert(kStoreLayoutVersion >= 1);
static_assert(kContainerHeaderSize == 64);
static_assert(kContainerMagic.size() == 8);
static_assert(kPointerMagic.size() == 8);
static_assert(kVersionMajor == 1);

FDEP_TEST(version, constants_are_consistent) {
  FDEP_CHECK_EQ(std::string{version_string()}, std::string{"1.0.0"});
  FDEP_CHECK_EQ(kVersionMajor, 1u);
  FDEP_CHECK_EQ(kVersionMinor, 0u);
  FDEP_CHECK_EQ(kVersionPatch, 0u);
  FDEP_CHECK_EQ(kContainerMagic.size(), std::size_t{8});
  FDEP_CHECK_EQ(kPointerMagic.size(), std::size_t{8});
  FDEP_CHECK_EQ(kContainerHeaderSize, std::uint64_t{64});
  FDEP_CHECK_EQ(std::string{kPointerFileName}, std::string{"CURRENT"});
  FDEP_CHECK_EQ(std::string{kWriterLockFileName}, std::string{"writer.lock"});
}

FDEP_TEST(version, error_codes_have_unique_tokens) {
  const ErrorCode codes[] = {
      ErrorCode::Ok,
      ErrorCode::InvalidNodeDomain,
      ErrorCode::InvalidNodeIdSyntax,
      ErrorCode::InvalidNodeReference,
      ErrorCode::InvalidEdgeId,
      ErrorCode::InvalidEdgeRevision,
      ErrorCode::InvalidGeneration,
      ErrorCode::InvalidKindToken,
      ErrorCode::InvalidStrengthToken,
      ErrorCode::InvalidDirectionToken,
      ErrorCode::InvalidLifecycleToken,
      ErrorCode::InvalidProvenanceSource,
      ErrorCode::InvalidProvenanceIdentifier,
      ErrorCode::InvalidAnnotation,
      ErrorCode::InvalidConstraintKind,
      ErrorCode::InvalidConstraintValue,
      ErrorCode::InvalidTimestamp,
      ErrorCode::InvalidLimits,
      ErrorCode::InvalidArguments,
      ErrorCode::InvalidStorePath,
      ErrorCode::DuplicateEdge,
      ErrorCode::EdgeNotFound,
      ErrorCode::SelfDependencyProhibited,
      ErrorCode::EndpointDomainNotAllowed,
      ErrorCode::DirectionNotAllowedForKind,
      ErrorCode::ConstraintNotAllowedForKind,
      ErrorCode::DuplicateConstraintKind,
      ErrorCode::TooManyConstraints,
      ErrorCode::ProhibitedCycle,
      ErrorCode::InvalidLifecycleTransition,
      ErrorCode::EdgeRetired,
      ErrorCode::DeclaredRefReferenced,
      ErrorCode::DuplicateDeclaration,
      ErrorCode::StaleGeneration,
      ErrorCode::StaleEdgeRevision,
      ErrorCode::RegistryClosed,
      ErrorCode::WriterFenced,
      ErrorCode::NotDurable,
      ErrorCode::GenerationExhausted,
      ErrorCode::EdgeCapacityExceeded,
      ErrorCode::DeclaredRefCapacityExceeded,
      ErrorCode::RequestLimitExceeded,
      ErrorCode::QueryResultLimitExceeded,
      ErrorCode::PayloadTooLarge,
      ErrorCode::AnalysisLimitExceeded,
      ErrorCode::Cancelled,
      ErrorCode::StoreNotFound,
      ErrorCode::StoreLocked,
      ErrorCode::StoreIoError,
      ErrorCode::StoreCorrupt,
      ErrorCode::StoreTruncated,
      ErrorCode::StoreIntegrityFailure,
      ErrorCode::StoreVersionUnsupported,
      ErrorCode::StoreReadOnly,
      ErrorCode::StoreLayoutInvalid,
      ErrorCode::StorePublishFailed,
  };

  std::set<std::string> tokens;
  std::set<std::uint16_t> values;
  for (const auto code : codes) {
    const std::string token{to_token(code)};
    FDEP_CHECK(!token.empty());
    FDEP_CHECK(token != "unknown-error-code");
    FDEP_CHECK(tokens.insert(token).second);
    FDEP_CHECK(values.insert(static_cast<std::uint16_t>(code)).second);
    FDEP_CHECK(!describe(code).empty());
  }
  FDEP_CHECK_EQ(tokens.size(), values.size());

  // The category of a code is derived from its numeric range and agrees with
  // the documented grouping.
  FDEP_CHECK(category_of(ErrorCode::Ok) == ErrorCategory::None);
  FDEP_CHECK(category_of(ErrorCode::InvalidNodeDomain) == ErrorCategory::Input);
  FDEP_CHECK(category_of(ErrorCode::DuplicateEdge) == ErrorCategory::Semantics);
  FDEP_CHECK(category_of(ErrorCode::StaleGeneration) == ErrorCategory::Authority);
  FDEP_CHECK(category_of(ErrorCode::Cancelled) == ErrorCategory::Bounds);
  FDEP_CHECK(category_of(ErrorCode::StoreCorrupt) == ErrorCategory::Persistence);
}

FDEP_TEST(version, error_rendering_is_stable) {
  const RegistryError error{ErrorCode::StaleGeneration, "expected 3 but found 4"};
  FDEP_CHECK_EQ(error.to_string(), std::string{"stale-generation: expected 3 but found 4"});
  FDEP_CHECK(error.explain().find("[stale-generation]") != std::string::npos);
  FDEP_CHECK(error.explain().find("expected 3 but found 4") != std::string::npos);
  FDEP_CHECK_EQ(error.category(), ErrorCategory::Authority);

  const RegistryError empty{ErrorCode::EdgeNotFound, ""};
  FDEP_CHECK_EQ(empty.to_string(), std::string{"edge-not-found"});
}

FDEP_TEST(version, status_and_result_carry_the_error) {
  const Status ok = Status::success();
  FDEP_CHECK(ok.ok());
  FDEP_CHECK(static_cast<bool>(ok));
  FDEP_CHECK_EQ(ok.code(), ErrorCode::Ok);

  const Status bad = Status::failure(ErrorCode::StoreLocked, "held by pid 42");
  FDEP_CHECK(!bad.ok());
  FDEP_CHECK_EQ(bad.code(), ErrorCode::StoreLocked);

  const Result<int> value{7};
  FDEP_CHECK(value.has_value());
  FDEP_CHECK_EQ(value.value(), 7);
  FDEP_CHECK_EQ(value.code(), ErrorCode::Ok);
  FDEP_CHECK(value.status().ok());

  const Result<int> failure = Result<int>::failure(ErrorCode::InvalidArguments, "nope");
  FDEP_CHECK(!failure.has_value());
  FDEP_CHECK_EQ(failure.error().code(), ErrorCode::InvalidArguments);
  FDEP_CHECK_EQ(failure.code(), ErrorCode::InvalidArguments);
  FDEP_CHECK(!failure.status().ok());
}
