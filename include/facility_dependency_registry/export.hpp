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

#ifndef FACILITY_DEPENDENCY_REGISTRY_EXPORT_HPP
#define FACILITY_DEPENDENCY_REGISTRY_EXPORT_HPP

#include <cstdint>
#include <string>

#include "facility_dependency_registry/errors.hpp"
#include "facility_dependency_registry/snapshot.hpp"

namespace facility_dependency_registry {

/// Controls canonical text and JSON rendering.
///
/// Both renderings are deterministic: the field order is fixed, the record
/// order is the canonical order, no timestamp of the rendering itself is
/// emitted, and the output for one snapshot is byte identical on every machine
/// and every run.
struct ExportOptions {
  bool include_provenance = true;
  bool include_constraints = true;
  bool include_nodes = true;
  bool include_declared_refs = true;
  /// Indentation for JSON. Text output ignores it.
  bool pretty = true;
  /// Additional bound on the number of edges rendered. Zero means the
  /// snapshot's own edge limit is the only bound.
  std::uint32_t max_edges = 0;
};

/// Renders a snapshot as the canonical line oriented text form. The first line
/// is always `format facility-dependency-registry/<major>`.
[[nodiscard]] Result<std::string> export_text(const RegistrySnapshot& snapshot, const ExportOptions& options = {});

/// Renders a snapshot as canonical JSON. Object member order is fixed and the
/// output contains no floating point values, so it is byte stable.
[[nodiscard]] Result<std::string> export_json(const RegistrySnapshot& snapshot, const ExportOptions& options = {});

/// Escapes one UTF-8 string as a JSON string body (without the quotes).
[[nodiscard]] std::string json_escape(std::string_view text);

}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_EXPORT_HPP
