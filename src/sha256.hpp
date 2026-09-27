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

#ifndef FACILITY_DEPENDENCY_REGISTRY_SRC_SHA256_HPP
#define FACILITY_DEPENDENCY_REGISTRY_SRC_SHA256_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "facility_dependency_registry/digest.hpp"

namespace facility_dependency_registry {
namespace detail {

/// Incremental SHA-256 (FIPS 180-4).
///
/// This exists so that a large durable payload can be hashed while it is being
/// written and verified without holding a second copy of it in memory.
class Sha256 {
 public:
  Sha256() noexcept;

  void update(std::span<const std::byte> bytes) noexcept;
  void update(std::string_view text) noexcept;

  /// Finalizes and returns the digest. The instance must not be updated after
  /// this call; a second call returns the same digest.
  [[nodiscard]] ContentDigest finish() noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_;
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
  ContentDigest digest_{};
  bool finished_ = false;
};

}  // namespace detail
}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_SRC_SHA256_HPP
