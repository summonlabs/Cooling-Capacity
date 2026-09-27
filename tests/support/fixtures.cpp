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

#include "fixtures.hpp"

#include <string>

#include "test_process.hpp"

namespace ccap_test {

SiteRecord make_site(const char* id) {
  SiteRecord record;
  record.id = SiteId::literal(id);
  record.label = BoundedText::literal(id);
  record.revision = RecordRevision::first();
  return record;
}

FacilityRecord make_facility(const char* id, const SiteId& site) {
  FacilityRecord record;
  record.id = FacilityId::literal(id);
  record.site = site;
  record.label = BoundedText::literal(id);
  record.revision = RecordRevision::first();
  return record;
}

EquipmentRecord make_unit(const char* id, EquipmentKind kind, std::int64_t nominal_watts) {
  EquipmentRecord record;
  record.id = EquipmentId::literal(id);
  record.facility = FacilityId::literal("dc-1");
  record.label = BoundedText::literal(id);
  record.kind = kind;
  record.state = OperatingState::InService;
  record.nominal = watts(nominal_watts);
  record.revision = RecordRevision::first();
  return record;
}

LoopRecord make_loop(const char* id, LoopKind kind, std::vector<EquipmentId> equipment,
                     std::int64_t transport_watts, cooling_capacity::RedundancyClass redundancy) {
  LoopRecord record;
  record.id = LoopId::literal(id);
  record.facility = FacilityId::literal("dc-1");
  record.label = BoundedText::literal(id);
  record.kind = kind;
  record.transport_limit = watts(transport_watts);
  record.transport_limit_declared = true;
  record.equipment = std::move(equipment);
  record.redundancy = redundancy;
  record.revision = RecordRevision::first();
  return record;
}

ZoneRecord make_zone(const char* id, std::vector<LoopId> loops,
                     std::vector<CompatibilityClass> classes) {
  ZoneRecord record;
  record.id = ZoneId::literal(id);
  record.facility = FacilityId::literal("dc-1");
  record.label = BoundedText::literal(id);
  for (const CompatibilityClass value : classes) {
    record.media.insert(cooling_capacity::medium_of(value));
  }
  record.compatibility = std::move(classes);
  record.loops = std::move(loops);
  record.revision = RecordRevision::first();
  return record;
}

PlantRecord make_plant(const char* id, CoolingMedium medium, std::vector<EquipmentId> equipment,
                       cooling_capacity::RedundancyClass redundancy) {
  PlantRecord record;
  record.id = PlantId::literal(id);
  record.facility = FacilityId::literal("dc-1");
  record.label = BoundedText::literal(id);
  record.medium = medium;
  record.equipment = std::move(equipment);
  record.redundancy = redundancy;
  record.revision = RecordRevision::first();
  return record;
}

SimpleFacility simple_facility() {
  SimpleFacility facility;
  facility.site = make_site("site-a");
  facility.facility = make_facility("dc-1", facility.site.id);
  facility.crah_a = make_unit("crah-a", EquipmentKind::Crah, 400000);
  facility.crah_b = make_unit("crah-b", EquipmentKind::Crah, 400000);
  facility.loop = make_loop("air-loop-1", LoopKind::AirSupply,
                            {facility.crah_a.id, facility.crah_b.id}, 700000,
                            cooling_capacity::RedundancyClass::NPlusOne);
  facility.zone = make_zone("hall-1", {facility.loop.id},
                            {CompatibilityClass::AirConvection});
  return facility;
}

Result<void> add_simple_facility(SnapshotBuilder& builder, const SimpleFacility& facility) {
  CCAP_TRY(builder.add(facility.site));
  CCAP_TRY(builder.add(facility.facility));
  CCAP_TRY(builder.add(facility.crah_a));
  CCAP_TRY(builder.add(facility.crah_b));
  CCAP_TRY(builder.add(facility.loop));
  CCAP_TRY(builder.add(facility.zone));
  return Result<void>();
}

Result<void> upsert_simple_facility(SnapshotBuilder& builder, const SimpleFacility& facility) {
  const auto upsert = [&builder](EntityKind kind, const Identifier& id, const auto& record) {
    return builder.contains(kind, id) ? builder.replace(record) : builder.add(record);
  };
  if (!builder.contains(EntityKind::Site, facility.site.id.value())) {
    CCAP_TRY(builder.add(facility.site));
  }
  if (!builder.contains(EntityKind::Facility, facility.facility.id.value())) {
    CCAP_TRY(builder.add(facility.facility));
  }
  CCAP_TRY(upsert(EntityKind::Equipment, facility.crah_a.id.value(), facility.crah_a));
  CCAP_TRY(upsert(EntityKind::Equipment, facility.crah_b.id.value(), facility.crah_b));
  CCAP_TRY(upsert(EntityKind::Loop, facility.loop.id.value(), facility.loop));
  CCAP_TRY(upsert(EntityKind::Zone, facility.zone.id.value(), facility.zone));
  return Result<void>();
}

std::shared_ptr<const CoolingSnapshot> build_snapshot(SnapshotBuilder& builder) {
  cooling_capacity::ValidationReport report;
  Result<std::shared_ptr<const CoolingSnapshot>> snapshot = builder.build(&report);
  if (!snapshot.ok()) {
    std::fprintf(stderr, "fixture snapshot failed validation:\n%s\n",
                 report.render().c_str());
    std::abort();
  }
  return std::move(snapshot).value();
}

Result<std::string> fresh_directory(const std::string& name) {
  CCAP_TRY(remove_tree(name));
  CCAP_TRY(make_directory(name));
  return name;
}

Cleanup::~Cleanup() {
  const Result<void> removed = remove_tree(path_);
  (void)removed;
}

}  // namespace ccap_test
