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

#include <cstdlib>
#include <iostream>
#include <string>

#include "test_harness.hpp"
#include "test_process.hpp"

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--child") {
    const int outcome = ccap_test::run_child_mode(argc, argv);
    if (outcome < 0) {
      std::cerr << "unknown child mode\n";
      return 64;
    }
    return outcome;
  }
  std::string filter;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--list") {
      for (const ccap_test::TestCase& test : ccap_test::registry()) {
        std::cout << test.name << "\n";
      }
      return 0;
    }
    if (argument.compare(0, 9, "--filter=") == 0) {
      filter = argument.substr(9);
      continue;
    }
    if (argument == "--filter") {
      if (index + 1 >= argc) {
        std::cerr << "--filter needs a value\n";
        return 2;
      }
      filter = argv[++index];
      continue;
    }
    std::cerr << "unknown argument: " << argument << "\n";
    return 2;
  }
  const int skipped = 0;
  (void)skipped;
  return ccap_test::run_all(filter);
}
