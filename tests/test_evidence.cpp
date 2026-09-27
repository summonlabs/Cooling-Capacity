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

using cooling_capacity::AnalysisBasis;
using cooling_capacity::CoolingAnalyzer;
using cooling_capacity::CoolingMedium;
using cooling_capacity::CoolingSnapshot;
using cooling_capacity::SnapshotBuilder;
using cooling_capacity::ZoneCapacity;

SnapshotBuilder facility_builder(const SimpleFacility& facility) {
  SnapshotBuilder builder;
  builder.set_generation(CapacityGeneration::first());
  builder.set_constructed_at(fixture_now());
  const Result<void> added = add_simple_facility(builder, facility);
  CCAP_CHECK_VOID(added);
  return builder;
}

Result<ZoneCapacity> zone_view(const CoolingSnapshot& snapshot) {
  const CoolingAnalyzer analyzer(snapshot, fixture_now(), AnalysisBasis::Fresh);
  return analyzer.zone_capacity(zone_id("hall-1"), CoolingMedium::Air,
                                CompatibilityClass::AirConvection);
}

}  // namespace

CCAP_TEST(a_zone_can_be_offered_less_than_its_capacity_by_a_reserve) {
  SnapshotBuilder builder = facility_builder(simple_facility());
  cooling_capacity::CoolingPolicy policy = cooling_capacity::CoolingPolicy::defaults();
  policy.minimum_reserve = ppm(250000);
  builder.set_policy(policy);
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  CCAP_CHECK_OK(view, zone_view(*snapshot));
  // 400 kW less a 25 percent reserve is 300 kW.
  CCAP_CHECK_EQ(view.offered.amount_or_zero().milliwatts(), 300000000);
  CCAP_CHECK_EQ(view.local_headroom.amount_or_zero().milliwatts(), 400000000);
}

CCAP_TEST(a_zone_reserve_floor_overrides_a_weaker_policy) {
  SimpleFacility facility = simple_facility();
  facility.zone.reserve_floor = ppm(500000);
  SnapshotBuilder builder = facility_builder(facility);
  cooling_capacity::CoolingPolicy policy = cooling_capacity::CoolingPolicy::defaults();
  policy.minimum_reserve = ppm(100000);
  builder.set_policy(policy);
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  CCAP_CHECK_OK(view, zone_view(*snapshot));
  CCAP_CHECK_EQ(view.offered.amount_or_zero().milliwatts(), 200000000);
}

CCAP_TEST(observed_load_reduces_physical_headroom_but_not_authority) {
  SimpleFacility facility = simple_facility();
  SnapshotBuilder builder = facility_builder(facility);
  EvidenceRecord measurement;
  measurement.id = evidence_id("load-1");
  measurement.kind = EvidenceKind::LoadMeasurement;
  measurement.source = EvidenceSource::BmsTelemetry;
  measurement.subject = zone_id("hall-1");
  measurement.medium = CoolingMedium::Air;
  measurement.value = milliwatts(150000000);
  measurement.observed_at = fixture_now();
  measurement.provenance.actor = actor_id("bms");
  measurement.provenance.reference = cooling_capacity::DocumentRef::literal("bms-1");
  measurement.revision = RecordRevision::first();
  CCAP_CHECK_VOID(builder.add(measurement));

  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  CCAP_CHECK_OK(view, zone_view(*snapshot));
  CCAP_CHECK_EQ(view.breakdown.observed.amount_or_zero().milliwatts(), 150000000);
  CCAP_CHECK_EQ(view.breakdown.observed_headroom.amount_or_zero().milliwatts(), 250000000);
  // Nothing has been committed, so the reserve is still the whole capacity.
  CCAP_CHECK_EQ(view.breakdown.reserve.amount_or_zero().milliwatts(), 400000000);
  CCAP_CHECK_EQ(view.offered.amount_or_zero().milliwatts(), 400000000);
}

CCAP_TEST(an_observation_outside_its_window_is_not_used) {
  SimpleFacility facility = simple_facility();
  SnapshotBuilder builder = facility_builder(facility);
  EvidenceRecord measurement;
  measurement.id = evidence_id("load-1");
  measurement.kind = EvidenceKind::LoadMeasurement;
  measurement.source = EvidenceSource::BmsTelemetry;
  measurement.subject = zone_id("hall-1");
  measurement.medium = CoolingMedium::Air;
  measurement.value = milliwatts(150000000);
  measurement.observed_at =
      Timestamp::from_unix_milliseconds(fixture_now().unix_milliseconds() - 3600000LL);
  measurement.provenance.actor = actor_id("bms");
  measurement.provenance.reference = cooling_capacity::DocumentRef::literal("bms-1");
  measurement.revision = RecordRevision::first();
  CCAP_CHECK_VOID(builder.add(measurement));

  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  CCAP_CHECK_OK(view, zone_view(*snapshot));
  CCAP_CHECK_EQ(view.breakdown.observed.status(), cooling_capacity::CapacityStatus::Unknown);
  CCAP_CHECK_EQ(view.breakdown.observed.reason(), cooling_capacity::CapacityReason::NoObservation);
}

CCAP_TEST(a_commitment_reduces_the_reserve_by_exactly_its_load) {
  SnapshotBuilder builder = facility_builder(simple_facility());
  cooling_capacity::CommitmentRecord commitment;
  commitment.id = cooling_capacity::CommitmentId::literal("c-1");
  commitment.attempt = cooling_capacity::AttemptId::literal("a-1");
  commitment.zone = zone_id("hall-1");
  commitment.pinned_loop = loop_id("air-loop-1");
  commitment.medium = CoolingMedium::Air;
  commitment.compatibility = CompatibilityClass::AirConvection;
  commitment.thermal = milliwatts(120000000);
  commitment.state = cooling_capacity::CommitmentState::Committed;
  commitment.actor = actor_id("operator");
  commitment.created_at = fixture_now();
  commitment.created_generation = CapacityGeneration::first();
  commitment.revision = RecordRevision::first();
  CCAP_CHECK_VOID(builder.add(commitment));

  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  CCAP_CHECK_OK(view, zone_view(*snapshot));
  CCAP_CHECK_EQ(view.breakdown.committed.amount_or_zero().milliwatts(), 120000000);
  CCAP_CHECK_EQ(view.breakdown.reserve.amount_or_zero().milliwatts(), 280000000);
  CCAP_CHECK_EQ(view.offered.amount_or_zero().milliwatts(), 280000000);
  CCAP_CHECK_FALSE(view.breakdown.over_committed);
}

CCAP_TEST(a_planned_commitment_consumes_nothing) {
  SnapshotBuilder builder = facility_builder(simple_facility());
  cooling_capacity::CommitmentRecord commitment;
  commitment.id = cooling_capacity::CommitmentId::literal("c-1");
  commitment.attempt = cooling_capacity::AttemptId::literal("a-1");
  commitment.zone = zone_id("hall-1");
  commitment.pinned_loop = loop_id("air-loop-1");
  commitment.medium = CoolingMedium::Air;
  commitment.compatibility = CompatibilityClass::AirConvection;
  commitment.thermal = milliwatts(120000000);
  commitment.state = cooling_capacity::CommitmentState::Planned;
  commitment.actor = actor_id("operator");
  commitment.created_at = fixture_now();
  commitment.created_generation = CapacityGeneration::first();
  commitment.revision = RecordRevision::first();
  CCAP_CHECK_VOID(builder.add(commitment));

  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  CCAP_CHECK_OK(view, zone_view(*snapshot));
  CCAP_CHECK_EQ(view.breakdown.committed.amount_or_zero().milliwatts(), 0);
  CCAP_CHECK_EQ(view.offered.amount_or_zero().milliwatts(), 400000000);
}

CCAP_TEST(over_commitment_is_reported_rather_than_clamped_away) {
  SnapshotBuilder builder = facility_builder(simple_facility());
  cooling_capacity::CommitmentRecord commitment;
  commitment.id = cooling_capacity::CommitmentId::literal("c-1");
  commitment.attempt = cooling_capacity::AttemptId::literal("a-1");
  commitment.zone = zone_id("hall-1");
  commitment.pinned_loop = loop_id("air-loop-1");
  commitment.medium = CoolingMedium::Air;
  commitment.compatibility = CompatibilityClass::AirConvection;
  commitment.thermal = milliwatts(900000000);
  commitment.state = cooling_capacity::CommitmentState::Committed;
  commitment.actor = actor_id("operator");
  commitment.created_at = fixture_now();
  commitment.created_generation = CapacityGeneration::first();
  commitment.revision = RecordRevision::first();
  CCAP_CHECK_VOID(builder.add(commitment));

  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  CCAP_CHECK_OK(view, zone_view(*snapshot));
  CCAP_CHECK(view.breakdown.over_committed);
  CCAP_CHECK_EQ(view.breakdown.deficit.amount_or_zero().milliwatts(), 500000000);
  CCAP_CHECK_EQ(view.offered.amount_or_zero().milliwatts(), 0);
}
