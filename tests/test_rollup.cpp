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

#include <cstdio>
#include <cstdlib>

namespace {

using cooling_capacity::AnalysisBasis;
using cooling_capacity::CapacityBasis;
using cooling_capacity::CapacityReason;
using cooling_capacity::CapacityStatus;
using cooling_capacity::CoolingAnalyzer;
using cooling_capacity::CoolingMedium;
using cooling_capacity::CoolingSnapshot;
using cooling_capacity::DerateFactor;
using cooling_capacity::DerateReason;
using cooling_capacity::DegradationBasis;
using cooling_capacity::LoopCapacity;
using cooling_capacity::SnapshotBuilder;
using cooling_capacity::ZoneCapacity;

SnapshotBuilder builder_with(const SimpleFacility& facility) {
  SnapshotBuilder builder;
  builder.set_generation(CapacityGeneration::first());
  builder.set_constructed_at(fixture_now());
  const Result<void> added = add_simple_facility(builder, facility);
  if (!added.ok()) {
    std::fprintf(stderr, "fixture setup failed: %s\n", added.error().to_string().c_str());
    std::abort();
  }
  return builder;
}

CoolingAnalyzer analyzer_for(const std::shared_ptr<const CoolingSnapshot>& snapshot) {
  return CoolingAnalyzer(*snapshot, fixture_now(), AnalysisBasis::Fresh);
}

}  // namespace

CCAP_TEST(loop_capacity_applies_the_declared_redundancy) {
  const SimpleFacility facility = simple_facility();
  SnapshotBuilder builder = builder_with(facility);
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer = analyzer_for(snapshot);

  CCAP_CHECK_OK(loop, analyzer.loop_capacity(loop_id("air-loop-1")));
  // Two 400 kW units under N+1 and a 700 kW transport limit: 400 kW is usable.
  CCAP_CHECK(loop.breakdown.usable.is_known());
  CCAP_CHECK_EQ(loop.breakdown.usable.amount_or_zero().milliwatts(), 400000000);
  CCAP_CHECK_EQ(loop.breakdown.nominal.amount_or_zero().milliwatts(), 800000000);
  CCAP_CHECK_EQ(loop.breakdown.validated.amount_or_zero().milliwatts(), 800000000);
  CCAP_CHECK_EQ(loop.redundancy.declared, cooling_capacity::RedundancyClass::NPlusOne);
  CCAP_CHECK_EQ(loop.redundancy.effective, cooling_capacity::RedundancyClass::NPlusOne);
  CCAP_CHECK_EQ(loop.redundancy.members, 2U);
  CCAP_CHECK_EQ(loop.redundancy.counted, 2U);
  CCAP_CHECK_EQ(loop.redundancy.excluded, 0U);
  CCAP_CHECK_EQ(loop.breakdown.basis, CapacityBasis::Nominal);
  CCAP_CHECK_EQ(loop.breakdown.reserve.amount_or_zero().milliwatts(), 400000000);
  CCAP_CHECK(loop.bottleneck.has_value());
}

CCAP_TEST(zone_capacity_rolls_up_its_loops) {
  const SimpleFacility facility = simple_facility();
  SnapshotBuilder builder = builder_with(facility);
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer = analyzer_for(snapshot);

  CCAP_CHECK_OK(zone, analyzer.zone_capacity(zone_id("hall-1"), CoolingMedium::Air,
                                             CompatibilityClass::AirConvection));
  CCAP_CHECK_EQ(zone.loops.size(), 1U);
  CCAP_CHECK_EQ(zone.shared_pools.size(), 0U);
  CCAP_CHECK_FALSE(zone.shared_headroom_is_shared);
  CCAP_CHECK_FALSE(zone.shared_headroom.has_value());
  CCAP_CHECK(zone.local_headroom.is_known());
  CCAP_CHECK_EQ(zone.local_headroom.amount_or_zero().milliwatts(), 400000000);
  CCAP_CHECK(zone.offered.is_known());
  CCAP_CHECK_EQ(zone.offered.amount_or_zero().milliwatts(), 400000000);
  CCAP_CHECK_EQ(zone.breakdown.nominal.amount_or_zero().milliwatts(), 800000000);
}

CCAP_TEST(zone_capacity_is_unsupported_where_the_medium_is_absent) {
  const SimpleFacility facility = simple_facility();
  SnapshotBuilder builder = builder_with(facility);
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer = analyzer_for(snapshot);

  CCAP_CHECK_OK(zone, analyzer.zone_capacity(zone_id("hall-1"), CoolingMedium::Liquid,
                                             CompatibilityClass::LiquidColdPlate));
  CCAP_CHECK_EQ(zone.offered.status(), CapacityStatus::Unsupported);
  CCAP_CHECK_EQ(zone.offered.reason(), CapacityReason::MediumNotSupported);
}

CCAP_TEST(unknown_identifiers_are_not_found) {
  const SimpleFacility facility = simple_facility();
  SnapshotBuilder builder = builder_with(facility);
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer = analyzer_for(snapshot);
  CCAP_CHECK_ERR(analyzer.loop_capacity(loop_id("absent")), cooling_capacity::ErrorCode::NotFound);
  CCAP_CHECK_ERR(analyzer.zone_capacity(zone_id("absent"), CoolingMedium::Air,
                                        CompatibilityClass::AirConvection),
                 cooling_capacity::ErrorCode::NotFound);
  CCAP_CHECK_ERR(analyzer.plant_capacity(plant_id("absent")),
                 cooling_capacity::ErrorCode::NotFound);
  CCAP_CHECK_ERR(analyzer.zone_capacity(zone_id("hall-1"), CoolingMedium::Air,
                                        CompatibilityClass::LiquidColdPlate),
                 cooling_capacity::ErrorCode::IncompatibleMedium);
}

CCAP_TEST(derating_is_applied_in_exact_integer_arithmetic) {
  SimpleFacility facility = simple_facility();
  facility.crah_a.derates = {
      DerateFactor{identifier("altitude"), ppm(900000), DerateReason::AltitudeAirDensity,
                   std::nullopt},
      DerateFactor{identifier("ambient"), ppm(900000), DerateReason::AmbientDesign,
                   std::nullopt},
  };
  facility.crah_b.derates = facility.crah_a.derates;
  SnapshotBuilder builder = builder_with(facility);
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer = analyzer_for(snapshot);

  CCAP_CHECK_OK(loop, analyzer.loop_capacity(loop_id("air-loop-1")));
  // Each unit: 400 kW * 0.9 * 0.9 = 324 kW. Under N+1: 648 - 324 = 324 kW.
  CCAP_CHECK_EQ(loop.breakdown.derated.amount_or_zero().milliwatts(), 648000000);
  CCAP_CHECK_EQ(loop.breakdown.usable.amount_or_zero().milliwatts(), 324000000);
  CCAP_CHECK_EQ(loop.equipment.size(), 2U);
  CCAP_CHECK_EQ(loop.equipment[0].cumulative_derate.parts_per_million(), 810000);
  CCAP_CHECK_EQ(loop.equipment[0].derated.amount_or_zero().milliwatts(), 324000000);
}

CCAP_TEST(a_deep_derate_below_the_policy_floor_is_unavailable_not_unknown) {
  SimpleFacility facility = simple_facility();
  facility.crah_a.derates = {DerateFactor{identifier("fouling"), ppm(10000),
                                          DerateReason::Fouling, std::nullopt}};
  facility.crah_b.derates = facility.crah_a.derates;
  SnapshotBuilder builder = builder_with(facility);
  cooling_capacity::CoolingPolicy policy = cooling_capacity::CoolingPolicy::defaults();
  policy.minimum_cumulative_derate = ppm(500000);
  builder.set_policy(policy);
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer = analyzer_for(snapshot);

  CCAP_CHECK_OK(loop, analyzer.loop_capacity(loop_id("air-loop-1")));
  CCAP_CHECK_EQ(loop.breakdown.usable.status(), CapacityStatus::Unavailable);
  CCAP_CHECK_EQ(loop.breakdown.usable.reason(), CapacityReason::DerateBelowFloor);
}

CCAP_TEST(degradation_and_validated_capacity_are_applied) {
  SimpleFacility facility = simple_facility();
  facility.crah_a.validated = watts(380000);
  facility.crah_b.validated = watts(380000);
  // A validated figure is used only when it is backed by evidence that passes
  // the policy's freshness window, so the fixture carries a commissioning
  // record for each unit.
  EvidenceRecord commissioning;
  commissioning.id = evidence_id("commissioning-1");
  commissioning.kind = EvidenceKind::CapacityValidation;
  commissioning.source = EvidenceSource::CommissioningTool;
  commissioning.subject = facility.crah_a.id;
  commissioning.medium = CoolingMedium::Air;
  commissioning.value = milliwatts(380000000);
  commissioning.observed_at = fixture_now();
  commissioning.provenance.actor = actor_id("commissioner");
  commissioning.provenance.reference = cooling_capacity::DocumentRef::literal("report-1");
  commissioning.revision = RecordRevision::first();
  facility.crah_a.validated_evidence = commissioning.id;

  EvidenceRecord commissioning_b = commissioning;
  commissioning_b.id = evidence_id("commissioning-2");
  commissioning_b.subject = facility.crah_b.id;
  facility.crah_b.validated_evidence = commissioning_b.id;

  facility.crah_a.degradation.factor = ppm(950000);
  facility.crah_a.degradation.basis = DegradationBasis::ConditionAssessment;
  facility.crah_a.degradation.assessed_at = fixture_now();
  facility.crah_b.degradation = facility.crah_a.degradation;

  SnapshotBuilder builder = builder_with(facility);
  CCAP_CHECK_VOID(builder.add(commissioning));
  CCAP_CHECK_VOID(builder.add(commissioning_b));
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer = analyzer_for(snapshot);

  CCAP_CHECK_OK(loop, analyzer.loop_capacity(loop_id("air-loop-1")));
  // 380 kW per unit, degraded to 361 kW. Under N+1: 722 - 361 = 361 kW.
  CCAP_CHECK_EQ(loop.breakdown.validated.amount_or_zero().milliwatts(), 760000000);
  CCAP_CHECK_EQ(loop.breakdown.degraded.amount_or_zero().milliwatts(), 722000000);
  CCAP_CHECK_EQ(loop.breakdown.usable.amount_or_zero().milliwatts(), 361000000);
  CCAP_CHECK_EQ(loop.breakdown.basis, CapacityBasis::Validated);
}

CCAP_TEST(a_validated_figure_without_evidence_cannot_outrank_the_nameplate) {
  SimpleFacility facility = simple_facility();
  facility.crah_a.validated = watts(380000);
  facility.crah_b.validated = watts(380000);
  SnapshotBuilder builder = builder_with(facility);
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer = analyzer_for(snapshot);

  CCAP_CHECK_OK(loop, analyzer.loop_capacity(loop_id("air-loop-1")));
  // The declared validation is ignored, the nameplate is used, and the basis
  // says so rather than pretending the figure was validated.
  CCAP_CHECK_EQ(loop.breakdown.validated.amount_or_zero().milliwatts(), 800000000);
  CCAP_CHECK_EQ(loop.breakdown.basis, CapacityBasis::Nominal);
}

CCAP_TEST(an_undeclared_transport_limit_makes_the_loop_unknown) {
  SimpleFacility facility = simple_facility();
  facility.loop.transport_limit_declared = false;
  SnapshotBuilder builder = builder_with(facility);
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer = analyzer_for(snapshot);

  CCAP_CHECK_OK(loop, analyzer.loop_capacity(loop_id("air-loop-1")));
  CCAP_CHECK_EQ(loop.breakdown.usable.status(), CapacityStatus::Unknown);
  CCAP_CHECK_EQ(loop.breakdown.usable.reason(), CapacityReason::TransportNotDeclared);
}

CCAP_TEST(a_transport_limited_loop_is_bound_by_its_transport) {
  SimpleFacility facility = simple_facility();
  facility.loop.transport_limit = watts(100000);
  SnapshotBuilder builder = builder_with(facility);
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer = analyzer_for(snapshot);

  CCAP_CHECK_OK(loop, analyzer.loop_capacity(loop_id("air-loop-1")));
  CCAP_CHECK_EQ(loop.breakdown.usable.amount_or_zero().milliwatts(), 100000000);
  CCAP_CHECK(loop.bottleneck.has_value());
  CCAP_CHECK_EQ(loop.bottleneck->kind, cooling_capacity::BottleneckKind::LoopTransport);
}

CCAP_TEST(an_out_of_service_unit_is_excluded_not_unknown) {
  SimpleFacility facility = simple_facility();
  facility.crah_b.state = OperatingState::Maintenance;
  SnapshotBuilder builder = builder_with(facility);
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer = analyzer_for(snapshot);

  CCAP_CHECK_OK(loop, analyzer.loop_capacity(loop_id("air-loop-1")));
  // One unit is counted; N+1 over a single unit leaves zero usable capacity.
  CCAP_CHECK(loop.breakdown.usable.is_known());
  CCAP_CHECK_EQ(loop.breakdown.usable.amount_or_zero().milliwatts(), 0);
  CCAP_CHECK_EQ(loop.redundancy.excluded, 1U);
  CCAP_CHECK_EQ(loop.redundancy.effective, cooling_capacity::RedundancyClass::None);
  CCAP_CHECK_EQ(loop.equipment[1].usable.status(), CapacityStatus::Unavailable);
  CCAP_CHECK_EQ(loop.equipment[1].usable.reason(), CapacityReason::Maintenance);
}

CCAP_TEST(an_unknown_operating_state_makes_the_group_unknown) {
  SimpleFacility facility = simple_facility();
  facility.crah_b.state = OperatingState::Unknown;
  SnapshotBuilder builder = builder_with(facility);
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer = analyzer_for(snapshot);

  CCAP_CHECK_OK(loop, analyzer.loop_capacity(loop_id("air-loop-1")));
  CCAP_CHECK_EQ(loop.breakdown.usable.status(), CapacityStatus::Unknown);
  CCAP_CHECK_EQ(loop.breakdown.usable.reason(), CapacityReason::OperatingStateUnknown);
  CCAP_CHECK(loop.redundancy.unknown_member_state);
}

CCAP_TEST(a_fully_out_of_service_loop_is_zero_not_its_transport_limit) {
  SimpleFacility facility = simple_facility();
  facility.crah_a.state = OperatingState::Faulted;
  facility.crah_b.state = OperatingState::Faulted;
  SnapshotBuilder builder = builder_with(facility);
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer = analyzer_for(snapshot);

  CCAP_CHECK_OK(loop, analyzer.loop_capacity(loop_id("air-loop-1")));
  // A declared 700 kW transport limit does not survive an equipment group that
  // is definitely empty: the smallest of the limits is zero.
  CCAP_CHECK_FALSE(loop.breakdown.usable.is_known());
  CCAP_CHECK_EQ(loop.breakdown.usable.status(), CapacityStatus::Unavailable);
  CCAP_CHECK_EQ(loop.breakdown.usable.reason(), CapacityReason::Faulted);
  CCAP_CHECK_EQ(loop.breakdown.reserve.amount_or_zero().milliwatts(), 0);
}

CCAP_TEST(the_same_generation_and_instant_always_give_the_same_answer) {
  const SimpleFacility facility = simple_facility();
  SnapshotBuilder builder = builder_with(facility);
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer first = analyzer_for(snapshot);
  const CoolingAnalyzer second = analyzer_for(snapshot);
  CCAP_CHECK_OK(left, first.zone_capacity(zone_id("hall-1"), CoolingMedium::Air,
                                          CompatibilityClass::AirConvection));
  CCAP_CHECK_OK(right, second.zone_capacity(zone_id("hall-1"), CoolingMedium::Air,
                                            CompatibilityClass::AirConvection));
  CCAP_CHECK(left == right);
  CCAP_CHECK(left.explanation == right.explanation);
}
