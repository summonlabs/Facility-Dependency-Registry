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
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "facility_dependency_registry/facility_dependency_registry.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

using namespace facility_dependency_registry;
using fdep_test::asset;
using fdep_test::feed;
using fdep_test::Harness;

namespace {

std::string generation_name(std::uint64_t generation) {
  std::string digits = std::to_string(generation);
  std::string name{"gen-"};
  name.append(20 - digits.size(), '0');
  name.append(digits);
  name.append(".fdepstate");
  return name;
}

std::filesystem::path gen_path(const std::filesystem::path& root, std::uint64_t generation) {
  return root / std::filesystem::path{generation_name(generation)};
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
  std::ifstream stream{path, std::ios::binary};
  std::vector<std::byte> bytes;
  char buffer[4096];
  while (stream.read(buffer, sizeof(buffer)) || stream.gcount() > 0) {
    const auto count = stream.gcount();
    for (std::streamsize index = 0; index < count; ++index) {
      bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(buffer[index])));
    }
  }
  return bytes;
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  if (!bytes.empty()) {
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
}

void write_text(const std::filesystem::path& path, const std::string& text) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  stream << text;
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream stream{path, std::ios::binary};
  return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void poke(std::vector<std::byte>& bytes, std::size_t offset, std::uint8_t value) {
  if (offset < bytes.size()) {
    bytes[offset] = static_cast<std::byte>(value);
  }
}

void poke_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned index = 0; index < 4; ++index) {
    poke(bytes, offset + index, static_cast<std::uint8_t>((value >> (8u * index)) & 0xFFu));
  }
}

void poke_u64(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value) {
  for (unsigned index = 0; index < 8; ++index) {
    poke(bytes, offset + index, static_cast<std::uint8_t>((value >> (8u * index)) & 0xFFu));
  }
}

/// A store with two edges at generation two, and the raw bytes of its newest
/// generation file.
struct BuiltStore {
  std::filesystem::path root;
  std::vector<std::byte> newest;
};

BuiltStore build_store(std::string_view label) {
  BuiltStore built;
  built.root = fdep_test::make_temp_directory(label);
  Harness harness = Harness::durable(built.root);
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  harness.add(asset("b"), feed("f1"), DependencyKind::RequiresPowerFrom);
  if (!harness.registry().close().ok()) {
    fdep_test::fail_now("could not close the store");
  }
  built.newest = read_bytes(gen_path(built.root, 2));
  return built;
}

/// Removes the pointer and reopens, so the store has to recover through its own
/// scan of the generation files. The newest intact generation is one, so a
/// damaged generation two must be refused and generation one loaded.
void expect_rejected_newest(const BuiltStore& built, std::string_view what) {
  std::error_code error;
  std::filesystem::remove(built.root / "CURRENT", error);
  Harness harness = Harness::durable(built.root);
  const RecoveryReport& report = harness.registry().recovery_report();
  if (report.outcome != RecoveryOutcome::LoadedFallback) {
    fdep_test::fail_now(std::string{"expected a fallback for "} + std::string{what});
  }
  FDEP_CHECK_EQ(report.loaded_generation.value(), std::uint64_t{1});
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{1});
  FDEP_CHECK(std::find(report.rejected_generations.begin(), report.rejected_generations.end(),
                       DependencyGeneration::from_value(2)) != report.rejected_generations.end());
  if (!harness.registry().close().ok()) {
    fdep_test::fail_now("could not close the store");
  }
}

}  // namespace

FDEP_TEST(corruption, a_damaged_container_header_is_refused) {
  const BuiltStore built = build_store("corrupt-magic");
  std::vector<std::byte> bytes = built.newest;

  poke(bytes, 0, static_cast<std::uint8_t>('X'));
  write_bytes(gen_path(built.root, 2), bytes);
  expect_rejected_newest(built, "a wrong magic");

  bytes = built.newest;
  poke_u32(bytes, 8, kContainerFormatVersion + 1);
  write_bytes(gen_path(built.root, 2), bytes);
  expect_rejected_newest(built, "an unsupported container version");

  bytes = built.newest;
  poke_u32(bytes, 8, 0);
  write_bytes(gen_path(built.root, 2), bytes);
  expect_rejected_newest(built, "container version zero");

  bytes = built.newest;
  poke_u32(bytes, 12, 1);
  write_bytes(gen_path(built.root, 2), bytes);
  expect_rejected_newest(built, "unknown flags");

  bytes = built.newest;
  poke_u64(bytes, 16, static_cast<std::uint64_t>(bytes.size()));
  write_bytes(gen_path(built.root, 2), bytes);
  expect_rejected_newest(built, "a payload length that does not match the file");

  bytes = built.newest;
  poke_u32(bytes, 24, 0x12345678u);
  write_bytes(gen_path(built.root, 2), bytes);
  expect_rejected_newest(built, "a wrong CRC-32");

  // A digest field that no longer matches the payload, with the CRC still
  // correct: the stronger check is the one that catches it.
  bytes = built.newest;
  poke(bytes, 32, static_cast<std::uint8_t>(static_cast<unsigned char>(bytes[32]) ^ 0x01));
  write_bytes(gen_path(built.root, 2), bytes);
  expect_rejected_newest(built, "a wrong SHA-256");

  bytes = built.newest;
  bytes.resize(bytes.size() - 4);
  write_bytes(gen_path(built.root, 2), bytes);
  expect_rejected_newest(built, "a truncated file");

  bytes = built.newest;
  bytes.resize(bytes.size() + 16, std::byte{0});
  write_bytes(gen_path(built.root, 2), bytes);
  expect_rejected_newest(built, "a file with trailing bytes");

  write_bytes(gen_path(built.root, 2), {});
  expect_rejected_newest(built, "an empty file");

  bytes = built.newest;
  bytes.resize(10);
  write_bytes(gen_path(built.root, 2), bytes);
  expect_rejected_newest(built, "a file shorter than its header");

  fdep_test::remove_tree(built.root);
}

FDEP_TEST(corruption, a_flipped_payload_byte_is_caught_by_both_checks) {
  const BuiltStore built = build_store("corrupt-payload");
  std::vector<std::byte> bytes = built.newest;
  FDEP_REQUIRE(bytes.size() > kContainerHeaderSize + 8);
  const std::size_t offset = bytes.size() - 4;
  poke(bytes, offset, static_cast<std::uint8_t>(static_cast<unsigned char>(bytes[offset]) ^ 0x80));
  write_bytes(gen_path(built.root, 2), bytes);
  expect_rejected_newest(built, "a flipped payload byte");
  fdep_test::remove_tree(built.root);
}

FDEP_TEST(corruption, an_oversized_generation_file_is_refused_before_it_is_read) {
  const BuiltStore built = build_store("corrupt-oversized");
  RegistryLimits limits;
  limits.max_persisted_bytes = 4096;
  {
    std::ofstream stream{gen_path(built.root, 2), std::ios::binary | std::ios::trunc};
    const std::string filler(8192, 'x');
    stream << filler;
  }
  RegistryOpenRequest request;
  request.root = built.root;
  request.store.limits = limits;
  // The pointer names generation two, which cannot be read inside the bound, so
  // the store falls back to generation one and reports the rejection.
  auto registry = DependencyRegistry::open(request);
  FDEP_REQUIRE_OK(registry);
  FDEP_CHECK(registry.value().recovery_report().outcome == RecoveryOutcome::LoadedFallback);
  FDEP_CHECK_EQ(registry.value().generation().value(), std::uint64_t{1});
  bool mentions_size = false;
  for (const auto& note : registry.value().recovery_report().diagnostics) {
    if (note.find("configured maximum") != std::string::npos) {
      mentions_size = true;
    }
  }
  FDEP_CHECK(mentions_size);
  FDEP_REQUIRE(registry.value().close().ok());
  fdep_test::remove_tree(built.root);
}

FDEP_TEST(corruption, a_generation_file_under_the_wrong_name_is_refused) {
  const BuiltStore built = build_store("corrupt-misnamed");
  // The bytes of generation two, stored under the name of generation three,
  // with the pointer naming three. The name and the content must agree.
  write_bytes(gen_path(built.root, 3), built.newest);
  write_text(built.root / "CURRENT",
             "FDEPCUR1\ngeneration 3\nsha256 " + std::string(64, '0') + "\n");

  Harness harness = Harness::durable(built.root);
  FDEP_CHECK(harness.registry().recovery_report().outcome == RecoveryOutcome::LoadedFallback);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{2});
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{2});
  bool mentions_name = false;
  for (const auto& note : harness.registry().recovery_report().diagnostics) {
    if (note.find("under the name of generation") != std::string::npos) {
      mentions_name = true;
    }
  }
  FDEP_CHECK(mentions_name);
  FDEP_REQUIRE(harness.registry().close().ok());
  fdep_test::remove_tree(built.root);
}

FDEP_TEST(corruption, malformed_pointer_files_are_refused) {
  const BuiltStore built = build_store("corrupt-pointer");
  const std::string digest = std::string(64, '0');

  const std::string cases[] = {
      "",
      "FDEPCUR1\n",
      "WRONGMAGIC\ngeneration 2\nsha256 " + digest + "\n",
      "FDEPCUR1\ngeneration two\nsha256 " + digest + "\n",
      "FDEPCUR1\ngeneration 2\nsha256 zzzz\n",
      "FDEPCUR1\ngeneration 2\n",
      "FDEPCUR1\ngeneration 02\nsha256 " + digest + "\n",
      "FDEPCUR1\ngeneration 2\nsha256 " + digest + "\n\n",
      "FDEPCUR1\ngeneration 2\nsha256 " + digest,
  };
  for (const std::string& content : cases) {
    write_text(built.root / "CURRENT", content);
    Harness harness = Harness::durable(built.root);
    const RecoveryReport& report = harness.registry().recovery_report();
    FDEP_CHECK(report.outcome == RecoveryOutcome::LoadedFallback);
    FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{2});
    FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{2});
    if (!harness.registry().close().ok()) {
      fdep_test::fail_now("could not close the store");
    }
  }

  // A pointer whose digest does not describe the file it names is treated as
  // untrustworthy: the generation is still recovered, but through the scan, and
  // the open reports that the pointer was not believed.
  write_text(built.root / "CURRENT", "FDEPCUR1\ngeneration 2\nsha256 " + std::string(64, 'a') + "\n");
  Harness harness = Harness::durable(built.root);
  FDEP_CHECK(harness.registry().recovery_report().outcome == RecoveryOutcome::LoadedFallback);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{2});
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{2});
  bool mentions_digest = false;
  for (const auto& note : harness.registry().recovery_report().diagnostics) {
    if (note.find("does not describe generation") != std::string::npos) {
      mentions_digest = true;
    }
  }
  FDEP_CHECK(mentions_digest);
  FDEP_REQUIRE(harness.registry().close().ok());
  fdep_test::remove_tree(built.root);
}

FDEP_TEST(corruption, a_damaged_lock_record_is_a_diagnostic_not_a_failure) {
  const BuiltStore built = build_store("corrupt-lock");
  write_text(built.root / "writer.lock", std::string("\x01\x02\x03", 3) + "garbage");

  // Inspection reports no holder, because the record cannot be read. That is a
  // missing diagnostic, not a failure: the lock itself is what grants or
  // refuses authority, and the record is only there to say who holds it.
  {
    StoreOptions options;
    options.mode = OpenMode::ReadOnly;
    RegistrySnapshot snapshot;
    RecoveryReport report;
    auto handle = DurableStore::open(built.root, options, snapshot, report);
    FDEP_REQUIRE_OK(handle);
    const auto status = handle.value()->status();
    FDEP_REQUIRE_OK(status);
    FDEP_CHECK(!status.value().lock_incarnation.valid());
    FDEP_CHECK(!status.value().writer_lock_held);
    FDEP_CHECK_EQ(status.value().pointer_generation.value(), std::uint64_t{2});
    FDEP_REQUIRE(handle.value()->close().ok());
  }

  // A writer takes the store anyway and replaces the unreadable record with its
  // own, without ever treating the old bytes as authority.
  Harness harness = Harness::durable(built.root);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{2});
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{2});
  const auto status = harness.registry().store_status();
  FDEP_REQUIRE_OK(status);
  FDEP_CHECK(status.value().writer_lock_held);
  FDEP_CHECK(status.value().lock_incarnation.valid());
  FDEP_CHECK_EQ(status.value().lock_incarnation.process_id(),
                harness.registry().writer_incarnation().process_id());
  FDEP_REQUIRE(harness.registry().close().ok());
  fdep_test::remove_tree(built.root);
}

FDEP_TEST(corruption, a_directory_named_like_a_generation_is_ignored) {
  const BuiltStore built = build_store("corrupt-directory");
  std::error_code error;
  std::filesystem::create_directory(gen_path(built.root, 99), error);
  FDEP_REQUIRE(!error);

  Harness harness = Harness::durable(built.root);
  FDEP_CHECK(harness.registry().recovery_report().outcome == RecoveryOutcome::LoadedCurrent);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{2});
  const auto retained = harness.registry().store_status();
  FDEP_REQUIRE_OK(retained);
  FDEP_CHECK(std::find(retained.value().retained_generations.begin(),
                       retained.value().retained_generations.end(),
                       DependencyGeneration::from_value(99)) == retained.value().retained_generations.end());
  FDEP_REQUIRE(harness.registry().close().ok());
  fdep_test::remove_tree(built.root);
}

FDEP_TEST(corruption, a_generation_name_with_the_wrong_width_is_not_a_generation) {
  const BuiltStore built = build_store("corrupt-width");
  // The same generation, spelled with a different amount of padding, is not the
  // canonical name of anything and is ignored rather than guessed at.
  write_text(built.root / "gen-2.fdepstate", "junk");
  write_text(built.root / "gen-000000000000000000002.fdepstate", "junk");
  write_text(built.root / "gen-00000000000000000002.fdepstate.bak", "junk");

  Harness harness = Harness::durable(built.root);
  FDEP_CHECK(harness.registry().recovery_report().outcome == RecoveryOutcome::LoadedCurrent);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{2});
  const auto retained = harness.registry().store_status();
  FDEP_REQUIRE_OK(retained);
  FDEP_CHECK_EQ(retained.value().retained_generations.size(), std::size_t{2});
  FDEP_REQUIRE(harness.registry().close().ok());
  fdep_test::remove_tree(built.root);
}

FDEP_TEST(corruption, every_sampled_byte_of_the_newest_generation_can_be_damaged_safely) {
  // A systematic sweep: damage one byte at a time across the whole file. The
  // store must come back with one of the two states that were actually
  // written, never with a third state that no publication produced.
  const BuiltStore built = build_store("corrupt-sweep");
  const std::vector<std::byte> original = built.newest;
  const std::size_t stride = std::max<std::size_t>(original.size() / 64, 1);
  int checked = 0;
  for (std::size_t offset = 0; offset < original.size(); offset += stride) {
    std::vector<std::byte> bytes = original;
    poke(bytes, offset, static_cast<std::uint8_t>(static_cast<unsigned char>(bytes[offset]) ^ 0xFF));
    write_bytes(gen_path(built.root, 2), bytes);
    std::error_code error;
    std::filesystem::remove(built.root / "CURRENT", error);

    RegistryOpenRequest request;
    request.root = built.root;
    request.clock = std::make_shared<FixedClock>(1'700'000'000'000);
    auto registry = DependencyRegistry::open(request);
    if (!registry) {
      // Generation one is intact, so the store as a whole is never unreadable.
      fdep_test::fail_now("a damaged generation made the whole store unreadable at offset " +
                          std::to_string(offset));
    }
    const auto generation = registry.value().generation();
    if (generation.value() == 2) {
      FDEP_CHECK_EQ(registry.value().snapshot().edge_count(), std::size_t{2});
    } else {
      FDEP_CHECK_EQ(generation.value(), std::uint64_t{1});
      FDEP_CHECK_EQ(registry.value().snapshot().edge_count(), std::size_t{1});
    }
    if (!registry.value().close().ok()) {
      fdep_test::fail_now("could not close the store");
    }
    ++checked;
    write_bytes(gen_path(built.root, 2), original);
  }
  FDEP_CHECK(checked > 16);
  fdep_test::remove_tree(built.root);
}
