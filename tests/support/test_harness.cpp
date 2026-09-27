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

#include "test_harness.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <random>
#include <system_error>
#include <utility>

namespace fdep_test {
namespace {

struct Entry {
  std::string suite;
  std::string name;
  TestFunction function;
};

std::vector<Entry>& registry() {
  static std::vector<Entry> entries;
  return entries;
}

std::map<std::string, std::function<int(const std::vector<std::string>&)>, std::less<>>& child_modes() {
  static std::map<std::string, std::function<int(const std::vector<std::string>&)>, std::less<>> modes;
  return modes;
}

bool g_current_failed = false;
std::filesystem::path g_executable_path;
std::atomic<std::uint64_t> g_temp_counter{0};

std::string random_token() {
  static thread_local std::mt19937_64 generator{std::random_device{}()};
  const std::uint64_t value = generator();
  const std::uint64_t counter = g_temp_counter.fetch_add(1) + 1;
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (int shift = 60; shift >= 0; shift -= 4) {
    result.push_back(digits[(value >> shift) & 0x0Fu]);
  }
  result.push_back('-');
  result.append(std::to_string(counter));
  return result;
}

}  // namespace

Registrar::Registrar(const char* suite, const char* name, TestFunction function) {
  registry().push_back(Entry{suite, name, function});
}

void register_child_mode(std::string_view mode, std::function<int(const std::vector<std::string>&)> handler) {
  child_modes().insert_or_assign(std::string{mode}, std::move(handler));
}

int dispatch_child(const std::string& mode, const std::vector<std::string>& args) {
  const auto position = child_modes().find(mode);
  if (position == child_modes().end()) {
    std::cerr << "no child mode named " << mode << "\n";
    return 64;
  }
  return position->second(args);
}

void report_failure(const char* file, int line, const std::string& message) {
  g_current_failed = true;
  std::cout << "        " << file << ":" << line << ": " << message << "\n";
}

bool current_test_has_failed() noexcept { return g_current_failed; }

const std::filesystem::path& executable_path() { return g_executable_path; }

void set_executable_path(const std::filesystem::path& path) { g_executable_path = path; }

std::filesystem::path make_temp_directory(std::string_view label) {
  std::error_code error;
  const auto base = std::filesystem::temp_directory_path(error);
  const auto root = error ? std::filesystem::path{"."} : base;
  const auto directory = root / ("fdep-test-" + std::string{label} + "-" + random_token());
  std::filesystem::create_directories(directory, error);
  return directory;
}

void remove_tree(const std::filesystem::path& path) noexcept {
  std::error_code error;
  std::filesystem::remove_all(path, error);
  if (error) {
    // A temporary directory that survives its test is a defect in the test, not
    // a detail: it usually means a file inside it was still open. Reporting it
    // here turns a silent leak into a failed test.
    report_failure(__FILE__, __LINE__,
                   "could not remove the temporary directory " + path.string() + ": " + error.message());
  } else if (std::filesystem::exists(path, error) && !error) {
    report_failure(__FILE__, __LINE__,
                   "the temporary directory " + path.string() + " still exists after removal");
  }
}

std::string render(std::string_view value) { return std::string{value}; }
std::string render(const std::string& value) { return value; }
std::string render(bool value) { return value ? "true" : "false"; }
std::string render(const char* value) { return std::string{value}; }

int run_all(const std::vector<std::string>& arguments) {
  std::string filter;
  bool list_only = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (arguments[index] == "--filter" && index + 1 < arguments.size()) {
      filter = arguments[index + 1];
      ++index;
    } else if (arguments[index] == "--list") {
      list_only = true;
    }
  }

  auto entries = registry();
  // A deterministic order that does not depend on link order.
  std::sort(entries.begin(), entries.end(), [](const Entry& lhs, const Entry& rhs) {
    if (lhs.suite != rhs.suite) {
      return lhs.suite < rhs.suite;
    }
    return lhs.name < rhs.name;
  });

  std::size_t executed = 0;
  std::size_t failed = 0;
  const auto started = std::chrono::steady_clock::now();
  for (const auto& entry : entries) {
    const std::string full = entry.suite + "." + entry.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    if (list_only) {
      std::cout << full << "\n";
      continue;
    }
    ++executed;
    g_current_failed = false;
    std::cout << "[ RUN  ] " << full << "\n";
    try {
      entry.function();
    } catch (const TestAbort&) {
      // already reported
    } catch (const std::exception& error) {
      report_failure(__FILE__, __LINE__, std::string{"unexpected exception: "} + error.what());
    } catch (...) {
      report_failure(__FILE__, __LINE__, "unexpected non-standard exception");
    }
    if (g_current_failed) {
      ++failed;
      std::cout << "[ FAIL ] " << full << "\n";
    } else {
      std::cout << "[  OK  ] " << full << "\n";
    }
  }

  if (list_only) {
    return 0;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  std::cout << "\ntests run: " << executed << ", failed: " << failed << ", elapsed: " << elapsed << " ms\n";
  return failed == 0 ? 0 : 1;
}

}  // namespace fdep_test
