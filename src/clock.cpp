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

#include "cooling_capacity/clock.hpp"

#include <chrono>
#include <cstdint>

#include "cooling_capacity/errors.hpp"

namespace cooling_capacity {

Clock::~Clock() = default;

Timestamp SystemClock::now() const {
  const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch);
  return Timestamp::from_unix_milliseconds(static_cast<std::int64_t>(millis.count()));
}

namespace {

// Inverse of the civil-from-days conversion used by Timestamp::to_string.
std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) {
  year -= month <= 2U ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const auto year_of_era = static_cast<std::uint64_t>(year - era * 400);
  const std::uint64_t month_prime =
      month > 2U ? static_cast<std::uint64_t>(month) - 3U : static_cast<std::uint64_t>(month) + 9U;
  const std::uint64_t day_of_year = (153U * month_prime + 2U) / 5U + day - 1U;
  const std::uint64_t day_of_era =
      year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
  return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

bool is_digit_at(std::string_view text, std::size_t index) {
  return index < text.size() && text[index] >= '0' && text[index] <= '9';
}

std::uint32_t read_digits(std::string_view text, std::size_t index, std::size_t count) {
  std::uint32_t value = 0;
  for (std::size_t offset = 0; offset < count; ++offset) {
    value = value * 10U + static_cast<std::uint32_t>(text[index + offset] - '0');
  }
  return value;
}

bool is_leap_year(std::int64_t year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

unsigned days_in_month(std::int64_t year, unsigned month) {
  constexpr unsigned kLengths[] = {31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
  if (month < 1U || month > 12U) {
    return 0U;
  }
  if (month == 2U && is_leap_year(year)) {
    return 29U;
  }
  return kLengths[month - 1U];
}

}  // namespace

Result<Timestamp> parse_timestamp(std::string_view text) {
  // Exactly "YYYY-MM-DDTHH:MM:SS.mmmZ".
  constexpr std::size_t kExpectedLength = 24;
  if (text.size() != kExpectedLength) {
    return Error(ErrorCode::InvalidArgument,
                 "timestamp must have the exact form YYYY-MM-DDTHH:MM:SS.mmmZ")
        .with("length", std::to_string(text.size()));
  }
  if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':' ||
      text[19] != '.' || text[23] != 'Z') {
    return Error(ErrorCode::InvalidArgument,
                 "timestamp must have the exact form YYYY-MM-DDTHH:MM:SS.mmmZ")
        .with("text", std::string(text));
  }
  for (const std::size_t index : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U, 11U, 12U, 14U, 15U, 17U, 18U,
                                  20U, 21U, 22U}) {
    if (!is_digit_at(text, index)) {
      return Error(ErrorCode::InvalidArgument, "timestamp contains a non-digit")
          .with("text", std::string(text));
    }
  }
  const auto year = static_cast<std::int64_t>(read_digits(text, 0, 4));
  const std::uint32_t month = read_digits(text, 5, 2);
  const std::uint32_t day = read_digits(text, 8, 2);
  const std::uint32_t hour = read_digits(text, 11, 2);
  const std::uint32_t minute = read_digits(text, 14, 2);
  const std::uint32_t second = read_digits(text, 17, 2);
  const std::uint32_t millis = read_digits(text, 20, 3);
  if (month < 1U || month > 12U) {
    return Error(ErrorCode::InvalidArgument, "timestamp month is out of range")
        .with("month", std::to_string(month));
  }
  const unsigned length = days_in_month(year, month);
  if (day < 1U || day > length) {
    return Error(ErrorCode::InvalidArgument, "timestamp day is out of range for its month")
        .with("day", std::to_string(day))
        .with("month", std::to_string(month));
  }
  if (hour > 23U || minute > 59U || second > 59U) {
    return Error(ErrorCode::InvalidArgument, "timestamp time-of-day is out of range");
  }
  const std::int64_t days = days_from_civil(year, month, day);
  const std::int64_t day_millis = static_cast<std::int64_t>(hour) * 3600000 +
                                  static_cast<std::int64_t>(minute) * 60000 +
                                  static_cast<std::int64_t>(second) * 1000 +
                                  static_cast<std::int64_t>(millis);
  return Timestamp::from_unix_milliseconds(days * 86400000 + day_millis);
}

}  // namespace cooling_capacity
