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

#ifndef FACILITY_DEPENDENCY_REGISTRY_ERRORS_HPP
#define FACILITY_DEPENDENCY_REGISTRY_ERRORS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace facility_dependency_registry {

/// Stable, machine readable rejection and failure categories.
///
/// The numeric values of this enumeration are part of the public contract:
/// they never change meaning, and consumers may persist or compare them. New
/// categories are only ever added with a previously unused value.
enum class ErrorCode : std::uint16_t {
  /// No error. Only ever produced by Status::success().
  Ok = 0,

  // -- 1xx: input syntax and construction -------------------------------
  InvalidNodeDomain = 101,
  InvalidNodeIdSyntax = 102,
  InvalidNodeReference = 103,
  InvalidEdgeId = 104,
  InvalidEdgeRevision = 105,
  InvalidGeneration = 106,
  InvalidKindToken = 107,
  InvalidStrengthToken = 108,
  InvalidDirectionToken = 109,
  InvalidLifecycleToken = 110,
  InvalidProvenanceSource = 111,
  InvalidProvenanceIdentifier = 112,
  InvalidAnnotation = 113,
  InvalidConstraintKind = 114,
  InvalidConstraintValue = 115,
  InvalidTimestamp = 116,
  InvalidLimits = 117,
  InvalidArguments = 118,
  InvalidStorePath = 119,

  // -- 2xx: dependency semantics ----------------------------------------
  DuplicateEdge = 201,
  EdgeNotFound = 202,
  SelfDependencyProhibited = 203,
  EndpointDomainNotAllowed = 204,
  DirectionNotAllowedForKind = 205,
  ConstraintNotAllowedForKind = 206,
  DuplicateConstraintKind = 207,
  TooManyConstraints = 208,
  ProhibitedCycle = 209,
  InvalidLifecycleTransition = 210,
  EdgeRetired = 211,
  DeclaredRefReferenced = 213,
  DuplicateDeclaration = 214,

  // -- 3xx: authority and generation ------------------------------------
  StaleGeneration = 301,
  StaleEdgeRevision = 302,
  RegistryClosed = 303,
  WriterFenced = 304,
  NotDurable = 305,
  GenerationExhausted = 306,

  // -- 4xx: bounds, resources and cancellation --------------------------
  EdgeCapacityExceeded = 401,
  DeclaredRefCapacityExceeded = 402,
  RequestLimitExceeded = 403,
  QueryResultLimitExceeded = 404,
  PayloadTooLarge = 405,
  AnalysisLimitExceeded = 406,
  Cancelled = 407,

  // -- 5xx: durable state ------------------------------------------------
  StoreNotFound = 501,
  StoreLocked = 502,
  StoreIoError = 503,
  StoreCorrupt = 504,
  StoreTruncated = 505,
  StoreIntegrityFailure = 506,
  StoreVersionUnsupported = 507,
  StoreReadOnly = 508,
  StoreLayoutInvalid = 509,
  StorePublishFailed = 510,
};

/// Broad class of an ErrorCode. Stable.
enum class ErrorCategory : std::uint8_t {
  None = 0,
  Input = 1,
  Semantics = 2,
  Authority = 3,
  Bounds = 4,
  Persistence = 5,
};

/// The stable lower-case token of a code, for example "duplicate-edge".
/// Tokens never change and are unique across the enumeration.
[[nodiscard]] std::string_view to_token(ErrorCode code) noexcept;

/// A single clause describing the category, suitable for messages.
[[nodiscard]] std::string_view describe(ErrorCode code) noexcept;

[[nodiscard]] ErrorCategory category_of(ErrorCode code) noexcept;

/// A rejection or failure: a stable category plus a specific, human readable
/// detail. The detail never carries authority; only the code does.
class RegistryError {
 public:
  RegistryError() = default;
  RegistryError(ErrorCode code, std::string detail);

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] ErrorCategory category() const noexcept { return category_of(code_); }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

  /// "token: detail" -- the stable token first, the free-form detail second.
  [[nodiscard]] std::string to_string() const;

  /// A sentence for a human: "<describe> (<token>): <detail>".
  [[nodiscard]] std::string explain() const;

  friend bool operator==(const RegistryError& lhs, const RegistryError& rhs) noexcept {
    return lhs.code_ == rhs.code_ && lhs.detail_ == rhs.detail_;
  }

 private:
  ErrorCode code_{ErrorCode::Ok};
  std::string detail_{};
};

/// The outcome of an operation that produces no value.
class [[nodiscard]] Status {
 public:
  Status() noexcept = default;

  [[nodiscard]] static Status success() noexcept { return Status{}; }
  [[nodiscard]] static Status failure(ErrorCode code, std::string detail) {
    return Status{RegistryError{code, std::move(detail)}};
  }
  [[nodiscard]] static Status failure(RegistryError error) { return Status{std::move(error)}; }

  [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const RegistryError& error() const noexcept {
    static const RegistryError none{};
    return error_.has_value() ? *error_ : none;
  }

  [[nodiscard]] ErrorCode code() const noexcept { return error_.has_value() ? error_->code() : ErrorCode::Ok; }

  [[nodiscard]] std::string to_string() const { return ok() ? std::string{"ok"} : error_->to_string(); }
  [[nodiscard]] std::string explain() const { return ok() ? std::string{"ok"} : error_->explain(); }

 private:
  explicit Status(RegistryError error) : error_(std::move(error)) {}
  std::optional<RegistryError> error_{};
};

/// The outcome of an operation that produces a value.
///
/// `value()` and `error()` are precondition-checked: `value()` must only be
/// called on a successful result and `error()` only on a failed one. Both
/// remain defined behaviour when misused (value() throws std::bad_variant_access,
/// error() returns a stable placeholder error) so that a mistaken caller is
/// diagnosed rather than corrupted.
template <class T>
class [[nodiscard]] Result {
 public:
  using value_type = T;
  using error_type = RegistryError;

  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
  Result(RegistryError error) : storage_(std::in_place_index<1>, std::move(error)) {}

  [[nodiscard]] static Result success(T value) { return Result{std::move(value)}; }
  [[nodiscard]] static Result failure(ErrorCode code, std::string detail) {
    return Result{RegistryError{code, std::move(detail)}};
  }
  [[nodiscard]] static Result failure(RegistryError error) { return Result{std::move(error)}; }

  [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T& value() & { return std::get<0>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
  [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

  [[nodiscard]] T* operator->() { return &std::get<0>(storage_); }
  [[nodiscard]] const T* operator->() const { return &std::get<0>(storage_); }
  [[nodiscard]] T& operator*() & { return std::get<0>(storage_); }
  [[nodiscard]] const T& operator*() const& { return std::get<0>(storage_); }

  [[nodiscard]] const RegistryError& error() const noexcept {
    static const RegistryError none{};
    if (storage_.index() == 1) {
      return std::get<1>(storage_);
    }
    return none;
  }

  [[nodiscard]] ErrorCode code() const noexcept { return error().code(); }

  [[nodiscard]] Status status() const {
    return has_value() ? Status::success() : Status::failure(error());
  }

 private:
  std::variant<T, RegistryError> storage_;
};

}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_ERRORS_HPP
