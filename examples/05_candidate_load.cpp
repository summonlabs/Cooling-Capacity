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

// 05 - evaluating a candidate load.
//
// A candidate load is a proposed thermal load asking whether a zone can carry
// it. The answer is one of three dispositions:
//
//   Admitted       the zone's offer covers the load;
//   Rejected       the offer is known and smaller than the load - a definite no;
//   Indeterminate  the offer cannot be established, so the honest answer is
//                  "not known" and the caller must supply fresh evidence.
//
// The example evaluates five requests against two zones, including the exact
// boundary, an oversized request, an unknown-capacity zone and an invalid
// request that never reaches the accounting at all.

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>

#include "cooling_capacity/cooling_capacity.hpp"

namespace ccap = cooling_capacity;

namespace {

using Milliwatts = std::int64_t;

constexpr Milliwatts kCrahNominal = 400'000'000;
constexpr Milliwatts kSmallCrahNominal = 300'000'000;
// The surveyed transport limit of the first loop: below the equipment total, so
// the path is the binding constraint.
constexpr Milliwatts kLoopTransportLimit = 350'000'000;

int failure(const ccap::Error& error) {
  std::cout << "FAILED: " << error.to_string() << "\n";
  return 1;
}

void print_figure(const std::string& label, const ccap::CapacityValue& value) {
  std::cout << std::left << std::setw(26) << label << value.to_string() << "\n";
}

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

// Two air loops: one with a surveyed transport path and two units in N+1, one
// whose only unit has an unknown operating state.
ccap::Result<void> add_model(ccap::SnapshotBuilder& builder) {
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

  ccap::EquipmentRecord crah_one;
  crah_one.id = ccap::EquipmentId::literal("crah-1");
  crah_one.facility = facility.id;
  crah_one.label = ccap::BoundedText::literal("CRAH 1");
  crah_one.kind = ccap::EquipmentKind::Crah;
  crah_one.state = ccap::OperatingState::InService;
  crah_one.nominal = ccap::ThermalPower::from_milliwatts(kCrahNominal);
  crah_one.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(crah_one));

  ccap::EquipmentRecord crah_two = crah_one;
  crah_two.id = ccap::EquipmentId::literal("crah-2");
  crah_two.label = ccap::BoundedText::literal("CRAH 2");
  CCAP_TRY(builder.add(crah_two));

  // The operating state of this unit has never been reported. Unknown state is
  // not "probably fine" and it is not zero: the group cannot be summed.
  ccap::EquipmentRecord crah_three = crah_one;
  crah_three.id = ccap::EquipmentId::literal("crah-3");
  crah_three.label = ccap::BoundedText::literal("CRAH 3");
  crah_three.state = ccap::OperatingState::Unknown;
  crah_three.nominal = ccap::ThermalPower::from_milliwatts(kSmallCrahNominal);
  CCAP_TRY(builder.add(crah_three));

  ccap::LoopRecord served_loop;
  served_loop.id = ccap::LoopId::literal("loop-air-1");
  served_loop.facility = facility.id;
  served_loop.label = ccap::BoundedText::literal("Air loop 1");
  served_loop.kind = ccap::LoopKind::AirSupply;
  served_loop.transport_limit = ccap::ThermalPower::from_milliwatts(kLoopTransportLimit);
  served_loop.transport_limit_declared = true;
  served_loop.equipment = {crah_one.id, crah_two.id};
  served_loop.redundancy = ccap::RedundancyClass::NPlusOne;
  served_loop.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(served_loop));

  ccap::LoopRecord unknown_loop;
  unknown_loop.id = ccap::LoopId::literal("loop-air-2");
  unknown_loop.facility = facility.id;
  unknown_loop.label = ccap::BoundedText::literal("Air loop 2");
  unknown_loop.kind = ccap::LoopKind::AirSupply;
  unknown_loop.transport_limit = ccap::ThermalPower::from_milliwatts(kSmallCrahNominal);
  unknown_loop.transport_limit_declared = true;
  unknown_loop.equipment = {crah_three.id};
  unknown_loop.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(unknown_loop));

  ccap::ZoneRecord known_zone;
  known_zone.id = ccap::ZoneId::literal("hall-a");
  known_zone.facility = facility.id;
  known_zone.label = ccap::BoundedText::literal("Hall A");
  known_zone.media.insert(ccap::CoolingMedium::Air);
  known_zone.compatibility = {ccap::CompatibilityClass::AirConvection};
  known_zone.loops = {served_loop.id};
  known_zone.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(known_zone));

  ccap::ZoneRecord unknown_zone;
  unknown_zone.id = ccap::ZoneId::literal("hall-b");
  unknown_zone.facility = facility.id;
  unknown_zone.label = ccap::BoundedText::literal("Hall B");
  unknown_zone.media.insert(ccap::CoolingMedium::Air);
  unknown_zone.compatibility = {ccap::CompatibilityClass::AirConvection};
  unknown_zone.loops = {unknown_loop.id};
  unknown_zone.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(unknown_zone));
  return ccap::Result<void>();
}

// Evaluates one request and prints the whole answer.
ccap::Result<void> evaluate_and_print(const ccap::CoolingAnalyzer& analyzer, const char* title,
                                      const ccap::ZoneId& zone, Milliwatts requested,
                                      ccap::Timestamp instant) {
  ccap::CandidateLoad load;
  load.zone = zone;
  load.medium = ccap::CoolingMedium::Air;
  load.compatibility = ccap::CompatibilityClass::AirConvection;
  load.thermal = ccap::ThermalPower::from_milliwatts(requested);
  load.actor = ccap::ActorId::literal("operator-1");
  load.requested_at = instant;
  CCAP_TRY_DECLARE(evaluation, ccap::evaluate_candidate(analyzer, load));

  std::cout << title << "\n";
  std::cout << std::left << std::setw(26) << "  zone" << zone.str() << "\n";
  print_figure("  requested", ccap::CapacityValue::known(load.thermal));
  std::cout << std::left << std::setw(26) << "  disposition"
            << ccap::to_string(evaluation.disposition) << "\n";
  std::cout << std::left << std::setw(26) << "  reason" << ccap::to_string(evaluation.reason)
            << "\n";
  print_figure("  offered", evaluation.offered);
  std::cout << std::left << std::setw(26) << "  available"
            << evaluation.available.to_string() << "\n";
  std::cout << std::left << std::setw(26) << "  binding constraint"
            << (evaluation.binding.has_value() ? evaluation.binding->to_string()
                                               : std::string("none"))
            << "\n";
  std::string serving;
  for (const ccap::LoopId& loop : evaluation.serving_loops) {
    if (!serving.empty()) {
      serving.push_back(',');
    }
    serving.append(loop.str());
  }
  std::cout << std::left << std::setw(26) << "  serving loops" << serving << "\n";
  std::cout << std::left << std::setw(26) << "  generation"
            << evaluation.generation.to_string() << "\n\n";
  return ccap::Result<void>();
}

ccap::Result<void> run() {
  CCAP_TRY_DECLARE(start, ccap::parse_timestamp("2026-07-20T14:45:00.000Z"));
  ccap::ManualClock clock(start);

  ccap::SnapshotBuilder builder;
  builder.set_generation(ccap::CapacityGeneration::first());
  builder.set_constructed_at(clock.now());
  CCAP_TRY(add_model(builder));

  ccap::ValidationReport report;
  CCAP_TRY_DECLARE(snapshot, builder.build(&report));
  if (!report.ok()) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation, "the example model failed validation")
        .with("findings", report.render());
  }

  const ccap::CoolingAnalyzer analyzer(*snapshot, clock.now(), ccap::AnalysisBasis::Fresh);
  CCAP_TRY_DECLARE(known_zone, analyzer.zone_capacity(ccap::ZoneId::literal("hall-a"),
                                                      ccap::CoolingMedium::Air,
                                                      ccap::CompatibilityClass::AirConvection));
  CCAP_TRY_DECLARE(unknown_zone, analyzer.zone_capacity(ccap::ZoneId::literal("hall-b"),
                                                       ccap::CoolingMedium::Air,
                                                       ccap::CompatibilityClass::AirConvection));

  // Two 400 kW units in N+1 give 400000000 mW; the surveyed path carries
  // 350000000 mW, so the path binds.
  CCAP_TRY(expect_known(known_zone.offered, 350'000'000, "hall-a offered"));
  if (unknown_zone.offered.status() != ccap::CapacityStatus::Unknown ||
      unknown_zone.offered.reason() != ccap::CapacityReason::OperatingStateUnknown) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation,
                       "hall-b should be unknown because a unit's state is unknown")
        .with("offered", unknown_zone.offered.to_string());
  }

  std::cout << "Cooling Capacity example 05: candidate load evaluation\n\n";
  std::cout << std::left << std::setw(26) << "generation" << snapshot->generation().to_string()
            << "\n";
  std::cout << std::left << std::setw(26) << "evaluated at" << clock.now().to_string() << "\n";
  print_figure("hall-a offered", known_zone.offered);
  print_figure("hall-b offered", unknown_zone.offered);
  std::cout << "\n";

  // 1. A load that fits comfortably.
  CCAP_TRY(evaluate_and_print(analyzer, "case 1: a load that fits", ccap::ZoneId::literal("hall-a"),
                              200'000'000, clock.now()));
  // 2. Exactly the offer: admission is inclusive, there is no unstated margin.
  CCAP_TRY(evaluate_and_print(analyzer, "case 2: exactly the offered amount",
                              ccap::ZoneId::literal("hall-a"), 350'000'000, clock.now()));
  // 3. One kilowatt more than the zone can offer.
  CCAP_TRY(evaluate_and_print(analyzer, "case 3: one kilowatt too large",
                              ccap::ZoneId::literal("hall-a"), 351'000'000, clock.now()));
  // 4. A load against a zone whose capacity cannot be established.
  CCAP_TRY(evaluate_and_print(analyzer, "case 4: an unknown-capacity zone",
                              ccap::ZoneId::literal("hall-b"), 100'000'000, clock.now()));

  // 5. A request that is invalid before any accounting happens: a load must be
  //    strictly positive.
  ccap::CandidateLoad empty_load;
  empty_load.zone = ccap::ZoneId::literal("hall-a");
  empty_load.medium = ccap::CoolingMedium::Air;
  empty_load.compatibility = ccap::CompatibilityClass::AirConvection;
  empty_load.thermal = ccap::ThermalPower::from_milliwatts(0);
  empty_load.actor = ccap::ActorId::literal("operator-1");
  empty_load.requested_at = clock.now();
  const ccap::Result<ccap::CandidateEvaluation> invalid =
      ccap::evaluate_candidate(analyzer, empty_load);
  if (invalid.ok()) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation,
                       "a zero load must be refused as an invalid request");
  }
  std::cout << "case 5: an invalid request\n";
  std::cout << "  a load of zero is refused before any accounting happens:\n";
  std::cout << "    " << invalid.error().to_string() << "\n\n";

  std::cout << "what the three dispositions mean for the caller\n";
  std::cout << "  admitted      : capacity is held for the load; the binding constraint is\n"
            << "                  named so the operator knows what to watch.\n";
  std::cout << "  rejected      : the offer is known and too small. Asking again does not\n"
            << "                  help; the plant has to change.\n";
  std::cout << "  indeterminate : no answer can be established. Treating this as admission\n"
            << "                  would spend capacity that may not exist.\n";

  std::cout << "\nvalidation findings for this model\n";
  if (report.findings().empty()) {
    std::cout << "  (none)\n";
  } else {
    std::cout << report.render();
  }
  return ccap::Result<void>();
}

}  // namespace

int main() {
  const ccap::Result<void> outcome = run();
  if (!outcome.ok()) {
    return failure(outcome.error());
  }
  std::cout << "\nexample 05 finished: ok\n";
  return 0;
}
