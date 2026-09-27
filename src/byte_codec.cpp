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

#include "byte_codec.hpp"

namespace facility_dependency_registry {
namespace detail {

void ByteWriter::u8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }

void ByteWriter::u32(std::uint32_t value) {
  for (unsigned shift = 0; shift < 32u; shift += 8u) {
    bytes_.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
  }
}

void ByteWriter::u64(std::uint64_t value) {
  for (unsigned shift = 0; shift < 64u; shift += 8u) {
    bytes_.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
  }
}

void ByteWriter::i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

void ByteWriter::raw(std::span<const std::byte> bytes) { bytes_.insert(bytes_.end(), bytes.begin(), bytes.end()); }

void ByteWriter::sized_string(std::string_view text) {
  u32(static_cast<std::uint32_t>(text.size()));
  const auto* data = reinterpret_cast<const std::byte*>(text.data());
  bytes_.insert(bytes_.end(), data, data + text.size());
}

bool ByteReader::want(std::size_t count) noexcept {
  if (count > bytes_.size() - position_) {
    return false;
  }
  return true;
}

bool ByteReader::u8(std::uint8_t& out) noexcept {
  if (!want(1)) {
    return false;
  }
  out = static_cast<std::uint8_t>(bytes_[position_]);
  ++position_;
  return true;
}

bool ByteReader::u32(std::uint32_t& out) noexcept {
  if (!want(4)) {
    return false;
  }
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes_[position_ + index])) << (8u * index);
  }
  out = value;
  position_ += 4;
  return true;
}

bool ByteReader::u64(std::uint64_t& out) noexcept {
  if (!want(8)) {
    return false;
  }
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(bytes_[position_ + index])) << (8u * index);
  }
  out = value;
  position_ += 8;
  return true;
}

bool ByteReader::i64(std::int64_t& out) noexcept {
  std::uint64_t value = 0;
  if (!u64(value)) {
    return false;
  }
  out = static_cast<std::int64_t>(value);
  return true;
}

bool ByteReader::sized_string(std::size_t max_length, std::string& out) {
  std::uint32_t length = 0;
  if (!u32(length)) {
    return false;
  }
  if (length > max_length) {
    limit_exceeded_ = true;
    return false;
  }
  if (!want(length)) {
    return false;
  }
  out.assign(reinterpret_cast<const char*>(bytes_.data() + position_), length);
  position_ += length;
  return true;
}

}  // namespace detail
}  // namespace facility_dependency_registry
