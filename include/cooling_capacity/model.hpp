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

#ifndef COOLING_CAPACITY_MODEL_HPP
#define COOLING_CAPACITY_MODEL_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cooling_capacity/ids.hpp"
#include "cooling_capacity/medium.hpp"
#include "cooling_capacity/text.hpp"
#include "cooling_capacity/units.hpp"

namespace cooling_capacity {

// Every record below is a declaration about the physical cooling plant. The
// records are immutable value types: a change is a new snapshot generation, not
// a mutation of a record in place. Each record carries its own revision so that
// a diff can say which records actually changed.

struct SiteRecord {
  SiteId id;
  BoundedText label;
  RecordRevision revision;

  friend bool operator==(const SiteRecord& lhs, const SiteRecord& rhs) {
    return lhs.id == rhs.id && lhs.label == rhs.label && lhs.revision == rhs.revision;
  }
};

struct FacilityRecord {
  FacilityId id;
  SiteId site;
  BoundedText label;
  RecordRevision revision;

  friend bool operator==(const FacilityRecord& lhs, const FacilityRecord& rhs) {
    return lhs.id == rhs.id && lhs.site == rhs.site && lhs.label == rhs.label &&
           lhs.revision == rhs.revision;
  }
};

// A declared airflow or liquid domain. This is a reference target only: the
// library records that a zone or loop is associated with a domain and never
// models the domain's internal behaviour.
struct DomainRecord {
  DomainId id;
  FacilityId facility;
  CoolingMedium medium;
  BoundedText label;
  RecordRevision revision;

  friend bool operator==(const DomainRecord& lhs, const DomainRecord& rhs) {
    return lhs.id == rhs.id && lhs.facility == rhs.facility && lhs.medium == rhs.medium &&
           lhs.label == rhs.label && lhs.revision == rhs.revision;
  }
};

// A distribution manifold. It carries a declared transport limit and no
// thermal-removal capacity of its own.
struct ManifoldRecord {
  ManifoldId id;
  FacilityId facility;
  CoolingMedium medium;
  BoundedText label;
  ThermalPower transport_limit;
  std::optional<EvidenceId> transport_evidence;
  RecordRevision revision;

  friend bool operator==(const ManifoldRecord& lhs, const ManifoldRecord& rhs) {
    return lhs.id == rhs.id && lhs.facility == rhs.facility && lhs.medium == rhs.medium &&
           lhs.label == rhs.label && lhs.transport_limit == rhs.transport_limit &&
           lhs.transport_evidence == rhs.transport_evidence && lhs.revision == rhs.revision;
  }
};

// One factor that reduces an equipment unit's declared capacity. Factors are
// multiplied together in the order of the canonical name, in exact integer
// arithmetic, rounding down at every step.
struct DerateFactor {
  Identifier name;
  Fraction factor;
  DerateReason reason;
  std::optional<EvidenceId> evidence;

  friend bool operator==(const DerateFactor& lhs, const DerateFactor& rhs) {
    return lhs.name == rhs.name && lhs.factor == rhs.factor && lhs.reason == rhs.reason &&
           lhs.evidence == rhs.evidence;
  }
};

// Measured or assessed loss of capacity that is not a design derate.
struct DegradationState {
  Fraction factor = Fraction::one();
  DegradationBasis basis = DegradationBasis::None;
  std::optional<EvidenceId> evidence;
  std::optional<Timestamp> assessed_at;

  friend bool operator==(const DegradationState& lhs, const DegradationState& rhs) {
    return lhs.factor == rhs.factor && lhs.basis == rhs.basis && lhs.evidence == rhs.evidence &&
           lhs.assessed_at == rhs.assessed_at;
  }
};

// One thermal-removal unit: a chiller, cooling tower, CDU, CRAH, CRAC and so on.
//
// `nominal` is the nameplate figure. `validated` is the figure established by
// commissioning or by a later capacity validation; when it is absent the
// nominal value is used and the accounting records that the basis is nominal.
struct EquipmentRecord {
  EquipmentId id;
  FacilityId facility;
  BoundedText label;
  EquipmentKind kind;
  OperatingState state = OperatingState::Unknown;
  std::optional<EvidenceId> state_evidence;
  std::optional<Timestamp> state_since;
  ThermalPower nominal;
  std::optional<ThermalPower> validated;
  std::optional<EvidenceId> validated_evidence;
  std::vector<DerateFactor> derates;
  DegradationState degradation;
  RecordRevision revision;

  friend bool operator==(const EquipmentRecord& lhs, const EquipmentRecord& rhs);
};

// A cooling loop: the transport path that delivers thermal-removal capability
// from equipment to the zones it serves.
//
// `transport_limit_declared` distinguishes "the transport path can carry zero"
// from "nobody has told us what the transport path can carry". The second case
// is Unknown, and by default an Unknown transport limit makes the loop's
// capacity Unknown rather than the sum of its equipment.
struct LoopRecord {
  LoopId id;
  FacilityId facility;
  BoundedText label;
  LoopKind kind;
  ThermalPower transport_limit;
  bool transport_limit_declared = false;
  std::optional<EvidenceId> transport_evidence;
  std::vector<EquipmentId> equipment;
  std::vector<ManifoldId> manifolds;
  std::optional<DomainId> domain;
  std::optional<PlantId> primary_plant;
  std::optional<PlantId> secondary_plant;
  PlantSupplyMode supply_mode = PlantSupplyMode::Single;
  RedundancyClass redundancy = RedundancyClass::None;
  RecordRevision revision;

  friend bool operator==(const LoopRecord& lhs, const LoopRecord& rhs);
};

// A shared plant. Its capacity is a pool: several loops draw on it, so the
// plant's capacity must never be counted once per loop.
struct PlantRecord {
  PlantId id;
  FacilityId facility;
  BoundedText label;
  CoolingMedium medium;
  std::vector<EquipmentId> equipment;
  RedundancyClass redundancy = RedundancyClass::None;
  RecordRevision revision;

  friend bool operator==(const PlantRecord& lhs, const PlantRecord& rhs);
};

// A zone is the aggregation boundary at which capacity becomes interesting to a
// consumer: a hall, a room, a row or a rack group that is served by one or more
// loops.
struct ZoneRecord {
  ZoneId id;
  FacilityId facility;
  BoundedText label;
  MediumMask media;
  std::vector<CompatibilityClass> compatibility;
  std::vector<LoopId> loops;
  std::vector<DomainId> domains;
  Fraction reserve_floor = Fraction::zero();
  RecordRevision revision;

  friend bool operator==(const ZoneRecord& lhs, const ZoneRecord& rhs);
};

inline bool operator==(const EquipmentRecord& lhs, const EquipmentRecord& rhs) {
  return lhs.id == rhs.id && lhs.facility == rhs.facility && lhs.label == rhs.label &&
         lhs.kind == rhs.kind && lhs.state == rhs.state &&
         lhs.state_evidence == rhs.state_evidence && lhs.state_since == rhs.state_since &&
         lhs.nominal == rhs.nominal && lhs.validated == rhs.validated &&
         lhs.validated_evidence == rhs.validated_evidence && lhs.derates == rhs.derates &&
         lhs.degradation == rhs.degradation && lhs.revision == rhs.revision;
}

inline bool operator==(const LoopRecord& lhs, const LoopRecord& rhs) {
  return lhs.id == rhs.id && lhs.facility == rhs.facility && lhs.label == rhs.label &&
         lhs.kind == rhs.kind && lhs.transport_limit == rhs.transport_limit &&
         lhs.transport_limit_declared == rhs.transport_limit_declared &&
         lhs.transport_evidence == rhs.transport_evidence && lhs.equipment == rhs.equipment &&
         lhs.manifolds == rhs.manifolds && lhs.domain == rhs.domain &&
         lhs.primary_plant == rhs.primary_plant && lhs.secondary_plant == rhs.secondary_plant &&
         lhs.supply_mode == rhs.supply_mode && lhs.redundancy == rhs.redundancy &&
         lhs.revision == rhs.revision;
}

inline bool operator==(const PlantRecord& lhs, const PlantRecord& rhs) {
  return lhs.id == rhs.id && lhs.facility == rhs.facility && lhs.label == rhs.label &&
         lhs.medium == rhs.medium && lhs.equipment == rhs.equipment &&
         lhs.redundancy == rhs.redundancy && lhs.revision == rhs.revision;
}

inline bool operator==(const ZoneRecord& lhs, const ZoneRecord& rhs) {
  return lhs.id == rhs.id && lhs.facility == rhs.facility && lhs.label == rhs.label &&
         lhs.media == rhs.media && lhs.compatibility == rhs.compatibility &&
         lhs.loops == rhs.loops && lhs.domains == rhs.domains &&
         lhs.reserve_floor == rhs.reserve_floor && lhs.revision == rhs.revision;
}

}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_MODEL_HPP
