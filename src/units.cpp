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

#include "cooling_capacity/units.hpp"

#include <cstdio>
#include <limits>

namespace cooling_capacity {

namespace {

constexpr std::int64_t kMilliwattsPerWatt = 1000;

// Renders a signed integer without locale involvement.
std::string signed_decimal(std::int64_t value) {
  char buffer[32];
  const int written = std::snprintf(buffer, sizeof(buffer), "%lld",
                                    static_cast<long long>(value));
  return std::string(buffer, static_cast<std::size_t>(written));
}

// Howard Hinnant's civil-from-days: exact for the whole proleptic Gregorian
// range and free of any time-zone or locale dependency.
void civil_from_days(std::int64_t days, std::int64_t& year, unsigned& month, unsigned& day) {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto day_of_era = static_cast<std::uint64_t>(days - era * 146097);
  const auto year_of_era =
      (day_of_era - day_of_era / 1460U + day_of_era / 36524U - day_of_era / 146096U) / 365U;
  const auto year_value = static_cast<std::int64_t>(year_of_era) + era * 400;
  const auto day_of_year =
      day_of_era - (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
  const auto month_prime = (5U * day_of_year + 2U) / 153U;
  const auto day_value = day_of_year - (153U * month_prime + 2U) / 5U + 1U;
  const auto month_value = month_prime < 10U ? month_prime + 3U : month_prime - 9U;
  year = year_value + (month_value <= 2U ? 1 : 0);
  month = static_cast<unsigned>(month_value);
  day = static_cast<unsigned>(day_value);
}

}  // namespace

Result<ThermalPower> ThermalPower::from_watts(std::int64_t value) noexcept {
  if (value > std::numeric_limits<std::int64_t>::max() / kMilliwattsPerWatt ||
      value < std::numeric_limits<std::int64_t>::min() / kMilliwattsPerWatt) {
    return Error(ErrorCode::ArithmeticOverflow,
                 "watt value does not fit in the milliwatt representation");
  }
  return ThermalPower(value * kMilliwattsPerWatt);
}

std::string ThermalPower::to_string() const {
  return signed_decimal(value_) + " mW";
}

std::string VolumetricFlow::to_string() const {
  return signed_decimal(value_) + " mL/s";
}

std::string Temperature::to_string() const {
  return signed_decimal(value_) + " mC";
}

std::string Fraction::to_string() const {
  const bool negative = value_ < 0;
  const std::uint64_t magnitude =
      negative ? static_cast<std::uint64_t>(-(value_ + 1)) + 1U
               : static_cast<std::uint64_t>(value_);
  const std::uint64_t whole = magnitude / 10000U;
  const std::uint64_t remainder = magnitude % 10000U;
  std::string out;
  if (negative) {
    out.push_back('-');
  }
  out.append(std::to_string(whole));
  if (remainder != 0U) {
    char digits[5];
    const int written =
        std::snprintf(digits, sizeof(digits), "%04llu", static_cast<unsigned long long>(remainder));
    std::string tail(digits, static_cast<std::size_t>(written));
    while (!tail.empty() && tail.back() == '0') {
      tail.pop_back();
    }
    out.push_back('.');
    out.append(tail);
  }
  out.push_back('%');
  return out;
}

Result<Fraction> Fraction::parse_percent(std::string_view text) {
  if (text.empty()) {
    return Error(ErrorCode::InvalidArgument, "percentage text is empty");
  }
  if (text.back() == '%') {
    text.remove_suffix(1);
  }
  if (text.empty()) {
    return Error(ErrorCode::InvalidArgument, "percentage text is a lone percent sign");
  }
  const std::size_t dot = text.find('.');
  std::string_view whole_text = dot == std::string_view::npos ? text : text.substr(0, dot);
  const std::string_view fraction_text =
      dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
  if (whole_text.empty()) {
    return Error(ErrorCode::InvalidArgument, "percentage text has no integer part");
  }
  if (dot != std::string_view::npos && fraction_text.empty()) {
    return Error(ErrorCode::InvalidArgument, "percentage text has no fractional digits");
  }
  if (fraction_text.size() > 4) {
    return Error(ErrorCode::InvalidArgument,
                 "percentage text has more than four fractional digits; it would be rounded")
        .with("text", std::string(text));
  }
  if (whole_text.size() > 1 && whole_text.front() == '0') {
    return Error(ErrorCode::InvalidArgument, "percentage text has a leading zero")
        .with("text", std::string(text));
  }
  std::uint64_t whole = 0;
  for (const char raw : whole_text) {
    if (raw < '0' || raw > '9') {
      return Error(ErrorCode::InvalidArgument, "percentage text contains a non-digit");
    }
    whole = whole * 10U + static_cast<std::uint64_t>(raw - '0');
    if (whole > 1000U) {
      return Error(ErrorCode::InvalidArgument, "percentage text is out of range")
          .with("text", std::string(text));
    }
  }
  std::uint64_t fractional = 0;
  std::uint64_t scale_digits = 1;
  for (const char raw : fraction_text) {
    if (raw < '0' || raw > '9') {
      return Error(ErrorCode::InvalidArgument, "percentage text contains a non-digit");
    }
    fractional = fractional * 10U + static_cast<std::uint64_t>(raw - '0');
    scale_digits *= 10U;
  }
  const std::uint64_t parts = whole * 10000U + (fractional * 10000U) / scale_digits;
  if (parts > static_cast<std::uint64_t>(Fraction::kScale)) {
    return Error(ErrorCode::InvalidArgument,
                 "percentage text is above 100 percent; authoritative fractions cannot exceed one")
        .with("text", std::string(text));
  }
  return Fraction::from_parts_per_million(static_cast<std::int64_t>(parts));
}

std::string DurationMs::to_string() const {
  return signed_decimal(value_) + " ms";
}

std::string Timestamp::to_string() const {
  std::int64_t millis = value_;
  std::int64_t days = millis / 86400000;
  std::int64_t remainder = millis % 86400000;
  if (remainder < 0) {
    remainder += 86400000;
    days -= 1;
  }
  std::int64_t year = 0;
  unsigned month = 0;
  unsigned day = 0;
  civil_from_days(days, year, month, day);
  const auto hours = static_cast<unsigned>(remainder / 3600000);
  remainder %= 3600000;
  const auto minutes = static_cast<unsigned>(remainder / 60000);
  remainder %= 60000;
  const auto seconds = static_cast<unsigned>(remainder / 1000);
  const auto millis_part = static_cast<unsigned>(remainder % 1000);

  char buffer[40];
  const int written =
      std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02u:%02u:%02u.%03uZ",
                    static_cast<long long>(year), month, day, hours, minutes, seconds, millis_part);
  return std::string(buffer, static_cast<std::size_t>(written));
}

Result<ThermalPower> checked_add(ThermalPower lhs, ThermalPower rhs) noexcept {
  const std::int64_t a = lhs.milliwatts();
  const std::int64_t b = rhs.milliwatts();
  if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) {
    return Error(ErrorCode::ArithmeticOverflow, "thermal power addition overflowed");
  }
  if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) {
    return Error(ErrorCode::ArithmeticOverflow, "thermal power addition underflowed");
  }
  return ThermalPower::from_milliwatts(a + b);
}

Result<ThermalPower> checked_sub(ThermalPower lhs, ThermalPower rhs) noexcept {
  const std::int64_t a = lhs.milliwatts();
  const std::int64_t b = rhs.milliwatts();
  if (b < 0 && a > std::numeric_limits<std::int64_t>::max() + b) {
    return Error(ErrorCode::ArithmeticOverflow, "thermal power subtraction overflowed");
  }
  if (b > 0 && a < std::numeric_limits<std::int64_t>::min() + b) {
    return Error(ErrorCode::ArithmeticOverflow, "thermal power subtraction underflowed");
  }
  return ThermalPower::from_milliwatts(a - b);
}

Result<ThermalPower> scale(ThermalPower value, Fraction factor) noexcept {
  if (value.is_negative()) {
    return Error(ErrorCode::InvalidArgument, "cannot scale a negative thermal power");
  }
  if (!factor.in_unit_interval()) {
    return Error(ErrorCode::InvalidArgument, "scaling fraction is outside [0, 1]")
        .with("parts_per_million", std::to_string(factor.parts_per_million()));
  }
  const std::int64_t raw = value.milliwatts();
  const std::int64_t parts = factor.parts_per_million();
  if (parts == 0 || raw == 0) {
    return ThermalPower::from_milliwatts(0);
  }
  // floor(raw * parts / 1e6) computed without a 128-bit intermediate:
  // raw = high * 1e6 + low, so the result is high * parts + floor(low * parts / 1e6).
  const std::int64_t high = raw / Fraction::kScale;
  const std::int64_t low = raw % Fraction::kScale;
  const std::int64_t high_product = high * parts;  // <= raw because parts <= 1e6
  const std::int64_t low_product = (low * parts) / Fraction::kScale;  // <= 1e6
  if (high_product > std::numeric_limits<std::int64_t>::max() - low_product) {
    return Error(ErrorCode::ArithmeticOverflow, "scaled thermal power overflowed");
  }
  return ThermalPower::from_milliwatts(high_product + low_product);
}

Result<Fraction> compose(Fraction lhs, Fraction rhs) noexcept {
  if (!lhs.in_unit_interval() || !rhs.in_unit_interval()) {
    return Error(ErrorCode::InvalidArgument, "fraction composition requires fractions in [0, 1]");
  }
  const std::int64_t product = lhs.parts_per_million() * rhs.parts_per_million();
  return Fraction::from_parts_per_million(product / Fraction::kScale);
}

Result<Fraction> complement(Fraction factor) noexcept {
  if (!factor.in_unit_interval()) {
    return Error(ErrorCode::InvalidArgument, "fraction complement requires a fraction in [0, 1]")
        .with("parts_per_million", std::to_string(factor.parts_per_million()));
  }
  return Fraction::from_parts_per_million(Fraction::kScale - factor.parts_per_million());
}

Result<ThermalPower> apply_reserve(ThermalPower value, Fraction reserve) noexcept {
  CCAP_TRY_DECLARE(kept, complement(reserve));
  return scale(value, kept);
}

bool within_window(DurationMs age, DurationMs window) noexcept {
  if (age.is_negative()) {
    return false;
  }
  if (window.never_expires()) {
    return true;
  }
  if (window.is_negative()) {
    return false;
  }
  return age.milliseconds() <= window.milliseconds();
}

}  // namespace cooling_capacity
