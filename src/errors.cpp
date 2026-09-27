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

#include "facility_dependency_registry/errors.hpp"

namespace facility_dependency_registry {
namespace {

struct ErrorDescriptor {
  ErrorCode code;
  std::string_view token;
  ErrorCategory category;
  std::string_view clause;
};

// The single source of truth for error identity. The order of this table has
// no meaning; the numeric values in the header do.
constexpr ErrorDescriptor kErrors[] = {
    {ErrorCode::Ok, "ok", ErrorCategory::None, "no error"},

    {ErrorCode::InvalidNodeDomain, "invalid-node-domain", ErrorCategory::Input, "unknown node domain"},
    {ErrorCode::InvalidNodeIdSyntax, "invalid-node-id-syntax", ErrorCategory::Input,
     "identifier is not a canonical external identifier"},
    {ErrorCode::InvalidNodeReference, "invalid-node-reference", ErrorCategory::Input,
     "node reference is not usable"},
    {ErrorCode::InvalidEdgeId, "invalid-edge-id", ErrorCategory::Input, "edge identity is not assigned"},
    {ErrorCode::InvalidEdgeRevision, "invalid-edge-revision", ErrorCategory::Input,
     "edge revision is not assigned"},
    {ErrorCode::InvalidGeneration, "invalid-generation", ErrorCategory::Input,
     "generation is outside the accepted range"},
    {ErrorCode::InvalidKindToken, "invalid-kind-token", ErrorCategory::Input,
     "unknown dependency kind"},
    {ErrorCode::InvalidStrengthToken, "invalid-strength-token", ErrorCategory::Input,
     "unknown dependency strength"},
    {ErrorCode::InvalidDirectionToken, "invalid-direction-token", ErrorCategory::Input,
     "unknown direction"},
    {ErrorCode::InvalidLifecycleToken, "invalid-lifecycle-token", ErrorCategory::Input,
     "unknown lifecycle state"},
    {ErrorCode::InvalidProvenanceSource, "invalid-provenance-source", ErrorCategory::Input,
     "unknown provenance source"},
    {ErrorCode::InvalidProvenanceIdentifier, "invalid-provenance-identifier", ErrorCategory::Input,
     "provenance identifier is not a canonical external identifier"},
    {ErrorCode::InvalidAnnotation, "invalid-annotation", ErrorCategory::Input,
     "annotation is not acceptable text"},
    {ErrorCode::InvalidConstraintKind, "invalid-constraint-kind", ErrorCategory::Input,
     "unknown constraint kind"},
    {ErrorCode::InvalidConstraintValue, "invalid-constraint-value", ErrorCategory::Input,
     "constraint value is outside the declared domain"},
    {ErrorCode::InvalidTimestamp, "invalid-timestamp", ErrorCategory::Input,
     "timestamp is outside the accepted range"},
    {ErrorCode::InvalidLimits, "invalid-limits", ErrorCategory::Input,
     "resource limits are incoherent"},
    {ErrorCode::InvalidArguments, "invalid-arguments", ErrorCategory::Input,
     "arguments do not describe a usable request"},
    {ErrorCode::InvalidStorePath, "invalid-store-path", ErrorCategory::Input,
     "store path is not usable"},

    {ErrorCode::DuplicateEdge, "duplicate-edge", ErrorCategory::Semantics,
     "an edge with this key already exists"},
    {ErrorCode::EdgeNotFound, "edge-not-found", ErrorCategory::Semantics,
     "no edge with this identity exists"},
    {ErrorCode::SelfDependencyProhibited, "self-dependency-prohibited", ErrorCategory::Semantics,
     "an endpoint may not depend on itself"},
    {ErrorCode::EndpointDomainNotAllowed, "endpoint-domain-not-allowed", ErrorCategory::Semantics,
     "an endpoint domain is not allowed for this dependency kind"},
    {ErrorCode::DirectionNotAllowedForKind, "direction-not-allowed-for-kind", ErrorCategory::Semantics,
     "this direction is not allowed for this dependency kind"},
    {ErrorCode::ConstraintNotAllowedForKind, "constraint-not-allowed-for-kind", ErrorCategory::Semantics,
     "this constraint kind may not be attached to this dependency kind"},
    {ErrorCode::DuplicateConstraintKind, "duplicate-constraint-kind", ErrorCategory::Semantics,
     "the same constraint kind appears more than once"},
    {ErrorCode::TooManyConstraints, "too-many-constraints", ErrorCategory::Semantics,
     "more constraints than the configured maximum"},
    {ErrorCode::ProhibitedCycle, "prohibited-cycle", ErrorCategory::Semantics,
     "the edge would create a cycle that this dependency kind forbids"},
    {ErrorCode::InvalidLifecycleTransition, "invalid-lifecycle-transition", ErrorCategory::Semantics,
     "the lifecycle transition is not permitted"},
    {ErrorCode::EdgeRetired, "edge-retired", ErrorCategory::Semantics,
     "the edge is retired and can no longer be changed"},
    {ErrorCode::DeclaredRefReferenced, "declared-ref-referenced", ErrorCategory::Semantics,
     "the declaration is still referenced by stored edges"},
    {ErrorCode::DuplicateDeclaration, "duplicate-declaration", ErrorCategory::Semantics,
     "a different declaration for this reference already exists"},

    {ErrorCode::StaleGeneration, "stale-generation", ErrorCategory::Authority,
     "the request was addressed to a generation that is no longer current"},
    {ErrorCode::StaleEdgeRevision, "stale-edge-revision", ErrorCategory::Authority,
     "the request was addressed to a revision that is no longer current"},
    {ErrorCode::RegistryClosed, "registry-closed", ErrorCategory::Authority,
     "the registry is closed and accepts no further mutations"},
    {ErrorCode::WriterFenced, "writer-fenced", ErrorCategory::Authority,
     "this writer no longer holds authority over the store"},
    {ErrorCode::NotDurable, "not-durable", ErrorCategory::Authority,
     "the registry has no durable store"},
    {ErrorCode::GenerationExhausted, "generation-exhausted", ErrorCategory::Authority,
     "the generation limit has been reached"},

    {ErrorCode::EdgeCapacityExceeded, "edge-capacity-exceeded", ErrorCategory::Bounds,
     "the graph holds the maximum number of edges"},
    {ErrorCode::DeclaredRefCapacityExceeded, "declared-ref-capacity-exceeded", ErrorCategory::Bounds,
     "the registry holds the maximum number of declarations"},
    {ErrorCode::RequestLimitExceeded, "request-limit-exceeded", ErrorCategory::Bounds,
     "the request asks for more than the configured limit allows"},
    {ErrorCode::QueryResultLimitExceeded, "query-result-limit-exceeded", ErrorCategory::Bounds,
     "the result would exceed the configured result bound"},
    {ErrorCode::PayloadTooLarge, "payload-too-large", ErrorCategory::Bounds,
     "the payload exceeds the configured size bound"},
    {ErrorCode::AnalysisLimitExceeded, "analysis-limit-exceeded", ErrorCategory::Bounds,
     "the analysis would visit more nodes than the configured bound allows"},
    {ErrorCode::Cancelled, "cancelled", ErrorCategory::Bounds, "the request was cancelled"},

    {ErrorCode::StoreNotFound, "store-not-found", ErrorCategory::Persistence,
     "the store root does not exist"},
    {ErrorCode::StoreLocked, "store-locked", ErrorCategory::Persistence,
     "another writer holds the store lock"},
    {ErrorCode::StoreIoError, "store-io-error", ErrorCategory::Persistence,
     "the store could not be read or written"},
    {ErrorCode::StoreCorrupt, "store-corrupt", ErrorCategory::Persistence,
     "no usable authoritative state was found"},
    {ErrorCode::StoreTruncated, "store-truncated", ErrorCategory::Persistence,
     "the file is shorter than its own header declares"},
    {ErrorCode::StoreIntegrityFailure, "store-integrity-failure", ErrorCategory::Persistence,
     "the file failed its integrity check"},
    {ErrorCode::StoreVersionUnsupported, "store-version-unsupported", ErrorCategory::Persistence,
     "the file declares a format version this build does not implement"},
    {ErrorCode::StoreReadOnly, "store-read-only", ErrorCategory::Persistence,
     "the store was opened read-only"},
    {ErrorCode::StoreLayoutInvalid, "store-layout-invalid", ErrorCategory::Persistence,
     "the store directory does not have the expected layout"},
    {ErrorCode::StorePublishFailed, "store-publish-failed", ErrorCategory::Persistence,
     "the new generation could not be published"},
};

constexpr std::string_view kUnknownToken = "unknown-error-code";
constexpr std::string_view kUnknownClause = "unrecognised error code";

}  // namespace

std::string_view to_token(ErrorCode code) noexcept {
  for (const auto& descriptor : kErrors) {
    if (descriptor.code == code) {
      return descriptor.token;
    }
  }
  return kUnknownToken;
}

std::string_view describe(ErrorCode code) noexcept {
  for (const auto& descriptor : kErrors) {
    if (descriptor.code == code) {
      return descriptor.clause;
    }
  }
  return kUnknownClause;
}

ErrorCategory category_of(ErrorCode code) noexcept {
  for (const auto& descriptor : kErrors) {
    if (descriptor.code == code) {
      return descriptor.category;
    }
  }
  return ErrorCategory::None;
}

RegistryError::RegistryError(ErrorCode code, std::string detail) : code_(code), detail_(std::move(detail)) {}

std::string RegistryError::to_string() const {
  std::string result;
  result.reserve(detail_.size() + 32);
  result.append(to_token(code_));
  if (!detail_.empty()) {
    result.append(": ");
    result.append(detail_);
  }
  return result;
}

std::string RegistryError::explain() const {
  std::string result;
  result.reserve(detail_.size() + 96);
  result.append(describe(code_));
  result.append(" [");
  result.append(to_token(code_));
  result.append("]");
  if (!detail_.empty()) {
    result.append(": ");
    result.append(detail_);
  }
  return result;
}

}  // namespace facility_dependency_registry
