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

#include "facility_dependency_registry/constraint.hpp"

#include <array>
#include <string>

namespace facility_dependency_registry {
namespace {

struct ConstraintDescriptor {
  ConstraintKind kind;
  std::string_view token;
  std::string_view description;
  ConstraintValueType value_type;
  std::int64_t integer_min;
  std::int64_t integer_max;
  std::size_t token_min;
  std::size_t token_max;
  std::string_view token_domain;  // "a|b|c" for token kinds, empty otherwise
};
// The token domain is stored as a pipe separated list so that it is declared in
// one place and checked by the same code that reports it.

constexpr std::array<ConstraintDescriptor, kConstraintKindCount> kConstraintTable{{
    {ConstraintKind::MaxLatencyMicros, "max-latency-micros",
     "maximum acceptable added latency across the dependency, in microseconds", ConstraintValueType::Integer, 0,
     3'600'000'000, 0, 0, {}},
    {ConstraintKind::MinBandwidthMbps, "min-bandwidth-mbps",
     "minimum bandwidth the dependency must carry, in megabits per second", ConstraintValueType::Integer, 1,
     1'000'000, 0, 0, {}},
    {ConstraintKind::RedundancyClass, "redundancy-class",
     "declared redundancy class of the dependency", ConstraintValueType::Token, 0, 0, 1, 8,
     "N|N+1|2N|2N+1"},
    {ConstraintKind::RedundancyCount, "redundancy-count",
     "number of independent paths the dependency is declared to have", ConstraintValueType::Integer, 0, 64, 0, 0,
     {}},
    {ConstraintKind::FailoverMode, "failover-mode", "declared failover behaviour of the dependency",
     ConstraintValueType::Token, 0, 0, 4, 9, "automatic|manual|none"},
}};

const ConstraintDescriptor* descriptor_of(ConstraintKind kind) noexcept {
  const auto ordinal = static_cast<unsigned>(kind);
  if (ordinal < 1 || ordinal > kConstraintKindCount) {
    return nullptr;
  }
  return &kConstraintTable[ordinal - 1];
}

bool token_in_domain(std::string_view domain, std::string_view token) noexcept {
  std::size_t start = 0;
  while (start <= domain.size()) {
    const std::size_t separator = domain.find('|', start);
    const std::string_view candidate =
        separator == std::string_view::npos ? domain.substr(start) : domain.substr(start, separator - start);
    if (candidate == token) {
      return true;
    }
    if (separator == std::string_view::npos) {
      return false;
    }
    start = separator + 1;
  }
  return false;
}

}  // namespace

std::string_view to_token(ConstraintKind kind) noexcept {
  const auto* descriptor = descriptor_of(kind);
  return descriptor == nullptr ? std::string_view{"unknown-constraint-kind"} : descriptor->token;
}

std::string_view describe(ConstraintKind kind) noexcept {
  const auto* descriptor = descriptor_of(kind);
  return descriptor == nullptr ? std::string_view{"unrecognised constraint kind"} : descriptor->description;
}

std::optional<ConstraintKind> parse_constraint_kind(std::string_view token) noexcept {
  for (const auto& entry : kConstraintTable) {
    if (entry.token == token) {
      return entry.kind;
    }
  }
  return std::nullopt;
}

ConstraintValueType value_type_of(ConstraintKind kind) noexcept {
  const auto* descriptor = descriptor_of(kind);
  return descriptor == nullptr ? ConstraintValueType::Unspecified : descriptor->value_type;
}

std::pair<std::int64_t, std::int64_t> integer_range_of(ConstraintKind kind) noexcept {
  const auto* descriptor = descriptor_of(kind);
  if (descriptor == nullptr) {
    return {0, -1};  // empty range
  }
  return {descriptor->integer_min, descriptor->integer_max};
}

std::pair<std::size_t, std::size_t> token_length_range_of(ConstraintKind kind) noexcept {
  const auto* descriptor = descriptor_of(kind);
  if (descriptor == nullptr) {
    return {1, 0};  // empty range
  }
  return {descriptor->token_min, descriptor->token_max};
}

ConstraintValue ConstraintValue::integer(std::int64_t value) noexcept {
  ConstraintValue result;
  result.storage_ = value;
  return result;
}

ConstraintValue ConstraintValue::token(std::string value) {
  ConstraintValue result;
  result.storage_ = std::move(value);
  return result;
}

ConstraintValueType ConstraintValue::type() const noexcept {
  return storage_.index() == 0 ? ConstraintValueType::Integer : ConstraintValueType::Token;
}

bool ConstraintValue::is_integer() const noexcept { return storage_.index() == 0; }

bool ConstraintValue::is_token() const noexcept { return storage_.index() == 1; }

std::int64_t ConstraintValue::integer() const { return std::get<std::int64_t>(storage_); }

const std::string& ConstraintValue::token() const { return std::get<std::string>(storage_); }

std::string ConstraintValue::to_text() const {
  if (is_integer()) {
    return std::to_string(integer());
  }
  return token();
}

std::strong_ordering operator<=>(const ConstraintValue& lhs, const ConstraintValue& rhs) noexcept {
  if (lhs.storage_.index() != rhs.storage_.index()) {
    return lhs.storage_.index() <=> rhs.storage_.index();
  }
  if (lhs.is_integer()) {
    return std::get<std::int64_t>(lhs.storage_) <=> std::get<std::int64_t>(rhs.storage_);
  }
  return std::get<std::string>(lhs.storage_).compare(std::get<std::string>(rhs.storage_)) <=> 0;
}

Status validate_constraint_token(ConstraintKind kind, std::string_view token) {
  const auto* descriptor = descriptor_of(kind);
  if (descriptor == nullptr) {
    return Status::failure(ErrorCode::InvalidConstraintKind, "unrecognised constraint kind");
  }
  if (descriptor->value_type != ConstraintValueType::Token) {
    std::string detail{"constraint "};
    detail.append(descriptor->token);
    detail.append(" takes an integer value");
    return Status::failure(ErrorCode::InvalidConstraintValue, std::move(detail));
  }
  if (token.size() < descriptor->token_min || token.size() > descriptor->token_max) {
    std::string detail{"constraint "};
    detail.append(descriptor->token);
    detail.append(" value length is outside the declared range");
    return Status::failure(ErrorCode::InvalidConstraintValue, std::move(detail));
  }
  if (!token_in_domain(descriptor->token_domain, token)) {
    std::string detail{"constraint "};
    detail.append(descriptor->token);
    detail.append(" value is not one of ");
    detail.append(descriptor->token_domain);
    return Status::failure(ErrorCode::InvalidConstraintValue, std::move(detail));
  }
  return Status::success();
}

Result<DependencyConstraint> DependencyConstraint::make(ConstraintKind kind, std::int64_t value) {
  const auto* descriptor = descriptor_of(kind);
  if (descriptor == nullptr) {
    return Result<DependencyConstraint>::failure(ErrorCode::InvalidConstraintKind,
                                                "unrecognised constraint kind");
  }
  if (descriptor->value_type != ConstraintValueType::Integer) {
    std::string detail{"constraint "};
    detail.append(descriptor->token);
    detail.append(" takes a token value from ");
    detail.append(descriptor->token_domain);
    return Result<DependencyConstraint>::failure(ErrorCode::InvalidConstraintValue, std::move(detail));
  }
  if (value < descriptor->integer_min || value > descriptor->integer_max) {
    std::string detail{"constraint "};
    detail.append(descriptor->token);
    detail.append(" value ");
    detail.append(std::to_string(value));
    detail.append(" is outside [");
    detail.append(std::to_string(descriptor->integer_min));
    detail.append(", ");
    detail.append(std::to_string(descriptor->integer_max));
    detail.append("]");
    return Result<DependencyConstraint>::failure(ErrorCode::InvalidConstraintValue, std::move(detail));
  }
  DependencyConstraint result;
  result.kind_ = kind;
  result.value_ = ConstraintValue::integer(value);
  return result;
}

Result<DependencyConstraint> DependencyConstraint::make(ConstraintKind kind, std::string_view token) {
  if (const auto status = validate_constraint_token(kind, token); !status.ok()) {
    return Result<DependencyConstraint>::failure(status.error());
  }
  DependencyConstraint result;
  result.kind_ = kind;
  result.value_ = ConstraintValue::token(std::string{token});
  return result;
}

std::string DependencyConstraint::to_text() const {
  std::string result;
  result.append(to_token(kind_));
  result.push_back('=');
  result.append(value_.to_text());
  return result;
}

std::strong_ordering operator<=>(const DependencyConstraint& lhs, const DependencyConstraint& rhs) noexcept {
  if (const auto by_kind = static_cast<unsigned>(lhs.kind_) <=> static_cast<unsigned>(rhs.kind_); by_kind != 0) {
    return by_kind;
  }
  return lhs.value_ <=> rhs.value_;
}

}  // namespace facility_dependency_registry
