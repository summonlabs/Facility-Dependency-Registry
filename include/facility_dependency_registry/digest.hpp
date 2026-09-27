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

#ifndef FACILITY_DEPENDENCY_REGISTRY_DIGEST_HPP
#define FACILITY_DEPENDENCY_REGISTRY_DIGEST_HPP

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "facility_dependency_registry/errors.hpp"

namespace facility_dependency_registry {

/// A 256-bit content digest.
///
/// The digest is SHA-256 over a canonical byte sequence. Two states have equal
/// digests exactly when their canonical encodings are byte identical, which is
/// the definition of "the same authoritative state" used throughout this
/// repository.
class ContentDigest {
 public:
  static constexpr std::size_t byte_size = 32;

  /// The all-zero digest. A zero digest is never produced by hashing: it means
  /// "not computed".
  ContentDigest() = default;

  [[nodiscard]] static ContentDigest from_bytes(const std::array<std::byte, byte_size>& bytes) noexcept;

  /// Parses exactly 64 lower-case hexadecimal digits. Upper case, a "0x"
  /// prefix, whitespace and any other length are rejected.
  [[nodiscard]] static Result<ContentDigest> from_hex(std::string_view hex);

  [[nodiscard]] const std::array<std::byte, byte_size>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::string to_hex() const;
  [[nodiscard]] bool is_zero() const noexcept;

  friend bool operator==(const ContentDigest& lhs, const ContentDigest& rhs) noexcept {
    return lhs.bytes_ == rhs.bytes_;
  }
  friend bool operator!=(const ContentDigest& lhs, const ContentDigest& rhs) noexcept { return !(lhs == rhs); }
  friend std::strong_ordering operator<=>(const ContentDigest& lhs, const ContentDigest& rhs) noexcept {
    return lhs.bytes_ <=> rhs.bytes_;
  }

 private:
  std::array<std::byte, byte_size> bytes_{};
};

/// SHA-256 (FIPS 180-4) of a byte sequence.
[[nodiscard]] ContentDigest sha256(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] ContentDigest sha256(std::string_view text) noexcept;

/// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320) of a byte sequence.
/// Used as a cheap first integrity check in front of SHA-256, never as the
/// only one.
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] std::uint32_t crc32(std::string_view text) noexcept;

}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_DIGEST_HPP
