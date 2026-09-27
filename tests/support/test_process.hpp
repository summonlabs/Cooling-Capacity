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

#ifndef COOLING_CAPACITY_TESTS_SUPPORT_TEST_PROCESS_HPP
#define COOLING_CAPACITY_TESTS_SUPPORT_TEST_PROCESS_HPP

#include <string>
#include <vector>

#include "cooling_capacity/errors.hpp"

namespace ccap_test {

// Real operating-system process control. The test binary re-executes itself to
// obtain independent processes for the writer-lock, restart and crash tests;
// nothing here uses threads to stand in for a process.
struct ChildProcess {
  void* handle = nullptr;
  unsigned long pid = 0;
  bool running = false;
};

// Absolute path of the running test executable.
[[nodiscard]] std::string executable_path();

[[nodiscard]] cooling_capacity::Result<ChildProcess> spawn(
    const std::vector<std::string>& arguments);

// Waits for the child to exit. A wait that exceeds the budget is reported as a
// failure; it is never converted into a pass, and no test relies on the budget
// to bound a hang.
[[nodiscard]] cooling_capacity::Result<unsigned long> wait_for_exit(ChildProcess& child,
                                                                    unsigned budget_ms);

// Terminates the child and waits for it. Used by the crash test to kill a
// writer in the middle of its work.
[[nodiscard]] cooling_capacity::Result<unsigned long> terminate(ChildProcess& child);

void release(ChildProcess& child);

[[nodiscard]] bool file_exists(const std::string& path);
[[nodiscard]] cooling_capacity::Result<void> write_text(const std::string& path,
                                                        const std::string& text);
[[nodiscard]] cooling_capacity::Result<std::string> read_text(const std::string& path);
[[nodiscard]] cooling_capacity::Result<void> remove_path(const std::string& path);
[[nodiscard]] cooling_capacity::Result<void> remove_tree(const std::string& path);
[[nodiscard]] cooling_capacity::Result<void> make_directory(const std::string& path);

// Waits for a file to appear. Exhausting the budget is an error.
[[nodiscard]] cooling_capacity::Result<void> wait_for_file(const std::string& path,
                                                           unsigned budget_ms);

// Child entry points. Each returns the process exit code.
[[nodiscard]] int child_hold_lock(const std::string& directory, const std::string& ready_file,
                                  const std::string& stop_file);
[[nodiscard]] int child_commit_loop(const std::string& directory, const std::string& marker_file);
// Opens the store, writes the head generation to `output_file` and exits.
[[nodiscard]] int child_read_head(const std::string& directory, const std::string& output_file);
// Tries to take writer authority once and records the outcome in `output_file`
// as "acquired <epoch>" or "refused <code>".
[[nodiscard]] int child_try_lock(const std::string& directory, const std::string& output_file);

// Dispatches the `--child` mode. Returns -1 when the arguments do not name a
// child mode.
[[nodiscard]] int run_child_mode(int argc, char** argv);

}  // namespace ccap_test

#endif  // COOLING_CAPACITY_TESTS_SUPPORT_TEST_PROCESS_HPP
