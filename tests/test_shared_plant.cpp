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
using cooling_capacity::CapacityValue;
using cooling_capacity::CommitmentRecord;
using cooling_capacity::CommitmentState;
using cooling_capacity::CoolingAnalyzer;
using cooling_capacity::CoolingSnapshot;
using cooling_capacity::CoolingMedium;
using cooling_capacity::PlantCapacity;
using cooling_capacity::PlantSupplyMode;
using cooling_capacity::RedundancyClass;
using cooling_capacity::SnapshotBuilder;
using cooling_capacity::ZoneCapacity;

// Two liquid loops, each with a 500 kW CDU, both fed by one chiller plant with
// two 1000 kW chillers in N+1. The plant therefore holds 1000 kW in total, and
// that pool is shared.
SnapshotBuilder shared_plant_builder() {
  SnapshotBuilder builder;
  builder.set_generation(CapacityGeneration::first());
  builder.set_constructed_at(fixture_now());
  const SimpleFacility simple = simple_facility();
  const Result<void> site = builder.add(simple.site);
  const Result<void> dc = builder.add(simple.facility);
  EquipmentRecord chiller_a = make_unit("chiller-a", EquipmentKind::Chiller, 1000000);
  EquipmentRecord chiller_b = make_unit("chiller-b", EquipmentKind::Chiller, 1000000);
  EquipmentRecord cdu_a = make_unit("cdu-a", EquipmentKind::Cdu, 500000);
  EquipmentRecord cdu_b = make_unit("cdu-b", EquipmentKind::Cdu, 500000);
  const Result<void> added_chiller_a = builder.add(chiller_a);
  const Result<void> added_chiller_b = builder.add(chiller_b);
  const Result<void> added_cdu_a = builder.add(cdu_a);
  const Result<void> added_cdu_b = builder.add(cdu_b);
  PlantRecord plant = make_plant("plant-1", CoolingMedium::Liquid, {chiller_a.id, chiller_b.id},
                                 RedundancyClass::NPlusOne);
  const Result<void> added_plant = builder.add(plant);
  LoopRecord loop_a = make_loop("liquid-loop-a", LoopKind::SecondaryLiquid, {cdu_a.id}, 600000,
                                RedundancyClass::None);
  loop_a.primary_plant = plant.id;
  LoopRecord loop_b = make_loop("liquid-loop-b", LoopKind::SecondaryLiquid, {cdu_b.id}, 600000,
                                RedundancyClass::None);
  loop_b.primary_plant = plant.id;
  const Result<void> added_loop_a = builder.add(loop_a);
  const Result<void> added_loop_b = builder.add(loop_b);
  ZoneRecord zone_a = make_zone("hall-a", {loop_a.id}, {CompatibilityClass::LiquidColdPlate});
  ZoneRecord zone_b = make_zone("hall-b", {loop_b.id}, {CompatibilityClass::LiquidColdPlate});
  const Result<void> added_zone_a = builder.add(zone_a);
  const Result<void> added_zone_b = builder.add(zone_b);
  if (!site.ok() || !dc.ok() || !added_chiller_a.ok() || !added_chiller_b.ok() ||
      !added_cdu_a.ok() || !added_cdu_b.ok() || !added_plant.ok() || !added_loop_a.ok() ||
      !added_loop_b.ok() || !added_zone_a.ok() || !added_zone_b.ok()) {
    std::fprintf(stderr, "shared plant fixture setup failed\n");
    std::abort();
  }
  return builder;
}

// Adds a consuming commitment pinned to a loop, so the shared pool carries load.
Result<void> add_commitment(SnapshotBuilder& builder, const char* commitment, const char* attempt,
                            const char* zone, const char* loop, std::int64_t thermal_milliwatts) {
  CommitmentRecord record;
  record.id = cooling_capacity::CommitmentId::literal(commitment);
  record.attempt = cooling_capacity::AttemptId::literal(attempt);
  record.zone = zone_id(zone);
  record.pinned_loop = loop_id(loop);
  record.medium = CoolingMedium::Liquid;
  record.compatibility = CompatibilityClass::LiquidColdPlate;
  record.thermal = milliwatts(thermal_milliwatts);
  record.state = CommitmentState::Committed;
  record.actor = actor_id("operator");
  record.created_at = fixture_now();
  record.created_generation = CapacityGeneration::first();
  record.revision = RecordRevision::first();
  return builder.add(record);
}

}  // namespace

CCAP_TEST(a_shared_plant_is_counted_once_per_loop_not_once_per_pool_member) {
  SnapshotBuilder builder = shared_plant_builder();
  CCAP_CHECK_OK(commitment, add_commitment(builder, "c-a", "a-1", "hall-a", "liquid-loop-a",
                                           300000000));
  CCAP_CHECK_OK(commitment_b, add_commitment(builder, "c-b", "a-2", "hall-b", "liquid-loop-b",
                                             300000000));
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer(*snapshot, fixture_now(), AnalysisBasis::Fresh);

  CCAP_CHECK_OK(plant, analyzer.plant_capacity(plant_id("plant-1")));
  // Two 1000 kW chillers under N+1 leave 1000 kW in the pool, with 600 kW
  // committed across both loops exactly once.
  CCAP_CHECK_EQ(plant.breakdown.usable.amount_or_zero().milliwatts(), 1000000000);
  CCAP_CHECK_EQ(plant.breakdown.committed.amount_or_zero().milliwatts(), 600000000);
  CCAP_CHECK_EQ(plant.breakdown.reserve.amount_or_zero().milliwatts(), 400000000);
  CCAP_CHECK_EQ(plant.served_loops.size(), 2U);

  CCAP_CHECK_OK(zone_a, analyzer.zone_capacity(zone_id("hall-a"), CoolingMedium::Liquid,
                                               CompatibilityClass::LiquidColdPlate));
  CCAP_CHECK_OK(zone_b, analyzer.zone_capacity(zone_id("hall-b"), CoolingMedium::Liquid,
                                               CompatibilityClass::LiquidColdPlate));
  // Local headroom is the CDU capacity less its own load: 500 - 300 = 200 kW.
  CCAP_CHECK_EQ(zone_a.local_headroom.amount_or_zero().milliwatts(), 200000000);
  CCAP_CHECK_EQ(zone_b.local_headroom.amount_or_zero().milliwatts(), 200000000);
  // The shared pool offers 400 kW to each zone, and the zone offer is the
  // smaller of the two.
  CCAP_CHECK(zone_a.shared_headroom.has_value());
  CCAP_CHECK(zone_a.shared_headroom_is_shared);
  CCAP_CHECK_EQ(zone_a.shared_headroom->amount_or_zero().milliwatts(), 400000000);
  CCAP_CHECK_EQ(zone_b.shared_headroom->amount_or_zero().milliwatts(), 400000000);
  CCAP_CHECK_EQ(zone_a.offered.amount_or_zero().milliwatts(), 200000000);
  CCAP_CHECK_EQ(zone_b.offered.amount_or_zero().milliwatts(), 200000000);
  // The two offers together equal the pool headroom: they are not additive.
  CCAP_CHECK_EQ(zone_a.offered.amount_or_zero().milliwatts() +
                    zone_b.offered.amount_or_zero().milliwatts(),
                zone_a.shared_headroom->amount_or_zero().milliwatts());
  CCAP_CHECK_EQ(zone_a.shared_pools.size(), 1U);
  CCAP_CHECK_EQ(zone_a.shared_pools[0].plant.str(), std::string("plant-1"));
}

CCAP_TEST(a_shared_pool_binds_a_zone_that_has_local_room) {
  SnapshotBuilder builder = shared_plant_builder();
  // Loop B takes almost the whole pool, so loop A has local room but no shared
  // headroom left.
  CCAP_CHECK_OK(commitment, add_commitment(builder, "c-b", "a-2", "hall-b", "liquid-loop-b",
                                           950000000));
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer(*snapshot, fixture_now(), AnalysisBasis::Fresh);

  CCAP_CHECK_OK(zone_a, analyzer.zone_capacity(zone_id("hall-a"), CoolingMedium::Liquid,
                                               CompatibilityClass::LiquidColdPlate));
  CCAP_CHECK_EQ(zone_a.local_headroom.amount_or_zero().milliwatts(), 500000000);
  CCAP_CHECK_EQ(zone_a.shared_headroom->amount_or_zero().milliwatts(), 50000000);
  CCAP_CHECK_EQ(zone_a.offered.amount_or_zero().milliwatts(), 50000000);
  CCAP_CHECK(zone_a.bottleneck.has_value());
  CCAP_CHECK_EQ(zone_a.bottleneck->kind, cooling_capacity::BottleneckKind::SharedPool);
  CCAP_CHECK_EQ(zone_a.bottleneck->subject.str(), std::string("plant-1"));
}

CCAP_TEST(an_alternating_supply_charges_both_plants_and_takes_the_tighter_one) {
  SnapshotBuilder builder = shared_plant_builder();
  // Split the plant in two: plant-2 has one 800 kW chiller with no redundancy.
  EquipmentRecord chiller_c = make_unit("chiller-c", EquipmentKind::Chiller, 800000);
  CCAP_CHECK_VOID(builder.add(chiller_c));
  PlantRecord plant_two =
      make_plant("plant-2", CoolingMedium::Liquid, {chiller_c.id}, RedundancyClass::None);
  CCAP_CHECK_VOID(builder.add(plant_two));
  CCAP_CHECK_OK(loop, builder.replace([&builder]() {
    const cooling_capacity::LoopRecord* existing = nullptr;
    (void)existing;
    cooling_capacity::LoopRecord record = make_loop(
        "liquid-loop-a", LoopKind::SecondaryLiquid, {equipment_id("cdu-a")}, 600000,
        RedundancyClass::None);
    record.primary_plant = plant_id("plant-1");
    record.secondary_plant = plant_id("plant-2");
    record.supply_mode = PlantSupplyMode::Alternates;
    (void)builder;
    return record;
  }()));

  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer(*snapshot, fixture_now(), AnalysisBasis::Fresh);
  CCAP_CHECK_OK(zone, analyzer.zone_capacity(zone_id("hall-a"), CoolingMedium::Liquid,
                                             CompatibilityClass::LiquidColdPlate));
  CCAP_CHECK(zone.shared_pools.size() >= 1U);
  // The tighter supply decides: 800 kW of plant-2 against 1000 kW of plant-1.
  CCAP_CHECK(zone.shared_headroom.has_value());
  CCAP_CHECK_EQ(zone.shared_headroom->amount_or_zero().milliwatts(), 800000000);

  CCAP_CHECK_OK(plant_two_view, analyzer.plant_capacity(plant_id("plant-2")));
  CCAP_CHECK_EQ(plant_two_view.breakdown.usable.amount_or_zero().milliwatts(), 800000000);
}

CCAP_TEST(equipment_shared_between_two_loops_is_divided_not_duplicated) {
  SnapshotBuilder builder;
  builder.set_generation(CapacityGeneration::first());
  builder.set_constructed_at(fixture_now());
  const SimpleFacility simple = simple_facility();
  CCAP_CHECK_VOID(builder.add(simple.site));
  CCAP_CHECK_VOID(builder.add(simple.facility));
  // One 600 kW shared air handler feeding two 600 kW loops.
  EquipmentRecord shared = make_unit("ahu-shared", EquipmentKind::AirHandler, 600000);
  CCAP_CHECK_VOID(builder.add(shared));
  LoopRecord loop_a =
      make_loop("air-loop-a", LoopKind::AirSupply, {shared.id}, 600000, RedundancyClass::None);
  LoopRecord loop_b =
      make_loop("air-loop-b", LoopKind::AirSupply, {shared.id}, 600000, RedundancyClass::None);
  CCAP_CHECK_VOID(builder.add(loop_a));
  CCAP_CHECK_VOID(builder.add(loop_b));
  ZoneRecord zone = make_zone("hall-1", {loop_a.id, loop_b.id},
                              {CompatibilityClass::AirConvection});
  CCAP_CHECK_VOID(builder.add(zone));

  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer(*snapshot, fixture_now(), AnalysisBasis::Fresh);
  CCAP_CHECK_OK(loop, analyzer.loop_capacity(loop_id("air-loop-a")));
  // 600 kW shared by two loops is 300 kW on each, not 600 kW on each.
  CCAP_CHECK_EQ(loop.breakdown.usable.amount_or_zero().milliwatts(), 300000000);
  CCAP_CHECK_EQ(loop.equipment[0].sharing_loops, 2U);
  CCAP_CHECK(loop.equipment[0].shared);

  CCAP_CHECK_OK(zone_view, analyzer.zone_capacity(zone_id("hall-1"), CoolingMedium::Air,
                                                  CompatibilityClass::AirConvection));
  CCAP_CHECK_EQ(zone_view.breakdown.usable.amount_or_zero().milliwatts(), 600000000);
}
