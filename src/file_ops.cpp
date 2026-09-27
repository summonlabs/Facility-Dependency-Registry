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

#include "file_ops.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <random>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace facility_dependency_registry {
namespace detail {
namespace {

std::atomic<std::uint64_t> g_token_counter{0};

Status io_failure(std::string_view action, const std::filesystem::path& path, std::string_view reason) {
  std::string detail{action};
  detail.append(" ");
  detail.append(to_utf8(path));
  detail.append(": ");
  detail.append(reason);
  return Status::failure(ErrorCode::StoreIoError, std::move(detail));
}

}  // namespace

std::string to_utf8(const std::filesystem::path& path) {
#if defined(_WIN32)
  const auto text = path.u8string();
  return std::string{reinterpret_cast<const char*>(text.data()), text.size()};
#else
  return path.string();
#endif
}

std::uint64_t current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

std::string random_token() {
  static thread_local std::mt19937_64 generator{std::random_device{}()};
  const std::uint64_t value = generator();
  const std::uint64_t counter = g_token_counter.fetch_add(1) + 1;
  std::string result;
  result.reserve(40);
  const auto append_hex = [&result](std::uint64_t number) {
    static constexpr char digits[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
      result.push_back(digits[(number >> shift) & 0x0Fu]);
    }
  };
  append_hex(value);
  result.push_back('-');
  append_hex(counter);
  return result;
}

bool path_exists(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

Result<std::uint64_t> file_size(const std::filesystem::path& path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return Result<std::uint64_t>::failure(
        ErrorCode::StoreIoError, "cannot determine the size of " + to_utf8(path) + ": " + error.message());
  }
  return static_cast<std::uint64_t>(size);
}

Status ensure_directory(const std::filesystem::path& path) {
  std::error_code error;
  if (std::filesystem::exists(path, error)) {
    if (!std::filesystem::is_directory(path, error)) {
      return Status::failure(ErrorCode::StoreLayoutInvalid,
                             to_utf8(path) + " exists but is not a directory");
    }
    return Status::success();
  }
  std::filesystem::create_directories(path, error);
  if (error) {
    return io_failure("cannot create directory", path, error.message());
  }
  return Status::success();
}

Status remove_file(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error) {
    return io_failure("cannot remove", path, error.message());
  }
  return Status::success();
}

#if defined(_WIN32)

namespace {

std::wstring wide(const std::filesystem::path& path) { return path.wstring(); }

std::string last_error_text() {
  const DWORD code = GetLastError();
  LPWSTR buffer = nullptr;
  const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                          FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                      reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  std::string result;
  if (length != 0 && buffer != nullptr) {
    const std::wstring text{buffer, length};
    result.reserve(text.size());
    for (const wchar_t character : text) {
      if (character == L'\r' || character == L'\n') {
        continue;
      }
      result.push_back(character < 128 ? static_cast<char>(character) : '?');
    }
  }
  if (buffer != nullptr) {
    LocalFree(buffer);
  }
  if (result.empty()) {
    result = "windows error " + std::to_string(code);
  }
  return result;
}

}  // namespace

Result<std::vector<std::byte>> read_file_bounded(const std::filesystem::path& path, std::uint64_t max_bytes) {
  // A reader asks for read access only, and shares both read and write. That
  // combination is what lets this path read the writer lock record while the
  // writer holds the lock file open: Windows requires a new open to permit the
  // access that existing handles already hold, and it is the writer's request
  // for write access with a read-only share mode that keeps a second writer
  // out. Sharing here grants nobody anything this handle does not have.
  const HANDLE handle = CreateFileW(wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Result<std::vector<std::byte>>::failure(ErrorCode::StoreIoError,
                                                   "cannot open " + to_utf8(path) + ": " + last_error_text());
  }
  LARGE_INTEGER size{};
  if (GetFileSizeEx(handle, &size) == 0) {
    const std::string reason = last_error_text();
    CloseHandle(handle);
    return Result<std::vector<std::byte>>::failure(ErrorCode::StoreIoError,
                                                   "cannot size " + to_utf8(path) + ": " + reason);
  }
  if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > max_bytes) {
    CloseHandle(handle);
    return Result<std::vector<std::byte>>::failure(
        ErrorCode::PayloadTooLarge,
        to_utf8(path) + " is larger than the configured maximum of " + std::to_string(max_bytes) + " bytes");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 20));
    DWORD read = 0;
    if (ReadFile(handle, bytes.data() + offset, chunk, &read, nullptr) == 0) {
      const std::string reason = last_error_text();
      CloseHandle(handle);
      return Result<std::vector<std::byte>>::failure(ErrorCode::StoreIoError,
                                                     "cannot read " + to_utf8(path) + ": " + reason);
    }
    if (read == 0) {
      CloseHandle(handle);
      return Result<std::vector<std::byte>>::failure(ErrorCode::StoreTruncated,
                                                     to_utf8(path) + " ended before its declared size");
    }
    offset += read;
  }
  CloseHandle(handle);
  return bytes;
}

Status write_new_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
  const HANDLE handle = CreateFileW(wide(path).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Status::failure(ErrorCode::StoreIoError,
                           "cannot create " + to_utf8(path) + ": " + last_error_text());
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 20));
    DWORD written = 0;
    if (WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) == 0 || written == 0) {
      const std::string reason = last_error_text();
      CloseHandle(handle);
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      return Status::failure(ErrorCode::StoreIoError, "cannot write " + to_utf8(path) + ": " + reason);
    }
    offset += written;
  }
  if (FlushFileBuffers(handle) == 0) {
    const std::string reason = last_error_text();
    CloseHandle(handle);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return Status::failure(ErrorCode::StoreIoError, "cannot flush " + to_utf8(path) + ": " + reason);
  }
  CloseHandle(handle);
  return Status::success();
}

Status sync_file(const std::filesystem::path& path) {
  const HANDLE handle = CreateFileW(wide(path).c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Status::failure(ErrorCode::StoreIoError,
                           "cannot open " + to_utf8(path) + " to flush it: " + last_error_text());
  }
  const BOOL flushed = FlushFileBuffers(handle);
  const std::string reason = flushed == 0 ? last_error_text() : std::string{};
  CloseHandle(handle);
  if (flushed == 0) {
    return Status::failure(ErrorCode::StoreIoError, "cannot flush " + to_utf8(path) + ": " + reason);
  }
  return Status::success();
}

Status replace_file(const std::filesystem::path& source, const std::filesystem::path& destination) {
  if (MoveFileExW(wide(source).c_str(), wide(destination).c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return Status::failure(ErrorCode::StorePublishFailed,
                           "cannot replace " + to_utf8(destination) + ": " + last_error_text());
  }
  return Status::success();
}

Status sync_directory(const std::filesystem::path& path) {
  // Windows has no directory fsync. The replacement that has to be durable is
  // performed by MoveFileEx with MOVEFILE_WRITE_THROUGH, which does not return
  // before the metadata change is flushed. This function therefore has nothing
  // left to do and says so rather than pretending otherwise.
  static_cast<void>(path);
  return Status::success();
}

#else  // POSIX

namespace {

Status errno_failure(std::string_view action, const std::filesystem::path& path) {
  return io_failure(action, path, std::strerror(errno));
}

}  // namespace

Result<std::vector<std::byte>> read_file_bounded(const std::filesystem::path& path, std::uint64_t max_bytes) {
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    return Result<std::vector<std::byte>>::failure(ErrorCode::StoreIoError,
                                                   "cannot open " + to_utf8(path) + ": " + std::strerror(errno));
  }
  struct stat status {};
  if (::fstat(descriptor, &status) != 0) {
    const std::string reason = std::strerror(errno);
    ::close(descriptor);
    return Result<std::vector<std::byte>>::failure(ErrorCode::StoreIoError,
                                                   "cannot size " + to_utf8(path) + ": " + reason);
  }
  if (status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) > max_bytes) {
    ::close(descriptor);
    return Result<std::vector<std::byte>>::failure(
        ErrorCode::PayloadTooLarge,
        to_utf8(path) + " is larger than the configured maximum of " + std::to_string(max_bytes) + " bytes");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(status.st_size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t read = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (read < 0) {
      const std::string reason = std::strerror(errno);
      ::close(descriptor);
      return Result<std::vector<std::byte>>::failure(ErrorCode::StoreIoError,
                                                     "cannot read " + to_utf8(path) + ": " + reason);
    }
    if (read == 0) {
      ::close(descriptor);
      return Result<std::vector<std::byte>>::failure(ErrorCode::StoreTruncated,
                                                     to_utf8(path) + " ended before its declared size");
    }
    offset += static_cast<std::size_t>(read);
  }
  ::close(descriptor);
  return bytes;
}

Status write_new_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
  const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (descriptor < 0) {
    return Status::failure(ErrorCode::StoreIoError,
                           "cannot create " + to_utf8(path) + ": " + std::strerror(errno));
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (written <= 0) {
      const std::string reason = std::strerror(errno);
      ::close(descriptor);
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      return Status::failure(ErrorCode::StoreIoError, "cannot write " + to_utf8(path) + ": " + reason);
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(descriptor) != 0) {
    const std::string reason = std::strerror(errno);
    ::close(descriptor);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return Status::failure(ErrorCode::StoreIoError, "cannot flush " + to_utf8(path) + ": " + reason);
  }
  if (::close(descriptor) != 0) {
    return errno_failure("cannot close", path);
  }
  return Status::success();
}

Status sync_file(const std::filesystem::path& path) {
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    return errno_failure("cannot open to flush", path);
  }
  const int result = ::fsync(descriptor);
  const int saved = errno;
  ::close(descriptor);
  if (result != 0) {
    errno = saved;
    return errno_failure("cannot flush", path);
  }
  return Status::success();
}

Status replace_file(const std::filesystem::path& source, const std::filesystem::path& destination) {
  if (::rename(source.c_str(), destination.c_str()) != 0) {
    return Status::failure(ErrorCode::StorePublishFailed,
                           "cannot replace " + to_utf8(destination) + ": " + std::strerror(errno));
  }
  return sync_directory(destination.parent_path());
}

Status sync_directory(const std::filesystem::path& path) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
  if (descriptor < 0) {
    return errno_failure("cannot open directory to flush", path);
  }
  const int result = ::fsync(descriptor);
  const int saved = errno;
  ::close(descriptor);
  if (result != 0) {
    errno = saved;
    return errno_failure("cannot flush directory", path);
  }
  return Status::success();
}

#endif

}  // namespace detail
}  // namespace facility_dependency_registry
