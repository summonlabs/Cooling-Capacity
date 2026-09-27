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

#ifndef COOLING_CAPACITY_ERRORS_HPP
#define COOLING_CAPACITY_ERRORS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "cooling_capacity/export.hpp"

namespace cooling_capacity {

// Stable machine-readable outcome codes. The numeric values are part of the
// public contract: they are serialized, printed by the command line tool and
// asserted by tests. Values are never reused or renumbered.
enum class ErrorCode : std::uint32_t {
  Ok = 0,

  // Caller supplied something that cannot be accepted as-is.
  InvalidArgument = 1,
  InvalidIdentifier = 2,
  InvalidEnumValue = 3,
  InvalidUtf8 = 4,
  InvalidText = 5,
  DuplicateIdentity = 6,
  MissingReference = 7,
  DanglingReference = 8,
  SelfReference = 9,
  CyclicReference = 10,

  // Preconditions on current authority.
  NotFound = 20,
  AlreadyExists = 21,
  Conflict = 22,
  StaleGeneration = 23,
  StaleAuthority = 24,
  StaleEpoch = 25,
  StaleEvidence = 26,
  PreconditionFailed = 27,
  ReadOnly = 28,
  NotOpen = 29,
  Closed = 30,
  WriterBusy = 31,
  IdempotencyConflict = 32,
  CapacityExceeded = 33,
  IncompatibleClass = 34,
  IncompatibleMedium = 35,
  RedundancyUnsatisfied = 36,
  ReserveViolation = 37,

  // Serialized input that cannot be trusted.
  IncompatibleVersion = 40,
  Corruption = 41,
  Truncated = 42,
  Oversized = 43,
  WrongEndianness = 44,
  DigestMismatch = 45,
  MissingManifest = 46,
  UnsupportedFormat = 47,

  // Resource and environment limits.
  LimitExceeded = 60,
  ArithmeticOverflow = 61,
  Unsupported = 62,
  Unavailable = 63,
  Unknown = 64,
  Indeterminate = 65,
  PermissionDenied = 70,
  IoFailure = 71,
  LockConflict = 72,
  PathInvalid = 73,
  ProcessFailure = 74,
  InvariantViolation = 90,
  InternalError = 91,
};

// Coarse grouping of ErrorCode. The category is derived, never stored, so a
// code can never disagree with its own category.
enum class ErrorCategory : std::uint8_t {
  Ok = 0,
  InvalidArgument = 1,
  Precondition = 2,
  Integrity = 3,
  Resource = 4,
  Environment = 5,
  Internal = 6,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(ErrorCode code) noexcept;
[[nodiscard]] CCAP_EXPORT std::string_view to_string(ErrorCategory category) noexcept;
[[nodiscard]] CCAP_EXPORT ErrorCategory category_of(ErrorCode code) noexcept;

// Optional key/value detail attached to an Error. Insertion order is preserved
// so that a rendered error is deterministic.
struct ErrorContext {
  std::string key;
  std::string value;

  friend bool operator==(const ErrorContext& lhs, const ErrorContext& rhs) {
    return lhs.key == rhs.key && lhs.value == rhs.value;
  }
};

class CCAP_EXPORT Error {
 public:
  Error() : code_(ErrorCode::Ok) {}
  Error(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] ErrorCategory category() const noexcept { return category_of(code_); }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] const std::vector<ErrorContext>& context() const noexcept { return context_; }
  [[nodiscard]] const std::optional<std::uint64_t>& expected_generation() const noexcept {
    return expected_generation_;
  }
  [[nodiscard]] const std::optional<std::uint64_t>& actual_generation() const noexcept {
    return actual_generation_;
  }

  // Detail chaining. Returns *this so that factories read as one expression.
  Error& with(std::string key, std::string value);
  Error& with_generations(std::uint64_t expected, std::uint64_t actual);
  [[nodiscard]] Error copy_with(std::string key, std::string value) const;

  // "code: message [key=value, ...]" plus generation detail when present.
  [[nodiscard]] std::string to_string() const;

  [[nodiscard]] bool operator==(const Error& other) const {
    return code_ == other.code_ && message_ == other.message_ && context_ == other.context_ &&
           expected_generation_ == other.expected_generation_ &&
           actual_generation_ == other.actual_generation_;
  }

  // Convenience factories for the codes used most often by the implementation.
  static Error invalid_argument(std::string message);
  static Error not_found(std::string message);
  static Error stale_generation(std::string message, std::uint64_t expected,
                                std::uint64_t actual);
  static Error corruption(std::string message);
  static Error limit_exceeded(std::string message);
  static Error unsupported(std::string message);
  static Error invariant(std::string message);

 private:
  ErrorCode code_;
  std::string message_;
  std::vector<ErrorContext> context_;
  std::optional<std::uint64_t> expected_generation_;
  std::optional<std::uint64_t> actual_generation_;
};

namespace detail {

[[noreturn]] CCAP_EXPORT void contract_violation(const char* expression, const char* file,
                                                 int line) noexcept;

}  // namespace detail

// Result<T> carries either a value or an Error, never both and never neither.
// It deliberately has no throwing accessor: reading the value of a failed
// Result is a programming error and terminates with a diagnostic.
template <class T>
class Result {
 public:
  using value_type = T;

  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
  Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

  Result(const Result&) = default;
  Result(Result&&) noexcept = default;
  Result& operator=(const Result&) = default;
  Result& operator=(Result&&) noexcept = default;
  ~Result() = default;

  [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const T& value() const& {
    if (!ok()) {
      detail::contract_violation("Result::value() on an error result", __FILE__, __LINE__);
    }
    return std::get<0>(storage_);
  }
  [[nodiscard]] T& value() & {
    if (!ok()) {
      detail::contract_violation("Result::value() on an error result", __FILE__, __LINE__);
    }
    return std::get<0>(storage_);
  }
  [[nodiscard]] T&& value() && {
    if (!ok()) {
      detail::contract_violation("Result::value() on an error result", __FILE__, __LINE__);
    }
    return std::get<0>(std::move(storage_));
  }

  [[nodiscard]] const Error& error() const& {
    if (ok()) {
      detail::contract_violation("Result::error() on an ok result", __FILE__, __LINE__);
    }
    return std::get<1>(storage_);
  }
  [[nodiscard]] Error& error() & {
    if (ok()) {
      detail::contract_violation("Result::error() on an ok result", __FILE__, __LINE__);
    }
    return std::get<1>(storage_);
  }

  template <class U>
  [[nodiscard]] T value_or(U&& fallback) const& {
    return ok() ? std::get<0>(storage_) : static_cast<T>(std::forward<U>(fallback));
  }

 private:
  std::variant<T, Error> storage_;
};

// The value type of a successful Result<void>. It exists so that generic code
// and test harnesses can treat every Result uniformly; it carries no data.
struct Unit {
  friend constexpr bool operator==(Unit, Unit) noexcept { return true; }
  friend constexpr bool operator!=(Unit, Unit) noexcept { return false; }
};

template <>
class Result<void> {
 public:
  using value_type = void;

  Result() : error_(std::nullopt) {}
  Result(Error error) : error_(std::move(error)) {}

  [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] Unit value() const {
    if (!ok()) {
      detail::contract_violation("Result<void>::value() on an error result", __FILE__, __LINE__);
    }
    return Unit{};
  }

  [[nodiscard]] const Error& error() const& {
    if (ok()) {
      detail::contract_violation("Result<void>::error() on an ok result", __FILE__, __LINE__);
    }
    return *error_;
  }

 private:
  std::optional<Error> error_;
};

}  // namespace cooling_capacity

// Propagates the error of an inner Result out of the enclosing function. The
// enclosing function must return a Result type.
#define CCAP_TRY(expression)                                              \
  do {                                                                    \
    auto&& ccap_try_result = (expression);                                \
    if (!ccap_try_result.ok()) {                                          \
      return ::cooling_capacity::Error(ccap_try_result.error());          \
    }                                                                     \
  } while (false)

// Declares `name` from the value of an inner Result, propagating the error.
// Must be used where a declaration is legal.
#define CCAP_TRY_DECLARE(name, expression)                                 \
  auto ccap_try_##name = (expression);                                     \
  if (!ccap_try_##name.ok()) {                                             \
    return ::cooling_capacity::Error(ccap_try_##name.error());             \
  }                                                                        \
  auto name = std::move(ccap_try_##name).value()

#endif  // COOLING_CAPACITY_ERRORS_HPP
