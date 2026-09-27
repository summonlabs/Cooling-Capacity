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

#include "cooling_capacity/medium.hpp"

#include "enum_table.hpp"

namespace cooling_capacity {

namespace {

using detail::EnumName;

constexpr detail::EnumName<CoolingMedium> kMediumNames[] = {
    {CoolingMedium::Air, "air"},
    {CoolingMedium::Liquid, "liquid"},
};

constexpr detail::EnumName<CompatibilityClass> kCompatibilityNames[] = {
    {CompatibilityClass::AirConvection, "air-convection"},
    {CompatibilityClass::AirRearDoor, "air-rear-door"},
    {CompatibilityClass::LiquidColdPlate, "liquid-cold-plate"},
    {CompatibilityClass::LiquidImmersionSinglePhase, "liquid-immersion-single-phase"},
    {CompatibilityClass::LiquidImmersionTwoPhase, "liquid-immersion-two-phase"},
    {CompatibilityClass::LiquidRearDoor, "liquid-rear-door"},
    {CompatibilityClass::FacilityWater, "facility-water"},
};

constexpr detail::EnumName<LoopKind> kLoopKindNames[] = {
    {LoopKind::AirSupply, "air-supply"},         {LoopKind::AirReturn, "air-return"},
    {LoopKind::ChilledWater, "chilled-water"},   {LoopKind::CondenserWater, "condenser-water"},
    {LoopKind::Glycol, "glycol"},                {LoopKind::SecondaryLiquid, "secondary-liquid"},
    {LoopKind::ImmersionFluid, "immersion-fluid"},
    {LoopKind::Refrigerant, "refrigerant"},
};

constexpr detail::EnumName<EquipmentKind> kEquipmentKindNames[] = {
    {EquipmentKind::Chiller, "chiller"},
    {EquipmentKind::CoolingTower, "cooling-tower"},
    {EquipmentKind::DryCooler, "dry-cooler"},
    {EquipmentKind::FluidCooler, "fluid-cooler"},
    {EquipmentKind::Pump, "pump"},
    {EquipmentKind::HeatExchanger, "heat-exchanger"},
    {EquipmentKind::Cdu, "cdu"},
    {EquipmentKind::Crah, "crah"},
    {EquipmentKind::Crac, "crac"},
    {EquipmentKind::AirHandler, "air-handler"},
    {EquipmentKind::ImmersionTank, "immersion-tank"},
    {EquipmentKind::Economizer, "economizer"},
};

constexpr detail::EnumName<OperatingState> kOperatingStateNames[] = {
    {OperatingState::InService, "in-service"},   {OperatingState::Standby, "standby"},
    {OperatingState::Maintenance, "maintenance"}, {OperatingState::Faulted, "faulted"},
    {OperatingState::Decommissioned, "decommissioned"},
    {OperatingState::Unknown, "unknown"},
};

constexpr detail::EnumName<RedundancyClass> kRedundancyNames[] = {
    {RedundancyClass::None, "n"},           {RedundancyClass::NPlusOne, "n+1"},
    {RedundancyClass::NPlusTwo, "n+2"},     {RedundancyClass::TwoN, "2n"},
    {RedundancyClass::TwoNPlusOne, "2n+1"}, {RedundancyClass::TwoNPlusTwo, "2(n+1)"},
};

constexpr detail::EnumName<PlantSupplyMode> kSupplyModeNames[] = {
    {PlantSupplyMode::Single, "single"},
    {PlantSupplyMode::Alternates, "alternates"},
};

constexpr detail::EnumName<DerateReason> kDerateReasonNames[] = {
    {DerateReason::AmbientDesign, "ambient-design"},
    {DerateReason::AltitudeAirDensity, "altitude-air-density"},
    {DerateReason::FluidTemperature, "fluid-temperature"},
    {DerateReason::GlycolConcentration, "glycol-concentration"},
    {DerateReason::Fouling, "fouling"},
    {DerateReason::VendorCurve, "vendor-curve"},
    {DerateReason::Commissioning, "commissioning"},
    {DerateReason::Age, "age"},
    {DerateReason::FilterLoading, "filter-loading"},
    {DerateReason::ContainmentDeficit, "containment-deficit"},
    {DerateReason::Custom, "custom"},
};

constexpr detail::EnumName<DegradationBasis> kDegradationBasisNames[] = {
    {DegradationBasis::None, "none"},
    {DegradationBasis::Commissioning, "commissioning"},
    {DegradationBasis::ConditionAssessment, "condition-assessment"},
    {DegradationBasis::VendorCurve, "vendor-curve"},
    {DegradationBasis::ElapsedService, "elapsed-service"},
};

}  // namespace

std::string_view to_string(CoolingMedium medium) noexcept {
  return medium == CoolingMedium::Air ? "air" : "liquid";
}

Result<CoolingMedium> parse_cooling_medium(std::string_view text) {
  return detail::parse_from_table("cooling medium", kMediumNames, text);
}

CoolingMedium cooling_medium_at(std::size_t index) {
  return index == 0 ? CoolingMedium::Air : CoolingMedium::Liquid;
}

std::string_view to_string(CompatibilityClass value) noexcept {
  for (const EnumName<CompatibilityClass>& entry : kCompatibilityNames) {
    if (entry.value == value) {
      return entry.name;
    }
  }
  return "unrecognised";
}

Result<CompatibilityClass> parse_compatibility_class(std::string_view text) {
  return detail::parse_from_table("compatibility class", kCompatibilityNames, text);
}

CoolingMedium medium_of(CompatibilityClass value) noexcept {
  switch (value) {
    case CompatibilityClass::AirConvection:
    case CompatibilityClass::AirRearDoor:
      return CoolingMedium::Air;
    case CompatibilityClass::LiquidColdPlate:
    case CompatibilityClass::LiquidImmersionSinglePhase:
    case CompatibilityClass::LiquidImmersionTwoPhase:
    case CompatibilityClass::LiquidRearDoor:
    case CompatibilityClass::FacilityWater:
      return CoolingMedium::Liquid;
  }
  return CoolingMedium::Air;
}

std::string_view to_string(LoopKind kind) noexcept {
  for (const EnumName<LoopKind>& entry : kLoopKindNames) {
    if (entry.value == kind) {
      return entry.name;
    }
  }
  return "unrecognised";
}

Result<LoopKind> parse_loop_kind(std::string_view text) {
  return detail::parse_from_table("loop kind", kLoopKindNames, text);
}

CoolingMedium medium_of(LoopKind kind) noexcept {
  switch (kind) {
    case LoopKind::AirSupply:
    case LoopKind::AirReturn:
      return CoolingMedium::Air;
    case LoopKind::ChilledWater:
    case LoopKind::CondenserWater:
    case LoopKind::Glycol:
    case LoopKind::SecondaryLiquid:
    case LoopKind::ImmersionFluid:
    case LoopKind::Refrigerant:
      return CoolingMedium::Liquid;
  }
  return CoolingMedium::Liquid;
}

bool serves(LoopKind kind, CompatibilityClass value) noexcept {
  if (medium_of(kind) != medium_of(value)) {
    return false;
  }
  switch (kind) {
    case LoopKind::AirSupply:
    case LoopKind::AirReturn:
      return value == CompatibilityClass::AirConvection ||
             value == CompatibilityClass::AirRearDoor;
    case LoopKind::ChilledWater:
      return value == CompatibilityClass::LiquidColdPlate ||
             value == CompatibilityClass::LiquidRearDoor ||
             value == CompatibilityClass::FacilityWater;
    case LoopKind::CondenserWater:
    case LoopKind::Glycol:
      return value == CompatibilityClass::FacilityWater;
    case LoopKind::SecondaryLiquid:
      return value == CompatibilityClass::LiquidColdPlate ||
             value == CompatibilityClass::LiquidRearDoor;
    case LoopKind::ImmersionFluid:
      return value == CompatibilityClass::LiquidImmersionSinglePhase ||
             value == CompatibilityClass::LiquidImmersionTwoPhase;
    case LoopKind::Refrigerant:
      return false;
  }
  return false;
}

std::string_view to_string(EquipmentKind kind) noexcept {
  for (const EnumName<EquipmentKind>& entry : kEquipmentKindNames) {
    if (entry.value == kind) {
      return entry.name;
    }
  }
  return "unrecognised";
}

Result<EquipmentKind> parse_equipment_kind(std::string_view text) {
  return detail::parse_from_table("equipment kind", kEquipmentKindNames, text);
}

CoolingMedium medium_of(EquipmentKind kind) noexcept {
  switch (kind) {
    case EquipmentKind::Crah:
    case EquipmentKind::Crac:
    case EquipmentKind::AirHandler:
      return CoolingMedium::Air;
    case EquipmentKind::Chiller:
    case EquipmentKind::CoolingTower:
    case EquipmentKind::DryCooler:
    case EquipmentKind::FluidCooler:
    case EquipmentKind::Pump:
    case EquipmentKind::HeatExchanger:
    case EquipmentKind::Cdu:
    case EquipmentKind::ImmersionTank:
    case EquipmentKind::Economizer:
      return CoolingMedium::Liquid;
  }
  return CoolingMedium::Liquid;
}

bool is_transport_only(EquipmentKind kind) noexcept {
  return kind == EquipmentKind::Pump;
}

std::string_view to_string(OperatingState state) noexcept {
  for (const EnumName<OperatingState>& entry : kOperatingStateNames) {
    if (entry.value == state) {
      return entry.name;
    }
  }
  return "unrecognised";
}

Result<OperatingState> parse_operating_state(std::string_view text) {
  return detail::parse_from_table("operating state", kOperatingStateNames, text);
}

bool is_available_now(OperatingState state) noexcept {
  return state == OperatingState::InService;
}

bool is_countable_for_redundancy(OperatingState state) noexcept {
  return state == OperatingState::InService || state == OperatingState::Standby;
}

std::string_view to_string(RedundancyClass value) noexcept {
  for (const EnumName<RedundancyClass>& entry : kRedundancyNames) {
    if (entry.value == value) {
      return entry.name;
    }
  }
  return "unrecognised";
}

Result<RedundancyClass> parse_redundancy_class(std::string_view text) {
  return detail::parse_from_table("redundancy class", kRedundancyNames, text);
}

unsigned redundancy_rank(RedundancyClass value) noexcept {
  return static_cast<unsigned>(value);
}

RedundancyClass degrade(RedundancyClass value, unsigned steps) noexcept {
  const unsigned rank = redundancy_rank(value);
  if (steps >= rank) {
    return RedundancyClass::None;
  }
  return static_cast<RedundancyClass>(rank - steps);
}

std::string_view to_string(PlantSupplyMode mode) noexcept {
  for (const EnumName<PlantSupplyMode>& entry : kSupplyModeNames) {
    if (entry.value == mode) {
      return entry.name;
    }
  }
  return "unrecognised";
}

Result<PlantSupplyMode> parse_plant_supply_mode(std::string_view text) {
  return detail::parse_from_table("plant supply mode", kSupplyModeNames, text);
}

std::string_view to_string(DerateReason reason) noexcept {
  for (const EnumName<DerateReason>& entry : kDerateReasonNames) {
    if (entry.value == reason) {
      return entry.name;
    }
  }
  return "unrecognised";
}

Result<DerateReason> parse_derate_reason(std::string_view text) {
  return detail::parse_from_table("derate reason", kDerateReasonNames, text);
}

std::string_view to_string(DegradationBasis basis) noexcept {
  for (const EnumName<DegradationBasis>& entry : kDegradationBasisNames) {
    if (entry.value == basis) {
      return entry.name;
    }
  }
  return "unrecognised";
}

Result<DegradationBasis> parse_degradation_basis(std::string_view text) {
  return detail::parse_from_table("degradation basis", kDegradationBasisNames, text);
}

std::string MediumMask::to_string() const {
  if (bits_ == 0) {
    return "none";
  }
  if (bits_ == 3) {
    return "air+liquid";
  }
  return contains(CoolingMedium::Air) ? "air" : "liquid";
}

}  // namespace cooling_capacity
