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

#include <limits>

#include "cooling_capacity/clock.hpp"
#include "cooling_capacity/text.hpp"
#include "cooling_capacity/units.hpp"

namespace {

using cooling_capacity::DurationMs;
using cooling_capacity::ErrorCode;
using cooling_capacity::Fraction;
using cooling_capacity::Temperature;
using cooling_capacity::ThermalPower;
using cooling_capacity::Timestamp;
using cooling_capacity::VolumetricFlow;

}  // namespace

CCAP_TEST(thermal_power_is_exact_integer_milliwatts) {
  CCAP_CHECK_OK(power, ThermalPower::from_watts(400));
  CCAP_CHECK_EQ(power.milliwatts(), 400000);
  CCAP_CHECK_EQ(power.to_string(), std::string("400000 mW"));
  const ThermalPower small = ThermalPower::from_milliwatts(-5);
  CCAP_CHECK(small.is_negative());
  CCAP_CHECK(ThermalPower::from_milliwatts(0).is_zero());
}

CCAP_TEST(thermal_power_watt_conversion_is_checked) {
  CCAP_CHECK_ERR(ThermalPower::from_watts(std::numeric_limits<std::int64_t>::max()),
                 ErrorCode::ArithmeticOverflow);
  CCAP_CHECK_ERR(ThermalPower::from_watts(std::numeric_limits<std::int64_t>::min()),
                 ErrorCode::ArithmeticOverflow);
}

CCAP_TEST(thermal_power_addition_is_checked) {
  CCAP_CHECK_OK(sum, checked_add(ThermalPower::from_milliwatts(3),
                                 ThermalPower::from_milliwatts(4)));
  CCAP_CHECK_EQ(sum.milliwatts(), 7);

  CCAP_CHECK_OK(difference, checked_sub(ThermalPower::from_milliwatts(3),
                                        ThermalPower::from_milliwatts(4)));
  CCAP_CHECK_EQ(difference.milliwatts(), -1);

  const ThermalPower maximum = ThermalPower::from_milliwatts(
      std::numeric_limits<std::int64_t>::max());
  CCAP_CHECK_ERR(checked_add(maximum, ThermalPower::from_milliwatts(1)),
                 ErrorCode::ArithmeticOverflow);
  const ThermalPower minimum = ThermalPower::from_milliwatts(
      std::numeric_limits<std::int64_t>::min());
  CCAP_CHECK_ERR(checked_sub(minimum, ThermalPower::from_milliwatts(1)),
                 ErrorCode::ArithmeticOverflow);
}

CCAP_TEST(scaling_rounds_down_and_stays_exact) {
  // 100 mW at 33.3333 percent is 33.3333 mW, which rounds down to 33 mW.
  CCAP_CHECK_OK(scaled, scale(ThermalPower::from_milliwatts(100),
                              Fraction::from_parts_per_million(333333)));
  CCAP_CHECK_EQ(scaled.milliwatts(), 33);

  CCAP_CHECK_OK(whole, scale(ThermalPower::from_milliwatts(400000000),
                             Fraction::from_parts_per_million(900000)));
  CCAP_CHECK_EQ(whole.milliwatts(), 360000000);

  CCAP_CHECK_OK(identity, scale(ThermalPower::from_milliwatts(12345), Fraction::one()));
  CCAP_CHECK_EQ(identity.milliwatts(), 12345);

  CCAP_CHECK_OK(zero, scale(ThermalPower::from_milliwatts(12345), Fraction::zero()));
  CCAP_CHECK_EQ(zero.milliwatts(), 0);

  // Large values must not overflow the intermediate product.
  const std::int64_t large = 9000000000000000LL;
  CCAP_CHECK_OK(big, scale(ThermalPower::from_milliwatts(large),
                           Fraction::from_parts_per_million(500000)));
  CCAP_CHECK_EQ(big.milliwatts(), large / 2);
}

CCAP_TEST(scaling_rejects_out_of_range_input) {
  CCAP_CHECK_ERR(scale(ThermalPower::from_milliwatts(-1), Fraction::one()),
                 ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(scale(ThermalPower::from_milliwatts(1),
                       Fraction::from_parts_per_million(1000001)),
                 ErrorCode::InvalidArgument);
}

CCAP_TEST(fraction_composition_and_complement) {
  CCAP_CHECK_OK(composed, compose(Fraction::from_parts_per_million(900000),
                                  Fraction::from_parts_per_million(900000)));
  CCAP_CHECK_EQ(composed.parts_per_million(), 810000);

  CCAP_CHECK_OK(inverted, complement(Fraction::from_parts_per_million(250000)));
  CCAP_CHECK_EQ(inverted.parts_per_million(), 750000);

  CCAP_CHECK_OK(kept, apply_reserve(ThermalPower::from_milliwatts(1000),
                                    Fraction::from_parts_per_million(100000)));
  CCAP_CHECK_EQ(kept.milliwatts(), 900);

  CCAP_CHECK_ERR(compose(Fraction::from_parts_per_million(1000001), Fraction::one()),
                 ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(complement(Fraction::from_parts_per_million(-1)),
                 ErrorCode::InvalidArgument);
}

CCAP_TEST(fraction_formatting_and_parsing) {
  CCAP_CHECK_EQ(Fraction::from_parts_per_million(1000000).to_string(), std::string("100%"));
  CCAP_CHECK_EQ(Fraction::from_parts_per_million(875000).to_string(), std::string("87.5%"));
  CCAP_CHECK_EQ(Fraction::from_parts_per_million(1).to_string(), std::string("0.0001%"));
  CCAP_CHECK_EQ(Fraction::from_parts_per_million(0).to_string(), std::string("0%"));

  CCAP_CHECK_OK(parsed, Fraction::parse_percent("87.5"));
  CCAP_CHECK_EQ(parsed.parts_per_million(), 875000);
  CCAP_CHECK_OK(with_sign, Fraction::parse_percent("87.5%"));
  CCAP_CHECK_EQ(with_sign.parts_per_million(), 875000);
  CCAP_CHECK_OK(small, Fraction::parse_percent("0.0001"));
  CCAP_CHECK_EQ(small.parts_per_million(), 1);
  CCAP_CHECK_OK(full, Fraction::parse_percent("100"));
  CCAP_CHECK_EQ(full.parts_per_million(), 1000000);

  CCAP_CHECK_ERR(Fraction::parse_percent("100.0001"), ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(Fraction::parse_percent("0.00001"), ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(Fraction::parse_percent(""), ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(Fraction::parse_percent("%"), ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(Fraction::parse_percent("1."), ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(Fraction::parse_percent(".5"), ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(Fraction::parse_percent("00.5"), ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(Fraction::parse_percent("1x"), ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(Fraction::parse_percent("-1"), ErrorCode::InvalidArgument);
}

CCAP_TEST(timestamp_formatting_is_canonical_utc) {
  const Timestamp instant = Timestamp::from_unix_milliseconds(1767225600000LL);
  CCAP_CHECK_EQ(instant.to_string(), std::string("2026-01-01T00:00:00.000Z"));
  CCAP_CHECK_EQ(Timestamp::epoch().to_string(), std::string("1970-01-01T00:00:00.000Z"));
  const Timestamp negative = Timestamp::from_unix_milliseconds(-1);
  CCAP_CHECK_EQ(negative.to_string(), std::string("1969-12-31T23:59:59.999Z"));
}

CCAP_TEST(timestamp_parsing_is_strict) {
  CCAP_CHECK_OK(parsed, cooling_capacity::parse_timestamp("2026-01-01T00:00:00.000Z"));
  CCAP_CHECK_EQ(parsed.unix_milliseconds(), 1767225600000LL);
  CCAP_CHECK_OK(leap, cooling_capacity::parse_timestamp("2024-02-29T12:34:56.789Z"));
  CCAP_CHECK_EQ(leap.to_string(), std::string("2024-02-29T12:34:56.789Z"));

  CCAP_CHECK_ERR(cooling_capacity::parse_timestamp("2023-02-29T00:00:00.000Z"),
                 ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(cooling_capacity::parse_timestamp("2026-13-01T00:00:00.000Z"),
                 ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(cooling_capacity::parse_timestamp("2026-01-01T24:00:00.000Z"),
                 ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(cooling_capacity::parse_timestamp("2026-01-01 00:00:00.000Z"),
                 ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(cooling_capacity::parse_timestamp("2026-01-01T00:00:00Z"),
                 ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(cooling_capacity::parse_timestamp(""), ErrorCode::InvalidArgument);
}

CCAP_TEST(freshness_window_comparison) {
  CCAP_CHECK(within_window(DurationMs::from_milliseconds(0),
                           DurationMs::from_milliseconds(0)));
  CCAP_CHECK(within_window(DurationMs::from_milliseconds(100),
                           DurationMs::from_milliseconds(100)));
  CCAP_CHECK_FALSE(within_window(DurationMs::from_milliseconds(101),
                                 DurationMs::from_milliseconds(100)));
  CCAP_CHECK(within_window(DurationMs::from_milliseconds(100000), DurationMs::no_expiry()));
  CCAP_CHECK(DurationMs::no_expiry().never_expires());
  // Evidence stamped in the future is not fresh; it is a provenance defect.
  CCAP_CHECK_FALSE(within_window(DurationMs::from_milliseconds(-1), DurationMs::no_expiry()));
  CCAP_CHECK_FALSE(within_window(DurationMs::from_milliseconds(0),
                                 DurationMs::from_milliseconds(-1)));
}

CCAP_TEST(other_quantities_render_their_canonical_unit) {
  CCAP_CHECK_EQ(VolumetricFlow::from_millilitres_per_second(1500).to_string(),
                std::string("1500 mL/s"));
  CCAP_CHECK_EQ(Temperature::from_millidegrees_celsius(22500).to_string(),
                std::string("22500 mC"));
  CCAP_CHECK_EQ(DurationMs::from_milliseconds(250).to_string(), std::string("250 ms"));
}

CCAP_TEST(text_validation_is_strict) {
  CCAP_CHECK(cooling_capacity::is_valid_utf8("plain ascii"));
  CCAP_CHECK(cooling_capacity::is_valid_utf8("\xC2\xA9"));
  CCAP_CHECK(cooling_capacity::is_valid_utf8("\xC4\x80"));
  CCAP_CHECK_FALSE(cooling_capacity::is_valid_utf8("\xC0\x80"));
  CCAP_CHECK_FALSE(cooling_capacity::is_valid_utf8("\xED\xA0\x80"));
  CCAP_CHECK_FALSE(cooling_capacity::is_valid_utf8("\xF5\x80\x80\x80"));
  CCAP_CHECK_FALSE(cooling_capacity::is_valid_utf8("\x80"));

  CCAP_CHECK(cooling_capacity::is_printable_text("\xC4\x80"));
  CCAP_CHECK_FALSE(cooling_capacity::is_printable_text("line\nbreak"));
  CCAP_CHECK_FALSE(cooling_capacity::is_printable_text("tab\there"));
  CCAP_CHECK_FALSE(cooling_capacity::is_printable_text("c1\xC2\x80"));
}

CCAP_TEST(canonical_decimal_parsing) {
  CCAP_CHECK_OK(value, cooling_capacity::parse_uint64("0"));
  CCAP_CHECK_EQ(value, 0U);
  CCAP_CHECK_OK(big, cooling_capacity::parse_uint64("18446744073709551615"));
  CCAP_CHECK_EQ(big, std::numeric_limits<std::uint64_t>::max());
  CCAP_CHECK_ERR(cooling_capacity::parse_uint64("18446744073709551616"),
                 ErrorCode::ArithmeticOverflow);
  CCAP_CHECK_ERR(cooling_capacity::parse_uint64("007"), ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(cooling_capacity::parse_uint64("+7"), ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(cooling_capacity::parse_uint64(" 7"), ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(cooling_capacity::parse_uint64(""), ErrorCode::InvalidArgument);

  CCAP_CHECK_OK(signed_value, cooling_capacity::parse_int64("-9"));
  CCAP_CHECK_EQ(signed_value, -9);
  CCAP_CHECK_OK(minimum, cooling_capacity::parse_int64("-9223372036854775808"));
  CCAP_CHECK_EQ(minimum, std::numeric_limits<std::int64_t>::min());
  CCAP_CHECK_ERR(cooling_capacity::parse_int64("-9223372036854775809"),
                 ErrorCode::ArithmeticOverflow);
  CCAP_CHECK_ERR(cooling_capacity::parse_int64("-"), ErrorCode::InvalidArgument);
}
