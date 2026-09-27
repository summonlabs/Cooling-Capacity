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
using cooling_capacity::CoolingAnalyzer;
using cooling_capacity::CoolingSnapshot;
using cooling_capacity::LoopCapacity;
using cooling_capacity::RedundancyClass;
using cooling_capacity::SnapshotBuilder;

// One loop with three 100 kW units, so every redundancy formula is a round
// number and any arithmetic slip shows up immediately.
SnapshotBuilder group_builder(RedundancyClass redundancy, OperatingState state) {
  SnapshotBuilder builder;
  builder.set_generation(CapacityGeneration::first());
  builder.set_constructed_at(fixture_now());
  const SimpleFacility facility = simple_facility();
  const Result<void> site = builder.add(facility.site);
  const Result<void> dc = builder.add(facility.facility);
  EquipmentRecord a = make_unit("unit-a", EquipmentKind::Chiller, 100000);
  EquipmentRecord b = make_unit("unit-b", EquipmentKind::Chiller, 100000);
  EquipmentRecord c = make_unit("unit-c", EquipmentKind::Chiller, 100000);
  c.state = state;
  const Result<void> added_a = builder.add(a);
  const Result<void> added_b = builder.add(b);
  const Result<void> added_c = builder.add(c);
  LoopRecord loop = make_loop("liquid-loop", LoopKind::ChilledWater, {a.id, b.id, c.id}, 1000000,
                              redundancy);
  const Result<void> added_loop = builder.add(loop);
  ZoneRecord zone = make_zone("hall-1", {loop.id}, {CompatibilityClass::LiquidColdPlate});
  const Result<void> added_zone = builder.add(zone);
  if (!site.ok() || !dc.ok() || !added_a.ok() || !added_b.ok() || !added_c.ok() ||
      !added_loop.ok() || !added_zone.ok()) {
    std::fprintf(stderr, "group fixture setup failed\n");
    std::abort();
  }
  return builder;
}

std::int64_t usable_milliwatts(RedundancyClass redundancy,
                               OperatingState state = OperatingState::InService) {
  SnapshotBuilder builder = group_builder(redundancy, state);
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer(*snapshot, fixture_now(), AnalysisBasis::Fresh);
  const Result<LoopCapacity> loop = analyzer.loop_capacity(loop_id("liquid-loop"));
  if (!loop.ok() || !loop.value().breakdown.usable.is_known()) {
    std::fprintf(stderr, "group rollup failed\n");
    std::abort();
  }
  return loop.value().breakdown.usable.amount_or_zero().milliwatts();
}

}  // namespace

CCAP_TEST(redundancy_formulas_are_exact) {
  // Three 100 kW units, total 300 kW.
  CCAP_CHECK_EQ(usable_milliwatts(RedundancyClass::None), 300000000);
  CCAP_CHECK_EQ(usable_milliwatts(RedundancyClass::NPlusOne), 200000000);
  CCAP_CHECK_EQ(usable_milliwatts(RedundancyClass::NPlusTwo), 100000000);
  CCAP_CHECK_EQ(usable_milliwatts(RedundancyClass::TwoN), 150000000);
  CCAP_CHECK_EQ(usable_milliwatts(RedundancyClass::TwoNPlusOne), 100000000);
  CCAP_CHECK_EQ(usable_milliwatts(RedundancyClass::TwoNPlusTwo), 50000000);
}

CCAP_TEST(redundancy_formula_uses_the_largest_unit_not_the_average) {
  SnapshotBuilder builder;
  builder.set_generation(CapacityGeneration::first());
  builder.set_constructed_at(fixture_now());
  const SimpleFacility facility = simple_facility();
  CCAP_CHECK_VOID(builder.add(facility.site));
  CCAP_CHECK_VOID(builder.add(facility.facility));
  EquipmentRecord big = make_unit("unit-big", EquipmentKind::Chiller, 300000);
  EquipmentRecord small = make_unit("unit-small", EquipmentKind::Chiller, 100000);
  CCAP_CHECK_VOID(builder.add(big));
  CCAP_CHECK_VOID(builder.add(small));
  LoopRecord loop = make_loop("liquid-loop", LoopKind::ChilledWater, {big.id, small.id}, 1000000,
                              RedundancyClass::NPlusOne);
  CCAP_CHECK_VOID(builder.add(loop));
  ZoneRecord zone = make_zone("hall-1", {loop.id}, {CompatibilityClass::LiquidColdPlate});
  CCAP_CHECK_VOID(builder.add(zone));

  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer(*snapshot, fixture_now(), AnalysisBasis::Fresh);
  CCAP_CHECK_OK(view, analyzer.loop_capacity(loop_id("liquid-loop")));
  // 400 kW total minus the largest (300 kW) leaves 100 kW.
  CCAP_CHECK_EQ(view.breakdown.usable.amount_or_zero().milliwatts(), 100000000);
}

CCAP_TEST(one_excluded_unit_degrades_the_effective_class_and_the_capacity) {
  // Declared N+1 over three units, one in maintenance: two units remain, so the
  // N+1 formula over 200 kW leaves 100 kW and the label drops to N.
  CCAP_CHECK_EQ(usable_milliwatts(RedundancyClass::NPlusOne, OperatingState::Maintenance),
                100000000);
  SnapshotBuilder builder = group_builder(RedundancyClass::NPlusOne, OperatingState::Maintenance);
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer(*snapshot, fixture_now(), AnalysisBasis::Fresh);
  CCAP_CHECK_OK(view, analyzer.loop_capacity(loop_id("liquid-loop")));
  CCAP_CHECK_EQ(view.redundancy.declared, RedundancyClass::NPlusOne);
  CCAP_CHECK_EQ(view.redundancy.effective, RedundancyClass::None);
  CCAP_CHECK_EQ(view.redundancy.members, 3U);
  CCAP_CHECK_EQ(view.redundancy.counted, 2U);
  CCAP_CHECK_EQ(view.redundancy.excluded, 1U);
}

CCAP_TEST(a_standby_unit_carries_no_present_load_but_supports_redundancy) {
  SnapshotBuilder builder = group_builder(RedundancyClass::NPlusOne, OperatingState::Standby);
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer(*snapshot, fixture_now(), AnalysisBasis::Fresh);
  CCAP_CHECK_OK(view, analyzer.loop_capacity(loop_id("liquid-loop")));
  // Two units in service (200 kW) and one standby (100 kW) counted for the
  // formula: 300 - 100 = 200 kW.
  CCAP_CHECK_EQ(view.breakdown.usable.amount_or_zero().milliwatts(), 200000000);
  CCAP_CHECK_EQ(view.redundancy.counted, 3U);
}

CCAP_TEST(every_unit_out_of_service_is_a_known_zero_not_an_unknown) {
  SnapshotBuilder builder = group_builder(RedundancyClass::NPlusOne, OperatingState::Faulted);
  // The group fixture faults one unit; every unit has to be out of service for
  // this test to be about a group that is definitely empty.
  for (const char* name : {"unit-a", "unit-b", "unit-c"}) {
    EquipmentRecord record = make_unit(name, EquipmentKind::Chiller, 100000);
    record.state = OperatingState::Faulted;
    CCAP_CHECK_VOID(builder.replace(record));
  }
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer(*snapshot, fixture_now(), AnalysisBasis::Fresh);
  CCAP_CHECK_OK(view, analyzer.loop_capacity(loop_id("liquid-loop")));
  CCAP_CHECK_EQ(view.breakdown.usable.status(), cooling_capacity::CapacityStatus::Unavailable);
  CCAP_CHECK_EQ(view.breakdown.usable.reason(), cooling_capacity::CapacityReason::Faulted);
  CCAP_CHECK(view.redundancy.unknown_member_state == false);
  CCAP_CHECK_EQ(view.redundancy.counted, 0U);
  CCAP_CHECK_EQ(view.redundancy.excluded, 3U);
}
