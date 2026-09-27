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

#ifndef FACILITY_DEPENDENCY_REGISTRY_CONSTRAINT_HPP
#define FACILITY_DEPENDENCY_REGISTRY_CONSTRAINT_HPP

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "facility_dependency_registry/errors.hpp"
#include "facility_dependency_registry/mask.hpp"

namespace facility_dependency_registry {

/// The bounded schema of declared requirement metadata.
///
/// Every kind below is a *declaration* attached to an edge: the operator, the
/// import or the composing domain states it. This repository validates,
/// stores, reports and diffs these declarations. It never evaluates them,
/// never reserves against them and never derives a capacity or recovery
/// decision from them. Values are contiguous and start at 1.
enum class ConstraintKind : std::uint8_t {
  /// Maximum acceptable added latency across the dependency, in microseconds.
  MaxLatencyMicros = 1,
  /// Minimum bandwidth the dependency must be able to carry, in megabits per
  /// second, in decimal units.
  MinBandwidthMbps = 2,
  /// Declared redundancy class of the dependency.
  RedundancyClass = 3,
  /// Number of independent paths the dependency is declared to have.
  RedundancyCount = 4,
  /// Declared failover behaviour of the dependency.
  FailoverMode = 5,
};

inline constexpr std::size_t kConstraintKindCount = 5;

using ConstraintKindMask = EnumMask<ConstraintKind, kConstraintKindCount>;

/// The value shape of a constraint kind.
enum class ConstraintValueType : std::uint8_t {
  Unspecified = 0,
  Integer = 1,
  Token = 2,
};

[[nodiscard]] std::string_view to_token(ConstraintKind kind) noexcept;
[[nodiscard]] std::string_view describe(ConstraintKind kind) noexcept;
[[nodiscard]] std::optional<ConstraintKind> parse_constraint_kind(std::string_view token) noexcept;
[[nodiscard]] ConstraintValueType value_type_of(ConstraintKind kind) noexcept;

/// Inclusive integer range accepted for an integer valued constraint kind.
[[nodiscard]] std::pair<std::int64_t, std::int64_t> integer_range_of(ConstraintKind kind) noexcept;

/// Inclusive token length range accepted for a token valued constraint kind.
[[nodiscard]] std::pair<std::size_t, std::size_t> token_length_range_of(ConstraintKind kind) noexcept;

/// A validated constraint value: an integer or a bounded canonical token.
class ConstraintValue {
 public:
  ConstraintValue() = default;

  [[nodiscard]] static ConstraintValue integer(std::int64_t value) noexcept;
  [[nodiscard]] static ConstraintValue token(std::string value);

  [[nodiscard]] ConstraintValueType type() const noexcept;
  [[nodiscard]] bool is_integer() const noexcept;
  [[nodiscard]] bool is_token() const noexcept;

  /// Precondition: `is_integer()`. Throws std::bad_variant_access otherwise.
  [[nodiscard]] std::int64_t integer() const;
  /// Precondition: `is_token()`. Throws std::bad_variant_access otherwise.
  [[nodiscard]] const std::string& token() const;

  [[nodiscard]] std::string to_text() const;

  friend bool operator==(const ConstraintValue& lhs, const ConstraintValue& rhs) noexcept {
    return lhs.storage_ == rhs.storage_;
  }
  friend bool operator!=(const ConstraintValue& lhs, const ConstraintValue& rhs) noexcept { return !(lhs == rhs); }
  friend std::strong_ordering operator<=>(const ConstraintValue& lhs, const ConstraintValue& rhs) noexcept;

 private:
  std::variant<std::int64_t, std::string> storage_{std::int64_t{0}};
};

/// One declared requirement attached to one edge.
class DependencyConstraint {
 public:
  DependencyConstraint() = default;

  /// Builds an integer valued constraint and validates it against the range of
  /// `kind`. Fails with InvalidConstraintValue when `kind` is token valued.
  [[nodiscard]] static Result<DependencyConstraint> make(ConstraintKind kind, std::int64_t value);

  /// Builds a token valued constraint and validates it against the token
  /// domain of `kind`. Fails with InvalidConstraintValue when `kind` is integer
  /// valued or the token is outside the declared domain.
  [[nodiscard]] static Result<DependencyConstraint> make(ConstraintKind kind, std::string_view token);

  [[nodiscard]] ConstraintKind kind() const noexcept { return kind_; }
  [[nodiscard]] const ConstraintValue& value() const noexcept { return value_; }

  [[nodiscard]] std::string to_text() const;

  friend bool operator==(const DependencyConstraint& lhs, const DependencyConstraint& rhs) noexcept {
    return lhs.kind_ == rhs.kind_ && lhs.value_ == rhs.value_;
  }
  friend bool operator!=(const DependencyConstraint& lhs, const DependencyConstraint& rhs) noexcept {
    return !(lhs == rhs);
  }

  /// Canonical order: by constraint kind, then by value.
  friend std::strong_ordering operator<=>(const DependencyConstraint& lhs, const DependencyConstraint& rhs) noexcept;

 private:
  ConstraintKind kind_{};
  ConstraintValue value_{};
};

/// Validates a token for a token valued kind, including its declared domain.
[[nodiscard]] Status validate_constraint_token(ConstraintKind kind, std::string_view token);

}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_CONSTRAINT_HPP
