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
#include <utility>

#include "byte_codec.hpp"
#include "facility_dependency_registry/version.hpp"
#include "graph_state.hpp"

namespace facility_dependency_registry {
namespace detail {
namespace {

/// The two forms of the canonical payload. The form is part of the payload so
/// that a decoder can never mistake the content form for an authoritative
/// state.
constexpr std::uint8_t kFormState = 1;
constexpr std::uint8_t kFormContent = 2;

/// Conservative lower bounds on the encoded size of one record. A payload that
/// claims more records than could possibly fit in the bytes that remain is
/// rejected before any container is reserved, so a tiny hostile file can never
/// ask for a large allocation.
constexpr std::size_t kMinEncodedEdgeBytes = 64;
constexpr std::size_t kMinEncodedRefBytes = 32;

void write_node_ref(ByteWriter& writer, const DependencyNodeRef& ref) {
  writer.u8(static_cast<std::uint8_t>(ref.domain()));
  writer.sized_string(ref.id());
}

void write_provenance(ByteWriter& writer, const ProvenanceRecord& provenance) {
  writer.u8(static_cast<std::uint8_t>(provenance.source()));
  writer.sized_string(provenance.source_id());
  writer.sized_string(provenance.principal());
  writer.i64(provenance.recorded_at_unix_ms());
  writer.sized_string(provenance.annotation());
}

void write_constraint(ByteWriter& writer, const DependencyConstraint& constraint) {
  writer.u8(static_cast<std::uint8_t>(constraint.kind()));
  if (constraint.value().is_integer()) {
    writer.u8(1);
    writer.i64(constraint.value().integer());
  } else {
    writer.u8(2);
    writer.sized_string(constraint.value().token());
  }
}

Result<DependencyNodeRef> read_node_ref(ByteReader& reader, const RegistryLimits& limits) {
  std::uint8_t domain = 0;
  if (!reader.u8(domain)) {
    return Result<DependencyNodeRef>::failure(ErrorCode::StoreTruncated, "the payload ends inside a node domain");
  }
  std::string id;
  if (!reader.sized_string(limits.max_id_length, id)) {
    if (reader.limit_exceeded()) {
      return Result<DependencyNodeRef>::failure(ErrorCode::PayloadTooLarge,
                                                "a node identifier exceeds the configured maximum length");
    }
    return Result<DependencyNodeRef>::failure(ErrorCode::StoreTruncated,
                                              "the payload ends inside a node identifier");
  }
  if (domain < 1 || domain > kNodeDomainCount) {
    return Result<DependencyNodeRef>::failure(ErrorCode::StoreCorrupt,
                                              "a node reference declares an unknown domain");
  }
  auto ref = DependencyNodeRef::create(static_cast<NodeDomain>(domain), id, limits.max_id_length);
  if (!ref) {
    return Result<DependencyNodeRef>::failure(ErrorCode::StoreCorrupt,
                                              std::string{"a node reference is not canonical: "} +
                                                  ref.error().detail());
  }
  return ref;
}

Result<ProvenanceRecord> read_provenance(ByteReader& reader, const RegistryLimits& limits) {
  std::uint8_t source = 0;
  std::string source_id;
  std::string principal;
  std::int64_t recorded_at = 0;
  std::string annotation;
  if (!reader.u8(source)) {
    return Result<ProvenanceRecord>::failure(ErrorCode::StoreTruncated,
                                             "the payload ends inside a provenance source");
  }
  if (!reader.sized_string(limits.max_id_length, source_id) ||
      !reader.sized_string(limits.max_id_length, principal)) {
    if (reader.limit_exceeded()) {
      return Result<ProvenanceRecord>::failure(ErrorCode::PayloadTooLarge,
                                               "a provenance identifier exceeds the configured maximum length");
    }
    return Result<ProvenanceRecord>::failure(ErrorCode::StoreTruncated,
                                             "the payload ends inside a provenance identifier");
  }
  if (!reader.i64(recorded_at)) {
    return Result<ProvenanceRecord>::failure(ErrorCode::StoreTruncated,
                                             "the payload ends inside a provenance timestamp");
  }
  if (!reader.sized_string(limits.max_annotation_length, annotation)) {
    if (reader.limit_exceeded()) {
      return Result<ProvenanceRecord>::failure(ErrorCode::PayloadTooLarge,
                                               "an annotation exceeds the configured maximum length");
    }
    return Result<ProvenanceRecord>::failure(ErrorCode::StoreTruncated,
                                             "the payload ends inside a provenance annotation");
  }
  if (source < 1 || source > kProvenanceSourceCount) {
    return Result<ProvenanceRecord>::failure(ErrorCode::StoreCorrupt,
                                             "a provenance record declares an unknown source");
  }
  auto record = ProvenanceRecord::create(static_cast<ProvenanceSource>(source), source_id, principal, recorded_at,
                                        annotation, limits.max_id_length, limits.max_annotation_length);
  if (!record) {
    return Result<ProvenanceRecord>::failure(ErrorCode::StoreCorrupt,
                                             std::string{"a provenance record is not acceptable: "} +
                                                 record.error().detail());
  }
  return record;
}

Result<DependencyConstraint> read_constraint(ByteReader& reader, const RegistryLimits& limits) {
  std::uint8_t kind = 0;
  std::uint8_t value_type = 0;
  if (!reader.u8(kind) || !reader.u8(value_type)) {
    return Result<DependencyConstraint>::failure(ErrorCode::StoreTruncated,
                                                 "the payload ends inside a constraint");
  }
  if (kind < 1 || kind > kConstraintKindCount) {
    return Result<DependencyConstraint>::failure(ErrorCode::StoreCorrupt,
                                                 "a constraint declares an unknown kind");
  }
  const auto constraint_kind = static_cast<ConstraintKind>(kind);
  if (value_type == 1) {
    std::int64_t value = 0;
    if (!reader.i64(value)) {
      return Result<DependencyConstraint>::failure(ErrorCode::StoreTruncated,
                                                   "the payload ends inside a constraint value");
    }
    auto constraint = DependencyConstraint::make(constraint_kind, value);
    if (!constraint) {
      return Result<DependencyConstraint>::failure(ErrorCode::StoreCorrupt,
                                                   std::string{"a constraint is not acceptable: "} +
                                                       constraint.error().detail());
    }
    return constraint;
  }
  if (value_type == 2) {
    std::string token;
    if (!reader.sized_string(limits.max_annotation_length, token)) {
      if (reader.limit_exceeded()) {
        return Result<DependencyConstraint>::failure(ErrorCode::PayloadTooLarge,
                                                     "a constraint token exceeds the configured maximum length");
      }
      return Result<DependencyConstraint>::failure(ErrorCode::StoreTruncated,
                                                   "the payload ends inside a constraint token");
    }
    auto constraint = DependencyConstraint::make(constraint_kind, token);
    if (!constraint) {
      return Result<DependencyConstraint>::failure(ErrorCode::StoreCorrupt,
                                                   std::string{"a constraint is not acceptable: "} +
                                                       constraint.error().detail());
    }
    return constraint;
  }
  return Result<DependencyConstraint>::failure(ErrorCode::StoreCorrupt,
                                               "a constraint declares an unknown value encoding");
}

}  // namespace

Result<std::vector<std::byte>> encode_state(const GraphState& state, bool include_generation) {
  ByteWriter writer;
  // A rough reservation, bounded by the configured edge capacity.
  writer.reserve(state.edges().size() * 128 + state.declared_refs().size() * 64 + 32);
  writer.u32(kStateFormatVersion);
  writer.u8(include_generation ? kFormState : kFormContent);
  if (include_generation) {
    writer.u64(state.generation().value());
  }
  writer.u64(state.next_edge_ordinal());

  writer.u32(static_cast<std::uint32_t>(state.declared_refs().size()));
  for (const auto& declaration : state.declared_refs()) {
    write_node_ref(writer, declaration.ref());
    write_provenance(writer, declaration.provenance());
    writer.u64(declaration.declared_generation().value());
  }

  writer.u32(static_cast<std::uint32_t>(state.edges().size()));
  for (const auto& record : state.edges()) {
    writer.u64(record.id().value());
    writer.u64(record.revision().value());
    writer.u8(static_cast<std::uint8_t>(record.kind()));
    writer.u8(static_cast<std::uint8_t>(record.strength()));
    writer.u8(static_cast<std::uint8_t>(record.direction()));
    writer.u8(static_cast<std::uint8_t>(record.lifecycle()));
    write_node_ref(writer, record.source());
    write_node_ref(writer, record.target());
    writer.u32(static_cast<std::uint32_t>(record.constraints().size()));
    for (const auto& constraint : record.constraints()) {
      write_constraint(writer, constraint);
    }
    write_provenance(writer, record.provenance());
    writer.u64(record.registered_generation().value());
    writer.u64(record.last_modified_generation().value());
  }
  return std::move(writer).take();
}

Result<std::shared_ptr<const GraphState>> decode_state(std::span<const std::byte> payload,
                                                       const RegistryLimits& limits) {
  if (const auto status = limits.validate(); !status.ok()) {
    return Result<std::shared_ptr<const GraphState>>::failure(status.error());
  }

  ByteReader reader{payload};
  std::uint32_t version = 0;
  if (!reader.u32(version)) {
    return Result<std::shared_ptr<const GraphState>>::failure(
        ErrorCode::StoreTruncated, "the payload is shorter than its version field");
  }
  if (version != kStateFormatVersion) {
    return Result<std::shared_ptr<const GraphState>>::failure(
        ErrorCode::StoreVersionUnsupported,
        "the payload declares state format version " + std::to_string(version));
  }
  std::uint8_t form = 0;
  if (!reader.u8(form)) {
    return Result<std::shared_ptr<const GraphState>>::failure(ErrorCode::StoreTruncated,
                                                              "the payload is shorter than its form field");
  }
  if (form != kFormState) {
    return Result<std::shared_ptr<const GraphState>>::failure(
        ErrorCode::StoreCorrupt, "the payload is a content digest form, not an authoritative state");
  }
  std::uint64_t generation = 0;
  std::uint64_t next_ordinal = 0;
  if (!reader.u64(generation) || !reader.u64(next_ordinal)) {
    return Result<std::shared_ptr<const GraphState>>::failure(
        ErrorCode::StoreTruncated, "the payload is shorter than its generation fields");
  }
  if (generation > limits.max_generation) {
    return Result<std::shared_ptr<const GraphState>>::failure(
        ErrorCode::StoreCorrupt, "the payload declares a generation beyond the configured limit");
  }
  if (next_ordinal < kFirstEdgeOrdinal || next_ordinal > kHardMaxGeneration) {
    return Result<std::shared_ptr<const GraphState>>::failure(
        ErrorCode::StoreCorrupt, "the payload declares an unusable next edge ordinal");
  }

  std::uint32_t ref_count = 0;
  if (!reader.u32(ref_count)) {
    return Result<std::shared_ptr<const GraphState>>::failure(
        ErrorCode::StoreTruncated, "the payload is shorter than its declaration count");
  }
  if (ref_count > limits.max_declared_refs) {
    return Result<std::shared_ptr<const GraphState>>::failure(
        ErrorCode::PayloadTooLarge, "the payload declares more external references than the configured maximum");
  }
  if (static_cast<std::size_t>(ref_count) > reader.remaining() / kMinEncodedRefBytes) {
    return Result<std::shared_ptr<const GraphState>>::failure(
        ErrorCode::StoreCorrupt, "the payload declares more external references than it can contain");
  }

  std::vector<DeclaredExternalRef> refs;
  refs.reserve(ref_count);
  for (std::uint32_t index = 0; index < ref_count; ++index) {
    auto ref = read_node_ref(reader, limits);
    if (!ref) {
      return Result<std::shared_ptr<const GraphState>>::failure(
          RegistryError{ref.error().code(), std::string{"declaration: "} + ref.error().detail()});
    }
    auto provenance = read_provenance(reader, limits);
    if (!provenance) {
      return Result<std::shared_ptr<const GraphState>>::failure(
          RegistryError{provenance.error().code(), std::string{"declaration: "} + provenance.error().detail()});
    }
    std::uint64_t declared_generation = 0;
    if (!reader.u64(declared_generation)) {
      return Result<std::shared_ptr<const GraphState>>::failure(
          ErrorCode::StoreTruncated, "the payload ends inside a declaration generation");
    }
    refs.push_back(EdgeRecordFactory::make_declared_ref(
        std::move(ref).value(), std::move(provenance).value(),
        DependencyGeneration::from_value(declared_generation)));
  }

  std::uint32_t edge_count = 0;
  if (!reader.u32(edge_count)) {
    return Result<std::shared_ptr<const GraphState>>::failure(
        ErrorCode::StoreTruncated, "the payload is shorter than its edge count");
  }
  if (edge_count > limits.max_edges) {
    return Result<std::shared_ptr<const GraphState>>::failure(
        ErrorCode::PayloadTooLarge, "the payload declares more edges than the configured maximum");
  }
  if (static_cast<std::size_t>(edge_count) > reader.remaining() / kMinEncodedEdgeBytes) {
    return Result<std::shared_ptr<const GraphState>>::failure(
        ErrorCode::StoreCorrupt, "the payload declares more edges than it can contain");
  }

  std::vector<DependencyEdgeRecord> edges;
  edges.reserve(edge_count);
  for (std::uint32_t index = 0; index < edge_count; ++index) {
    std::uint64_t id = 0;
    std::uint64_t revision = 0;
    std::uint8_t kind = 0;
    std::uint8_t strength = 0;
    std::uint8_t direction = 0;
    std::uint8_t lifecycle = 0;
    if (!reader.u64(id) || !reader.u64(revision) || !reader.u8(kind) || !reader.u8(strength) ||
        !reader.u8(direction) || !reader.u8(lifecycle)) {
      return Result<std::shared_ptr<const GraphState>>::failure(
          ErrorCode::StoreTruncated, "the payload ends inside an edge header");
    }
    if (kind < 1 || kind > kDependencyKindCount || strength < 1 || strength > kDependencyStrengthCount ||
        direction < 1 || direction > kDirectionCount || lifecycle < 1 || lifecycle > kLifecycleStateCount) {
      return Result<std::shared_ptr<const GraphState>>::failure(
          ErrorCode::StoreCorrupt, "an edge declares a value outside its enumeration domain");
    }

    auto source = read_node_ref(reader, limits);
    if (!source) {
      return Result<std::shared_ptr<const GraphState>>::failure(
          RegistryError{source.error().code(), std::string{"edge source: "} + source.error().detail()});
    }
    auto target = read_node_ref(reader, limits);
    if (!target) {
      return Result<std::shared_ptr<const GraphState>>::failure(
          RegistryError{target.error().code(), std::string{"edge target: "} + target.error().detail()});
    }

    std::uint32_t constraint_count = 0;
    if (!reader.u32(constraint_count)) {
      return Result<std::shared_ptr<const GraphState>>::failure(
          ErrorCode::StoreTruncated, "the payload ends inside an edge constraint count");
    }
    if (constraint_count > limits.max_constraints_per_edge) {
      return Result<std::shared_ptr<const GraphState>>::failure(
          ErrorCode::PayloadTooLarge, "an edge declares more constraints than the configured maximum");
    }
    std::vector<DependencyConstraint> constraints;
    constraints.reserve(constraint_count);
    for (std::uint32_t constraint_index = 0; constraint_index < constraint_count; ++constraint_index) {
      auto constraint = read_constraint(reader, limits);
      if (!constraint) {
        return Result<std::shared_ptr<const GraphState>>::failure(
            RegistryError{constraint.error().code(), std::string{"edge constraint: "} + constraint.error().detail()});
      }
      constraints.push_back(std::move(constraint).value());
    }

    auto provenance = read_provenance(reader, limits);
    if (!provenance) {
      return Result<std::shared_ptr<const GraphState>>::failure(
          RegistryError{provenance.error().code(), std::string{"edge provenance: "} + provenance.error().detail()});
    }
    std::uint64_t registered = 0;
    std::uint64_t modified = 0;
    if (!reader.u64(registered) || !reader.u64(modified)) {
      return Result<std::shared_ptr<const GraphState>>::failure(
          ErrorCode::StoreTruncated, "the payload ends inside an edge generation pair");
    }

    DependencyEdgeSpec spec;
    spec.source = std::move(source).value();
    spec.target = std::move(target).value();
    spec.kind = static_cast<DependencyKind>(kind);
    spec.strength = static_cast<DependencyStrength>(strength);
    spec.direction = static_cast<Direction>(direction);
    spec.initial_lifecycle = static_cast<LifecycleState>(lifecycle);
    spec.constraints = std::move(constraints);
    spec.provenance = std::move(provenance).value();

    edges.push_back(EdgeRecordFactory::make_edge(DependencyEdgeId::from_value(id),
                                                 EdgeRevision::from_value(revision), std::move(spec),
                                                 DependencyGeneration::from_value(registered),
                                                 DependencyGeneration::from_value(modified)));
  }

  if (!reader.at_end()) {
    return Result<std::shared_ptr<const GraphState>>::failure(
        ErrorCode::StoreCorrupt,
        "the payload has " + std::to_string(reader.remaining()) + " trailing bytes after the last edge");
  }

  auto state = GraphState::build(limits, DependencyGeneration::from_value(generation), next_ordinal,
                                 std::move(edges), std::move(refs));
  if (!state) {
    return Result<std::shared_ptr<const GraphState>>::failure(
        RegistryError{ErrorCode::StoreCorrupt, std::string{"the payload is not a valid state: "} +
                                                   state.error().detail()});
  }
  return state;
}

}  // namespace detail
}  // namespace facility_dependency_registry
