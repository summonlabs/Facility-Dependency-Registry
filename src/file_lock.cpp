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

#include "file_lock.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>

#include "file_ops.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace facility_dependency_registry {
namespace detail {

#if defined(_WIN32)

FileLock::~FileLock() { release(); }

FileLock::FileLock(FileLock&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    release();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

Result<FileLock> FileLock::acquire(const std::filesystem::path& path, std::string_view record) {
  // FILE_SHARE_READ lets an inspector read the holder record; withholding
  // FILE_SHARE_WRITE is what makes the second writer fail.
  const HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
      return Result<FileLock>::failure(ErrorCode::StoreLocked,
                                       "another process holds the writer lock on " + to_utf8(path));
    }
    return Result<FileLock>::failure(ErrorCode::StoreIoError,
                                     "cannot open the writer lock " + to_utf8(path) +
                                         ": windows error " + std::to_string(code));
  }
  if (SetFilePointer(handle, 0, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER &&
      GetLastError() != NO_ERROR) {
    CloseHandle(handle);
    return Result<FileLock>::failure(ErrorCode::StoreIoError,
                                     "cannot position the writer lock " + to_utf8(path));
  }
  if (SetEndOfFile(handle) == 0) {
    CloseHandle(handle);
    return Result<FileLock>::failure(ErrorCode::StoreIoError, "cannot truncate the writer lock " + to_utf8(path));
  }
  DWORD written = 0;
  if (!record.empty()) {
    if (WriteFile(handle, record.data(), static_cast<DWORD>(record.size()), &written, nullptr) == 0) {
      CloseHandle(handle);
      return Result<FileLock>::failure(ErrorCode::StoreIoError,
                                       "cannot record the holder in " + to_utf8(path));
    }
    if (FlushFileBuffers(handle) == 0) {
      CloseHandle(handle);
      return Result<FileLock>::failure(ErrorCode::StoreIoError, "cannot flush the writer lock " + to_utf8(path));
    }
  }
  FileLock lock;
  lock.handle_ = handle;
  return lock;
}

bool FileLock::held() const noexcept { return handle_ != nullptr; }

void FileLock::release() noexcept {
  if (handle_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
}

#else  // POSIX

FileLock::~FileLock() { release(); }

FileLock::FileLock(FileLock&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    release();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

Result<FileLock> FileLock::acquire(const std::filesystem::path& path, std::string_view record) {
  const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
  if (descriptor < 0) {
    return Result<FileLock>::failure(ErrorCode::StoreIoError,
                                     "cannot open the writer lock " + to_utf8(path) + ": " + std::strerror(errno));
  }
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    const int saved = errno;
    ::close(descriptor);
    if (saved == EWOULDBLOCK || saved == EAGAIN) {
      return Result<FileLock>::failure(ErrorCode::StoreLocked,
                                       "another process holds the writer lock on " + to_utf8(path));
    }
    return Result<FileLock>::failure(ErrorCode::StoreIoError,
                                     "cannot lock " + to_utf8(path) + ": " + std::strerror(saved));
  }
  if (::ftruncate(descriptor, 0) != 0) {
    const std::string reason = std::strerror(errno);
    ::flock(descriptor, LOCK_UN);
    ::close(descriptor);
    return Result<FileLock>::failure(ErrorCode::StoreIoError,
                                     "cannot truncate the writer lock " + to_utf8(path) + ": " + reason);
  }
  if (!record.empty()) {
    const ssize_t written = ::pwrite(descriptor, record.data(), record.size(), 0);
    if (written != static_cast<ssize_t>(record.size())) {
      const std::string reason = std::strerror(errno);
      ::flock(descriptor, LOCK_UN);
      ::close(descriptor);
      return Result<FileLock>::failure(ErrorCode::StoreIoError,
                                       "cannot record the holder in " + to_utf8(path) + ": " + reason);
    }
    if (::fsync(descriptor) != 0) {
      const std::string reason = std::strerror(errno);
      ::flock(descriptor, LOCK_UN);
      ::close(descriptor);
      return Result<FileLock>::failure(ErrorCode::StoreIoError,
                                       "cannot flush the writer lock " + to_utf8(path) + ": " + reason);
    }
  }
  FileLock lock;
  lock.handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptor));
  return lock;
}

bool FileLock::held() const noexcept { return handle_ != nullptr; }

void FileLock::release() noexcept {
  if (handle_ != nullptr) {
    const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
    ::flock(descriptor, LOCK_UN);
    ::close(descriptor);
    handle_ = nullptr;
  }
}

#endif

Result<std::string> read_lock_record(const std::filesystem::path& path) {
  if (!path_exists(path)) {
    return std::string{};
  }
  const auto bytes = read_file_bounded(path, 4096);
  if (!bytes) {
    return Result<std::string>::failure(bytes.error());
  }
  const auto& data = bytes.value();
  std::string record;
  record.reserve(data.size());
  for (const auto byte : data) {
    const auto value = static_cast<unsigned char>(byte);
    if (value == 0) {
      break;
    }
    if (value < 0x20 || value > 0x7E) {
      break;  // a torn record is not a record
    }
    record.push_back(static_cast<char>(value));
  }
  return record;
}

}  // namespace detail
}  // namespace facility_dependency_registry
