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

#include "sha256.hpp"

#include <cstring>

namespace facility_dependency_registry {
namespace detail {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{{
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
}};

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned count) noexcept {
  return (value >> count) | (value << (32u - count));
}

void store_be32(std::uint8_t* destination, std::uint32_t value) noexcept {
  destination[0] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
  destination[1] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
  destination[2] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
  destination[3] = static_cast<std::uint8_t>(value & 0xFFu);
}

}  // namespace

Sha256::Sha256() noexcept
    : state_{0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au, 0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu,
             0x5BE0CD19u} {}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16; ++index) {
    const std::size_t base = index * 4;
    schedule[index] = (static_cast<std::uint32_t>(block[base]) << 24) |
                      (static_cast<std::uint32_t>(block[base + 1]) << 16) |
                      (static_cast<std::uint32_t>(block[base + 2]) << 8) |
                      static_cast<std::uint32_t>(block[base + 3]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t previous = schedule[index - 15];
    const std::uint32_t recent = schedule[index - 2];
    const std::uint32_t sigma0 = rotate_right(previous, 7) ^ rotate_right(previous, 18) ^ (previous >> 3);
    const std::uint32_t sigma1 = rotate_right(recent, 17) ^ rotate_right(recent, 19) ^ (recent >> 10);
    schedule[index] = schedule[index - 16] + sigma0 + schedule[index - 7] + sigma1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t big_sigma1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + big_sigma1 + choose + kRoundConstants[index] + schedule[index];
    const std::uint32_t big_sigma0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = big_sigma0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::span<const std::byte> bytes) noexcept {
  if (finished_ || bytes.empty()) {
    return;
  }
  total_bytes_ += bytes.size();
  const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
  std::size_t remaining = bytes.size();
  std::size_t offset = 0;

  if (buffered_ > 0) {
    while (remaining > 0 && buffered_ < buffer_.size()) {
      buffer_[buffered_++] = data[offset++];
      --remaining;
    }
    if (buffered_ == buffer_.size()) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }

  while (remaining >= buffer_.size()) {
    compress(data + offset);
    offset += buffer_.size();
    remaining -= buffer_.size();
  }

  while (remaining > 0) {
    buffer_[buffered_++] = data[offset++];
    --remaining;
  }
}

void Sha256::update(std::string_view text) noexcept {
  update(std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()), text.size()});
}

ContentDigest Sha256::finish() noexcept {
  if (finished_) {
    return digest_;
  }
  const std::uint64_t bit_length = total_bytes_ * 8u;

  const std::uint8_t terminator = 0x80u;
  update(std::span<const std::byte>{reinterpret_cast<const std::byte*>(&terminator), 1});
  const std::uint8_t zero = 0x00u;
  while (buffered_ != 56) {
    update(std::span<const std::byte>{reinterpret_cast<const std::byte*>(&zero), 1});
  }
  std::uint8_t length_bytes[8]{};
  for (std::size_t index = 0; index < 8; ++index) {
    length_bytes[index] = static_cast<std::uint8_t>((bit_length >> (56u - 8u * index)) & 0xFFu);
  }
  update(std::span<const std::byte>{reinterpret_cast<const std::byte*>(length_bytes), 8});

  std::array<std::byte, ContentDigest::byte_size> bytes{};
  for (std::size_t index = 0; index < 8; ++index) {
    std::uint8_t word[4]{};
    store_be32(word, state_[index]);
    for (std::size_t byte_index = 0; byte_index < 4; ++byte_index) {
      bytes[index * 4 + byte_index] = static_cast<std::byte>(word[byte_index]);
    }
  }
  digest_ = ContentDigest::from_bytes(bytes);
  finished_ = true;
  return digest_;
}

}  // namespace detail
}  // namespace facility_dependency_registry
