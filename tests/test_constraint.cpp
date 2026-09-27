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

FDEP_TEST(constraint, kinds_have_stable_shapes) {
  const ConstraintKind kinds[] = {ConstraintKind::MaxLatencyMicros, ConstraintKind::MinBandwidthMbps,
                                  ConstraintKind::RedundancyClass, ConstraintKind::RedundancyCount,
                                  ConstraintKind::FailoverMode};
  for (const auto kind : kinds) {
    const std::string token{to_token(kind)};
    FDEP_CHECK(!token.empty());
    FDEP_CHECK(!describe(kind).empty());
    const auto parsed = parse_constraint_kind(token);
    FDEP_REQUIRE(parsed.has_value());
    FDEP_CHECK(*parsed == kind);
  }
  FDEP_CHECK(!parse_constraint_kind("").has_value());
  FDEP_CHECK(!parse_constraint_kind("custom").has_value());
  FDEP_CHECK_EQ(to_token(static_cast<ConstraintKind>(0)), std::string{"unknown-constraint-kind"});
  FDEP_CHECK(value_type_of(ConstraintKind::MaxLatencyMicros) == ConstraintValueType::Integer);
  FDEP_CHECK(value_type_of(ConstraintKind::RedundancyClass) == ConstraintValueType::Token);
  FDEP_CHECK(value_type_of(static_cast<ConstraintKind>(0)) == ConstraintValueType::Unspecified);
}

FDEP_TEST(constraint, integer_ranges_are_enforced) {
  FDEP_REQUIRE_OK(DependencyConstraint::make(ConstraintKind::MaxLatencyMicros, 0));
  FDEP_REQUIRE_OK(DependencyConstraint::make(ConstraintKind::MaxLatencyMicros, 3'600'000'000));
  FDEP_CHECK_CODE(DependencyConstraint::make(ConstraintKind::MaxLatencyMicros, -1),
                  ErrorCode::InvalidConstraintValue);
  FDEP_CHECK_CODE(DependencyConstraint::make(ConstraintKind::MaxLatencyMicros, 3'600'000'001),
                  ErrorCode::InvalidConstraintValue);

  FDEP_REQUIRE_OK(DependencyConstraint::make(ConstraintKind::MinBandwidthMbps, 1));
  FDEP_CHECK_CODE(DependencyConstraint::make(ConstraintKind::MinBandwidthMbps, 0),
                  ErrorCode::InvalidConstraintValue);

  FDEP_REQUIRE_OK(DependencyConstraint::make(ConstraintKind::RedundancyCount, 0));
  FDEP_REQUIRE_OK(DependencyConstraint::make(ConstraintKind::RedundancyCount, 64));
  FDEP_CHECK_CODE(DependencyConstraint::make(ConstraintKind::RedundancyCount, 65),
                  ErrorCode::InvalidConstraintValue);

  // A token kind refuses an integer and the other way round.
  FDEP_CHECK_CODE(DependencyConstraint::make(ConstraintKind::RedundancyClass, 2),
                  ErrorCode::InvalidConstraintValue);
  FDEP_CHECK_CODE(DependencyConstraint::make(ConstraintKind::MaxLatencyMicros, "fast"),
                  ErrorCode::InvalidConstraintValue);
  FDEP_CHECK_CODE(DependencyConstraint::make(static_cast<ConstraintKind>(0), 1),
                  ErrorCode::InvalidConstraintKind);
}

FDEP_TEST(constraint, token_domains_are_closed) {
  for (const char* token : {"N", "N+1", "2N", "2N+1"}) {
    FDEP_REQUIRE_OK(DependencyConstraint::make(ConstraintKind::RedundancyClass, token));
  }
  FDEP_CHECK_CODE(DependencyConstraint::make(ConstraintKind::RedundancyClass, "n"),
                  ErrorCode::InvalidConstraintValue);
  FDEP_CHECK_CODE(DependencyConstraint::make(ConstraintKind::RedundancyClass, "N+2"),
                  ErrorCode::InvalidConstraintValue);
  FDEP_CHECK_CODE(DependencyConstraint::make(ConstraintKind::RedundancyClass, ""),
                  ErrorCode::InvalidConstraintValue);
  FDEP_CHECK_CODE(DependencyConstraint::make(ConstraintKind::RedundancyClass, "NNNNNNNNN"),
                  ErrorCode::InvalidConstraintValue);

  for (const char* token : {"automatic", "manual", "none"}) {
    FDEP_REQUIRE_OK(DependencyConstraint::make(ConstraintKind::FailoverMode, token));
  }
  FDEP_CHECK_CODE(DependencyConstraint::make(ConstraintKind::FailoverMode, "Automatic"),
                  ErrorCode::InvalidConstraintValue);
  FDEP_CHECK_CODE(DependencyConstraint::make(ConstraintKind::FailoverMode, "auto"),
                  ErrorCode::InvalidConstraintValue);
}

FDEP_TEST(constraint, values_render_and_compare_deterministically) {
  auto bandwidth = DependencyConstraint::make(ConstraintKind::MinBandwidthMbps, 40000);
  FDEP_REQUIRE_OK(bandwidth);
  FDEP_CHECK_EQ(bandwidth.value().to_text(), std::string{"min-bandwidth-mbps=40000"});
  FDEP_CHECK(bandwidth.value().value().is_integer());
  FDEP_CHECK_EQ(bandwidth.value().value().integer(), std::int64_t{40000});

  auto redundancy = DependencyConstraint::make(ConstraintKind::RedundancyClass, "N+1");
  FDEP_REQUIRE_OK(redundancy);
  FDEP_CHECK_EQ(redundancy.value().to_text(), std::string{"redundancy-class=N+1"});
  FDEP_CHECK(redundancy.value().value().is_token());
  FDEP_CHECK_EQ(redundancy.value().value().token(), std::string{"N+1"});

  // Ordering is by kind first.
  FDEP_CHECK(bandwidth.value() < redundancy.value());
  const auto equal = DependencyConstraint::make(ConstraintKind::MinBandwidthMbps, 40000);
  FDEP_REQUIRE_OK(equal);
  FDEP_CHECK(bandwidth.value() == equal.value());
  FDEP_CHECK(!(bandwidth.value() < equal.value()));

  const DependencyConstraint empty;
  FDEP_CHECK(empty != bandwidth.value());
  FDEP_CHECK_EQ(static_cast<unsigned>(empty.kind()), 0u);
}

FDEP_TEST(constraint, value_accessors_are_typed) {
  const ConstraintValue integer_value = ConstraintValue::integer(-5);
  FDEP_CHECK(integer_value.is_integer());
  FDEP_CHECK(!integer_value.is_token());
  FDEP_CHECK(integer_value.type() == ConstraintValueType::Integer);
  FDEP_CHECK_EQ(integer_value.integer(), std::int64_t{-5});
  FDEP_CHECK_EQ(integer_value.to_text(), std::string{"-5"});

  const ConstraintValue token_value = ConstraintValue::token("manual");
  FDEP_CHECK(token_value.is_token());
  FDEP_CHECK_EQ(token_value.token(), std::string{"manual"});
  FDEP_CHECK_EQ(token_value.to_text(), std::string{"manual"});
  FDEP_CHECK(integer_value != token_value);
  FDEP_CHECK(integer_value < token_value);

  FDEP_CHECK(!validate_constraint_token(ConstraintKind::RedundancyClass, "bogus").ok());
  FDEP_CHECK(validate_constraint_token(ConstraintKind::FailoverMode, "none").ok());
  FDEP_CHECK(!validate_constraint_token(ConstraintKind::RedundancyCount, "none").ok());
}
