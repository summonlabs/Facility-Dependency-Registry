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

#include <algorithm>
#include <string>
#include <vector>

#include "facility_dependency_registry/facility_dependency_registry.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

using namespace facility_dependency_registry;
using fdep_test::asset;
using fdep_test::node;
using fdep_test::rack;

FDEP_TEST(node_ref, domain_tokens_round_trip) {
  const NodeDomain domains[] = {NodeDomain::Asset,          NodeDomain::Rack,
                                NodeDomain::ElectricalDomain, NodeDomain::CoolingDomain,
                                NodeDomain::FacilityService,  NodeDomain::AsiDomain,
                                NodeDomain::DfiDomain};
  for (const auto domain : domains) {
    const std::string token{to_token(domain)};
    FDEP_CHECK(!token.empty());
    FDEP_CHECK(!describe(domain).empty());
    const auto parsed = parse_node_domain(token);
    FDEP_REQUIRE(parsed.has_value());
    FDEP_CHECK(*parsed == domain);
  }
  FDEP_CHECK(!parse_node_domain("").has_value());
  FDEP_CHECK(!parse_node_domain("Asset").has_value());  // tokens are lower case
  FDEP_CHECK(!parse_node_domain("unknown").has_value());
  FDEP_CHECK_EQ(to_token(static_cast<NodeDomain>(0)), std::string{"unknown-node-domain"});
  FDEP_CHECK_EQ(to_token(static_cast<NodeDomain>(99)), std::string{"unknown-node-domain"});
}

FDEP_TEST(node_ref, create_and_canonical_text) {
  const auto ref = DependencyNodeRef::create(NodeDomain::Asset, "row-a-rack-07-node-3");
  FDEP_REQUIRE_OK(ref);
  FDEP_CHECK_EQ(ref.value().to_canonical(), std::string{"asset:row-a-rack-07-node-3"});
  FDEP_CHECK_EQ(ref.value().id(), std::string{"row-a-rack-07-node-3"});
  FDEP_CHECK(ref.value().domain() == NodeDomain::Asset);
  FDEP_CHECK(ref.value().valid());

  const auto parsed = DependencyNodeRef::parse("asset:row-a-rack-07-node-3");
  FDEP_REQUIRE_OK(parsed);
  FDEP_CHECK(parsed.value() == ref.value());
}

FDEP_TEST(node_ref, defaults_are_invalid_and_never_compare_equal_to_a_real_ref) {
  const DependencyNodeRef empty;
  FDEP_CHECK(!empty.valid());
  FDEP_CHECK(empty.id().empty());
  FDEP_CHECK(empty != asset("a"));
  const auto created = DependencyNodeRef::create(NodeDomain::Asset, "");
  FDEP_CHECK_CODE(created, ErrorCode::InvalidNodeIdSyntax);
}

FDEP_TEST(node_ref, identifier_syntax_is_narrow) {
  const auto accepts = [](std::string_view id) {
    return DependencyNodeRef::create(NodeDomain::Asset, id).has_value();
  };
  FDEP_CHECK(accepts("a"));
  FDEP_CHECK(accepts("A"));
  FDEP_CHECK(accepts("0"));
  FDEP_CHECK(accepts("a-b"));
  FDEP_CHECK(accepts("a.b"));
  FDEP_CHECK(accepts("a_b"));
  FDEP_CHECK(accepts("a__b"));
  FDEP_CHECK(accepts("rack-07.node_3"));
  FDEP_CHECK(accepts("A1-b2.C3_d4"));

  FDEP_CHECK(!accepts(""));
  FDEP_CHECK(!accepts("-a"));
  FDEP_CHECK(!accepts("a-"));
  FDEP_CHECK(!accepts(".a"));
  FDEP_CHECK(!accepts("a."));
  FDEP_CHECK(!accepts("_a"));
  FDEP_CHECK(!accepts("a_"));
  FDEP_CHECK(!accepts("a..b"));
  FDEP_CHECK(!accepts("a--b"));
  FDEP_CHECK(!accepts("a b"));
  FDEP_CHECK(!accepts(" a"));
  FDEP_CHECK(!accepts("a "));
  FDEP_CHECK(!accepts("a/b"));
  FDEP_CHECK(!accepts("a\\b"));
  FDEP_CHECK(!accepts("a:b"));
  FDEP_CHECK(!accepts("a;b"));
  FDEP_CHECK(!accepts("a*b"));
  FDEP_CHECK(!accepts("../etc/passwd"));
  FDEP_CHECK(!accepts(".."));
  FDEP_CHECK(!accepts("caf\u00e9"));  // non ASCII is rejected, not folded

  const std::string embedded_nul{"a\0b", 3};
  FDEP_CHECK(!accepts(embedded_nul));
  FDEP_CHECK(!accepts(std::string(161, 'a')));
  FDEP_CHECK(accepts(std::string(160, 'a')));
}

FDEP_TEST(node_ref, create_reports_the_specific_rejection) {
  const auto bad_domain = DependencyNodeRef::create(static_cast<NodeDomain>(0), "a");
  FDEP_CHECK_CODE(bad_domain, ErrorCode::InvalidNodeDomain);
  const auto bad_id = DependencyNodeRef::create(NodeDomain::Rack, "a..b");
  FDEP_CHECK_CODE(bad_id, ErrorCode::InvalidNodeIdSyntax);
  const auto too_long = DependencyNodeRef::create(NodeDomain::Rack, std::string(200, 'a'));
  FDEP_CHECK_CODE(too_long, ErrorCode::InvalidNodeIdSyntax);
  const auto custom_limit = DependencyNodeRef::create(NodeDomain::Rack, "abcdef", 3);
  FDEP_CHECK_CODE(custom_limit, ErrorCode::InvalidNodeIdSyntax);
}

FDEP_TEST(node_ref, parse_rejects_malformed_canonical_forms) {
  FDEP_CHECK_CODE(DependencyNodeRef::parse("asset"), ErrorCode::InvalidNodeReference);
  FDEP_CHECK_CODE(DependencyNodeRef::parse(""), ErrorCode::InvalidNodeReference);
  FDEP_CHECK_CODE(DependencyNodeRef::parse(":a"), ErrorCode::InvalidNodeDomain);
  FDEP_CHECK_CODE(DependencyNodeRef::parse("unknown:a"), ErrorCode::InvalidNodeDomain);
  FDEP_CHECK_CODE(DependencyNodeRef::parse("asset:"), ErrorCode::InvalidNodeIdSyntax);
  FDEP_CHECK_CODE(DependencyNodeRef::parse("asset:a:b"), ErrorCode::InvalidNodeIdSyntax);
  FDEP_CHECK_CODE(DependencyNodeRef::parse("asset:../x"), ErrorCode::InvalidNodeIdSyntax);

  // The first colon separates, so a domain token with a trailing colon is
  // simply an unknown domain rather than something that gets trimmed.
  FDEP_CHECK_CODE(DependencyNodeRef::parse(" asset:a"), ErrorCode::InvalidNodeDomain);
}

FDEP_TEST(node_ref, canonical_order_is_domain_then_identifier) {
  std::vector<DependencyNodeRef> refs{rack("a"), asset("b"), asset("a"), node(NodeDomain::DfiDomain, "a")};
  std::sort(refs.begin(), refs.end());
  FDEP_CHECK(refs[0] == asset("a"));
  FDEP_CHECK(refs[1] == asset("b"));
  FDEP_CHECK(refs[2] == rack("a"));
  FDEP_CHECK(refs[3] == node(NodeDomain::DfiDomain, "a"));

  // Identifier bytes decide within one domain, and 'Z' sorts before 'a'.
  FDEP_CHECK(asset("Z") < asset("a"));
  FDEP_CHECK(asset("a-1") < asset("a_1"));
  FDEP_CHECK(asset("a") < asset("a-1"));
}

FDEP_TEST(node_ref, node_ref_set_deduplicates_sorts_and_bounds) {
  auto set = NodeRefSet::create({rack("b"), asset("a"), rack("b"), asset("a")}, 8);
  FDEP_REQUIRE_OK(set);
  FDEP_CHECK_EQ(set.value().size(), std::size_t{2});
  FDEP_CHECK(set.value().contains(asset("a")));
  FDEP_CHECK(set.value().contains(rack("b")));
  FDEP_CHECK(!set.value().contains(asset("b")));
  FDEP_CHECK(set.value().refs()[0] == asset("a"));
  FDEP_CHECK(set.value().refs()[1] == rack("b"));

  auto too_many = NodeRefSet::create({asset("a"), asset("b")}, 1);
  FDEP_CHECK_CODE(too_many, ErrorCode::RequestLimitExceeded);

  auto invalid = NodeRefSet::create({DependencyNodeRef{}}, 8);
  FDEP_CHECK_CODE(invalid, ErrorCode::InvalidNodeReference);

  const auto empty = NodeRefSet::create({}, 8);
  FDEP_REQUIRE_OK(empty);
  FDEP_CHECK(empty.value().empty());
  FDEP_CHECK_EQ(to_text(empty.value()), std::string{});
}
