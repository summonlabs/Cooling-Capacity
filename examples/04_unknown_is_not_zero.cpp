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

// 04 - unknown is not zero.
//
// Four answers a capacity question can have, and they are not interchangeable:
//
//   Known        the amount is established: this includes a definite zero;
//   Unknown      nobody has said what the amount is, so nothing may be concluded;
//   Unsupported  this medium or compatibility class does not exist here at all;
//   Unavailable  the amount is definitely zero right now, for a named reason.
//
// The example builds a loop with no declared transport limit, which makes the
// zone's capacity Unknown and a candidate load Indeterminate, and contrasts that
// with Unsupported and Unavailable next to it.

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>

#include "cooling_capacity/cooling_capacity.hpp"

namespace ccap = cooling_capacity;

namespace {

using Milliwatts = std::int64_t;

constexpr Milliwatts kCrahNominal = 300'000'000;
constexpr Milliwatts kChillerNominal = 500'000'000;
// A load the caller would like to place.
constexpr Milliwatts kCandidateLoad = 100'000'000;

int failure(const ccap::Error& error) {
  std::cout << "FAILED: " << error.to_string() << "\n";
  return 1;
}

void print_answer(const std::string& label, const ccap::CapacityValue& value) {
  std::cout << std::left << std::setw(24) << label << std::setw(14)
            << ccap::to_string(value.status()) << value.to_string() << "\n";
}

ccap::Result<void> expect_status(const ccap::CapacityValue& value, ccap::CapacityStatus status,
                                 ccap::CapacityReason reason, const char* what) {
  if (value.status() != status || value.reason() != reason) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation,
                       "a capacity answer does not have the expected status and reason")
        .with("answer", what)
        .with("actual", value.to_string())
        .with("expected_status", std::string(ccap::to_string(status)))
        .with("expected_reason", std::string(ccap::to_string(reason)));
  }
  return ccap::Result<void>();
}

// One loop without a transport declaration, one loop whose declared transport
// limit is exactly zero, and a plant whose only chiller is in maintenance.
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

  ccap::EquipmentRecord crah_a;
  crah_a.id = ccap::EquipmentId::literal("crah-a");
  crah_a.facility = facility.id;
  crah_a.label = ccap::BoundedText::literal("CRAH A");
  crah_a.kind = ccap::EquipmentKind::Crah;
  crah_a.state = ccap::OperatingState::InService;
  crah_a.nominal = ccap::ThermalPower::from_milliwatts(kCrahNominal);
  crah_a.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(crah_a));

  ccap::EquipmentRecord crah_c = crah_a;
  crah_c.id = ccap::EquipmentId::literal("crah-c");
  crah_c.label = ccap::BoundedText::literal("CRAH C");
  CCAP_TRY(builder.add(crah_c));

  // Nobody has surveyed this loop's transport path. The equipment is in service
  // and its nameplate is known, but the loop can carry an unknown amount, so the
  // loop's capacity is Unknown and not the sum of its equipment.
  ccap::LoopRecord open_loop;
  open_loop.id = ccap::LoopId::literal("loop-air-open");
  open_loop.facility = facility.id;
  open_loop.label = ccap::BoundedText::literal("Air loop without a survey");
  open_loop.kind = ccap::LoopKind::AirSupply;
  open_loop.transport_limit_declared = false;
  open_loop.equipment = {crah_a.id};
  open_loop.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(open_loop));

  // This path has been surveyed and it can carry nothing at all. That is a
  // definite zero, which is a Known answer.
  ccap::LoopRecord zero_loop;
  zero_loop.id = ccap::LoopId::literal("loop-air-zero");
  zero_loop.facility = facility.id;
  zero_loop.label = ccap::BoundedText::literal("Air loop with a closed path");
  zero_loop.kind = ccap::LoopKind::AirSupply;
  zero_loop.transport_limit = ccap::ThermalPower::from_milliwatts(0);
  zero_loop.transport_limit_declared = true;
  zero_loop.equipment = {crah_c.id};
  zero_loop.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(zero_loop));

  ccap::ZoneRecord open_zone;
  open_zone.id = ccap::ZoneId::literal("hall-open");
  open_zone.facility = facility.id;
  open_zone.label = ccap::BoundedText::literal("Hall with an unsurveyed loop");
  open_zone.media.insert(ccap::CoolingMedium::Air);
  open_zone.compatibility = {ccap::CompatibilityClass::AirConvection};
  open_zone.loops = {open_loop.id};
  open_zone.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(open_zone));

  ccap::ZoneRecord zero_zone;
  zero_zone.id = ccap::ZoneId::literal("hall-zero");
  zero_zone.facility = facility.id;
  zero_zone.label = ccap::BoundedText::literal("Hall behind a closed path");
  zero_zone.media.insert(ccap::CoolingMedium::Air);
  zero_zone.compatibility = {ccap::CompatibilityClass::AirConvection};
  zero_zone.loops = {zero_loop.id};
  zero_zone.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(zero_zone));

  // A liquid plant whose only chiller is in maintenance: definitely zero right
  // now, for a reason the operator can act on.
  ccap::EquipmentRecord chiller;
  chiller.id = ccap::EquipmentId::literal("chiller-maint");
  chiller.facility = facility.id;
  chiller.label = ccap::BoundedText::literal("Chiller under maintenance");
  chiller.kind = ccap::EquipmentKind::Chiller;
  chiller.state = ccap::OperatingState::Maintenance;
  chiller.nominal = ccap::ThermalPower::from_milliwatts(kChillerNominal);
  chiller.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(chiller));

  ccap::PlantRecord plant;
  plant.id = ccap::PlantId::literal("plant-standby");
  plant.facility = facility.id;
  plant.label = ccap::BoundedText::literal("Standby chiller plant");
  plant.medium = ccap::CoolingMedium::Liquid;
  plant.equipment = {chiller.id};
  plant.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(plant));
  return ccap::Result<void>();
}

ccap::Result<ccap::CandidateEvaluation> evaluate(const ccap::CoolingAnalyzer& analyzer,
                                                 const ccap::ZoneId& zone,
                                                 ccap::CompatibilityClass compatibility,
                                                 ccap::Timestamp instant) {
  ccap::CandidateLoad load;
  load.zone = zone;
  load.medium = ccap::medium_of(compatibility);
  load.compatibility = compatibility;
  load.thermal = ccap::ThermalPower::from_milliwatts(kCandidateLoad);
  load.actor = ccap::ActorId::literal("operator-1");
  load.requested_at = instant;
  return ccap::evaluate_candidate(analyzer, load);
}

ccap::Result<void> run() {
  CCAP_TRY_DECLARE(start, ccap::parse_timestamp("2026-06-15T10:00:00.000Z"));
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

  CCAP_TRY_DECLARE(open_zone, analyzer.zone_capacity(ccap::ZoneId::literal("hall-open"),
                                                     ccap::CoolingMedium::Air,
                                                     ccap::CompatibilityClass::AirConvection));
  CCAP_TRY_DECLARE(zero_zone, analyzer.zone_capacity(ccap::ZoneId::literal("hall-zero"),
                                                     ccap::CoolingMedium::Air,
                                                     ccap::CompatibilityClass::AirConvection));
  CCAP_TRY_DECLARE(no_liquid, analyzer.zone_capacity(ccap::ZoneId::literal("hall-open"),
                                                     ccap::CoolingMedium::Liquid,
                                                     ccap::CompatibilityClass::LiquidColdPlate));
  CCAP_TRY_DECLARE(no_class, analyzer.zone_capacity(ccap::ZoneId::literal("hall-open"),
                                                    ccap::CoolingMedium::Air,
                                                    ccap::CompatibilityClass::AirRearDoor));
  CCAP_TRY_DECLARE(plant, analyzer.plant_capacity(ccap::PlantId::literal("plant-standby")));
  CCAP_TRY_DECLARE(loop, analyzer.loop_capacity(ccap::LoopId::literal("loop-air-open")));

  // Unknown: the loop has no declared transport limit and the policy fails
  // closed, so the loop and the zone are Unknown rather than the sum of the
  // equipment.
  CCAP_TRY(expect_status(loop.breakdown.usable, ccap::CapacityStatus::Unknown,
                         ccap::CapacityReason::TransportNotDeclared, "loop usable"));
  CCAP_TRY(expect_status(open_zone.offered, ccap::CapacityStatus::Unknown,
                         ccap::CapacityReason::TransportNotDeclared, "zone offered"));

  // Known(zero): the declared transport limit is zero, which is an answer.
  CCAP_TRY(expect_status(zero_zone.offered, ccap::CapacityStatus::Known, ccap::CapacityReason::None,
                         "zone offered"));

  // Unsupported: the zone declares air and air convection only.
  CCAP_TRY(expect_status(no_liquid.offered, ccap::CapacityStatus::Unsupported,
                         ccap::CapacityReason::MediumNotSupported, "zone offered"));
  CCAP_TRY(expect_status(no_class.offered, ccap::CapacityStatus::Unsupported,
                         ccap::CapacityReason::ClassNotSupported, "zone offered"));

  // Unavailable: the plant's only chiller is in maintenance, so the plant is
  // definitely zero. The reason names maintenance rather than a generic "out of
  // service", because the specific cause is what an operator has to act on.
  CCAP_TRY(expect_status(plant.breakdown.usable, ccap::CapacityStatus::Unavailable,
                         ccap::CapacityReason::Maintenance, "plant usable"));

  CCAP_TRY_DECLARE(indeterminate, evaluate(analyzer, ccap::ZoneId::literal("hall-open"),
                                           ccap::CompatibilityClass::AirConvection, clock.now()));
  if (indeterminate.disposition != ccap::CandidateDisposition::Indeterminate ||
      indeterminate.reason != ccap::CandidateReason::LoopTransportUndeclared) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation,
                       "an unsurveyed loop must make the candidate indeterminate")
        .with("evaluation", indeterminate.to_string());
  }
  CCAP_TRY_DECLARE(rejected, evaluate(analyzer, ccap::ZoneId::literal("hall-zero"),
                                      ccap::CompatibilityClass::AirConvection, clock.now()));
  if (rejected.disposition != ccap::CandidateDisposition::Rejected ||
      rejected.reason != ccap::CandidateReason::InsufficientLocalCapacity) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation,
                       "a known zero must reject the candidate rather than leave it open")
        .with("evaluation", rejected.to_string());
  }

  std::cout << "Cooling Capacity example 04: unknown is not zero\n\n";
  std::cout << std::left << std::setw(30) << "generation" << snapshot->generation().to_string()
            << "\n";
  std::cout << std::left << std::setw(30) << "evaluated at" << clock.now().to_string() << "\n";
  std::cout << std::left << std::setw(30) << "candidate load"
            << ccap::ThermalPower::from_milliwatts(kCandidateLoad).to_string() << "\n\n";

  std::cout << "the four answers\n";
  std::cout << std::left << std::setw(24) << "answer" << std::setw(14) << "status" << "value\n";
  print_answer("hall-open offered", open_zone.offered);
  print_answer("hall-zero offered", zero_zone.offered);
  print_answer("hall-open as liquid", no_liquid.offered);
  print_answer("hall-open rear-door", no_class.offered);
  print_answer("plant-standby usable", plant.breakdown.usable);

  std::cout << "\nwhy each answer is what it is\n";
  std::cout << "  hall-open  : loop loop-air-open has no declared transport limit, so the\n"
            << "               capacity the path can carry is Unknown. The equipment being\n"
            << "               in service does not establish a bound for the path.\n";
  std::cout << "  hall-zero  : the transport survey says the path carries 0 mW. That is a\n"
            << "               Known amount, and a load cannot be placed against it.\n";
  std::cout << "  liquid     : the zone declares the air medium only.\n";
  std::cout << "  rear-door  : the zone declares air convection only.\n";
  std::cout << "  plant      : the only chiller is in maintenance, so the pool is definitely\n"
            << "               zero right now.\n";

  // The blunt accessor reads zero for every non-Known status; the exact accessor
  // refuses. This is the difference between Unknown and Known(zero).
  const ccap::CapacityValue& offered = open_zone.offered;
  std::cout << "\nUnknown is not zero\n";
  std::cout << "  amount_or_zero() reads " << offered.amount_or_zero().to_string()
            << " while is_known() is " << (offered.is_known() ? "true" : "false") << "\n";
  const ccap::Result<ccap::ThermalPower> exact = offered.exact_amount();
  if (exact.ok()) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation,
                       "an unknown capacity must not produce an exact amount")
        .with("amount", exact.value().to_string());
  }
  std::cout << "  exact_amount() refuses: " << exact.error().to_string() << "\n";
  const ccap::CapacityValue& zero = zero_zone.offered;
  CCAP_TRY_DECLARE(zero_amount, zero.exact_amount());
  std::cout << "  the known zero does produce an exact amount: " << zero_amount.to_string()
            << "\n";

  std::cout << "\ncandidate evaluation\n";
  std::cout << "  against hall-open : " << indeterminate.to_string() << "\n";
  std::cout << "  against hall-zero : " << rejected.to_string() << "\n";
  std::cout << "\nan indeterminate answer is not a soft rejection: the caller has to obtain a\n"
            << "transport declaration or fresh evidence before the load can be placed.\n";

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
  std::cout << "\nexample 04 finished: ok\n";
  return 0;
}
