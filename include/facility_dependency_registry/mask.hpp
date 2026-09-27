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

#ifndef FACILITY_DEPENDENCY_REGISTRY_MASK_HPP
#define FACILITY_DEPENDENCY_REGISTRY_MASK_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace facility_dependency_registry {

/// A small, strongly typed set of enumeration values.
///
/// `Enum` must be a scoped enumeration whose underlying values are contiguous
/// and start at 1; bit `n` of the mask represents the value `n + 1`. Enums used
/// with this template are documented as such, and every one of them reserves
/// zero as "not a value" so that a zero-initialized field can never be mistaken
/// for a member of the domain.
template <class Enum, std::size_t Bits>
class EnumMask {
  static_assert(Bits >= 1, "EnumMask needs at least one bit");
  static_assert(Bits <= 32, "EnumMask supports up to 32 values");

 public:
  using enum_type = Enum;
  using bits_type = std::uint32_t;

  constexpr EnumMask() noexcept = default;

  [[nodiscard]] static constexpr EnumMask none() noexcept { return EnumMask{}; }

  [[nodiscard]] static constexpr EnumMask all() noexcept {
    return EnumMask{Bits == 32 ? 0xFFFFFFFFu : ((1u << Bits) - 1u)};
  }

  [[nodiscard]] static constexpr EnumMask of(Enum value) noexcept { return EnumMask{bit_of(value)}; }

  [[nodiscard]] static constexpr EnumMask from_bits(bits_type bits) noexcept {
    return EnumMask{bits & all().bits_};
  }

  [[nodiscard]] constexpr bits_type bits() const noexcept { return bits_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }
  [[nodiscard]] constexpr bool contains(Enum value) const noexcept { return (bits_ & bit_of(value)) != 0; }
  [[nodiscard]] constexpr bool contains_all(const EnumMask& other) const noexcept {
    return (bits_ & other.bits_) == other.bits_;
  }
  [[nodiscard]] constexpr bool contains_any(const EnumMask& other) const noexcept {
    return (bits_ & other.bits_) != 0;
  }
  [[nodiscard]] constexpr std::size_t count() const noexcept {
    std::size_t total = 0;
    for (std::size_t index = 0; index < Bits; ++index) {
      if ((bits_ & (static_cast<bits_type>(1u) << index)) != 0) {
        ++total;
      }
    }
    return total;
  }

  constexpr EnumMask with(Enum value) const noexcept { return EnumMask{bits_ | bit_of(value)}; }
  constexpr EnumMask without(Enum value) const noexcept { return EnumMask{bits_ & ~bit_of(value)}; }

  friend constexpr bool operator==(const EnumMask& lhs, const EnumMask& rhs) noexcept {
    return lhs.bits_ == rhs.bits_;
  }
  friend constexpr bool operator!=(const EnumMask& lhs, const EnumMask& rhs) noexcept {
    return !(lhs == rhs);
  }
  friend constexpr EnumMask operator|(const EnumMask& lhs, const EnumMask& rhs) noexcept {
    return EnumMask{lhs.bits_ | rhs.bits_};
  }
  friend constexpr EnumMask operator&(const EnumMask& lhs, const EnumMask& rhs) noexcept {
    return EnumMask{lhs.bits_ & rhs.bits_};
  }

  /// Calls `fn(Enum)` for every contained value, in ascending value order.
  template <class Fn>
  void for_each(Fn&& fn) const {
    for (std::size_t index = 0; index < Bits; ++index) {
      if ((bits_ & (static_cast<bits_type>(1u) << index)) != 0) {
        fn(static_cast<Enum>(index + 1));
      }
    }
  }

 private:
  explicit constexpr EnumMask(bits_type bits) noexcept : bits_{bits} {}

  [[nodiscard]] static constexpr bits_type bit_of(Enum value) noexcept {
    const auto ordinal = static_cast<bits_type>(value);
    if (ordinal == 0 || ordinal > Bits) {
      return 0;
    }
    return static_cast<bits_type>(1u) << (ordinal - 1);
  }

  bits_type bits_{0};
};

/// Renders a mask as a comma separated list of tokens in ascending value
/// order, using `token_fn` for each member. An empty mask renders as "-".
template <class Enum, std::size_t Bits>
[[nodiscard]] std::string to_token_list(const EnumMask<Enum, Bits>& mask, std::string_view (*token_fn)(Enum)) {
  std::string result;
  mask.for_each([&](Enum value) {
    if (!result.empty()) {
      result.push_back(',');
    }
    result.append(token_fn(value));
  });
  if (result.empty()) {
    result.push_back('-');
  }
  return result;
}

}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_MASK_HPP
