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

#ifndef COOLING_CAPACITY_TESTS_SUPPORT_FIXTURES_HPP
#define COOLING_CAPACITY_TESTS_SUPPORT_FIXTURES_HPP

#include <memory>
#include <string>
#include <vector>

#include "cooling_capacity/cooling_capacity.hpp"

namespace ccap_test {

using cooling_capacity::ActorId;
using cooling_capacity::BoundedText;
using cooling_capacity::CapacityGeneration;
using cooling_capacity::CompatibilityClass;
using cooling_capacity::CoolingMedium;
using cooling_capacity::CoolingSnapshot;
using cooling_capacity::DerateFactor;
using cooling_capacity::DocumentRef;
using cooling_capacity::EntityKind;
using cooling_capacity::EquipmentId;
using cooling_capacity::EquipmentKind;
using cooling_capacity::EquipmentRecord;
using cooling_capacity::EvidenceId;
using cooling_capacity::EvidenceKind;
using cooling_capacity::EvidenceRecord;
using cooling_capacity::EvidenceSource;
using cooling_capacity::FacilityId;
using cooling_capacity::FacilityRecord;
using cooling_capacity::Fraction;
using cooling_capacity::Identifier;
using cooling_capacity::LoopId;
using cooling_capacity::LoopKind;
using cooling_capacity::LoopRecord;
using cooling_capacity::ManualClock;
using cooling_capacity::OperatingState;
using cooling_capacity::PlantId;
using cooling_capacity::PlantRecord;
using cooling_capacity::RecordRevision;
using cooling_capacity::Result;
using cooling_capacity::SiteId;
using cooling_capacity::SiteRecord;
using cooling_capacity::SnapshotBuilder;
using cooling_capacity::ThermalPower;
using cooling_capacity::Timestamp;
using cooling_capacity::ZoneId;
using cooling_capacity::ZoneRecord;

inline SiteId site_id(const char* text) { return SiteId::literal(text); }
inline FacilityId facility_id(const char* text) { return FacilityId::literal(text); }
inline ZoneId zone_id(const char* text) { return ZoneId::literal(text); }
inline LoopId loop_id(const char* text) { return LoopId::literal(text); }
inline PlantId plant_id(const char* text) { return PlantId::literal(text); }
inline EquipmentId equipment_id(const char* text) { return EquipmentId::literal(text); }
inline EvidenceId evidence_id(const char* text) { return EvidenceId::literal(text); }
inline ActorId actor_id(const char* text) { return ActorId::literal(text); }
inline Identifier identifier(const char* text) { return Identifier::literal(text); }

inline ThermalPower watts(std::int64_t value) {
  return ThermalPower::from_watts(value).value();
}

inline ThermalPower milliwatts(std::int64_t value) {
  return ThermalPower::from_milliwatts(value);
}

inline Fraction ppm(std::int64_t value) {
  return Fraction::from_parts_per_million(value);
}

// A fixed instant used by every fixture so that freshness decisions are
// reproducible.
inline Timestamp fixture_now() { return Timestamp::from_unix_milliseconds(1767225600000LL); }

SiteRecord make_site(const char* id);
FacilityRecord make_facility(const char* id, const SiteId& site);
EquipmentRecord make_unit(const char* id, EquipmentKind kind, std::int64_t nominal_watts);
LoopRecord make_loop(const char* id, LoopKind kind, std::vector<EquipmentId> equipment,
                     std::int64_t transport_watts, cooling_capacity::RedundancyClass redundancy);
ZoneRecord make_zone(const char* id, std::vector<LoopId> loops,
                     std::vector<CompatibilityClass> classes);
PlantRecord make_plant(const char* id, CoolingMedium medium, std::vector<EquipmentId> equipment,
                       cooling_capacity::RedundancyClass redundancy);

// A simple, valid facility: site, facility, one air loop, one zone.
struct SimpleFacility {
  SiteRecord site;
  FacilityRecord facility;
  EquipmentRecord crah_a;
  EquipmentRecord crah_b;
  LoopRecord loop;
  ZoneRecord zone;
};

SimpleFacility simple_facility();

// Adds the simple facility to a builder and returns the equipment identifiers.
Result<void> add_simple_facility(SnapshotBuilder& builder, const SimpleFacility& facility);

// Adds the simple facility to a builder, replacing anything with the same
// identity that is already there. Used when a test publishes onto a store that
// already holds the facility, where a plain add would be a duplicate identity.
Result<void> upsert_simple_facility(SnapshotBuilder& builder, const SimpleFacility& facility);

// Builds and validates a snapshot from a builder, failing the test on error.
std::shared_ptr<const CoolingSnapshot> build_snapshot(SnapshotBuilder& builder);

// A monotone clock the tests move by hand.
struct TestClock {
  cooling_capacity::ManualClock clock;
  explicit TestClock(Timestamp start) : clock(start) {}
};

// Creates a directory for a test under the current working directory and
// removes any previous contents. The name is fixed so that residue is
// predictable and can be cleaned up.
Result<std::string> fresh_directory(const std::string& name);

// Removes a directory tree when it goes out of scope. Declared before the store
// and engine handles of a test, it is destroyed after them, which is what makes
// the removal succeed on Windows: a directory that still contains an open lock
// file cannot be deleted.
class Cleanup {
 public:
  explicit Cleanup(std::string path) : path_(std::move(path)) {}
  ~Cleanup();
  Cleanup(const Cleanup&) = delete;
  Cleanup& operator=(const Cleanup&) = delete;

 private:
  std::string path_;
};

}  // namespace ccap_test

#endif  // COOLING_CAPACITY_TESTS_SUPPORT_FIXTURES_HPP
