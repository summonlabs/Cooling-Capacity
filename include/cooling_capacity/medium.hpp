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

#ifndef COOLING_CAPACITY_MEDIUM_HPP
#define COOLING_CAPACITY_MEDIUM_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "cooling_capacity/errors.hpp"
#include "cooling_capacity/export.hpp"

namespace cooling_capacity {

// The medium that carries heat away from a load. Air and liquid capacity are
// never added together: a zone that can remove 400 kW with air cannot accept a
// liquid-cooled load, and an air-cooled load cannot be served by liquid
// capacity. Accounting is always per medium.
enum class CoolingMedium : std::uint8_t {
  Air = 0,
  Liquid = 1,
};

inline constexpr std::size_t kCoolingMediumCount = 2;

[[nodiscard]] CCAP_EXPORT std::string_view to_string(CoolingMedium medium) noexcept;
[[nodiscard]] CCAP_EXPORT Result<CoolingMedium> parse_cooling_medium(std::string_view text);
[[nodiscard]] CCAP_EXPORT CoolingMedium cooling_medium_at(std::size_t index);

// How a load is coupled to its cooling medium. A zone declares which classes it
// can serve; a candidate load declares which class it needs. Admission requires
// the zone to support the class, which is what stops air capacity from being
// spent on a cold-plate load.
enum class CompatibilityClass : std::uint8_t {
  AirConvection = 0,
  AirRearDoor = 1,
  LiquidColdPlate = 2,
  LiquidImmersionSinglePhase = 3,
  LiquidImmersionTwoPhase = 4,
  LiquidRearDoor = 5,
  FacilityWater = 6,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(CompatibilityClass value) noexcept;
[[nodiscard]] CCAP_EXPORT Result<CompatibilityClass> parse_compatibility_class(
    std::string_view text);
[[nodiscard]] CCAP_EXPORT CoolingMedium medium_of(CompatibilityClass value) noexcept;

// The physical transport a loop provides.
enum class LoopKind : std::uint8_t {
  AirSupply = 0,
  AirReturn = 1,
  ChilledWater = 2,
  CondenserWater = 3,
  Glycol = 4,
  SecondaryLiquid = 5,
  ImmersionFluid = 6,
  Refrigerant = 7,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(LoopKind kind) noexcept;
[[nodiscard]] CCAP_EXPORT Result<LoopKind> parse_loop_kind(std::string_view text);
[[nodiscard]] CCAP_EXPORT CoolingMedium medium_of(LoopKind kind) noexcept;

// Which compatibility classes a loop kind can physically serve. A chilled-water
// loop serves liquid classes; an air-supply loop serves air classes. The map is
// total and deliberately conservative.
[[nodiscard]] CCAP_EXPORT bool serves(LoopKind kind, CompatibilityClass value) noexcept;

// The kind of thermal-removal equipment. `medium_of` reports the medium the
// equipment removes heat from, which is the medium its capacity is accounted in.
enum class EquipmentKind : std::uint8_t {
  Chiller = 0,
  CoolingTower = 1,
  DryCooler = 2,
  FluidCooler = 3,
  Pump = 4,
  HeatExchanger = 5,
  Cdu = 6,
  Crah = 7,
  Crac = 8,
  AirHandler = 9,
  ImmersionTank = 10,
  Economizer = 11,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(EquipmentKind kind) noexcept;
[[nodiscard]] CCAP_EXPORT Result<EquipmentKind> parse_equipment_kind(std::string_view text);
[[nodiscard]] CCAP_EXPORT CoolingMedium medium_of(EquipmentKind kind) noexcept;

// True for equipment that carries capacity rather than creating it. A manifold
// is a transport component: it can appear as a point of connection but it never
// contributes thermal-removal capacity of its own.
[[nodiscard]] CCAP_EXPORT bool is_transport_only(EquipmentKind kind) noexcept;

// The operating state of one equipment unit. `Unknown` is a first-class answer
// and propagates: a group containing a unit in an unknown state has unknown
// capacity rather than a silently smaller one.
enum class OperatingState : std::uint8_t {
  InService = 0,
  Standby = 1,
  Maintenance = 2,
  Faulted = 3,
  Decommissioned = 4,
  Unknown = 5,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(OperatingState state) noexcept;
[[nodiscard]] CCAP_EXPORT Result<OperatingState> parse_operating_state(std::string_view text);

// True when the unit could serve load now.
[[nodiscard]] CCAP_EXPORT bool is_available_now(OperatingState state) noexcept;
// True when the unit may be counted towards a redundant group even though it is
// not carrying load at this instant.
[[nodiscard]] CCAP_EXPORT bool is_countable_for_redundancy(OperatingState state) noexcept;

// Declared fault tolerance of a group of parallel units.
enum class RedundancyClass : std::uint8_t {
  None = 0,
  NPlusOne = 1,
  NPlusTwo = 2,
  TwoN = 3,
  TwoNPlusOne = 4,
  TwoNPlusTwo = 5,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(RedundancyClass value) noexcept;
[[nodiscard]] CCAP_EXPORT Result<RedundancyClass> parse_redundancy_class(std::string_view text);
[[nodiscard]] CCAP_EXPORT unsigned redundancy_rank(RedundancyClass value) noexcept;

// The descriptive label reached by stepping `steps` places down the redundancy
// ladder N, N+1, N+2, 2N, 2N+1, 2(N+1). The label is informational; the
// authoritative number is always the capacity computed by the group formula.
[[nodiscard]] CCAP_EXPORT RedundancyClass degrade(RedundancyClass value, unsigned steps) noexcept;

// How a loop draws on its upstream plant.
enum class PlantSupplyMode : std::uint8_t {
  // The loop is fed by exactly one declared plant.
  Single = 0,
  // The loop can be fed by either declared plant. Accounting is conservative:
  // the loop's committed load is charged to both plants and the loop's shared
  // headroom is the smaller of the two.
  Alternates = 1,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(PlantSupplyMode mode) noexcept;
[[nodiscard]] CCAP_EXPORT Result<PlantSupplyMode> parse_plant_supply_mode(std::string_view text);

// A set of media. Used where a record can declare that it serves air, liquid or
// both without collapsing them into a count.
class CCAP_EXPORT MediumMask {
 public:
  constexpr MediumMask() = default;

  [[nodiscard]] static constexpr MediumMask none() noexcept { return MediumMask(0); }
  [[nodiscard]] static constexpr MediumMask air() noexcept { return MediumMask(1); }
  [[nodiscard]] static constexpr MediumMask liquid() noexcept { return MediumMask(2); }
  [[nodiscard]] static constexpr MediumMask both() noexcept { return MediumMask(3); }

  [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }
  [[nodiscard]] constexpr bool contains(CoolingMedium medium) const noexcept {
    return (bits_ & bit_of(medium)) != 0;
  }
  [[nodiscard]] constexpr std::uint8_t bits() const noexcept { return bits_; }

  void insert(CoolingMedium medium) noexcept { bits_ = static_cast<std::uint8_t>(bits_ | bit_of(medium)); }

  friend constexpr bool operator==(MediumMask lhs, MediumMask rhs) noexcept {
    return lhs.bits_ == rhs.bits_;
  }
  friend constexpr bool operator!=(MediumMask lhs, MediumMask rhs) noexcept {
    return !(lhs == rhs);
  }

  [[nodiscard]] std::string to_string() const;

 private:
  [[nodiscard]] static constexpr std::uint8_t bit_of(CoolingMedium medium) noexcept {
    return static_cast<std::uint8_t>(1U << static_cast<unsigned>(medium));
  }
  constexpr explicit MediumMask(std::uint8_t bits) noexcept : bits_(bits) {}
  std::uint8_t bits_ = 0;
};

// Why a derate factor was applied. The reason is recorded so that an operator
// can see which assumption reduced capacity rather than only by how much.
enum class DerateReason : std::uint8_t {
  AmbientDesign = 0,
  AltitudeAirDensity = 1,
  FluidTemperature = 2,
  GlycolConcentration = 3,
  Fouling = 4,
  VendorCurve = 5,
  Commissioning = 6,
  Age = 7,
  FilterLoading = 8,
  ContainmentDeficit = 9,
  Custom = 10,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(DerateReason reason) noexcept;
[[nodiscard]] CCAP_EXPORT Result<DerateReason> parse_derate_reason(std::string_view text);

// How a degradation factor was established.
enum class DegradationBasis : std::uint8_t {
  None = 0,
  Commissioning = 1,
  ConditionAssessment = 2,
  VendorCurve = 3,
  ElapsedService = 4,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(DegradationBasis basis) noexcept;
[[nodiscard]] CCAP_EXPORT Result<DegradationBasis> parse_degradation_basis(std::string_view text);

}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_MEDIUM_HPP
