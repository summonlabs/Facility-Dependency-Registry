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
#include <string_view>

#include "facility_dependency_registry/export.hpp"
#include "facility_dependency_registry/version.hpp"

namespace facility_dependency_registry {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

/// A deterministic JSON writer. Member order is the order the caller emits,
/// indentation is fixed, no floating point value is ever produced, and every
/// string is escaped the same way on every platform.
class JsonWriter {
 public:
  explicit JsonWriter(bool pretty) : pretty_(pretty) {}

  void begin_object() {
    separator();
    output_.push_back('{');
    ++depth_;
    first_ = true;
  }
  void end_object() {
    --depth_;
    if (!first_) {
      newline();
    }
    first_ = false;
    output_.push_back('}');
  }
  void begin_array() {
    separator();
    output_.push_back('[');
    ++depth_;
    first_ = true;
  }
  void end_array() {
    --depth_;
    if (!first_) {
      newline();
    }
    first_ = false;
    output_.push_back(']');
  }

  void key(std::string_view name) {
    separator();
    output_.push_back('"');
    output_.append(json_escape(name));
    output_.append("\": ");
    key_pending_ = true;
  }

  void value(std::string_view text) {
    separator();
    output_.push_back('"');
    output_.append(json_escape(text));
    output_.push_back('"');
  }
  void value(std::uint64_t number) {
    separator();
    output_.append(std::to_string(number));
  }
  /// Signed values are rendered by their own entry point so that no overload
  /// resolution can quietly reinterpret a negative number as an unsigned one.
  void value_signed(std::int64_t number) {
    separator();
    output_.append(std::to_string(number));
  }
  void value(bool flag) {
    separator();
    output_.append(flag ? "true" : "false");
  }
  void value_null() {
    separator();
    output_.append("null");
  }

  [[nodiscard]] std::string take() && { return std::move(output_); }

 private:
  /// Emits whatever has to come before the next token: a comma when a member
  /// has already been written at this level, and a line break with the current
  /// indentation. A key is followed immediately by its value, so no separator
  /// is emitted between them.
  void separator() {
    if (key_pending_) {
      key_pending_ = false;
      first_ = false;
      return;
    }
    if (!first_) {
      output_.push_back(',');
    }
    first_ = false;
    if (pretty_ && !output_.empty()) {
      // The very first token of the document is not preceded by a line break;
      // everything else is, because at that point an opening bracket or a
      // previous member is already in the buffer.
      newline();
    }
  }

  void newline() {
    if (!pretty_) {
      return;
    }
    output_.push_back('\n');
    output_.append(static_cast<std::size_t>(depth_) * 2, ' ');
  }

  std::string output_{};
  std::size_t depth_ = 0;
  bool first_ = true;
  bool key_pending_ = false;
  bool pretty_ = true;
};

void write_provenance(JsonWriter& writer, const ProvenanceRecord& provenance) {
  writer.begin_object();
  writer.key("source");
  writer.value(to_token(provenance.source()));
  writer.key("source_id");
  writer.value(provenance.source_id());
  writer.key("principal");
  writer.value(provenance.principal());
  writer.key("recorded_at_unix_ms");
  writer.value_signed(provenance.recorded_at_unix_ms());
  writer.key("annotation");
  writer.value(provenance.annotation());
  writer.end_object();
}

void write_node_ref(JsonWriter& writer, const DependencyNodeRef& ref) {
  writer.begin_object();
  writer.key("domain");
  writer.value(to_token(ref.domain()));
  writer.key("id");
  writer.value(ref.id());
  writer.key("canonical");
  writer.value(ref.to_canonical());
  writer.end_object();
}

}  // namespace

std::string json_escape(std::string_view text) {
  std::string result;
  result.reserve(text.size() + 8);
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    switch (character) {
      case '"':
        result.append("\\\"");
        continue;
      case '\\':
        result.append("\\\\");
        continue;
      case '\b':
        result.append("\\b");
        continue;
      case '\f':
        result.append("\\f");
        continue;
      case '\n':
        result.append("\\n");
        continue;
      case '\r':
        result.append("\\r");
        continue;
      case '\t':
        result.append("\\t");
        continue;
      default:
        break;
    }
    if (byte < 0x20) {
      result.append("\\u00");
      result.push_back(kHexDigits[(byte >> 4) & 0x0Fu]);
      result.push_back(kHexDigits[byte & 0x0Fu]);
      continue;
    }
    result.push_back(character);
  }
  return result;
}

Result<std::string> export_json(const RegistrySnapshot& snapshot, const ExportOptions& options) {
  if (options.max_edges != 0 && snapshot.edge_count() > options.max_edges) {
    return Result<std::string>::failure(
        ErrorCode::QueryResultLimitExceeded,
        "the snapshot holds " + std::to_string(snapshot.edge_count()) +
            " edges, above the requested export bound of " + std::to_string(options.max_edges));
  }

  JsonWriter writer{options.pretty};
  writer.begin_object();
  writer.key("format");
  writer.value(std::string{"facility-dependency-registry/"} + std::to_string(kVersionMajor));
  writer.key("generation");
  writer.value(snapshot.generation().value());
  writer.key("state_digest");
  writer.value(snapshot.state_digest().to_hex());
  writer.key("content_digest");
  writer.value(snapshot.content_digest().to_hex());
  writer.key("edge_count");
  writer.value(static_cast<std::uint64_t>(snapshot.edge_count()));
  writer.key("declaration_count");
  writer.value(static_cast<std::uint64_t>(snapshot.declared_ref_count()));
  writer.key("node_count");
  writer.value(static_cast<std::uint64_t>(snapshot.node_count()));

  writer.key("edges");
  writer.begin_array();
  for (const auto& record : snapshot.edges()) {
    writer.begin_object();
    writer.key("id");
    writer.value(record.id().value());
    writer.key("revision");
    writer.value(record.revision().value());
    writer.key("kind");
    writer.value(to_token(record.kind()));
    writer.key("strength");
    writer.value(to_token(record.strength()));
    writer.key("direction");
    writer.value(to_token(record.direction()));
    writer.key("lifecycle");
    writer.value(to_token(record.lifecycle()));
    writer.key("source");
    write_node_ref(writer, record.source());
    writer.key("target");
    write_node_ref(writer, record.target());
    if (options.include_constraints) {
      writer.key("constraints");
      writer.begin_array();
      for (const auto& constraint : record.constraints()) {
        writer.begin_object();
        writer.key("kind");
        writer.value(to_token(constraint.kind()));
        if (constraint.value().is_integer()) {
          writer.key("integer");
          writer.value_signed(constraint.value().integer());
        } else {
          writer.key("token");
          writer.value(constraint.value().token());
        }
        writer.end_object();
      }
      writer.end_array();
    }
    if (options.include_provenance) {
      writer.key("provenance");
      write_provenance(writer, record.provenance());
    }
    writer.key("registered_generation");
    writer.value(record.registered_generation().value());
    writer.key("last_modified_generation");
    writer.value(record.last_modified_generation().value());
    writer.end_object();
  }
  writer.end_array();

  if (options.include_declared_refs) {
    writer.key("declared_refs");
    writer.begin_array();
    for (const auto& declaration : snapshot.declared_refs()) {
      writer.begin_object();
      writer.key("ref");
      write_node_ref(writer, declaration.ref());
      writer.key("declared_generation");
      writer.value(declaration.declared_generation().value());
      if (options.include_provenance) {
        writer.key("provenance");
        write_provenance(writer, declaration.provenance());
      }
      writer.end_object();
    }
    writer.end_array();
  }

  if (options.include_nodes) {
    writer.key("nodes");
    writer.begin_array();
    for (const auto& node : snapshot.nodes()) {
      write_node_ref(writer, node);
    }
    writer.end_array();
  }

  writer.end_object();
  return std::move(writer).take();
}

}  // namespace facility_dependency_registry
