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
#include <filesystem>
#include <iostream>

namespace ccap_test {

namespace {

thread_local int failures_in_current_test = 0;

// Removes anything a test left in the working directory and reports it. This is
// hygiene rather than a pass/fail mechanism: a run that leaves scratch files
// behind still passes, but says so.
void sweep_scratch_files() {
  std::error_code error;
  std::vector<std::string> leftovers;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(std::filesystem::current_path(), error)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("ccap-test-", 0) != 0) {
      continue;
    }
    leftovers.push_back(name);
    std::error_code removal_error;
    std::filesystem::remove_all(entry.path(), removal_error);
  }
  if (!leftovers.empty()) {
    std::sort(leftovers.begin(), leftovers.end());
    std::cout << "swept " << leftovers.size() << " scratch entr"
              << (leftovers.size() == 1 ? "y" : "ies") << " left by the tests:";
    for (const std::string& name : leftovers) {
      std::cout << ' ' << name;
    }
    std::cout << "\n";
  }
}

}  // namespace

void register_test(const char* name, TestFunction function) {
  registry().push_back(TestCase{name, function});
}

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

void report_failure(const char* file, int line, const std::string& message) {
  ++failures_in_current_test;
  std::cout << "    failure at " << file << ":" << line << ": " << message << "\n";
  // Flushed immediately: a test that terminates the process must still leave the
  // failure it was reporting on disk.
  std::cout.flush();
}

int run_all(const std::string& filter) {
  std::size_t passed = 0;
  std::size_t failed = 0;
  std::size_t skipped = 0;
  for (const TestCase& test : registry()) {
    if (!filter.empty() && test.name.find(filter) == std::string::npos) {
      ++skipped;
      continue;
    }
    failures_in_current_test = 0;
    std::cout << "running " << test.name << "\n";
    std::cout.flush();
    test.function();
    if (failures_in_current_test == 0) {
      ++passed;
      std::cout << "  pass " << test.name << "\n";
    } else {
      ++failed;
      std::cout << "  FAIL " << test.name << " (" << failures_in_current_test
                << " failed check(s))\n";
    }
  }
  sweep_scratch_files();
  std::cout << "\n" << passed << " passed, " << failed << " failed, " << skipped << " skipped\n";
  return failed == 0 ? 0 : 1;
}

}  // namespace ccap_test
