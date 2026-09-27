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

#ifndef COOLING_CAPACITY_UNITS_HPP
#define COOLING_CAPACITY_UNITS_HPP

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "cooling_capacity/errors.hpp"
#include "cooling_capacity/export.hpp"

namespace cooling_capacity {

// Every authoritative quantity in this library is an exact signed integer in a
// single canonical fixed unit. Floating point never takes part in capacity
// accounting. Each quantity is a distinct type, so no two of them can be added,
// compared or assigned to one another even though they share a representation.

// Thermal-removal power, in milliwatts.
class CCAP_EXPORT ThermalPower {
 public:
  constexpr ThermalPower() = default;
  [[nodiscard]] static constexpr ThermalPower from_milliwatts(std::int64_t value) noexcept {
    return ThermalPower(value);
  }
  [[nodiscard]] static Result<ThermalPower> from_watts(std::int64_t value) noexcept;

  [[nodiscard]] constexpr std::int64_t milliwatts() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return value_ < 0; }

  friend constexpr bool operator==(ThermalPower lhs, ThermalPower rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend constexpr bool operator!=(ThermalPower lhs, ThermalPower rhs) noexcept {
    return !(lhs == rhs);
  }
  friend constexpr bool operator<(ThermalPower lhs, ThermalPower rhs) noexcept {
    return lhs.value_ < rhs.value_;
  }
  friend constexpr bool operator>(ThermalPower lhs, ThermalPower rhs) noexcept { return rhs < lhs; }
  friend constexpr bool operator<=(ThermalPower lhs, ThermalPower rhs) noexcept {
    return !(rhs < lhs);
  }
  friend constexpr bool operator>=(ThermalPower lhs, ThermalPower rhs) noexcept {
    return !(lhs < rhs);
  }

  // "1234 mW" and "1.234 W" renderings. Both are exact and deterministic.
  [[nodiscard]] std::string to_string() const;

 private:
  constexpr explicit ThermalPower(std::int64_t value) noexcept : value_(value) {}
  std::int64_t value_ = 0;
};

// Volumetric flow, in millilitres per second. Used by evidence payloads that
// report a measured or commissioned flow; it is never converted into thermal
// capacity by this library.
class CCAP_EXPORT VolumetricFlow {
 public:
  constexpr VolumetricFlow() = default;
  [[nodiscard]] static constexpr VolumetricFlow from_millilitres_per_second(
      std::int64_t value) noexcept {
    return VolumetricFlow(value);
  }

  [[nodiscard]] constexpr std::int64_t millilitres_per_second() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }

  friend constexpr bool operator==(VolumetricFlow lhs, VolumetricFlow rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend constexpr bool operator!=(VolumetricFlow lhs, VolumetricFlow rhs) noexcept {
    return !(lhs == rhs);
  }
  friend constexpr bool operator<(VolumetricFlow lhs, VolumetricFlow rhs) noexcept {
    return lhs.value_ < rhs.value_;
  }

  [[nodiscard]] std::string to_string() const;

 private:
  constexpr explicit VolumetricFlow(std::int64_t value) noexcept : value_(value) {}
  std::int64_t value_ = 0;
};

// Temperature, in millidegrees Celsius.
class CCAP_EXPORT Temperature {
 public:
  constexpr Temperature() = default;
  [[nodiscard]] static constexpr Temperature from_millidegrees_celsius(
      std::int64_t value) noexcept {
    return Temperature(value);
  }

  [[nodiscard]] constexpr std::int64_t millidegrees_celsius() const noexcept { return value_; }

  friend constexpr bool operator==(Temperature lhs, Temperature rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend constexpr bool operator!=(Temperature lhs, Temperature rhs) noexcept {
    return !(lhs == rhs);
  }
  friend constexpr bool operator<(Temperature lhs, Temperature rhs) noexcept {
    return lhs.value_ < rhs.value_;
  }

  [[nodiscard]] std::string to_string() const;

 private:
  constexpr explicit Temperature(std::int64_t value) noexcept : value_(value) {}
  std::int64_t value_ = 0;
};

// An exact ratio expressed in parts per million. Authoritative ratios are
// always in [0, 1000000]: a derate, degradation or reserve fraction above 1 is
// rejected rather than clamped.
class CCAP_EXPORT Fraction {
 public:
  static constexpr std::int64_t kScale = 1000000;

  constexpr Fraction() = default;
  [[nodiscard]] static constexpr Fraction from_parts_per_million(std::int64_t value) noexcept {
    return Fraction(value);
  }
  [[nodiscard]] static Fraction one() noexcept { return Fraction(kScale); }
  [[nodiscard]] static Fraction zero() noexcept { return Fraction(0); }

  [[nodiscard]] static Result<Fraction> parse_percent(std::string_view text);

  [[nodiscard]] constexpr std::int64_t parts_per_million() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }
  [[nodiscard]] constexpr bool is_one() const noexcept { return value_ == kScale; }
  [[nodiscard]] constexpr bool in_unit_interval() const noexcept {
    return value_ >= 0 && value_ <= kScale;
  }

  friend constexpr bool operator==(Fraction lhs, Fraction rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend constexpr bool operator!=(Fraction lhs, Fraction rhs) noexcept { return !(lhs == rhs); }
  friend constexpr bool operator<(Fraction lhs, Fraction rhs) noexcept {
    return lhs.value_ < rhs.value_;
  }
  friend constexpr bool operator>(Fraction lhs, Fraction rhs) noexcept { return rhs < lhs; }
  friend constexpr bool operator<=(Fraction lhs, Fraction rhs) noexcept { return !(rhs < lhs); }
  friend constexpr bool operator>=(Fraction lhs, Fraction rhs) noexcept { return !(lhs < rhs); }

  // "87.5%" with exact integer formatting and trailing zeros trimmed.
  [[nodiscard]] std::string to_string() const;

 private:
  constexpr explicit Fraction(std::int64_t value) noexcept : value_(value) {}
  std::int64_t value_ = 0;
};

// A signed duration in milliseconds. Non-negative wherever it is used as an
// age or a window length; the type itself allows negative values so that
// subtraction results can be represented before they are validated.
class CCAP_EXPORT DurationMs {
 public:
  constexpr DurationMs() = default;
  [[nodiscard]] static constexpr DurationMs from_milliseconds(std::int64_t value) noexcept {
    return DurationMs(value);
  }
  [[nodiscard]] static constexpr DurationMs zero() noexcept { return DurationMs(0); }

  // Sentinel used by a freshness rule to mean "this evidence kind never
  // expires". It is an explicit value, not a convention about zero.
  [[nodiscard]] static constexpr DurationMs no_expiry() noexcept {
    return DurationMs(std::numeric_limits<std::int64_t>::max());
  }
  [[nodiscard]] constexpr bool never_expires() const noexcept {
    return value_ == std::numeric_limits<std::int64_t>::max();
  }

  [[nodiscard]] constexpr std::int64_t milliseconds() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return value_ < 0; }

  friend constexpr bool operator==(DurationMs lhs, DurationMs rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend constexpr bool operator!=(DurationMs lhs, DurationMs rhs) noexcept {
    return !(lhs == rhs);
  }
  friend constexpr bool operator<(DurationMs lhs, DurationMs rhs) noexcept {
    return lhs.value_ < rhs.value_;
  }
  friend constexpr bool operator>(DurationMs lhs, DurationMs rhs) noexcept { return rhs < lhs; }
  friend constexpr bool operator<=(DurationMs lhs, DurationMs rhs) noexcept {
    return !(rhs < lhs);
  }
  friend constexpr bool operator>=(DurationMs lhs, DurationMs rhs) noexcept {
    return !(lhs < rhs);
  }

  [[nodiscard]] std::string to_string() const;

 private:
  constexpr explicit DurationMs(std::int64_t value) noexcept : value_(value) {}
  std::int64_t value_ = 0;
};

// An absolute instant, in milliseconds since the Unix epoch, UTC.
class CCAP_EXPORT Timestamp {
 public:
  constexpr Timestamp() = default;
  [[nodiscard]] static constexpr Timestamp from_unix_milliseconds(std::int64_t value) noexcept {
    return Timestamp(value);
  }
  [[nodiscard]] static constexpr Timestamp epoch() noexcept { return Timestamp(0); }

  [[nodiscard]] constexpr std::int64_t unix_milliseconds() const noexcept { return value_; }

  friend constexpr bool operator==(Timestamp lhs, Timestamp rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend constexpr bool operator!=(Timestamp lhs, Timestamp rhs) noexcept { return !(lhs == rhs); }
  friend constexpr bool operator<(Timestamp lhs, Timestamp rhs) noexcept {
    return lhs.value_ < rhs.value_;
  }
  friend constexpr bool operator>(Timestamp lhs, Timestamp rhs) noexcept { return rhs < lhs; }
  friend constexpr bool operator<=(Timestamp lhs, Timestamp rhs) noexcept { return !(rhs < lhs); }
  friend constexpr bool operator>=(Timestamp lhs, Timestamp rhs) noexcept { return !(lhs < rhs); }

  // "2026-01-02T03:04:05.678Z"; always UTC, always the same width.
  [[nodiscard]] std::string to_string() const;

 private:
  constexpr explicit Timestamp(std::int64_t value) noexcept : value_(value) {}
  std::int64_t value_ = 0;
};

// Checked arithmetic. Overflow is reported, never wrapped.
[[nodiscard]] CCAP_EXPORT Result<ThermalPower> checked_add(ThermalPower lhs,
                                                           ThermalPower rhs) noexcept;
[[nodiscard]] CCAP_EXPORT Result<ThermalPower> checked_sub(ThermalPower lhs,
                                                           ThermalPower rhs) noexcept;

// floor(value * factor / 1000000) computed in exact integer arithmetic. Both
// operands must be non-negative. Rounding is always towards zero, so every
// derived capacity is conservative.
[[nodiscard]] CCAP_EXPORT Result<ThermalPower> scale(ThermalPower value, Fraction factor) noexcept;

// floor(lhs * rhs / 1000000), the composition of two unit-interval fractions.
[[nodiscard]] CCAP_EXPORT Result<Fraction> compose(Fraction lhs, Fraction rhs) noexcept;

// Exact complement: 1000000 - factor. Rejects a fraction outside [0, 1000000].
[[nodiscard]] CCAP_EXPORT Result<Fraction> complement(Fraction factor) noexcept;

// The remaining part of a value after removing a reserved fraction:
// value - floor(value * fraction / 1000000).
[[nodiscard]] CCAP_EXPORT Result<ThermalPower> apply_reserve(ThermalPower value,
                                                             Fraction reserve) noexcept;

// Clamps a measured age to a policy window comparison. Returns false when the
// age exceeds the window. A negative age (evidence stamped in the future) is
// treated as not fresh: it is a provenance defect, not a licence to trust.
[[nodiscard]] CCAP_EXPORT bool within_window(DurationMs age, DurationMs window) noexcept;

}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_UNITS_HPP
