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

#include "cooling_capacity/errors.hpp"

namespace {

using cooling_capacity::Error;
using cooling_capacity::ErrorCategory;
using cooling_capacity::ErrorCode;
using cooling_capacity::Result;

}  // namespace

CCAP_TEST(error_code_names_are_stable) {
  CCAP_CHECK_EQ(cooling_capacity::to_string(ErrorCode::Ok), std::string_view("ok"));
  CCAP_CHECK_EQ(cooling_capacity::to_string(ErrorCode::StaleGeneration),
                std::string_view("stale_generation"));
  CCAP_CHECK_EQ(cooling_capacity::to_string(ErrorCode::DigestMismatch),
                std::string_view("digest_mismatch"));
  CCAP_CHECK_EQ(cooling_capacity::to_string(ErrorCode::Indeterminate),
                std::string_view("indeterminate"));
  CCAP_CHECK_EQ(cooling_capacity::to_string(ErrorCode::WrongEndianness),
                std::string_view("wrong_endianness"));
}

CCAP_TEST(error_categories_are_derived_not_stored) {
  CCAP_CHECK_EQ(cooling_capacity::category_of(ErrorCode::InvalidIdentifier),
                ErrorCategory::InvalidArgument);
  CCAP_CHECK_EQ(cooling_capacity::category_of(ErrorCode::NotFound),
                ErrorCategory::Precondition);
  CCAP_CHECK_EQ(cooling_capacity::category_of(ErrorCode::StaleAuthority),
                ErrorCategory::Precondition);
  CCAP_CHECK_EQ(cooling_capacity::category_of(ErrorCode::Corruption),
                ErrorCategory::Integrity);
  CCAP_CHECK_EQ(cooling_capacity::category_of(ErrorCode::Truncated), ErrorCategory::Integrity);
  CCAP_CHECK_EQ(cooling_capacity::category_of(ErrorCode::LimitExceeded),
                ErrorCategory::Resource);
  CCAP_CHECK_EQ(cooling_capacity::category_of(ErrorCode::Unknown), ErrorCategory::Resource);
  CCAP_CHECK_EQ(cooling_capacity::category_of(ErrorCode::Unsupported), ErrorCategory::Resource);
  CCAP_CHECK_EQ(cooling_capacity::category_of(ErrorCode::Unavailable), ErrorCategory::Resource);
  CCAP_CHECK_EQ(cooling_capacity::category_of(ErrorCode::IoFailure),
                ErrorCategory::Environment);
  CCAP_CHECK_EQ(cooling_capacity::category_of(ErrorCode::LockConflict),
                ErrorCategory::Environment);
  CCAP_CHECK_EQ(cooling_capacity::category_of(ErrorCode::InvariantViolation),
                ErrorCategory::Internal);
}

CCAP_TEST(error_context_renders_deterministically) {
  Error error(ErrorCode::StaleGeneration, "generation moved");
  error.with("zone", "hall-1").with("actor", "operator");
  error.with_generations(4, 7);
  const std::string rendered = error.to_string();
  CCAP_CHECK_EQ(rendered, std::string("stale_generation: generation moved [zone=hall-1, "
                                      "actor=operator, expected_generation=4, "
                                      "actual_generation=7]"));
  CCAP_CHECK_EQ(error.expected_generation().value(), 4U);
  CCAP_CHECK_EQ(error.actual_generation().value(), 7U);
}

CCAP_TEST(error_factories_populate_the_right_code) {
  CCAP_CHECK_EQ(Error::invalid_argument("x").code(), ErrorCode::InvalidArgument);
  CCAP_CHECK_EQ(Error::not_found("x").code(), ErrorCode::NotFound);
  CCAP_CHECK_EQ(Error::corruption("x").code(), ErrorCode::Corruption);
  CCAP_CHECK_EQ(Error::limit_exceeded("x").code(), ErrorCode::LimitExceeded);
  CCAP_CHECK_EQ(Error::unsupported("x").code(), ErrorCode::Unsupported);
  CCAP_CHECK_EQ(Error::invariant("x").code(), ErrorCode::InvariantViolation);
  const Error stale = Error::stale_generation("moved", 2, 5);
  CCAP_CHECK_EQ(stale.code(), ErrorCode::StaleGeneration);
  CCAP_CHECK_EQ(stale.expected_generation().value(), 2U);
  CCAP_CHECK_EQ(stale.actual_generation().value(), 5U);
}

CCAP_TEST(result_carries_one_of_two_states) {
  Result<int> good(7);
  CCAP_CHECK(good.ok());
  CCAP_CHECK_EQ(good.value(), 7);
  CCAP_CHECK_EQ(good.value_or(3), 7);

  Result<int> bad(Error(ErrorCode::NotFound, "absent"));
  CCAP_CHECK_FALSE(bad.ok());
  CCAP_CHECK_EQ(bad.error().code(), ErrorCode::NotFound);
  CCAP_CHECK_EQ(bad.value_or(3), 3);

  Result<void> nothing;
  CCAP_CHECK(nothing.ok());
  Result<void> failed(Error(ErrorCode::Unknown, "cannot say"));
  CCAP_CHECK_FALSE(failed.ok());
  CCAP_CHECK_EQ(failed.error().code(), ErrorCode::Unknown);
}

CCAP_TEST(result_propagates_errors_through_try) {
  const auto inner = [](bool succeed) -> Result<int> {
    if (!succeed) {
      return Error(ErrorCode::Conflict, "conflict");
    }
    return 11;
  };
  const auto outer = [&inner](bool succeed) -> Result<int> {
    CCAP_TRY_DECLARE(value, inner(succeed));
    return value + 1;
  };
  CCAP_CHECK_EQ(outer(true).value(), 12);
  CCAP_CHECK_EQ(outer(false).error().code(), ErrorCode::Conflict);
}
