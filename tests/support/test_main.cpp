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

#include <filesystem>
#include <string>
#include <vector>

#include "test_harness.hpp"

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc));
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  fdep_test::set_executable_path(std::filesystem::absolute(std::filesystem::path{argv[0]}));

  if (arguments.size() >= 2 && arguments[0] == "--child") {
    const std::vector<std::string> rest(arguments.begin() + 2, arguments.end());
    return fdep_test::dispatch_child(arguments[1], rest);
  }
  return fdep_test::run_all(arguments);
}
