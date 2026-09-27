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

#include <type_traits>
#include <unordered_map>

#include "facility_dependency_registry/facility_dependency_registry.hpp"
#include "test_harness.hpp"

using namespace facility_dependency_registry;

// The three identities are distinct types, and none of them converts to
// another or to its representation by accident.
static_assert(!std::is_convertible_v<DependencyEdgeId, EdgeRevision>);
static_assert(!std::is_convertible_v<EdgeRevision, DependencyEdgeId>);
static_assert(!std::is_convertible_v<DependencyEdgeId, DependencyGeneration>);
static_assert(!std::is_convertible_v<DependencyGeneration, DependencyEdgeId>);
static_assert(!std::is_convertible_v<EdgeRevision, DependencyGeneration>);
static_assert(!std::is_convertible_v<std::uint64_t, DependencyEdgeId>);
static_assert(!std::is_convertible_v<DependencyEdgeId, std::uint64_t>);
static_assert(std::is_trivially_copyable_v<DependencyEdgeId>);
static_assert(sizeof(DependencyEdgeId) == sizeof(std::uint64_t));

FDEP_TEST(ids, defaults_and_specials) {
  const DependencyEdgeId unassigned{};
  FDEP_CHECK(unassigned.is_zero());
  FDEP_CHECK(!is_assigned(unassigned));
  FDEP_CHECK(!is_assigned(DependencyEdgeId::from_value(0)));

  const DependencyEdgeId first = DependencyEdgeId::from_value(kFirstEdgeOrdinal);
  FDEP_CHECK(is_assigned(first));
  FDEP_CHECK_EQ(first.value(), std::uint64_t{1});

  const EdgeRevision revision{};
  FDEP_CHECK(!is_assigned(revision));
  FDEP_CHECK(is_assigned(kInitialRevision));
  FDEP_CHECK_EQ(kInitialRevision.value(), std::uint64_t{1});

  FDEP_CHECK_EQ(kInitialGeneration.value(), std::uint64_t{0});
}

FDEP_TEST(ids, ordering_is_by_value) {
  const auto three = DependencyEdgeId::from_value(3);
  const auto five = DependencyEdgeId::from_value(5);
  FDEP_CHECK(three < five);
  FDEP_CHECK(five > three);
  FDEP_CHECK(three == DependencyEdgeId::from_value(3));
  FDEP_CHECK(three != five);

  const auto low_revision = EdgeRevision::from_value(1);
  const auto high_revision = EdgeRevision::from_value(9);
  FDEP_CHECK(low_revision < high_revision);
}

FDEP_TEST(ids, increment_refuses_to_wrap) {
  EdgeRevision revision = EdgeRevision::from_value(UINT64_MAX);
  FDEP_CHECK(!try_increment(revision));
  FDEP_CHECK_EQ(revision.value(), UINT64_MAX);

  revision = EdgeRevision::from_value(4);
  FDEP_CHECK(try_increment(revision));
  FDEP_CHECK_EQ(revision.value(), std::uint64_t{5});

  DependencyGeneration generation = DependencyGeneration::from_value(UINT64_MAX);
  FDEP_CHECK(!try_increment(generation));

  generation = kInitialGeneration;
  FDEP_CHECK(try_increment(generation));
  FDEP_CHECK_EQ(generation.value(), std::uint64_t{1});
}

FDEP_TEST(ids, text_and_parse_round_trip) {
  const auto id = DependencyEdgeId::from_value(123456789);
  FDEP_CHECK_EQ(to_text(id), std::string{"123456789"});
  DependencyEdgeId parsed{};
  FDEP_CHECK(parse_edge_id("123456789", parsed));
  FDEP_CHECK_EQ(parsed.value(), id.value());

  const auto revision = EdgeRevision::from_value(7);
  FDEP_CHECK_EQ(to_text(revision), std::string{"7"});
  EdgeRevision parsed_revision{};
  FDEP_CHECK(parse_edge_revision("7", parsed_revision));
  FDEP_CHECK_EQ(parsed_revision.value(), std::uint64_t{7});

  const auto generation = DependencyGeneration::from_value(42);
  FDEP_CHECK_EQ(to_text(generation), std::string{"42"});
  DependencyGeneration parsed_generation{};
  FDEP_CHECK(parse_generation("42", parsed_generation));
  FDEP_CHECK_EQ(parsed_generation.value(), std::uint64_t{42});
}

FDEP_TEST(ids, parsing_rejects_everything_that_is_not_a_plain_number) {
  DependencyEdgeId id{};
  FDEP_CHECK(!parse_edge_id("", id));
  FDEP_CHECK(!parse_edge_id(" ", id));
  FDEP_CHECK(!parse_edge_id(" 1", id));
  FDEP_CHECK(!parse_edge_id("1 ", id));
  FDEP_CHECK(!parse_edge_id("+1", id));
  FDEP_CHECK(!parse_edge_id("-1", id));
  FDEP_CHECK(!parse_edge_id("01", id));
  FDEP_CHECK(!parse_edge_id("0x10", id));
  FDEP_CHECK(!parse_edge_id("1.0", id));
  FDEP_CHECK(!parse_edge_id("1e3", id));
  FDEP_CHECK(!parse_edge_id("abc", id));
  FDEP_CHECK(!parse_edge_id("18446744073709551616", id));  // 2^64
  FDEP_CHECK(parse_edge_id("0", id));
  FDEP_CHECK_EQ(id.value(), std::uint64_t{0});
  FDEP_CHECK(parse_edge_id("18446744073709551615", id));
  FDEP_CHECK_EQ(id.value(), UINT64_MAX);
}

FDEP_TEST(ids, hashing_is_available_and_consistent) {
  std::unordered_map<DependencyEdgeId, int> by_id;
  by_id.emplace(DependencyEdgeId::from_value(1), 10);
  by_id.emplace(DependencyEdgeId::from_value(2), 20);
  FDEP_CHECK_EQ(by_id.size(), std::size_t{2});
  FDEP_CHECK_EQ(by_id.at(DependencyEdgeId::from_value(1)), 10);
  FDEP_CHECK_EQ(by_id.at(DependencyEdgeId::from_value(2)), 20);
  FDEP_CHECK_EQ(std::hash<DependencyEdgeId>{}(DependencyEdgeId::from_value(2)),
                std::hash<DependencyEdgeId>{}(DependencyEdgeId::from_value(2)));
  FDEP_CHECK_EQ(std::hash<DependencyGeneration>{}(DependencyGeneration::from_value(5)), std::hash<std::uint64_t>{}(5));
}
