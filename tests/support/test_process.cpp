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

#include "test_process.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "test_harness.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fdep_test {
namespace {

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    return {};
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

#if defined(_WIN32)

std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
  return result;
}

std::string quote_argument(const std::string& argument) {
  std::string result{"\""};
  for (const char character : argument) {
    if (character == '"') {
      result.append("\\\"");
    } else {
      result.push_back(character);
    }
  }
  result.push_back('"');
  return result;
}

#endif

}  // namespace

ChildProcess::~ChildProcess() {
#if defined(_WIN32)
  if (handle_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(handle_));
  }
#endif
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : handle_(other.handle_), output_path_(std::move(other.output_path_)) {
  other.handle_ = nullptr;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
#if defined(_WIN32)
    if (handle_ != nullptr) {
      CloseHandle(static_cast<HANDLE>(handle_));
    }
#endif
    handle_ = other.handle_;
    other.handle_ = nullptr;
    output_path_ = std::move(other.output_path_);
  }
  return *this;
}

bool ChildProcess::valid() const noexcept { return handle_ != nullptr; }

#if defined(_WIN32)

std::uint64_t ChildProcess::process_id() const noexcept {
  return handle_ == nullptr ? 0 : static_cast<std::uint64_t>(GetProcessId(static_cast<HANDLE>(handle_)));
}

bool ChildProcess::running() const noexcept {
  if (handle_ == nullptr) {
    return false;
  }
  return WaitForSingleObject(static_cast<HANDLE>(handle_), 0) == WAIT_TIMEOUT;
}

ChildProcess ChildProcess::start(const std::string& mode, const std::vector<std::string>& args,
                                 const std::filesystem::path& output_path) {
  ChildProcess child;
  child.output_path_ = output_path;

  std::string command = quote_argument(executable_path().string());
  command.append(" --child ");
  command.append(quote_argument(mode));
  for (const auto& argument : args) {
    command.push_back(' ');
    command.append(quote_argument(argument));
  }

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  const HANDLE output = CreateFileW(output_path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output == INVALID_HANDLE_VALUE) {
    return child;
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = output;
  startup.hStdError = output;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION information{};
  std::wstring mutable_command = widen(command);
  const BOOL started = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                                      &startup, &information);
  CloseHandle(output);
  if (started == 0) {
    return child;
  }
  CloseHandle(information.hThread);
  child.handle_ = information.hProcess;
  return child;
}

bool ChildProcess::wait(int& exit_code, std::string& output) {
  if (handle_ == nullptr) {
    return false;
  }
  const DWORD result = WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
  if (result != WAIT_OBJECT_0) {
    return false;
  }
  DWORD code = 0;
  if (GetExitCodeProcess(static_cast<HANDLE>(handle_), &code) == 0) {
    return false;
  }
  exit_code = static_cast<int>(code);
  output = read_text_file(output_path_);
  CloseHandle(static_cast<HANDLE>(handle_));
  handle_ = nullptr;
  return true;
}

void ChildProcess::terminate() {
  if (handle_ == nullptr) {
    return;
  }
  TerminateProcess(static_cast<HANDLE>(handle_), 0xDEADu);
  WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
  CloseHandle(static_cast<HANDLE>(handle_));
  handle_ = nullptr;
}

#else  // POSIX

std::uint64_t ChildProcess::process_id() const noexcept {
  return handle_ == nullptr ? 0 : static_cast<std::uint64_t>(reinterpret_cast<std::intptr_t>(handle_));
}

bool ChildProcess::running() const noexcept {
  if (handle_ == nullptr) {
    return false;
  }
  int status = 0;
  const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(handle_));
  return ::waitpid(pid, &status, WNOHANG) == 0;
}

ChildProcess ChildProcess::start(const std::string& mode, const std::vector<std::string>& args,
                                 const std::filesystem::path& output_path) {
  ChildProcess child;
  child.output_path_ = output_path;

  const int descriptor = ::open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (descriptor < 0) {
    return child;
  }
  std::vector<std::string> storage;
  storage.push_back(executable_path().string());
  storage.push_back("--child");
  storage.push_back(mode);
  for (const auto& argument : args) {
    storage.push_back(argument);
  }
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (auto& item : storage) {
    argv.push_back(item.data());
  }
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, descriptor, STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, descriptor, STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, descriptor);

  pid_t pid = 0;
  const int result = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(descriptor);
  if (result != 0) {
    return child;
  }
  child.handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(pid));
  return child;
}

bool ChildProcess::wait(int& exit_code, std::string& output) {
  if (handle_ == nullptr) {
    return false;
  }
  int status = 0;
  const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(handle_));
  if (::waitpid(pid, &status, 0) != pid) {
    return false;
  }
  if (WIFEXITED(status)) {
    exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    exit_code = 128 + WTERMSIG(status);
  } else {
    exit_code = -1;
  }
  output = read_text_file(output_path_);
  handle_ = nullptr;
  return true;
}

void ChildProcess::terminate() {
  if (handle_ == nullptr) {
    return;
  }
  const pid_t pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(handle_));
  ::kill(pid, SIGKILL);
  int status = 0;
  ::waitpid(pid, &status, 0);
  handle_ = nullptr;
}

#endif

int run_child(const std::string& mode, const std::vector<std::string>& args, std::string& output) {
  const auto output_path = std::filesystem::temp_directory_path() / ("fdep-child-" + mode + ".out");
  ChildProcess child = ChildProcess::start(mode, args, output_path);
  if (!child.valid()) {
    output = "the child process could not be started";
    return -1;
  }
  int exit_code = -1;
  if (!child.wait(exit_code, output)) {
    output = "the child process could not be waited for";
    return -1;
  }
  std::error_code error;
  std::filesystem::remove(output_path, error);
  return exit_code;
}

void sleep_milliseconds(std::uint32_t milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds{milliseconds});
}

bool wait_for_file(const std::filesystem::path& path, const ChildProcess& child, std::uint32_t bound_milliseconds) {
  std::uint32_t waited = 0;
  while (waited < bound_milliseconds) {
    std::error_code error;
    if (std::filesystem::exists(path, error) && !error) {
      return true;
    }
    if (!child.running()) {
      return false;
    }
    sleep_milliseconds(5);
    waited += 5;
  }
  return false;
}

}  // namespace fdep_test
