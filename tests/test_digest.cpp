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

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "facility_dependency_registry/facility_dependency_registry.hpp"
#include "test_harness.hpp"

using namespace facility_dependency_registry;

namespace {

std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const char character : text) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
  return bytes;
}

}  // namespace

// The published test vectors of FIPS 180-4 and of the standard CRC-32 check
// value. These are the reason the digest code can be trusted without a third
// party library.
FDEP_TEST(digest, sha256_matches_the_published_vectors) {
  FDEP_CHECK_EQ(sha256(std::string_view{}).to_hex(),
                std::string{"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"});
  FDEP_CHECK_EQ(sha256(std::string_view{"abc"}).to_hex(),
                std::string{"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
  FDEP_CHECK_EQ(sha256(std::string_view{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"}).to_hex(),
                std::string{"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"});
  FDEP_CHECK_EQ(
      sha256(std::string_view{"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"})
          .to_hex(),
      std::string{"cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"});

  // One million 'a': the longest published vector, which exercises the
  // multi-block buffering path rather than a single padded block.
  std::string million(1'000'000, 'a');
  FDEP_CHECK_EQ(sha256(std::string_view{million}).to_hex(),
                std::string{"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"});
}

FDEP_TEST(digest, sha256_accepts_byte_spans) {
  const std::vector<std::byte> bytes = bytes_of("abc");
  FDEP_CHECK_EQ(sha256(std::span<const std::byte>{bytes}).to_hex(),
                std::string{"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
}

FDEP_TEST(digest, crc32_matches_the_standard_check_value) {
  FDEP_CHECK_EQ(crc32(std::string_view{}), 0u);
  FDEP_CHECK_EQ(crc32(std::string_view{"123456789"}), 0xCBF43926u);
  FDEP_CHECK_EQ(crc32(std::string_view{"The quick brown fox jumps over the lazy dog"}), 0x414FA339u);
  FDEP_CHECK_EQ(crc32(std::string_view{"a"}), 0xE8B7BE43u);
  const std::vector<std::byte> bytes = bytes_of("123456789");
  FDEP_CHECK_EQ(crc32(std::span<const std::byte>{bytes}), 0xCBF43926u);
}

FDEP_TEST(digest, content_digest_text_round_trips) {
  const ContentDigest digest = sha256(std::string_view{"abc"});
  const std::string hex = digest.to_hex();
  FDEP_CHECK_EQ(hex.size(), std::size_t{64});
  const auto parsed = ContentDigest::from_hex(hex);
  FDEP_REQUIRE_OK(parsed);
  FDEP_CHECK(parsed.value() == digest);
  FDEP_CHECK(!digest.is_zero());
  FDEP_CHECK(ContentDigest{}.is_zero());
  FDEP_CHECK(ContentDigest{} != digest);
  FDEP_CHECK(ContentDigest{} < digest);
}

FDEP_TEST(digest, hex_parsing_is_strict) {
  const ContentDigest zero{};
  FDEP_CHECK_EQ(zero.to_hex(), std::string(64, '0'));
  FDEP_CHECK_CODE(ContentDigest::from_hex(""), ErrorCode::InvalidArguments);
  FDEP_CHECK_CODE(ContentDigest::from_hex(std::string(63, '0')), ErrorCode::InvalidArguments);
  FDEP_CHECK_CODE(ContentDigest::from_hex(std::string(65, '0')), ErrorCode::InvalidArguments);
  FDEP_CHECK_CODE(ContentDigest::from_hex(std::string(64, 'A')), ErrorCode::InvalidArguments);
  FDEP_CHECK_CODE(ContentDigest::from_hex(std::string(64, 'z')), ErrorCode::InvalidArguments);
  FDEP_CHECK_CODE(ContentDigest::from_hex("0x" + std::string(62, '0')), ErrorCode::InvalidArguments);
  FDEP_REQUIRE_OK(ContentDigest::from_hex(std::string(64, 'f')));
  FDEP_REQUIRE_OK(ContentDigest::from_hex(std::string(64, '0')));
}

FDEP_TEST(digest, from_bytes_is_the_inverse_of_bytes) {
  std::array<std::byte, ContentDigest::byte_size> raw{};
  for (std::size_t index = 0; index < raw.size(); ++index) {
    raw[index] = static_cast<std::byte>(index);
  }
  const ContentDigest digest = ContentDigest::from_bytes(raw);
  FDEP_CHECK(digest.bytes() == raw);
  FDEP_CHECK_EQ(digest.to_hex(), std::string{"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"});
}
