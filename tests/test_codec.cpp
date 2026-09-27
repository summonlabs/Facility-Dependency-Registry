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

#include <cstddef>
#include <cstdint>
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
using fdep_test::service;

namespace {

std::vector<std::byte> encode_snapshot(const RegistrySnapshot& snapshot) {
  auto bytes = snapshot.encode();
  if (!bytes) {
    fdep_test::fail_now("encode failed: " + bytes.error().detail());
  }
  return std::move(bytes).value();
}

RegistrySnapshot decode_bytes(const std::vector<std::byte>& bytes, const RegistryLimits& limits = {}) {
  auto snapshot = RegistrySnapshot::decode(std::span<const std::byte>{bytes}, limits);
  if (!snapshot) {
    fdep_test::fail_now("decode failed: " + snapshot.error().to_string());
  }
  return std::move(snapshot).value();
}

/// The little-endian u32 at `offset`.
void write_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned index = 0; index < 4; ++index) {
    bytes[offset + index] = static_cast<std::byte>((value >> (8u * index)) & 0xFFu);
  }
}

void write_u64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
  for (unsigned index = 0; index < 8; ++index) {
    bytes[offset + index] = static_cast<std::byte>((value >> (8u * index)) & 0xFFu);
  }
}

}  // namespace

FDEP_TEST(codec, round_trip_preserves_every_field) {
  Harness harness = Harness::ephemeral();
  const auto redundancy = DependencyConstraint::make(ConstraintKind::RedundancyClass, "N+1");
  FDEP_REQUIRE_OK(redundancy);
  const auto latency = DependencyConstraint::make(ConstraintKind::MaxLatencyMicros, 250);
  FDEP_REQUIRE_OK(latency);

  harness.declare(asset("declared"), fdep_test::provenance("change-1", "alice", 11, "declared"));
  harness.add(fdep_test::make_spec(asset("a"), loop("l1"), DependencyKind::CooledBy, DependencyStrength::Soft,
                                   Direction::DependsOn, LifecycleState::Active,
                                   {latency.value(), redundancy.value()},
                                   fdep_test::provenance("change-2", "bob", 12, "cooled")));
  const DependencyEdgeId second = harness.add(asset("b"), service("s1"), DependencyKind::ServedBy,
                                              DependencyStrength::Advisory, Direction::DependsOn,
                                              LifecycleState::Proposed);
  harness.add(fdep_test::asi("x"), fdep_test::dfi("y"), DependencyKind::ComposedDomainDependsOn,
              DependencyStrength::Soft, Direction::Mutual);
  FDEP_REQUIRE(harness.transition(second, kInitialRevision, LifecycleState::Retired).ok());

  const RegistrySnapshot original = harness.snapshot();
  const std::vector<std::byte> bytes = encode_snapshot(original);
  const RegistrySnapshot decoded = decode_bytes(bytes);

  FDEP_CHECK_EQ(decoded.generation().value(), original.generation().value());
  FDEP_CHECK_EQ(decoded.edge_count(), original.edge_count());
  FDEP_CHECK_EQ(decoded.declared_ref_count(), original.declared_ref_count());
  FDEP_CHECK_EQ(decoded.node_count(), original.node_count());
  FDEP_CHECK_EQ(decoded.next_edge_ordinal(), original.next_edge_ordinal());
  FDEP_CHECK_EQ(decoded.state_digest(), original.state_digest());
  FDEP_CHECK_EQ(decoded.content_digest(), original.content_digest());

  for (std::size_t index = 0; index < original.edges().size(); ++index) {
    FDEP_CHECK(decoded.edges()[index] == original.edges()[index]);
  }
  for (std::size_t index = 0; index < original.declared_refs().size(); ++index) {
    FDEP_CHECK(decoded.declared_refs()[index] == original.declared_refs()[index]);
  }
  FDEP_CHECK(encode_snapshot(decoded) == bytes);
}

FDEP_TEST(codec, encoding_is_byte_stable_across_runs) {
  const auto build = []() {
    Harness harness = Harness::ephemeral();
    harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
    harness.add(asset("b"), rack("r1"), DependencyKind::HousedIn);
    return encode_snapshot(harness.snapshot());
  };
  const std::vector<std::byte> first = build();
  for (int attempt = 0; attempt < 3; ++attempt) {
    FDEP_CHECK(build() == first);
  }
  FDEP_CHECK(!first.empty());
}

FDEP_TEST(codec, an_empty_graph_round_trips) {
  Harness harness = Harness::ephemeral();
  const std::vector<std::byte> bytes = encode_snapshot(harness.snapshot());
  // version, form, generation, next ordinal, two counts.
  FDEP_CHECK_EQ(bytes.size(), std::size_t{4 + 1 + 8 + 8 + 4 + 4});
  const RegistrySnapshot decoded = decode_bytes(bytes);
  FDEP_CHECK_EQ(decoded.generation().value(), std::uint64_t{0});
  FDEP_CHECK_EQ(decoded.edge_count(), std::size_t{0});
  FDEP_CHECK_EQ(decoded.next_edge_ordinal(), std::uint64_t{1});
}

FDEP_TEST(codec, every_truncation_is_rejected) {
  Harness harness = Harness::ephemeral();
  harness.declare(asset("declared"));
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const std::vector<std::byte> bytes = encode_snapshot(harness.snapshot());
  FDEP_REQUIRE(bytes.size() > 40);

  for (std::size_t length = 0; length < bytes.size(); ++length) {
    const std::vector<std::byte> truncated(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length));
    const auto result = RegistrySnapshot::decode(std::span<const std::byte>{truncated}, RegistryLimits{});
    if (result.has_value()) {
      fdep_test::fail_now("a payload truncated to " + std::to_string(length) + " bytes was accepted");
    }
    const ErrorCode code = result.error().code();
    FDEP_CHECK(code == ErrorCode::StoreTruncated || code == ErrorCode::StoreCorrupt);
  }
}

FDEP_TEST(codec, trailing_bytes_are_rejected) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  std::vector<std::byte> bytes = encode_snapshot(harness.snapshot());
  bytes.push_back(std::byte{0});
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}),
                  ErrorCode::StoreCorrupt);
}

FDEP_TEST(codec, an_unknown_version_is_rejected_rather_than_guessed) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  std::vector<std::byte> bytes = encode_snapshot(harness.snapshot());

  write_u32(bytes, 0, kStateFormatVersion + 1);
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}),
                  ErrorCode::StoreVersionUnsupported);

  write_u32(bytes, 0, kStateFormatVersion - 1);
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}),
                  ErrorCode::StoreVersionUnsupported);

  write_u32(bytes, 0, 0);
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}),
                  ErrorCode::StoreVersionUnsupported);
}

FDEP_TEST(codec, the_content_form_is_not_an_authoritative_state) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  std::vector<std::byte> bytes = encode_snapshot(harness.snapshot());
  // Byte 4 is the form selector.
  bytes[4] = std::byte{2};
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}),
                  ErrorCode::StoreCorrupt);
  bytes[4] = std::byte{0};
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}),
                  ErrorCode::StoreCorrupt);
}

FDEP_TEST(codec, counts_beyond_the_configuration_are_rejected_before_allocating) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  std::vector<std::byte> bytes = encode_snapshot(harness.snapshot());

  RegistryLimits limits;
  limits.max_edges = 0;
  limits.max_analysis_nodes = 4;
  limits.max_cycle_length = 2;
  // An incoherent configuration is refused outright.
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, limits),
                  ErrorCode::InvalidLimits);

  // A payload that claims more edges than could possibly fit inside the bytes
  // that remain is refused even though the configured maximum would allow it.
  std::vector<std::byte> inflated = bytes;
  const std::size_t edge_count_offset = 4 + 1 + 8 + 8 + 4;  // after the declaration count
  write_u32(inflated, edge_count_offset, 10'000);
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{inflated}, RegistryLimits{}),
                  ErrorCode::StoreCorrupt);

  // A count above the configured maximum is refused with the bound category.
  write_u32(inflated, edge_count_offset, 1'000'000);
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{inflated}, RegistryLimits{}),
                  ErrorCode::PayloadTooLarge);

  // A payload that claims more edges than the configured maximum is refused
  // with the bound category.
  RegistryLimits small;
  small.max_edges = 1;
  std::vector<std::byte> two_edges;
  {
    Harness bigger = Harness::ephemeral();
    bigger.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
    bigger.add(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
    two_edges = encode_snapshot(bigger.snapshot());
  }
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{two_edges}, small),
                  ErrorCode::PayloadTooLarge);
}

FDEP_TEST(codec, generation_and_ordinal_ranges_are_checked) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  std::vector<std::byte> bytes = encode_snapshot(harness.snapshot());

  // generation sits at byte 5.
  write_u64(bytes, 5, kHardMaxGeneration + 1);
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}),
                  ErrorCode::StoreCorrupt);

  write_u64(bytes, 5, 1);
  // next edge ordinal sits at byte 13.
  write_u64(bytes, 13, 0);
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}),
                  ErrorCode::StoreCorrupt);

  write_u64(bytes, 13, 1);  // the edge identity is not below the ordinal any more
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}),
                  ErrorCode::StoreCorrupt);

  write_u64(bytes, 13, 2);
  FDEP_REQUIRE_OK(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}));

  RegistryLimits tight;
  tight.max_generation = 1;
  FDEP_REQUIRE_OK(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, tight));
  write_u64(bytes, 5, 2);
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, tight), ErrorCode::StoreCorrupt);
}

FDEP_TEST(codec, a_non_canonical_record_order_is_rejected) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  harness.add(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const std::vector<std::byte> bytes = encode_snapshot(harness.snapshot());

  // Re-encode with the two edges swapped by hand: impossible through the API,
  // so the decoder must refuse it rather than quietly accepting a state the
  // registry could never have produced.
  Harness reversed = Harness::ephemeral();
  reversed.add(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
  reversed.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const std::vector<std::byte> other = encode_snapshot(reversed.snapshot());
  FDEP_CHECK(other != bytes);
  FDEP_REQUIRE_OK(RegistrySnapshot::decode(std::span<const std::byte>{other}, RegistryLimits{}));

  // Swapping the two edge records inside the canonical encoding breaks the
  // canonical order, and the payload is rejected.
  const std::size_t header = 4 + 1 + 8 + 8 + 4 + 4;
  const std::size_t edge_bytes = bytes.size() - header;
  FDEP_REQUIRE(edge_bytes % 2 == 0);
  const std::size_t half = edge_bytes / 2;
  std::vector<std::byte> swapped;
  swapped.insert(swapped.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(header));
  swapped.insert(swapped.end(), bytes.begin() + static_cast<std::ptrdiff_t>(header + half), bytes.end());
  swapped.insert(swapped.end(), bytes.begin() + static_cast<std::ptrdiff_t>(header),
                 bytes.begin() + static_cast<std::ptrdiff_t>(header + half));
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{swapped}, RegistryLimits{}),
                  ErrorCode::StoreCorrupt);
}

FDEP_TEST(codec, an_out_of_domain_enumeration_is_rejected) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  std::vector<std::byte> bytes = encode_snapshot(harness.snapshot());

  // The edge header is: id(8) revision(8) kind(1) strength(1) direction(1)
  // lifecycle(1), starting right after the counts.
  const std::size_t edge_start = 4 + 1 + 8 + 8 + 4 + 4;
  const std::size_t kind_offset = edge_start + 16;
  bytes[kind_offset] = std::byte{0};
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}),
                  ErrorCode::StoreCorrupt);
  bytes[kind_offset] = std::byte{200};
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}),
                  ErrorCode::StoreCorrupt);
  bytes[kind_offset] = std::byte{1};
  FDEP_REQUIRE_OK(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}));

  bytes[kind_offset + 1] = std::byte{0};  // strength
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}),
                  ErrorCode::StoreCorrupt);
  bytes[kind_offset + 1] = std::byte{1};
  bytes[kind_offset + 2] = std::byte{9};  // direction
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}),
                  ErrorCode::StoreCorrupt);
  bytes[kind_offset + 2] = std::byte{1};
  bytes[kind_offset + 3] = std::byte{9};  // lifecycle
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}),
                  ErrorCode::StoreCorrupt);
  bytes[kind_offset + 3] = std::byte{2};
  FDEP_REQUIRE_OK(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}));
}

FDEP_TEST(codec, a_node_domain_outside_the_domain_is_rejected) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  std::vector<std::byte> bytes = encode_snapshot(harness.snapshot());
  const std::size_t edge_start = 4 + 1 + 8 + 8 + 4 + 4;
  const std::size_t source_domain_offset = edge_start + 20;
  bytes[source_domain_offset] = std::byte{0};
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}),
                  ErrorCode::StoreCorrupt);
  bytes[source_domain_offset] = std::byte{9};
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}),
                  ErrorCode::StoreCorrupt);
  bytes[source_domain_offset] = std::byte{1};
  FDEP_REQUIRE_OK(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}));
}

FDEP_TEST(codec, an_identifier_longer_than_the_limit_is_rejected) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  std::vector<std::byte> bytes = encode_snapshot(harness.snapshot());

  RegistryLimits tight;
  tight.max_id_length = 0;  // rejected as incoherent
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, tight),
                  ErrorCode::InvalidLimits);

  RegistryLimits smaller;
  smaller.max_id_length = 5;
  // Everything in this payload, including its provenance, fits inside the
  // tighter bound.
  Harness single = Harness::ephemeral();
  single.add(fdep_test::make_spec(asset("a"), feed("f"), DependencyKind::RequiresPowerFrom,
                                  DependencyStrength::Hard, Direction::DependsOn, LifecycleState::Active, {},
                                  fdep_test::provenance("src", "me", 0, "")));
  const std::vector<std::byte> single_bytes = encode_snapshot(single.snapshot());
  FDEP_REQUIRE_OK(RegistrySnapshot::decode(std::span<const std::byte>{single_bytes}, smaller));

  Harness longer = Harness::ephemeral();
  longer.add(fdep_test::make_spec(asset("abcdefghij"), feed("f"), DependencyKind::RequiresPowerFrom,
                                  DependencyStrength::Hard, Direction::DependsOn, LifecycleState::Active, {},
                                  fdep_test::provenance("src", "me", 0, "")));
  const std::vector<std::byte> longer_bytes = encode_snapshot(longer.snapshot());
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{longer_bytes}, smaller),
                  ErrorCode::PayloadTooLarge);
}

FDEP_TEST(codec, a_payload_that_violates_a_semantic_rule_is_rejected) {
  // A payload can carry a self relation only if it was written by something
  // other than this library. The decoder re-runs the same validation a
  // mutation runs, so such a payload is refused.
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  std::vector<std::byte> bytes = encode_snapshot(harness.snapshot());
  const std::size_t edge_start = 4 + 1 + 8 + 8 + 4 + 4;
  const std::size_t target_domain_offset = edge_start + 20 + 1 + 4 + 1;  // after the source record
  // Turn the target into the same asset as the source: source is asset:a, so
  // make the target domain Asset as well.
  bytes[target_domain_offset] = std::byte{1};
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}),
                  ErrorCode::StoreCorrupt);
}

FDEP_TEST(codec, decoding_never_reads_past_the_buffer) {
  // A payload whose declared lengths are huge must be refused without the
  // decoder believing the declaration.
  std::vector<std::byte> bytes;
  const auto push_u32 = [&bytes](std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
      bytes.push_back(static_cast<std::byte>((value >> (8u * index)) & 0xFFu));
    }
  };
  const auto push_u64 = [&bytes](std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
      bytes.push_back(static_cast<std::byte>((value >> (8u * index)) & 0xFFu));
    }
  };
  push_u32(kStateFormatVersion);
  bytes.push_back(std::byte{1});
  push_u64(1);
  push_u64(2);
  push_u32(UINT32_MAX);  // declaration count, far above the configured maximum
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}),
                  ErrorCode::PayloadTooLarge);

  bytes.clear();
  push_u32(kStateFormatVersion);
  bytes.push_back(std::byte{1});
  push_u64(1);
  push_u64(2);
  push_u32(0);           // no declarations
  push_u32(UINT32_MAX);  // edge count, far above the configured maximum
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}),
                  ErrorCode::PayloadTooLarge);

  // A declaration whose identifier length is enormous. The count fits inside
  // the payload, so the length bound is what refuses it, before any string is
  // allocated for it.
  bytes.clear();
  push_u32(kStateFormatVersion);
  bytes.push_back(std::byte{1});
  push_u64(1);
  push_u64(2);
  push_u32(1);
  bytes.push_back(std::byte{1});  // asset
  push_u32(UINT32_MAX);           // identifier length
  bytes.resize(bytes.size() + 256, std::byte{0});
  FDEP_CHECK_CODE(RegistrySnapshot::decode(std::span<const std::byte>{bytes}, RegistryLimits{}),
                  ErrorCode::PayloadTooLarge);
}
