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

#ifndef FACILITY_DEPENDENCY_REGISTRY_SRC_FILE_OPS_HPP
#define FACILITY_DEPENDENCY_REGISTRY_SRC_FILE_OPS_HPP

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "facility_dependency_registry/errors.hpp"

namespace facility_dependency_registry {
namespace detail {

/// Reads a whole file after checking its size against `max_bytes`. The size
/// check happens before the buffer is reserved, so a file that claims to be
/// enormous cannot make this process allocate for it.
[[nodiscard]] Result<std::vector<std::byte>> read_file_bounded(const std::filesystem::path& path,
                                                               std::uint64_t max_bytes);

/// Creates a new file and writes `bytes` into it. The file must not already
/// exist: the create is exclusive, so a pre-existing file -- including one an
/// attacker placed in advance -- is never overwritten or followed.
[[nodiscard]] Status write_new_file(const std::filesystem::path& path, std::span<const std::byte> bytes);

/// Flushes a file's contents to the storage device.
[[nodiscard]] Status sync_file(const std::filesystem::path& path);

/// Atomically replaces `destination` with `source`.
///
/// On Windows this is MoveFileEx with MOVEFILE_REPLACE_EXISTING and
/// MOVEFILE_WRITE_THROUGH, which is atomic with respect to other readers and
/// does not return before the replacement has been flushed. On POSIX it is
/// rename(2) followed by a directory fsync.
[[nodiscard]] Status replace_file(const std::filesystem::path& source, const std::filesystem::path& destination);

/// Flushes a directory entry. On POSIX this is fsync on the directory. Windows
/// has no equivalent operation; there the durability of a replacement comes
/// from MOVEFILE_WRITE_THROUGH, and this call reports success without doing
/// anything. That difference is documented in persistence.hpp.
[[nodiscard]] Status sync_directory(const std::filesystem::path& path);

[[nodiscard]] Result<std::uint64_t> file_size(const std::filesystem::path& path);
[[nodiscard]] bool path_exists(const std::filesystem::path& path);
[[nodiscard]] Status remove_file(const std::filesystem::path& path);
[[nodiscard]] Status ensure_directory(const std::filesystem::path& path);

/// A short, unpredictable token used to name transient files. It is combined
/// with an exclusive create, so unpredictability is defence in depth rather
/// than the primary protection.
[[nodiscard]] std::string random_token();

/// The process identity, for diagnostics and for the writer incarnation.
[[nodiscard]] std::uint64_t current_process_id() noexcept;

[[nodiscard]] std::string to_utf8(const std::filesystem::path& path);

}  // namespace detail
}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_SRC_FILE_OPS_HPP
