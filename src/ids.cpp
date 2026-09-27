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

#include "facility_dependency_registry/ids.hpp"

#include <string>

namespace facility_dependency_registry {
namespace {

// Strict unsigned decimal parse: digits only, no sign, no whitespace, no
// leading zeros beyond the single value "0", and no overflow. Anything else is
// rejected rather than partially accepted.
bool parse_uint64_strict(std::string_view text, std::uint64_t& out) noexcept {
  if (text.empty() || text.size() > 20) {
    return false;
  }
  std::uint64_t value = 0;
  std::size_t index = 0;
  if (text.front() == '0') {
    if (text.size() != 1) {
      return false;
    }
    out = 0;
    return true;
  }
  for (; index < text.size(); ++index) {
    const char character = text[index];
    if (character < '0' || character > '9') {
      return false;
    }
    const auto digit = static_cast<std::uint64_t>(character - '0');
    if (value > (UINT64_MAX - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  out = value;
  return true;
}

}  // namespace

std::string to_text(DependencyEdgeId id) { return std::to_string(id.value()); }

std::string to_text(EdgeRevision revision) { return std::to_string(revision.value()); }

std::string to_text(DependencyGeneration generation) { return std::to_string(generation.value()); }

bool parse_edge_id(std::string_view text, DependencyEdgeId& out) noexcept {
  std::uint64_t value = 0;
  if (!parse_uint64_strict(text, value)) {
    return false;
  }
  out = DependencyEdgeId::from_value(value);
  return true;
}

bool parse_edge_revision(std::string_view text, EdgeRevision& out) noexcept {
  std::uint64_t value = 0;
  if (!parse_uint64_strict(text, value)) {
    return false;
  }
  out = EdgeRevision::from_value(value);
  return true;
}

bool parse_generation(std::string_view text, DependencyGeneration& out) noexcept {
  std::uint64_t value = 0;
  if (!parse_uint64_strict(text, value)) {
    return false;
  }
  out = DependencyGeneration::from_value(value);
  return true;
}

}  // namespace facility_dependency_registry
