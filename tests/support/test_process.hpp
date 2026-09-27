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

#ifndef FACILITY_DEPENDENCY_REGISTRY_TESTS_SUPPORT_TEST_PROCESS_HPP
#define FACILITY_DEPENDENCY_REGISTRY_TESTS_SUPPORT_TEST_PROCESS_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fdep_test {

/// One real operating system process running this same test executable in a
/// named child mode.
///
/// The child's standard output and error are redirected into a file rather
/// than a pipe, so the harness never depends on pipe semantics that a
/// restricted environment might forbid.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();

  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  /// Starts `--child <mode> args...` with this executable. `output_path`
  /// receives the child's combined output.
  [[nodiscard]] static ChildProcess start(const std::string& mode, const std::vector<std::string>& args,
                                          const std::filesystem::path& output_path);

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint64_t process_id() const noexcept;
  [[nodiscard]] bool running() const noexcept;

  /// Waits for the child to finish and reads its output. Returns false when the
  /// child could not be waited for.
  [[nodiscard]] bool wait(int& exit_code, std::string& output);

  /// Terminates the child abruptly, without giving it a chance to run any
  /// cleanup. This is how a crash is simulated.
  void terminate();

 private:
  void* handle_ = nullptr;
  std::filesystem::path output_path_{};
};

/// Runs one child mode to completion and returns its exit code and output.
[[nodiscard]] int run_child(const std::string& mode, const std::vector<std::string>& args, std::string& output);

/// Waits until `path` exists, polling with a short sleep. Returns false when the
/// child exits first or the rendezvous bound is reached: both are reported as
/// test failures rather than being mistaken for success.
[[nodiscard]] bool wait_for_file(const std::filesystem::path& path, const ChildProcess& child,
                                 std::uint32_t bound_milliseconds = 60000);

void sleep_milliseconds(std::uint32_t milliseconds);

}  // namespace fdep_test

#endif  // FACILITY_DEPENDENCY_REGISTRY_TESTS_SUPPORT_TEST_PROCESS_HPP
