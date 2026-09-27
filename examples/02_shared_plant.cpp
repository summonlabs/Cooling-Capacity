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

// 02 - a shared plant.
//
// Two chilled-water loops draw on one chiller plant. The plant is a pool: its
// headroom belongs to every zone that draws on it, so it is reported once per
// pool and never added into a zone's own total. What a zone can offer a new
// load is min(local headroom, tightest shared pool headroom), less the zone's
// reserve.
//
// The example prints the local headroom, the pool headroom, the offer, and the
// binding constraint the analyzer chose.

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>

#include "cooling_capacity/cooling_capacity.hpp"

namespace ccap = cooling_capacity;

namespace {

using Milliwatts = std::int64_t;

// Two 150 kW chillers form the shared pool.
constexpr Milliwatts kChillerNominal = 150'000'000;
// Each loop's local transport equipment.
constexpr Milliwatts kCduNominal = 400'000'000;
constexpr Milliwatts kCduTransportLimit = 400'000'000;
// A load already held against the first loop, and therefore charged to the pool.
constexpr Milliwatts kHeldLoad = 50'000'000;

int failure(const ccap::Error& error) {
  std::cout << "FAILED: " << error.to_string() << "\n";
  return 1;
}

void print_figure(const std::string& label, const ccap::CapacityValue& value) {
  std::cout << std::left << std::setw(30) << label << value.to_string() << "\n";
}

// Checks a derived figure against the exact value this example expects.
ccap::Result<void> expect_known(const ccap::CapacityValue& value, Milliwatts expected,
                                const char* what) {
  if (!value.is_known()) {
    return ccap::Error(ccap::ErrorCode::Indeterminate, "a capacity figure is not known")
        .with("figure", what)
        .with("value", value.to_string());
  }
  if (value.amount_or_zero().milliwatts() != expected) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation,
                       "a capacity figure is not the expected one")
        .with("figure", what)
        .with("actual", value.to_string())
        .with("expected", ccap::ThermalPower::from_milliwatts(expected).to_string());
  }
  return ccap::Result<void>();
}

ccap::Result<void> expect_quantity(const ccap::ThermalPower& value, Milliwatts expected,
                                   const char* what) {
  if (value.milliwatts() != expected) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation,
                       "a thermal power is not the expected one")
        .with("quantity", what)
        .with("actual", value.to_string())
        .with("expected", ccap::ThermalPower::from_milliwatts(expected).to_string());
  }
  return ccap::Result<void>();
}

// One chiller plant, two chilled-water loops fed by it, and one liquid zone
// served by both loops. The plant pool is smaller than a single loop's local
// capacity, so the pool is what a new load actually competes for.
ccap::Result<void> add_model(ccap::SnapshotBuilder& builder, ccap::Timestamp instant) {
  ccap::SiteRecord site;
  site.id = ccap::SiteId::literal("site-a");
  site.label = ccap::BoundedText::literal("Site A");
  site.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(site));

  ccap::FacilityRecord facility;
  facility.id = ccap::FacilityId::literal("dc-1");
  facility.site = site.id;
  facility.label = ccap::BoundedText::literal("Data Center 1");
  facility.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(facility));

  ccap::EquipmentRecord chiller_one;
  chiller_one.id = ccap::EquipmentId::literal("chiller-1");
  chiller_one.facility = facility.id;
  chiller_one.label = ccap::BoundedText::literal("Chiller 1");
  chiller_one.kind = ccap::EquipmentKind::Chiller;
  chiller_one.state = ccap::OperatingState::InService;
  chiller_one.nominal = ccap::ThermalPower::from_milliwatts(kChillerNominal);
  chiller_one.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(chiller_one));

  ccap::EquipmentRecord chiller_two = chiller_one;
  chiller_two.id = ccap::EquipmentId::literal("chiller-2");
  chiller_two.label = ccap::BoundedText::literal("Chiller 2");
  CCAP_TRY(builder.add(chiller_two));

  // The pool. It is one plant record, so its capacity is counted once no matter
  // how many loops draw on it.
  ccap::PlantRecord plant;
  plant.id = ccap::PlantId::literal("plant-chillers");
  plant.facility = facility.id;
  plant.label = ccap::BoundedText::literal("Chiller plant");
  plant.medium = ccap::CoolingMedium::Liquid;
  plant.equipment = {chiller_one.id, chiller_two.id};
  plant.redundancy = ccap::RedundancyClass::None;
  plant.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(plant));

  // Each loop has its own CDU and its own declared transport limit; both are fed
  // by the same plant.
  for (const char* suffix : {"1", "2"}) {
    const std::string tag(suffix);
    ccap::EquipmentRecord cdu;
    cdu.id = ccap::EquipmentId::literal("cdu-" + tag);
    cdu.facility = facility.id;
    cdu.label = ccap::BoundedText::literal("CDU " + tag);
    cdu.kind = ccap::EquipmentKind::Cdu;
    cdu.state = ccap::OperatingState::InService;
    cdu.nominal = ccap::ThermalPower::from_milliwatts(kCduNominal);
    cdu.revision = ccap::RecordRevision::first();
    CCAP_TRY(builder.add(cdu));

    ccap::LoopRecord loop;
    loop.id = ccap::LoopId::literal("loop-cw-" + tag);
    loop.facility = facility.id;
    loop.label = ccap::BoundedText::literal("Chilled-water loop " + tag);
    loop.kind = ccap::LoopKind::ChilledWater;
    loop.transport_limit = ccap::ThermalPower::from_milliwatts(kCduTransportLimit);
    loop.transport_limit_declared = true;
    loop.equipment = {cdu.id};
    loop.primary_plant = plant.id;
    loop.redundancy = ccap::RedundancyClass::None;
    loop.revision = ccap::RecordRevision::first();
    CCAP_TRY(builder.add(loop));
  }

  ccap::ZoneRecord zone;
  zone.id = ccap::ZoneId::literal("hall-liquid");
  zone.facility = facility.id;
  zone.label = ccap::BoundedText::literal("Liquid-cooled hall");
  zone.media.insert(ccap::CoolingMedium::Liquid);
  zone.compatibility = {ccap::CompatibilityClass::LiquidColdPlate};
  zone.loops = {ccap::LoopId::literal("loop-cw-1"), ccap::LoopId::literal("loop-cw-2")};
  zone.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(zone));

  // One held load, pinned to the first loop. A loop's committed load is charged
  // to every plant that feeds it.
  ccap::CommitmentRecord held;
  held.id = ccap::CommitmentId::literal("load-77");
  held.attempt = ccap::AttemptId::literal("attempt-load-77");
  held.zone = zone.id;
  held.pinned_loop = ccap::LoopId::literal("loop-cw-1");
  held.medium = ccap::CoolingMedium::Liquid;
  held.compatibility = ccap::CompatibilityClass::LiquidColdPlate;
  held.thermal = ccap::ThermalPower::from_milliwatts(kHeldLoad);
  held.state = ccap::CommitmentState::Held;
  held.actor = ccap::ActorId::literal("operator-1");
  held.created_at = instant;
  held.created_generation = ccap::CapacityGeneration::first();
  held.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(held));
  return ccap::Result<void>();
}

ccap::Result<void> run() {
  CCAP_TRY_DECLARE(start, ccap::parse_timestamp("2026-04-06T12:00:00.000Z"));
  ccap::ManualClock clock(start);

  ccap::SnapshotBuilder builder;
  builder.set_generation(ccap::CapacityGeneration::first());
  builder.set_constructed_at(clock.now());
  CCAP_TRY(add_model(builder, clock.now()));

  ccap::ValidationReport report;
  CCAP_TRY_DECLARE(snapshot, builder.build(&report));
  if (!report.ok()) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation, "the example model failed validation")
        .with("findings", report.render());
  }

  const ccap::CoolingAnalyzer analyzer(*snapshot, clock.now(), ccap::AnalysisBasis::Fresh);
  CCAP_TRY_DECLARE(zone, analyzer.zone_capacity(ccap::ZoneId::literal("hall-liquid"),
                                                ccap::CoolingMedium::Liquid,
                                                ccap::CompatibilityClass::LiquidColdPlate));
  CCAP_TRY_DECLARE(plant, analyzer.plant_capacity(ccap::PlantId::literal("plant-chillers")));

  // The plant pool: 2 x 150000000 mW usable, less the 50000000 mW held against
  // the loop it feeds.
  CCAP_TRY(expect_known(plant.breakdown.usable, 300'000'000, "plant usable"));
  CCAP_TRY(expect_known(plant.breakdown.committed, kHeldLoad, "plant committed"));
  CCAP_TRY(expect_known(plant.breakdown.reserve, 250'000'000, "plant reserve"));

  // The zone: the two loops have 750000000 mW of local headroom between them,
  // but the pool offers only 250000000 mW, so that is the offer.
  CCAP_TRY(expect_known(zone.local_headroom, 750'000'000, "zone local headroom"));
  if (!zone.shared_headroom.has_value()) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation,
                       "the zone draws on a plant, so it must report shared headroom");
  }
  CCAP_TRY(expect_known(*zone.shared_headroom, 250'000'000, "zone shared headroom"));
  CCAP_TRY(expect_known(zone.offered, 250'000'000, "zone offered"));
  if (!zone.shared_headroom_is_shared) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation,
                       "the zone offer is constrained by a shared pool and must say so");
  }
  if (zone.shared_pools.size() != 1U) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation,
                       "the pool must be reported once, not once per loop")
        .with("pools", std::to_string(zone.shared_pools.size()));
  }
  if (zone.shared_pools.front().served_loops.size() != 2U) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation,
                       "both loops should be listed as drawing on the pool");
  }

  std::cout << "Cooling Capacity example 02: a shared plant\n";
  std::cout << "two chilled-water loops draw on one chiller plant\n\n";
  std::cout << std::left << std::setw(30) << "generation" << snapshot->generation().to_string()
            << "\n";
  std::cout << std::left << std::setw(30) << "evaluated at" << clock.now().to_string() << "\n";
  std::cout << std::left << std::setw(30) << "zone" << zone.zone.str() << "\n";
  std::cout << std::left << std::setw(30) << "pool" << plant.plant.str() << "\n\n";

  std::cout << "zone hall-liquid\n";
  print_figure("usable (sum of loops)", zone.breakdown.usable);
  print_figure("committed", zone.breakdown.committed);
  print_figure("local headroom", zone.local_headroom);
  print_figure("shared headroom (tightest)", *zone.shared_headroom);
  print_figure("offered", zone.offered);
  std::cout << std::left << std::setw(30) << "shared headroom is shared"
            << (zone.shared_headroom_is_shared ? "true" : "false") << "\n";
  std::cout << std::left << std::setw(30) << "bottleneck"
            << (zone.bottleneck.has_value() ? zone.bottleneck->to_string() : std::string("none"))
            << "\n\n";

  std::cout << "serving loops\n";
  for (const ccap::LoopCapacity& loop : zone.loops) {
    print_figure(std::string("  ") + loop.loop.str() + " usable", loop.breakdown.usable);
    print_figure(std::string("  ") + loop.loop.str() + " committed", loop.breakdown.committed);
    print_figure(std::string("  ") + loop.loop.str() + " reserve", loop.breakdown.reserve);
  }

  std::cout << "\nshared pool (reported once, not once per loop)\n";
  for (const ccap::SharedPoolView& pool : zone.shared_pools) {
    print_figure("  " + pool.plant.str() + " usable", pool.usable);
    print_figure("  " + pool.plant.str() + " committed", pool.committed);
    print_figure("  " + pool.plant.str() + " headroom", pool.headroom);
    std::string served;
    for (std::size_t index = 0; index < pool.served_loops.size(); ++index) {
      if (index != 0U) {
        served.push_back(',');
      }
      served.append(pool.served_loops[index].str());
    }
    std::cout << std::left << std::setw(30) << "  served loops" << served << "\n";
    std::cout << std::left << std::setw(30) << "  pool bottleneck"
              << (pool.bottleneck.has_value() ? pool.bottleneck->to_string()
                                              : std::string("none"))
              << "\n";
  }

  // The two mistakes this example is about, stated as numbers so that the model
  // cannot be read as if the pool were additive.
  const Milliwatts naive_sum = zone.local_headroom.amount_or_zero().milliwatts() +
                               zone.shared_headroom->amount_or_zero().milliwatts();
  CCAP_TRY(expect_quantity(ccap::ThermalPower::from_milliwatts(naive_sum), 1'000'000'000,
                           "local plus shared"));
  std::cout << "\nlocal + shared = " << ccap::ThermalPower::from_milliwatts(naive_sum).to_string()
            << " (wrong: the pool is not additive)\n";
  std::cout << "offer = min(local, shared) = " << zone.offered.to_string() << " (right)\n";
  std::cout << "the plant is charged once for both loops: plant committed "
            << plant.breakdown.committed.to_string() << "\n";

  std::cout << "\nexplanation of the zone answer\n";
  std::cout << ccap::render_steps(zone.explanation);
  return ccap::Result<void>();
}

}  // namespace

int main() {
  const ccap::Result<void> outcome = run();
  if (!outcome.ok()) {
    return failure(outcome.error());
  }
  std::cout << "\nexample 02 finished: ok\n";
  return 0;
}
