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

#ifndef FACILITY_DEPENDENCY_REGISTRY_VERSION_HPP
#define FACILITY_DEPENDENCY_REGISTRY_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace facility_dependency_registry {

/// Semantic version of this library.
inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

/// "1.0.0"
[[nodiscard]] std::string_view version_string() noexcept;

/// The version of the canonical state payload encoding written by this build.
/// A reader accepts exactly this version and rejects anything else: an
/// unknown payload version is never guessed at.
inline constexpr std::uint32_t kStateFormatVersion = 1;

/// The version of the durable file container written by this build.
inline constexpr std::uint32_t kContainerFormatVersion = 1;

/// The version of the durable store directory layout (the `CURRENT` pointer
/// and the generation file naming scheme).
inline constexpr std::uint32_t kStoreLayoutVersion = 1;

/// Eight byte magic of a durable generation file: "FDEPSTA1".
inline constexpr std::string_view kContainerMagic = "FDEPSTA1";

/// Eight byte magic of the store pointer file: "FDEPCUR1".
inline constexpr std::string_view kPointerMagic = "FDEPCUR1";

/// File name of the store pointer inside a store root.
inline constexpr std::string_view kPointerFileName = "CURRENT";

/// File name of the writer lock inside a store root.
inline constexpr std::string_view kWriterLockFileName = "writer.lock";

/// Prefix and suffix of generation file names: gen-<20 digit generation>.fdepstate
inline constexpr std::string_view kGenerationFilePrefix = "gen-";
inline constexpr std::string_view kGenerationFileSuffix = ".fdepstate";

/// Prefix of transient publish files. These are never authoritative and are
/// removed, not interpreted, when a store is opened.
inline constexpr std::string_view kTransientFilePrefix = "tmp-";

/// Fixed size of a durable generation file header, in bytes.
inline constexpr std::uint64_t kContainerHeaderSize = 64;

}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_VERSION_HPP
