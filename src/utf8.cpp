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

#include "utf8.hpp"

#include <string>

#include "facility_dependency_registry/provenance.hpp"

namespace facility_dependency_registry {
namespace detail {
namespace {

constexpr std::uint32_t kMaxCodePoint = 0x10FFFF;
constexpr std::uint32_t kSurrogateFirst = 0xD800;
constexpr std::uint32_t kSurrogateLast = 0xDFFF;

}  // namespace

bool decode_utf8(std::string_view text, std::size_t& index, std::uint32_t& code_point) noexcept {
  if (index >= text.size()) {
    return false;
  }
  const auto first = static_cast<std::uint8_t>(text[index]);
  std::size_t length = 0;
  std::uint32_t value = 0;
  std::uint32_t minimum = 0;

  if (first < 0x80) {
    code_point = first;
    ++index;
    return true;
  }
  if ((first & 0xE0) == 0xC0) {
    length = 2;
    value = first & 0x1Fu;
    minimum = 0x80;
  } else if ((first & 0xF0) == 0xE0) {
    length = 3;
    value = first & 0x0Fu;
    minimum = 0x800;
  } else if ((first & 0xF8) == 0xF0) {
    length = 4;
    value = first & 0x07u;
    minimum = 0x10000;
  } else {
    // 0x80..0xBF is a stray continuation byte; 0xF8..0xFF is never valid.
    return false;
  }

  if (index + length > text.size()) {
    return false;
  }
  for (std::size_t offset = 1; offset < length; ++offset) {
    const auto byte = static_cast<std::uint8_t>(text[index + offset]);
    if ((byte & 0xC0) != 0x80) {
      return false;
    }
    value = (value << 6) | (byte & 0x3Fu);
  }

  if (value < minimum) {
    return false;  // overlong encoding
  }
  if (value > kMaxCodePoint) {
    return false;
  }
  if (value >= kSurrogateFirst && value <= kSurrogateLast) {
    return false;  // UTF-16 surrogate, never a code point
  }

  code_point = value;
  index += length;
  return true;
}

bool is_forbidden_text_code_point(std::uint32_t code_point) noexcept {
  if (code_point < 0x20) {
    return true;  // C0 controls, including NUL, tab, CR and LF
  }
  if (code_point == 0x7F) {
    return true;  // DEL
  }
  if (code_point >= 0x80 && code_point <= 0x9F) {
    return true;  // C1 controls
  }
  return false;
}

}  // namespace detail

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  std::uint32_t code_point = 0;
  while (index < text.size()) {
    if (!detail::decode_utf8(text, index, code_point)) {
      return false;
    }
  }
  return true;
}

Status validate_annotation(std::string_view text, std::size_t max_length) {
  if (text.size() > max_length) {
    return Status::failure(ErrorCode::InvalidAnnotation, "annotation is longer than the configured maximum");
  }
  std::size_t index = 0;
  std::uint32_t code_point = 0;
  while (index < text.size()) {
    if (!detail::decode_utf8(text, index, code_point)) {
      return Status::failure(ErrorCode::InvalidAnnotation, "annotation is not well formed UTF-8");
    }
    if (detail::is_forbidden_text_code_point(code_point)) {
      return Status::failure(ErrorCode::InvalidAnnotation,
                             "annotation contains a control character, which this registry never stores");
    }
  }
  return Status::success();
}

}  // namespace facility_dependency_registry
