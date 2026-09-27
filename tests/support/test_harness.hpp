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

#ifndef COOLING_CAPACITY_TESTS_SUPPORT_TEST_HARNESS_HPP
#define COOLING_CAPACITY_TESTS_SUPPORT_TEST_HARNESS_HPP

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "cooling_capacity/errors.hpp"

namespace ccap_test {

using TestFunction = void (*)();

struct TestCase {
  std::string name;
  TestFunction function;
};

// The registry is a function-local static so that registration order across
// translation units cannot matter.
void register_test(const char* name, TestFunction function);

[[nodiscard]] std::vector<TestCase>& registry();

// Records a failure and continues the current test. A test fails if any check
// fails; there is no notion of an expected failure.
void report_failure(const char* file, int line, const std::string& message);

// Renders a value for a failure message. Values that can be streamed are
// streamed; everything else is described by type only.
template <class T>
std::string describe(const T& value) {
  if constexpr (requires(std::ostringstream& stream, const T& item) { stream << item; }) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else {
    return "<value>";
  }
}

inline std::string describe(const cooling_capacity::Error& error) { return error.to_string(); }

inline std::string describe(bool value) { return value ? "true" : "false"; }

// Runs every registered test whose name contains `filter`, or all of them when
// the filter is empty. Returns 0 when every test passed.
int run_all(const std::string& filter);

}  // namespace ccap_test

#define CCAP_TEST(test_name)                                                          \
  static void test_name();                                                            \
  namespace {                                                                         \
  const int test_name##_registration =                                                \
      (::ccap_test::register_test(#test_name, &test_name), 0);                        \
  }                                                                                   \
  static void test_name()

#define CCAP_CHECK(expression)                                                        \
  do {                                                                                \
    if (!(expression)) {                                                              \
      ::ccap_test::report_failure(__FILE__, __LINE__, "check failed: " #expression);  \
    }                                                                                 \
  } while (false)

#define CCAP_CHECK_FALSE(expression)                                                  \
  do {                                                                                \
    if (static_cast<bool>(expression)) {                                              \
      ::ccap_test::report_failure(__FILE__, __LINE__,                                 \
                                  "expected false: " #expression);                    \
    }                                                                                 \
  } while (false)

#define CCAP_CHECK_EQ(lhs, rhs)                                                       \
  do {                                                                                \
    const auto& ccap_lhs = (lhs);                                                     \
    const auto& ccap_rhs = (rhs);                                                     \
    if (!(ccap_lhs == ccap_rhs)) {                                                    \
      ::ccap_test::report_failure(                                                    \
          __FILE__, __LINE__,                                                         \
          std::string("expected " #lhs " == " #rhs " but got ") +                     \
              ::ccap_test::describe(ccap_lhs) + " and " +                             \
              ::ccap_test::describe(ccap_rhs));                                       \
    }                                                                                 \
  } while (false)

#define CCAP_CHECK_NE(lhs, rhs)                                                       \
  do {                                                                                \
    const auto& ccap_lhs = (lhs);                                                     \
    const auto& ccap_rhs = (rhs);                                                     \
    if (ccap_lhs == ccap_rhs) {                                                       \
      ::ccap_test::report_failure(__FILE__, __LINE__,                                 \
                                  "expected " #lhs " != " #rhs);                      \
    }                                                                                 \
  } while (false)

#define CCAP_CHECK_LT(lhs, rhs)                                                       \
  do {                                                                                \
    const auto& ccap_lhs = (lhs);                                                     \
    const auto& ccap_rhs = (rhs);                                                     \
    if (!(ccap_lhs < ccap_rhs)) {                                                     \
      ::ccap_test::report_failure(                                                    \
          __FILE__, __LINE__,                                                         \
          std::string("expected " #lhs " < " #rhs " but got ") +                      \
              ::ccap_test::describe(ccap_lhs) + " and " +                             \
              ::ccap_test::describe(ccap_rhs));                                       \
    }                                                                                 \
  } while (false)

// Requires an expression to succeed and yields its value.
#define CCAP_CHECK_OK(name, expression)                                               \
  auto ccap_ok_##name = (expression);                                                 \
  if (!ccap_ok_##name.ok()) {                                                         \
    ::ccap_test::report_failure(__FILE__, __LINE__,                                   \
                                std::string("expected success from " #expression       \
                                            " but got ") +                            \
                                    ccap_ok_##name.error().to_string());              \
    return;                                                                           \
  }                                                                                   \
  auto name = std::move(ccap_ok_##name).value()

// Requires an expression returning Result<void> to succeed.
#define CCAP_CHECK_VOID(expression)                                                   \
  do {                                                                                \
    auto ccap_void_result = (expression);                                             \
    if (!ccap_void_result.ok()) {                                                     \
      ::ccap_test::report_failure(__FILE__, __LINE__,                                 \
                                  std::string("expected success from " #expression     \
                                              " but got ") +                          \
                                      ccap_void_result.error().to_string());          \
    }                                                                                 \
  } while (false)

// Requires an expression to fail with a specific error code.
#define CCAP_CHECK_ERR(expression, expected_code)                                     \
  do {                                                                                \
    auto ccap_err_result = (expression);                                              \
    if (ccap_err_result.ok()) {                                                       \
      ::ccap_test::report_failure(__FILE__, __LINE__,                                 \
                                  "expected failure from " #expression);              \
    } else if (ccap_err_result.error().code() != (expected_code)) {                   \
      ::ccap_test::report_failure(                                                    \
          __FILE__, __LINE__,                                                         \
          std::string("expected " #expected_code " from " #expression " but got ") +  \
              std::string(cooling_capacity::to_string(ccap_err_result.error().code()))); \
    }                                                                                 \
  } while (false)

#endif  // COOLING_CAPACITY_TESTS_SUPPORT_TEST_HARNESS_HPP
