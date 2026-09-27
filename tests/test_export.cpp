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
using fdep_test::feed;
using fdep_test::Harness;
using fdep_test::loop;
using fdep_test::rack;

namespace {

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::string current;
  for (const char character : text) {
    if (character == '\n') {
      lines.push_back(current);
      current.clear();
    } else {
      current.push_back(character);
    }
  }
  if (!current.empty()) {
    lines.push_back(current);
  }
  return lines;
}

std::size_t count_occurrences(const std::string& text, std::string_view needle) {
  std::size_t total = 0;
  std::size_t position = 0;
  while ((position = text.find(needle, position)) != std::string::npos) {
    ++total;
    position += needle.size();
  }
  return total;
}

}  // namespace

FDEP_TEST(export, the_text_form_has_a_fixed_shape) {
  Harness harness = Harness::ephemeral();
  harness.declare(asset("declared"), fdep_test::provenance("change-1", "alice", 3, "observed"));
  harness.add(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
  harness.add(asset("a"), loop("l1"), DependencyKind::CooledBy);

  const auto text = export_text(harness.snapshot());
  FDEP_REQUIRE_OK(text);
  const std::vector<std::string> lines = split_lines(text.value());
  FDEP_REQUIRE(lines.size() >= 10);
  FDEP_CHECK_EQ(lines[0], std::string{"format facility-dependency-registry/1"});
  FDEP_CHECK_EQ(lines[1], std::string{"generation 3"});
  FDEP_CHECK(lines[2].rfind("state-digest ", 0) == 0);
  FDEP_CHECK_EQ(lines[2].size(), std::size_t{13 + 64});
  FDEP_CHECK(lines[3].rfind("content-digest ", 0) == 0);
  FDEP_CHECK_EQ(lines[4], std::string{"edge-count 2"});
  FDEP_CHECK_EQ(lines[5], std::string{"declaration-count 1"});
  FDEP_CHECK_EQ(lines[6], std::string{"node-count 5"});

  // Edges come first, in canonical order, then declarations, then nodes. Edge
  // two was registered second but sorts first, because canonical order is by
  // key and not by identity.
  FDEP_CHECK(lines[7].rfind("edge id=2 ", 0) == 0);
  FDEP_CHECK(lines[7].find("kind=cooled-by") != std::string::npos);
  FDEP_CHECK(lines[7].find("source=asset:a target=cooling-domain:l1") != std::string::npos);
  FDEP_CHECK(lines[8].rfind("edge id=1 ", 0) == 0);
  FDEP_CHECK(lines[8].find("kind=requires-power-from") != std::string::npos);
  FDEP_CHECK(lines[9].rfind("declaration ref=asset:declared ", 0) == 0);
  FDEP_CHECK_EQ(lines[10], std::string{"node asset:a"});
  FDEP_CHECK_EQ(lines[11], std::string{"node asset:b"});
  FDEP_CHECK_EQ(lines[12], std::string{"node asset:declared"});
  FDEP_CHECK_EQ(lines[13], std::string{"node electrical-domain:f1"});
  FDEP_CHECK_EQ(lines[14], std::string{"node cooling-domain:l1"});
  FDEP_CHECK_EQ(lines.size(), std::size_t{15});

  // Nothing is trailed by whitespace and the form is stable across calls.
  for (const auto& line : lines) {
    FDEP_CHECK(line.empty() || (line.back() != ' ' && line.back() != '\t'));
  }
  const auto again = export_text(harness.snapshot());
  FDEP_REQUIRE_OK(again);
  FDEP_CHECK_EQ(again.value(), text.value());
}

FDEP_TEST(export, the_text_form_can_omit_sections) {
  Harness harness = Harness::ephemeral();
  harness.declare(asset("declared"), fdep_test::provenance("change-1", "alice", 3, "note"));
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);

  ExportOptions options;
  options.include_provenance = false;
  const auto without_provenance = export_text(harness.snapshot(), options);
  FDEP_REQUIRE_OK(without_provenance);
  FDEP_CHECK(without_provenance.value().find("provenance=") == std::string::npos);
  FDEP_CHECK(without_provenance.value().find("edge id=1 ") != std::string::npos);

  options = ExportOptions{};
  options.include_nodes = false;
  const auto without_nodes = export_text(harness.snapshot(), options);
  FDEP_REQUIRE_OK(without_nodes);
  FDEP_CHECK(without_nodes.value().find("\nnode ") == std::string::npos);
  FDEP_CHECK(without_nodes.value().find("node-count 3") != std::string::npos);

  options = ExportOptions{};
  options.include_declared_refs = false;
  const auto without_declarations = export_text(harness.snapshot(), options);
  FDEP_REQUIRE_OK(without_declarations);
  FDEP_CHECK(without_declarations.value().find("declaration ref=") == std::string::npos);
  FDEP_CHECK(without_declarations.value().find("declaration-count 1") != std::string::npos);

  options = ExportOptions{};
  options.include_constraints = false;
  const auto without_constraints = export_text(harness.snapshot(), options);
  FDEP_REQUIRE_OK(without_constraints);
  FDEP_CHECK(without_constraints.value().find("constraints=") == std::string::npos);
}

FDEP_TEST(export, an_export_bound_is_enforced) {
  Harness harness = Harness::ephemeral();
  for (int index = 0; index < 4; ++index) {
    harness.add(asset("node-" + std::to_string(index)), feed("f1"), DependencyKind::RequiresPowerFrom);
  }
  ExportOptions options;
  options.max_edges = 3;
  FDEP_CHECK_CODE(export_text(harness.snapshot(), options), ErrorCode::QueryResultLimitExceeded);
  FDEP_CHECK_CODE(export_json(harness.snapshot(), options), ErrorCode::QueryResultLimitExceeded);
  options.max_edges = 4;
  FDEP_REQUIRE_OK(export_text(harness.snapshot(), options));
  options.max_edges = 0;
  FDEP_REQUIRE_OK(export_text(harness.snapshot(), options));
}

FDEP_TEST(export, the_json_form_is_deterministic_and_well_formed) {
  Harness harness = Harness::ephemeral();
  harness.declare(asset("declared"), fdep_test::provenance("change-1", "alice", 3, "a \"quoted\" note"));
  const auto constraint = DependencyConstraint::make(ConstraintKind::RedundancyClass, "2N");
  FDEP_REQUIRE_OK(constraint);
  harness.add(fdep_test::make_spec(asset("a"), loop("l1"), DependencyKind::CooledBy, DependencyStrength::Soft,
                                   Direction::DependsOn, LifecycleState::Active, {constraint.value()}));

  const auto text = export_json(harness.snapshot());
  FDEP_REQUIRE_OK(text);
  const std::string& json = text.value();

  // Structure: braces and brackets balance, and the fixed member order is
  // present.
  FDEP_CHECK_EQ(count_occurrences(json, "{"), count_occurrences(json, "}"));
  FDEP_CHECK_EQ(count_occurrences(json, "["), count_occurrences(json, "]"));
  FDEP_CHECK(json.rfind("{\n  \"format\": \"facility-dependency-registry/1\",", 0) == 0);
  FDEP_CHECK(json.find("\"generation\": 2") != std::string::npos);
  FDEP_CHECK(json.find("\"state_digest\": \"") != std::string::npos);
  FDEP_CHECK(json.find("\"content_digest\": \"") != std::string::npos);
  FDEP_CHECK(json.find("\"edges\": [") != std::string::npos);
  FDEP_CHECK(json.find("\"declared_refs\": [") != std::string::npos);
  FDEP_CHECK(json.find("\"nodes\": [") != std::string::npos);
  FDEP_CHECK(json.find("\"kind\": \"cooled-by\"") != std::string::npos);
  FDEP_CHECK(json.find("\"token\": \"2N\"") != std::string::npos);
  FDEP_CHECK(json.find("\"domain\": \"cooling-domain\"") != std::string::npos);
  FDEP_CHECK(json.find("\"canonical\": \"asset:a\"") != std::string::npos);
  FDEP_CHECK(json.find("\"principal\": \"alice\"") != std::string::npos);
  FDEP_CHECK(json.find("\\\"quoted\\\"") != std::string::npos);

  const auto again = export_json(harness.snapshot());
  FDEP_REQUIRE_OK(again);
  FDEP_CHECK_EQ(again.value(), json);

  // The compact form carries the same members.
  ExportOptions compact;
  compact.pretty = false;
  const auto minified = export_json(harness.snapshot(), compact);
  FDEP_REQUIRE_OK(minified);
  FDEP_CHECK(minified.value().find('\n') == std::string::npos);
  FDEP_CHECK(minified.value().find("\"generation\": 2") != std::string::npos);
}

FDEP_TEST(export, json_escaping_covers_the_dangerous_characters) {
  const char control[] = {'a', '\x01', 'b'};
  FDEP_CHECK_EQ(json_escape("plain"), std::string{"plain"});
  FDEP_CHECK_EQ(json_escape("a\"b"), std::string{"a\\\"b"});
  FDEP_CHECK_EQ(json_escape("a\\b"), std::string{"a\\\\b"});
  FDEP_CHECK_EQ(json_escape("a\nb"), std::string{"a\\nb"});
  FDEP_CHECK_EQ(json_escape("a\tb"), std::string{"a\\tb"});
  FDEP_CHECK_EQ(json_escape("a\rb"), std::string{"a\\rb"});
  FDEP_CHECK_EQ(json_escape("a\bb"), std::string{"a\\bb"});
  FDEP_CHECK_EQ(json_escape("a\fb"), std::string{"a\\fb"});
  FDEP_CHECK_EQ(json_escape(std::string{control, 3}), std::string{"a\\u0001b"});
  FDEP_CHECK_EQ(json_escape("caf\u00e9"), std::string{"caf\u00e9"});
  FDEP_CHECK_EQ(json_escape(""), std::string{});
}

FDEP_TEST(export, an_empty_graph_exports_cleanly) {
  const RegistrySnapshot empty = RegistrySnapshot::empty();
  const auto text = export_text(empty);
  FDEP_REQUIRE_OK(text);
  const std::vector<std::string> lines = split_lines(text.value());
  FDEP_REQUIRE(lines.size() == 7);
  FDEP_CHECK_EQ(lines[0], std::string{"format facility-dependency-registry/1"});
  FDEP_CHECK_EQ(lines[1], std::string{"generation 0"});
  FDEP_CHECK_EQ(lines[4], std::string{"edge-count 0"});
  FDEP_CHECK_EQ(lines[5], std::string{"declaration-count 0"});
  FDEP_CHECK_EQ(lines[6], std::string{"node-count 0"});

  const auto json = export_json(empty);
  FDEP_REQUIRE_OK(json);
  FDEP_CHECK(json.value().find("\"edges\": []") != std::string::npos);
  FDEP_CHECK(json.value().find("\"nodes\": []") != std::string::npos);
  FDEP_CHECK(json.value().find("\"declared_refs\": []") != std::string::npos);
}

FDEP_TEST(export, rendering_is_stable_for_the_same_state) {
  Harness forward = Harness::ephemeral();
  forward.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  forward.add(asset("b"), rack("r1"), DependencyKind::HousedIn);

  Harness backward = Harness::ephemeral();
  backward.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  backward.add(asset("b"), rack("r1"), DependencyKind::HousedIn);

  const auto first = export_text(forward.snapshot());
  const auto second = export_text(backward.snapshot());
  FDEP_REQUIRE_OK(first);
  FDEP_REQUIRE_OK(second);
  FDEP_CHECK_EQ(first.value(), second.value());

  // The exported digests are the ones the snapshot reports, so the text form
  // can be used to compare two stores without decoding them.
  FDEP_CHECK(first.value().find(forward.snapshot().state_digest().to_hex()) != std::string::npos);
  FDEP_CHECK(first.value().find(forward.snapshot().content_digest().to_hex()) != std::string::npos);
}
