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
#include "test_harness.hpp"

using namespace ccap_test;

namespace {

using cooling_capacity::CoolingSnapshot;
using cooling_capacity::EntityKind;
using cooling_capacity::ErrorCode;
using cooling_capacity::SnapshotBuilder;
using cooling_capacity::SnapshotOrigin;

SnapshotBuilder fresh_builder() {
  SnapshotBuilder builder;
  builder.set_generation(CapacityGeneration::first());
  builder.set_constructed_at(fixture_now());
  return builder;
}

}  // namespace

CCAP_TEST(snapshot_rejects_a_zero_generation) {
  SnapshotBuilder builder;
  builder.set_constructed_at(fixture_now());
  CCAP_CHECK_ERR(builder.build_unvalidated(), ErrorCode::InvalidArgument);
}

CCAP_TEST(snapshot_canonicalises_record_order) {
  const SimpleFacility facility = simple_facility();
  SnapshotBuilder forward = fresh_builder();
  CCAP_CHECK_VOID(forward.add(facility.site));
  CCAP_CHECK_VOID(forward.add(facility.facility));
  CCAP_CHECK_VOID(forward.add(facility.crah_a));
  CCAP_CHECK_VOID(forward.add(facility.crah_b));
  CCAP_CHECK_VOID(forward.add(facility.loop));
  CCAP_CHECK_VOID(forward.add(facility.zone));

  SnapshotBuilder reversed = fresh_builder();
  CCAP_CHECK_VOID(reversed.add(facility.zone));
  CCAP_CHECK_VOID(reversed.add(facility.loop));
  CCAP_CHECK_VOID(reversed.add(facility.crah_b));
  CCAP_CHECK_VOID(reversed.add(facility.crah_a));
  CCAP_CHECK_VOID(reversed.add(facility.facility));
  CCAP_CHECK_VOID(reversed.add(facility.site));

  const std::shared_ptr<const CoolingSnapshot> first = build_snapshot(forward);
  const std::shared_ptr<const CoolingSnapshot> second = build_snapshot(reversed);
  CCAP_CHECK_EQ(first->digest(), second->digest());
  CCAP_CHECK_EQ(first->equipment().front().id.str(), std::string("crah-a"));
  CCAP_CHECK(first->find_zone(zone_id("hall-1")) != nullptr);
  CCAP_CHECK(first->find_loop(loop_id("absent")) == nullptr);
}

CCAP_TEST(snapshot_rejects_duplicate_identity) {
  const SimpleFacility facility = simple_facility();
  SnapshotBuilder builder = fresh_builder();
  CCAP_CHECK_VOID(builder.add(facility.crah_a));
  CCAP_CHECK_VOID(builder.add(facility.crah_a));
  CCAP_CHECK_ERR(builder.build_unvalidated(), ErrorCode::DuplicateIdentity);
}

CCAP_TEST(snapshot_rejects_duplicates_inside_a_set) {
  const SimpleFacility facility = simple_facility();
  LoopRecord loop = facility.loop;
  loop.equipment = {facility.crah_a.id, facility.crah_a.id};
  SnapshotBuilder builder = fresh_builder();
  CCAP_CHECK_VOID(builder.add(loop));
  CCAP_CHECK_ERR(builder.build_unvalidated(), ErrorCode::DuplicateIdentity);
}

CCAP_TEST(snapshot_replaces_and_removes) {
  const SimpleFacility facility = simple_facility();
  SnapshotBuilder builder = fresh_builder();
  CCAP_CHECK_VOID(add_simple_facility(builder, facility));

  EquipmentRecord changed = facility.crah_a;
  changed.nominal = watts(500000);
  CCAP_CHECK_VOID(builder.replace(changed));
  CCAP_CHECK(builder.contains(EntityKind::Equipment, identifier("crah-a")));

  EquipmentRecord absent = facility.crah_a;
  absent.id = equipment_id("crah-z");
  CCAP_CHECK_ERR(builder.replace(absent), ErrorCode::NotFound);

  CCAP_CHECK_VOID(builder.remove(EntityKind::Equipment, identifier("crah-b")));
  CCAP_CHECK_FALSE(builder.contains(EntityKind::Equipment, identifier("crah-b")));
  CCAP_CHECK_ERR(builder.remove(EntityKind::Equipment, identifier("crah-b")),
                 ErrorCode::NotFound);
  CCAP_CHECK_ERR(builder.remove(EntityKind::Policy, identifier("default-policy")),
                 ErrorCode::InvalidArgument);
}

CCAP_TEST(snapshot_limits_are_enforced_on_insert) {
  SnapshotBuilder builder = fresh_builder();
  cooling_capacity::Limits limits;
  limits.max_sites = 1;
  builder.set_limits(limits);
  CCAP_CHECK_VOID(builder.add(make_site("site-a")));
  CCAP_CHECK_ERR(builder.add(make_site("site-b")), ErrorCode::LimitExceeded);
}

CCAP_TEST(snapshot_provenance_is_separate_from_contents) {
  const SimpleFacility facility = simple_facility();
  SnapshotBuilder builder = fresh_builder();
  CCAP_CHECK_VOID(add_simple_facility(builder, facility));
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  CCAP_CHECK_EQ(snapshot->origin(), SnapshotOrigin::Constructed);
  CCAP_CHECK_FALSE(snapshot->revalidated_at().has_value());

  const std::shared_ptr<const CoolingSnapshot> recovered =
      snapshot->with_provenance(SnapshotOrigin::RecoveredFromStore, std::nullopt);
  CCAP_CHECK_EQ(recovered->origin(), SnapshotOrigin::RecoveredFromStore);
  // The provenance stamp changes the authority of the state, not the state.
  CCAP_CHECK_EQ(recovered->digest(), snapshot->digest());
  CCAP_CHECK_EQ(recovered->record_count(), snapshot->record_count());

  const std::shared_ptr<const CoolingSnapshot> revalidated =
      snapshot->with_provenance(SnapshotOrigin::Constructed, fixture_now());
  CCAP_CHECK(revalidated->revalidated_at().has_value());
  CCAP_CHECK_EQ(revalidated->revalidated_at().value(), fixture_now());
}

CCAP_TEST(snapshot_record_count_covers_every_collection) {
  const SimpleFacility facility = simple_facility();
  SnapshotBuilder builder = fresh_builder();
  CCAP_CHECK_VOID(add_simple_facility(builder, facility));
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  CCAP_CHECK_EQ(snapshot->record_count(), 6U);
  CCAP_CHECK_EQ(snapshot->sites().size(), 1U);
  CCAP_CHECK_EQ(snapshot->facilities().size(), 1U);
  CCAP_CHECK_EQ(snapshot->equipment().size(), 2U);
  CCAP_CHECK_EQ(snapshot->loops().size(), 1U);
  CCAP_CHECK_EQ(snapshot->zones().size(), 1U);
  CCAP_CHECK_EQ(snapshot->evidence().size(), 0U);
  CCAP_CHECK_EQ(snapshot->commitments().size(), 0U);
}

CCAP_TEST(snapshot_build_reports_findings) {
  const SimpleFacility facility = simple_facility();
  LoopRecord orphan = facility.loop;
  orphan.id = loop_id("air-loop-2");
  orphan.equipment = {};
  orphan.transport_limit_declared = false;
  SnapshotBuilder builder = fresh_builder();
  CCAP_CHECK_VOID(add_simple_facility(builder, facility));
  CCAP_CHECK_VOID(builder.add(orphan));

  cooling_capacity::ValidationReport report;
  CCAP_CHECK_ERR(builder.build(&report), ErrorCode::InvariantViolation);
  CCAP_CHECK(report.has(cooling_capacity::ValidationCode::LoopWithoutEquipment));
  CCAP_CHECK(report.error_count() >= 1U);
  CCAP_CHECK_FALSE(report.ok());
}
