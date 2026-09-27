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

#ifndef FACILITY_DEPENDENCY_REGISTRY_SRC_BYTE_CODEC_HPP
#define FACILITY_DEPENDENCY_REGISTRY_SRC_BYTE_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace facility_dependency_registry {
namespace detail {

/// Appends fixed width little-endian values and length prefixed byte strings.
///
/// Fixed width fields are used everywhere rather than a variable length
/// encoding: a decoder that can be asked to interpret a compact length is one
/// more place where a hostile file can make an allocation decision, and the
/// size difference does not matter for a registry.
class ByteWriter {
 public:
  void u8(std::uint8_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void raw(std::span<const std::byte> bytes);
  /// A u32 length followed by the bytes. The caller is responsible for having
  /// validated that the text fits the configured bound.
  void sized_string(std::string_view text);

  [[nodiscard]] const std::vector<std::byte>& data() const noexcept { return bytes_; }
  [[nodiscard]] std::vector<std::byte> take() && { return std::move(bytes_); }
  [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
  void reserve(std::size_t bytes) { bytes_.reserve(bytes); }
  void clear() noexcept { bytes_.clear(); }

 private:
  std::vector<std::byte> bytes_{};
};

/// Reads the encoding produced by ByteWriter.
///
/// The reader never throws and never allocates beyond a caller supplied bound.
/// It distinguishes "the input ended" from "the input asked for more than the
/// bound allows", because those are different rejections to report.
class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

  [[nodiscard]] bool u8(std::uint8_t& out) noexcept;
  [[nodiscard]] bool u32(std::uint32_t& out) noexcept;
  [[nodiscard]] bool u64(std::uint64_t& out) noexcept;
  [[nodiscard]] bool i64(std::int64_t& out) noexcept;

  /// Reads a u32 length and then that many bytes into `out`. Fails without
  /// allocating when the declared length exceeds `max_length`, and sets
  /// `limit_exceeded()` so the caller can report PayloadTooLarge rather than a
  /// truncation.
  [[nodiscard]] bool sized_string(std::size_t max_length, std::string& out);

  [[nodiscard]] bool at_end() const noexcept { return position_ == bytes_.size(); }
  [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - position_; }
  [[nodiscard]] std::size_t position() const noexcept { return position_; }
  [[nodiscard]] bool limit_exceeded() const noexcept { return limit_exceeded_; }

 private:
  [[nodiscard]] bool want(std::size_t count) noexcept;

  std::span<const std::byte> bytes_{};
  std::size_t position_ = 0;
  bool limit_exceeded_ = false;
};

}  // namespace detail
}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_SRC_BYTE_CODEC_HPP
