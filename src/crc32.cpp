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
#include <cstdint>
#include <span>

#include "facility_dependency_registry/digest.hpp"

namespace facility_dependency_registry {
namespace {

constexpr std::uint32_t kPolynomial = 0xEDB88320u;

constexpr std::array<std::uint32_t, 256> make_crc_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256u; ++index) {
    std::uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1u) != 0u ? (kPolynomial ^ (value >> 1)) : (value >> 1);
    }
    table[index] = value;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrcTable = make_crc_table();

std::uint32_t crc32_bytes(std::span<const std::byte> bytes) noexcept {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const auto byte : bytes) {
    const auto index = static_cast<std::uint8_t>(crc ^ static_cast<std::uint32_t>(byte));
    crc = kCrcTable[index] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace

std::uint32_t crc32(std::span<const std::byte> bytes) noexcept { return crc32_bytes(bytes); }

std::uint32_t crc32(std::string_view text) noexcept {
  return crc32_bytes(std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()), text.size()});
}

}  // namespace facility_dependency_registry
