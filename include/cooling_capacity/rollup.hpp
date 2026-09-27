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

#ifndef COOLING_CAPACITY_ROLLUP_HPP
#define COOLING_CAPACITY_ROLLUP_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cooling_capacity/accounting.hpp"
#include "cooling_capacity/explanation.hpp"
#include "cooling_capacity/ids.hpp"
#include "cooling_capacity/medium.hpp"
#include "cooling_capacity/units.hpp"

namespace cooling_capacity {

class CoolingSnapshot;

// On what basis the analysis was performed. A snapshot recovered from a store
// has not been revalidated against current evidence, and answers derived from
// it say so. A snapshot that was only just built, or that has been explicitly
// revalidated at a named instant, carries the corresponding basis.
enum class AnalysisBasis : std::uint8_t {
  Fresh = 0,
  RecoveredPendingRevalidation = 1,
  Revalidated = 2,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(AnalysisBasis basis) noexcept;
[[nodiscard]] CCAP_EXPORT Result<AnalysisBasis> parse_analysis_basis(std::string_view text);

// Which constraint decided an answer.
enum class BottleneckKind : std::uint8_t {
  None = 0,
  ZoneCommittedLoad = 1,
  LoopTransport = 2,
  LoopEquipment = 3,
  LoopCommittedLoad = 4,
  SharedPool = 5,
  MinimumReserve = 6,
  EvidenceFreshness = 7,
  UnknownCapacity = 8,
  ZoneMediumUnsupported = 9,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(BottleneckKind kind) noexcept;

struct Bottleneck {
  BottleneckKind kind = BottleneckKind::None;
  Identifier subject;
  CapacityValue limit;
  ThermalPower consumed;
  ThermalPower remaining;

  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const Bottleneck& lhs, const Bottleneck& rhs) {
    return lhs.kind == rhs.kind && lhs.subject == rhs.subject && lhs.limit == rhs.limit &&
           lhs.consumed == rhs.consumed && lhs.remaining == rhs.remaining;
  }
};

// One equipment unit's contribution to one loop. The same unit can appear on
// more than one loop; when it does, its usable capacity is divided between the
// loops that share it, rounding down, so that shared equipment is never counted
// twice.
struct EquipmentContribution {
  EquipmentId equipment;
  LoopId loop;
  EquipmentKind kind = EquipmentKind::Chiller;
  OperatingState state = OperatingState::Unknown;
  CapacityBasis basis = CapacityBasis::None;
  CapacityValue nominal;
  CapacityValue validated;
  CapacityValue derated;
  CapacityValue degraded;
  CapacityValue usable;
  Fraction cumulative_derate = Fraction::one();
  Fraction degradation = Fraction::one();
  std::uint32_t sharing_loops = 1;
  bool shared = false;
  bool contributes_capacity = true;
  bool counted_for_redundancy = true;
  std::vector<ExplanationStep> explanation;

  friend bool operator==(const EquipmentContribution& lhs, const EquipmentContribution& rhs);
};

// The redundancy posture of a group of units (a loop's equipment set or a
// plant's equipment set).
struct RedundancyPosture {
  RedundancyClass declared = RedundancyClass::None;
  RedundancyClass effective = RedundancyClass::None;
  std::size_t members = 0;
  std::size_t counted = 0;
  std::size_t excluded = 0;
  std::size_t capacity_units = 0;
  bool unknown_member_state = false;

  friend bool operator==(const RedundancyPosture& lhs, const RedundancyPosture& rhs) {
    return lhs.declared == rhs.declared && lhs.effective == rhs.effective &&
           lhs.members == rhs.members && lhs.counted == rhs.counted &&
           lhs.excluded == rhs.excluded && lhs.capacity_units == rhs.capacity_units &&
           lhs.unknown_member_state == rhs.unknown_member_state;
  }
};

struct LoopCapacity {
  LoopId loop;
  LoopKind kind = LoopKind::AirSupply;
  CoolingMedium medium = CoolingMedium::Air;
  CapacityBreakdown breakdown;
  std::vector<EquipmentContribution> equipment;
  RedundancyPosture redundancy;
  std::optional<Bottleneck> bottleneck;
  std::vector<ExplanationStep> explanation;

  friend bool operator==(const LoopCapacity& lhs, const LoopCapacity& rhs);
};

struct PlantCapacity {
  PlantId plant;
  CoolingMedium medium = CoolingMedium::Air;
  CapacityBreakdown breakdown;
  std::vector<EquipmentContribution> equipment;
  RedundancyPosture redundancy;
  std::vector<LoopId> served_loops;
  std::vector<ExplanationStep> explanation;

  friend bool operator==(const PlantCapacity& lhs, const PlantCapacity& rhs);
};

// A plant seen from a zone. The headroom in a shared pool belongs to every zone
// that draws on it, so it is reported once per pool and never summed into a
// zone's own answer.
struct SharedPoolView {
  PlantId plant;
  CoolingMedium medium = CoolingMedium::Air;
  CapacityValue usable;
  CapacityValue committed;
  CapacityValue headroom;
  std::vector<LoopId> served_loops;
  RedundancyPosture redundancy;
  std::optional<Bottleneck> bottleneck;

  friend bool operator==(const SharedPoolView& lhs, const SharedPoolView& rhs);
};

struct ZoneCapacity {
  ZoneId zone;
  CoolingMedium medium = CoolingMedium::Air;
  CompatibilityClass compatibility = CompatibilityClass::AirConvection;
  CapacityBreakdown breakdown;
  // Sum of the headroom of the loops that serve this zone and this class.
  CapacityValue local_headroom;
  // The smallest headroom among the shared plants the zone depends on. Absent
  // when the zone depends on no shared plant at all.
  std::optional<CapacityValue> shared_headroom;
  // What the zone can actually offer a new load: the smaller of the local and
  // shared headroom, less the zone's reserve. Unknown propagates.
  CapacityValue offered;
  std::vector<LoopCapacity> loops;
  std::vector<SharedPoolView> shared_pools;
  // True when at least one shared pool constrains the zone, which means the
  // offered figure is not exclusive to this zone.
  bool shared_headroom_is_shared = false;
  std::optional<Bottleneck> bottleneck;
  std::vector<ExplanationStep> explanation;

  friend bool operator==(const ZoneCapacity& lhs, const ZoneCapacity& rhs);
};

struct CoolingCapacityReport {
  CapacityGeneration generation;
  PolicyId policy;
  PolicyRevision policy_revision;
  Timestamp evaluated_at;
  AnalysisBasis basis = AnalysisBasis::Fresh;
  std::vector<ZoneCapacity> zones;
  std::vector<LoopCapacity> loops;
  std::vector<PlantCapacity> plants;
  std::vector<ExplanationStep> explanation;
};

// Deterministic rollup engine. One analyzer is bound to one immutable snapshot
// and one evaluation instant; it holds no mutable state and can be used
// concurrently from several threads.
class CCAP_EXPORT CoolingAnalyzer {
 public:
  CoolingAnalyzer(const CoolingSnapshot& snapshot, Timestamp evaluated_at, AnalysisBasis basis);

  [[nodiscard]] const CoolingSnapshot& snapshot() const noexcept { return snapshot_; }
  [[nodiscard]] Timestamp evaluated_at() const noexcept { return evaluated_at_; }
  [[nodiscard]] AnalysisBasis basis() const noexcept { return basis_; }

  [[nodiscard]] Result<LoopCapacity> loop_capacity(LoopId loop) const;
  [[nodiscard]] Result<PlantCapacity> plant_capacity(PlantId plant) const;
  [[nodiscard]] Result<ZoneCapacity> zone_capacity(ZoneId zone, CoolingMedium medium,
                                                   CompatibilityClass compatibility) const;
  [[nodiscard]] Result<CoolingCapacityReport> report() const;

 private:
  const CoolingSnapshot& snapshot_;
  Timestamp evaluated_at_;
  AnalysisBasis basis_;
};

}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_ROLLUP_HPP
