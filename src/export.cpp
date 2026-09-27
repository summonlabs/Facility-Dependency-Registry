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

#include "facility_dependency_registry/export.hpp"
#include "facility_dependency_registry/version.hpp"

namespace facility_dependency_registry {
namespace {

/// Renders one edge exactly as the canonical text export defines it.
///
/// Line oriented, one record per line, field order fixed, no trailing
/// whitespace. The rendering never contains a newline, because every stored
/// string is validated to be free of control characters.
std::string render_edge(const DependencyEdgeRecord& record, const ExportOptions& options) {
  std::string result;
  result.reserve(record.source().id().size() + record.target().id().size() + 192);
  result.append("edge id=");
  result.append(facility_dependency_registry::to_text(record.id()));
  result.append(" revision=");
  result.append(facility_dependency_registry::to_text(record.revision()));
  result.append(" kind=");
  result.append(to_token(record.kind()));
  result.append(" strength=");
  result.append(to_token(record.strength()));
  result.append(" direction=");
  result.append(to_token(record.direction()));
  result.append(" lifecycle=");
  result.append(to_token(record.lifecycle()));
  result.append(" source=");
  result.append(record.source().to_canonical());
  result.append(" target=");
  result.append(record.target().to_canonical());
  result.append(" registered=");
  result.append(facility_dependency_registry::to_text(record.registered_generation()));
  result.append(" modified=");
  result.append(facility_dependency_registry::to_text(record.last_modified_generation()));
  if (options.include_constraints) {
    result.append(" constraints=");
    if (record.constraints().empty()) {
      result.append("none");
    } else {
      bool first = true;
      for (const auto& constraint : record.constraints()) {
        if (!first) {
          result.push_back(';');
        }
        first = false;
        result.append(constraint.to_text());
      }
    }
  }
  if (options.include_provenance) {
    result.append(" provenance={");
    result.append(record.provenance().to_text());
    result.push_back('}');
  }
  return result;
}

std::string render_declaration(const DeclaredExternalRef& declaration, const ExportOptions& options) {
  std::string result;
  result.reserve(declaration.ref().id().size() + 96);
  result.append("declaration ref=");
  result.append(declaration.ref().to_canonical());
  result.append(" declared=");
  result.append(facility_dependency_registry::to_text(declaration.declared_generation()));
  if (options.include_provenance) {
    result.append(" provenance={");
    result.append(declaration.provenance().to_text());
    result.push_back('}');
  }
  return result;
}

}  // namespace

Result<std::string> export_text(const RegistrySnapshot& snapshot, const ExportOptions& options) {
  if (options.max_edges != 0 && snapshot.edge_count() > options.max_edges) {
    return Result<std::string>::failure(
        ErrorCode::QueryResultLimitExceeded,
        "the snapshot holds " + std::to_string(snapshot.edge_count()) +
            " edges, above the requested export bound of " + std::to_string(options.max_edges));
  }

  std::string output;
  output.reserve(snapshot.edge_count() * 192 + snapshot.declared_ref_count() * 96 + 256);
  output.append("format facility-dependency-registry/");
  output.append(std::to_string(kVersionMajor));
  output.push_back('\n');
  output.append("generation ");
  output.append(facility_dependency_registry::to_text(snapshot.generation()));
  output.push_back('\n');
  output.append("state-digest ");
  output.append(snapshot.state_digest().to_hex());
  output.push_back('\n');
  output.append("content-digest ");
  output.append(snapshot.content_digest().to_hex());
  output.push_back('\n');
  output.append("edge-count ");
  output.append(std::to_string(snapshot.edge_count()));
  output.push_back('\n');
  output.append("declaration-count ");
  output.append(std::to_string(snapshot.declared_ref_count()));
  output.push_back('\n');
  output.append("node-count ");
  output.append(std::to_string(snapshot.node_count()));
  output.push_back('\n');

  for (const auto& record : snapshot.edges()) {
    output.append(render_edge(record, options));
    output.push_back('\n');
  }
  if (options.include_declared_refs) {
    for (const auto& declaration : snapshot.declared_refs()) {
      output.append(render_declaration(declaration, options));
      output.push_back('\n');
    }
  }
  if (options.include_nodes) {
    for (const auto& node : snapshot.nodes()) {
      output.append("node ");
      output.append(node.to_canonical());
      output.push_back('\n');
    }
  }
  return output;
}

}  // namespace facility_dependency_registry
