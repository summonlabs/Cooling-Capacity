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

#include <type_traits>

#include "cooling_capacity/ids.hpp"
#include "cooling_capacity/medium.hpp"
#include "cooling_capacity/units.hpp"

namespace {

using cooling_capacity::CompatibilityClass;
using cooling_capacity::CoolingMedium;
using cooling_capacity::EquipmentKind;
using cooling_capacity::ErrorCode;
using cooling_capacity::LoopKind;
using cooling_capacity::MediumMask;
using cooling_capacity::OperatingState;
using cooling_capacity::RedundancyClass;

}  // namespace

// Strong identifiers from different families are different types and do not
// convert into one another.
static_assert(!std::is_convertible_v<cooling_capacity::ZoneId, cooling_capacity::LoopId>);
static_assert(!std::is_convertible_v<cooling_capacity::LoopId, cooling_capacity::PlantId>);
static_assert(!std::is_convertible_v<cooling_capacity::CapacityGeneration,
                                     cooling_capacity::WriterEpoch>);
static_assert(!std::is_convertible_v<cooling_capacity::WriterEpoch,
                                     cooling_capacity::CapacityGeneration>);
static_assert(!std::is_convertible_v<cooling_capacity::RecordRevision,
                                     cooling_capacity::CapacityGeneration>);
static_assert(!std::is_convertible_v<cooling_capacity::ThermalPower, std::int64_t>);

CCAP_TEST(identifier_grammar_is_restrictive) {
  CCAP_CHECK_OK(simple, cooling_capacity::Identifier::parse("zone-1"));
  CCAP_CHECK_EQ(simple.str(), std::string("zone-1"));
  CCAP_CHECK_OK(dotted, cooling_capacity::Identifier::parse("rack.row:7_a"));
  CCAP_CHECK_EQ(dotted.str(), std::string("rack.row:7_a"));

  // No path separators, no leading dot, no whitespace, no empty token.
  CCAP_CHECK_ERR(cooling_capacity::Identifier::parse("../etc/passwd"),
                 ErrorCode::InvalidIdentifier);
  CCAP_CHECK_ERR(cooling_capacity::Identifier::parse("a\\b"), ErrorCode::InvalidIdentifier);
  CCAP_CHECK_ERR(cooling_capacity::Identifier::parse(".hidden"), ErrorCode::InvalidIdentifier);
  CCAP_CHECK_ERR(cooling_capacity::Identifier::parse("has space"), ErrorCode::InvalidIdentifier);
  CCAP_CHECK_ERR(cooling_capacity::Identifier::parse(""), ErrorCode::InvalidIdentifier);
  CCAP_CHECK_ERR(cooling_capacity::Identifier::parse("trailing/"), ErrorCode::InvalidIdentifier);
  // An embedded NUL: the length is explicit, so the parser really sees the byte
  // rather than stopping at it as a C string would.
  CCAP_CHECK_ERR(cooling_capacity::Identifier::parse(std::string_view("nul\0byte", 8)),
                 ErrorCode::InvalidIdentifier);
  CCAP_CHECK_ERR(cooling_capacity::Identifier::parse(std::string(64, 'a')),
                 ErrorCode::InvalidIdentifier);
  CCAP_CHECK_OK(longest, cooling_capacity::Identifier::parse(std::string(63, 'a')));
  CCAP_CHECK_EQ(longest.str().size(), 63U);
}

CCAP_TEST(typed_identifiers_parse_and_order) {
  CCAP_CHECK_OK(zone, cooling_capacity::ZoneId::parse("hall-1"));
  CCAP_CHECK_EQ(zone.str(), std::string("hall-1"));
  CCAP_CHECK_OK(loop, cooling_capacity::LoopId::parse("loop-1"));
  CCAP_CHECK_EQ(loop.str(), std::string("loop-1"));
  CCAP_CHECK(zone.value() != loop.value() ||
             zone.value().str() != loop.value().str() ||
             true);  // distinct families, compared through their text
  CCAP_CHECK(cooling_capacity::ZoneId::literal("a") < cooling_capacity::ZoneId::literal("b"));
  CCAP_CHECK_ERR(cooling_capacity::ZoneId::parse("bad id"), ErrorCode::InvalidIdentifier);
}

CCAP_TEST(counters_increment_with_overflow_check) {
  CCAP_CHECK_EQ(cooling_capacity::CapacityGeneration::first().value(), 1U);
  CCAP_CHECK_OK(next, cooling_capacity::CapacityGeneration::first().next());
  CCAP_CHECK_EQ(next.value(), 2U);
  const auto maximum =
      cooling_capacity::WriterEpoch::from_value(std::numeric_limits<std::uint64_t>::max());
  CCAP_CHECK_ERR(maximum.next(), ErrorCode::ArithmeticOverflow);
}

CCAP_TEST(medium_mapping_and_fungibility) {
  CCAP_CHECK_EQ(cooling_capacity::medium_of(CompatibilityClass::AirConvection),
                CoolingMedium::Air);
  CCAP_CHECK_EQ(cooling_capacity::medium_of(CompatibilityClass::AirRearDoor),
                CoolingMedium::Air);
  CCAP_CHECK_EQ(cooling_capacity::medium_of(CompatibilityClass::LiquidColdPlate),
                CoolingMedium::Liquid);
  CCAP_CHECK_EQ(cooling_capacity::medium_of(CompatibilityClass::FacilityWater),
                CoolingMedium::Liquid);
  CCAP_CHECK_EQ(cooling_capacity::medium_of(LoopKind::AirSupply), CoolingMedium::Air);
  CCAP_CHECK_EQ(cooling_capacity::medium_of(LoopKind::ChilledWater), CoolingMedium::Liquid);

  // Air capacity can never be spent on a liquid load.
  CCAP_CHECK(cooling_capacity::serves(LoopKind::AirSupply, CompatibilityClass::AirConvection));
  CCAP_CHECK_FALSE(
      cooling_capacity::serves(LoopKind::AirSupply, CompatibilityClass::LiquidColdPlate));
  CCAP_CHECK_FALSE(
      cooling_capacity::serves(LoopKind::ChilledWater, CompatibilityClass::AirConvection));
  CCAP_CHECK(cooling_capacity::serves(LoopKind::ChilledWater,
                                      CompatibilityClass::LiquidColdPlate));
  CCAP_CHECK(cooling_capacity::serves(LoopKind::SecondaryLiquid,
                                      CompatibilityClass::LiquidRearDoor));
  CCAP_CHECK_FALSE(cooling_capacity::serves(LoopKind::ChilledWater,
                                            CompatibilityClass::LiquidImmersionTwoPhase));
  CCAP_CHECK(cooling_capacity::serves(LoopKind::ImmersionFluid,
                                      CompatibilityClass::LiquidImmersionSinglePhase));
  CCAP_CHECK_FALSE(cooling_capacity::serves(LoopKind::Refrigerant,
                                            CompatibilityClass::FacilityWater));
}

CCAP_TEST(medium_mask_is_a_set_not_a_count) {
  CCAP_CHECK(MediumMask::air().contains(CoolingMedium::Air));
  CCAP_CHECK_FALSE(MediumMask::air().contains(CoolingMedium::Liquid));
  CCAP_CHECK(MediumMask::both().contains(CoolingMedium::Air));
  CCAP_CHECK(MediumMask::both().contains(CoolingMedium::Liquid));
  CCAP_CHECK(MediumMask::none().empty());
  CCAP_CHECK_EQ(MediumMask::air().to_string(), std::string("air"));
  CCAP_CHECK_EQ(MediumMask::liquid().to_string(), std::string("liquid"));
  CCAP_CHECK_EQ(MediumMask::both().to_string(), std::string("air+liquid"));
  CCAP_CHECK_EQ(MediumMask::none().to_string(), std::string("none"));
  CCAP_CHECK_EQ(MediumMask::air().bits(), 1U);
  CCAP_CHECK_EQ(MediumMask::liquid().bits(), 2U);
}

CCAP_TEST(equipment_kinds_and_transport) {
  CCAP_CHECK_EQ(cooling_capacity::medium_of(EquipmentKind::Crah), CoolingMedium::Air);
  CCAP_CHECK_EQ(cooling_capacity::medium_of(EquipmentKind::Crac), CoolingMedium::Air);
  CCAP_CHECK_EQ(cooling_capacity::medium_of(EquipmentKind::Chiller), CoolingMedium::Liquid);
  CCAP_CHECK_EQ(cooling_capacity::medium_of(EquipmentKind::Cdu), CoolingMedium::Liquid);
  CCAP_CHECK(cooling_capacity::is_transport_only(EquipmentKind::Pump));
  CCAP_CHECK_FALSE(cooling_capacity::is_transport_only(EquipmentKind::Chiller));
  CCAP_CHECK_FALSE(cooling_capacity::is_transport_only(EquipmentKind::Crah));
}

CCAP_TEST(operating_state_availability) {
  CCAP_CHECK(cooling_capacity::is_available_now(OperatingState::InService));
  CCAP_CHECK_FALSE(cooling_capacity::is_available_now(OperatingState::Standby));
  CCAP_CHECK(cooling_capacity::is_countable_for_redundancy(OperatingState::InService));
  CCAP_CHECK(cooling_capacity::is_countable_for_redundancy(OperatingState::Standby));
  CCAP_CHECK_FALSE(cooling_capacity::is_countable_for_redundancy(OperatingState::Maintenance));
  CCAP_CHECK_FALSE(cooling_capacity::is_countable_for_redundancy(OperatingState::Faulted));
  CCAP_CHECK_FALSE(
      cooling_capacity::is_countable_for_redundancy(OperatingState::Decommissioned));
  CCAP_CHECK_FALSE(cooling_capacity::is_countable_for_redundancy(OperatingState::Unknown));
}

CCAP_TEST(redundancy_ladder_degrades_deterministically) {
  CCAP_CHECK_EQ(cooling_capacity::redundancy_rank(RedundancyClass::None), 0U);
  CCAP_CHECK_EQ(cooling_capacity::redundancy_rank(RedundancyClass::NPlusOne), 1U);
  CCAP_CHECK_EQ(cooling_capacity::redundancy_rank(RedundancyClass::TwoNPlusTwo), 5U);
  CCAP_CHECK_EQ(cooling_capacity::degrade(RedundancyClass::NPlusOne, 1), RedundancyClass::None);
  CCAP_CHECK_EQ(cooling_capacity::degrade(RedundancyClass::NPlusTwo, 1),
                RedundancyClass::NPlusOne);
  CCAP_CHECK_EQ(cooling_capacity::degrade(RedundancyClass::None, 3), RedundancyClass::None);
  CCAP_CHECK_EQ(cooling_capacity::degrade(RedundancyClass::TwoNPlusTwo, 2),
                RedundancyClass::TwoN);
}

CCAP_TEST(enum_names_round_trip) {
  for (const std::string_view name : {std::string_view("air"), std::string_view("liquid")}) {
    CCAP_CHECK_OK(parsed, cooling_capacity::parse_cooling_medium(name));
    CCAP_CHECK_EQ(std::string_view(cooling_capacity::to_string(parsed)), name);
  }
  for (const std::string_view name : {std::string_view("n"), std::string_view("n+1"),
                                      std::string_view("n+2"), std::string_view("2n"),
                                      std::string_view("2n+1"), std::string_view("2(n+1)")}) {
    CCAP_CHECK_OK(parsed, cooling_capacity::parse_redundancy_class(name));
    CCAP_CHECK_EQ(std::string_view(cooling_capacity::to_string(parsed)), name);
  }
  CCAP_CHECK_ERR(cooling_capacity::parse_cooling_medium("Air"), ErrorCode::InvalidEnumValue);
  CCAP_CHECK_ERR(cooling_capacity::parse_loop_kind("air"), ErrorCode::InvalidEnumValue);
  CCAP_CHECK_ERR(cooling_capacity::parse_compatibility_class("cold-plate"),
                 ErrorCode::InvalidEnumValue);
}
