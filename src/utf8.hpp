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

#ifndef FACILITY_DEPENDENCY_REGISTRY_SRC_UTF8_HPP
#define FACILITY_DEPENDENCY_REGISTRY_SRC_UTF8_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace facility_dependency_registry {
namespace detail {

/// Decodes the code point starting at `index`, advancing `index` past it.
/// Returns false for any malformed sequence: a truncated sequence, a stray
/// continuation byte, an overlong encoding, a UTF-16 surrogate, or a code
/// point above U+10FFFF. `index` is left untouched on failure.
[[nodiscard]] bool decode_utf8(std::string_view text, std::size_t& index, std::uint32_t& code_point) noexcept;

/// True for code points that may not appear in stored text: C0 controls
/// (including NUL, tab, carriage return and line feed), DEL, and the C1 range.
[[nodiscard]] bool is_forbidden_text_code_point(std::uint32_t code_point) noexcept;

}  // namespace detail
}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_SRC_UTF8_HPP
