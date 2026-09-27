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

#include "facility_dependency_registry/digest.hpp"

#include <string>

#include "sha256.hpp"

namespace facility_dependency_registry {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

int hex_value(char character) noexcept {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  return -1;
}

}  // namespace

ContentDigest ContentDigest::from_bytes(const std::array<std::byte, byte_size>& bytes) noexcept {
  ContentDigest digest;
  digest.bytes_ = bytes;
  return digest;
}

Result<ContentDigest> ContentDigest::from_hex(std::string_view hex) {
  if (hex.size() != byte_size * 2) {
    return Result<ContentDigest>::failure(ErrorCode::InvalidArguments,
                                          "a content digest is exactly 64 lower-case hexadecimal digits");
  }
  std::array<std::byte, byte_size> bytes{};
  for (std::size_t index = 0; index < byte_size; ++index) {
    const int high = hex_value(hex[index * 2]);
    const int low = hex_value(hex[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return Result<ContentDigest>::failure(ErrorCode::InvalidArguments,
                                            "a content digest contains a character that is not a lower-case "
                                            "hexadecimal digit");
    }
    bytes[index] = static_cast<std::byte>((high << 4) | low);
  }
  return ContentDigest::from_bytes(bytes);
}

std::string ContentDigest::to_hex() const {
  std::string result;
  result.resize(byte_size * 2);
  for (std::size_t index = 0; index < byte_size; ++index) {
    const auto value = static_cast<unsigned>(bytes_[index]);
    result[index * 2] = kHexDigits[(value >> 4) & 0x0Fu];
    result[index * 2 + 1] = kHexDigits[value & 0x0Fu];
  }
  return result;
}

bool ContentDigest::is_zero() const noexcept {
  for (const auto byte : bytes_) {
    if (byte != std::byte{0}) {
      return false;
    }
  }
  return true;
}

ContentDigest sha256(std::span<const std::byte> bytes) noexcept {
  detail::Sha256 hasher;
  hasher.update(bytes);
  return hasher.finish();
}

ContentDigest sha256(std::string_view text) noexcept {
  detail::Sha256 hasher;
  hasher.update(text);
  return hasher.finish();
}

}  // namespace facility_dependency_registry
