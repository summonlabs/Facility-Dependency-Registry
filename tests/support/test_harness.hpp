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

#ifndef FACILITY_DEPENDENCY_REGISTRY_TESTS_SUPPORT_TEST_HARNESS_HPP
#define FACILITY_DEPENDENCY_REGISTRY_TESTS_SUPPORT_TEST_HARNESS_HPP

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace fdep_test {

/// Thrown by FDEP_REQUIRE to abandon the current test without abandoning the
/// run. Exceptions are used only inside the harness; the library itself never
/// throws for control flow.
class TestAbort {};

using TestFunction = void (*)();

/// Registers one test. Tests are grouped by suite so that a failure names the
/// area it belongs to.
struct Registrar {
  Registrar(const char* suite, const char* name, TestFunction function);
};

/// Registers a handler for `--child <mode> ...`, used by the tests that need a
/// real second operating system process.
void register_child_mode(std::string_view mode, std::function<int(const std::vector<std::string>&)> handler);

/// Runs the child handler for `mode`. Returns 64 when no handler is registered.
int dispatch_child(const std::string& mode, const std::vector<std::string>& args);

/// Runs every registered test, or the ones matching `filter`.
int run_all(const std::vector<std::string>& arguments);

// -- reporting ---------------------------------------------------------------

void report_failure(const char* file, int line, const std::string& message);
[[nodiscard]] bool current_test_has_failed() noexcept;

/// Compares two values through a function call rather than inline. Real tests
/// compare compile time constants, and a direct `if (a == b)` on constants is
/// diagnosed as a constant conditional expression by MSVC (C4127); routing the
/// comparison through here keeps the check honest without suppressing the
/// warning.
template <class Left, class Right>
[[nodiscard]] bool same(const Left& lhs, const Right& rhs) {
  return lhs == rhs;
}

/// The absolute path of the running test executable. Set once by test_main.
[[nodiscard]] const std::filesystem::path& executable_path();
void set_executable_path(const std::filesystem::path& path);

/// A unique, empty temporary directory for one test. Removed by
/// `remove_tree`, which the test is expected to call.
[[nodiscard]] std::filesystem::path make_temp_directory(std::string_view label);
void remove_tree(const std::filesystem::path& path) noexcept;

/// Renders any value that has a to_text() or is streamable, for diagnostics.
template <class T>
std::string render(const T& value);

std::string render(std::string_view value);
std::string render(const std::string& value);
std::string render(bool value);
std::string render(const char* value);

template <class T>
std::string render(const T& value) {
  if constexpr (requires { value.to_text(); }) {
    return value.to_text();
  } else if constexpr (requires { value.to_canonical(); }) {
    return value.to_canonical();
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (std::is_integral_v<T>) {
    return std::to_string(value);
  } else {
    return "<value>";
  }
}

}  // namespace fdep_test

#define FDEP_TEST(suite_name, test_name)                                                   \
  static void fdep_test_##suite_name##_##test_name();                                      \
  static const ::fdep_test::Registrar fdep_registrar_##suite_name##_##test_name{           \
      #suite_name, #test_name, &fdep_test_##suite_name##_##test_name};                     \
  static void fdep_test_##suite_name##_##test_name()

#define FDEP_CHECK(condition)                                                              \
  do {                                                                                     \
    if (!(condition)) {                                                                    \
      ::fdep_test::report_failure(__FILE__, __LINE__, "check failed: " #condition);        \
    }                                                                                      \
  } while (false)

#define FDEP_CHECK_EQ(actual, expected)                                                    \
  do {                                                                                     \
    const auto fdep_actual = (actual);                                                     \
    const auto fdep_expected = (expected);                                                 \
    if (!::fdep_test::same(fdep_actual, fdep_expected)) {                                   \
      ::fdep_test::report_failure(__FILE__, __LINE__,                                      \
                                  std::string{"expected "} + ::fdep_test::render(fdep_expected) + \
                                      " but found " + ::fdep_test::render(fdep_actual) +   \
                                      " (" #actual " == " #expected ")");                  \
    }                                                                                      \
  } while (false)

#define FDEP_CHECK_CODE(result, expected_code)                                             \
  do {                                                                                     \
    const auto& fdep_result = (result);                                                    \
    if (fdep_result.has_value()) {                                                         \
      ::fdep_test::report_failure(__FILE__, __LINE__,                                      \
                                  "expected the operation to fail with " #expected_code     \
                                  " but it succeeded");                                     \
    } else if (fdep_result.error().code() != (expected_code)) {                             \
      ::fdep_test::report_failure(__FILE__, __LINE__,                                       \
                                  std::string{"expected " #expected_code " but found "} +    \
                                      std::string{::facility_dependency_registry::to_token(  \
                                          fdep_result.error().code())} +                     \
                                      ": " + fdep_result.error().detail());                  \
    }                                                                                        \
  } while (false)

#define FDEP_CHECK_STATUS(status, expected_code)                                           \
  do {                                                                                     \
    const auto& fdep_status = (status);                                                    \
    if (fdep_status.ok()) {                                                                \
      ::fdep_test::report_failure(__FILE__, __LINE__,                                      \
                                  "expected the operation to fail with " #expected_code     \
                                  " but it succeeded");                                     \
    } else if (fdep_status.code() != (expected_code)) {                                     \
      ::fdep_test::report_failure(__FILE__, __LINE__,                                       \
                                  std::string{"expected " #expected_code " but found "} +    \
                                      std::string{::facility_dependency_registry::to_token(  \
                                          fdep_status.code())} +                             \
                                      ": " + fdep_status.error().detail());                  \
    }                                                                                        \
  } while (false)

#define FDEP_REQUIRE(condition)                                                            \
  do {                                                                                     \
    if (!(condition)) {                                                                    \
      ::fdep_test::report_failure(__FILE__, __LINE__, "requirement failed: " #condition);  \
      throw ::fdep_test::TestAbort{};                                                      \
    }                                                                                      \
  } while (false)

#define FDEP_REQUIRE_CODE(result, expected_code)                                           \
  do {                                                                                     \
    const auto& fdep_result = (result);                                                    \
    if (!fdep_result.has_value() || fdep_result.error().code() != (expected_code)) {        \
      ::fdep_test::report_failure(__FILE__, __LINE__,                                      \
                                  std::string{"expected failure " #expected_code " from " #result}); \
      throw ::fdep_test::TestAbort{};                                                      \
    }                                                                                      \
  } while (false)

#define FDEP_REQUIRE_OK(result)                                                            \
  do {                                                                                     \
    const auto& fdep_result = (result);                                                    \
    if (!fdep_result.has_value()) {                                                        \
      ::fdep_test::report_failure(__FILE__, __LINE__,                                      \
                                  std::string{"expected success from " #result " but found "} + \
                                      std::string{::facility_dependency_registry::to_token( \
                                          fdep_result.error().code())} +                   \
                                      ": " + fdep_result.error().detail());                \
      throw ::fdep_test::TestAbort{};                                                      \
    }                                                                                      \
  } while (false)

#endif  // FACILITY_DEPENDENCY_REGISTRY_TESTS_SUPPORT_TEST_HARNESS_HPP
