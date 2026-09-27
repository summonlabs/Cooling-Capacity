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

// 03 - redundancy and derating.
//
// Three chillers in an N+1 group on one chilled-water loop, one of them behind a
// vendor-curve derate. The example prints the exact integer arithmetic:
//
//   derated = floor(validated * parts_per_million / 1000000)
//   N+1     = total of the counted units - the largest of them
//
// Every multiplication is integer and rounds down at every step, so a derived
// figure is never larger than the exact value. The example recomputes both
// figures from the records and checks them against what the library reported.

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "cooling_capacity/cooling_capacity.hpp"

namespace ccap = cooling_capacity;

namespace {

using Milliwatts = std::int64_t;

// Three 390 kW chillers.
constexpr Milliwatts kChillerNominal = 390'000'000;
// The commissioning figure for chiller 1. It is deliberately not a round number
// of kilowatts, so that the derate below has to round down.
constexpr Milliwatts kChillerOneValidated = 386'317'000;
// The vendor curve for chiller 1, as an exact fraction of one.
constexpr const char* kDeratePercent = "93.73";
// What the loop's transport path can carry.
constexpr Milliwatts kLoopTransportLimit = 800'000'000;

int failure(const ccap::Error& error) {
  std::cout << "FAILED: " << error.to_string() << "\n";
  return 1;
}

void print_figure(const std::string& label, const ccap::CapacityValue& value) {
  std::cout << std::left << std::setw(30) << label << value.to_string() << "\n";
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

// The contribution of one unit within a loop view, or null when the unit is not
// on the loop at all.
const ccap::EquipmentContribution* find_unit(const ccap::LoopCapacity& loop,
                                             const ccap::EquipmentId& id) {
  for (const ccap::EquipmentContribution& unit : loop.equipment) {
    if (unit.equipment == id) {
      return &unit;
    }
  }
  return nullptr;
}

// Builds one loop with a group of three chillers, one of which is validated and
// derated, and one zone served by that loop.
ccap::Result<void> add_model(ccap::SnapshotBuilder& builder, ccap::Timestamp instant) {
  CCAP_TRY_DECLARE(vendor_derate, ccap::Fraction::parse_percent(kDeratePercent));

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

  ccap::EvidenceRecord validation;
  validation.id = ccap::EvidenceId::literal("ev-chiller-1-validation");
  validation.kind = ccap::EvidenceKind::CapacityValidation;
  validation.source = ccap::EvidenceSource::CommissioningTool;
  validation.subject = ccap::EquipmentId::literal("chiller-1");
  validation.medium = ccap::CoolingMedium::Liquid;
  validation.value = ccap::ThermalPower::from_milliwatts(kChillerOneValidated);
  validation.observed_at = instant;
  validation.provenance.actor = ccap::ActorId::literal("commissioning-tool");
  validation.provenance.reference = ccap::DocumentRef::literal("CX-2026-014");
  validation.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(validation));

  ccap::EquipmentRecord chiller_one;
  chiller_one.id = ccap::EquipmentId::literal("chiller-1");
  chiller_one.facility = facility.id;
  chiller_one.label = ccap::BoundedText::literal("Chiller 1");
  chiller_one.kind = ccap::EquipmentKind::Chiller;
  chiller_one.state = ccap::OperatingState::InService;
  chiller_one.nominal = ccap::ThermalPower::from_milliwatts(kChillerNominal);
  chiller_one.validated = ccap::ThermalPower::from_milliwatts(kChillerOneValidated);
  chiller_one.validated_evidence = validation.id;
  chiller_one.revision = ccap::RecordRevision::first();

  ccap::DerateFactor curve;
  curve.name = ccap::Identifier::literal("vendor-curve");
  curve.factor = vendor_derate;
  curve.reason = ccap::DerateReason::VendorCurve;
  chiller_one.derates = {curve};
  CCAP_TRY(builder.add(chiller_one));

  ccap::EquipmentRecord chiller_two = chiller_one;
  chiller_two.id = ccap::EquipmentId::literal("chiller-2");
  chiller_two.label = ccap::BoundedText::literal("Chiller 2");
  chiller_two.validated = std::nullopt;
  chiller_two.validated_evidence = std::nullopt;
  chiller_two.derates.clear();
  CCAP_TRY(builder.add(chiller_two));

  ccap::EquipmentRecord chiller_three = chiller_two;
  chiller_three.id = ccap::EquipmentId::literal("chiller-3");
  chiller_three.label = ccap::BoundedText::literal("Chiller 3");
  CCAP_TRY(builder.add(chiller_three));

  ccap::LoopRecord loop;
  loop.id = ccap::LoopId::literal("loop-chw-1");
  loop.facility = facility.id;
  loop.label = ccap::BoundedText::literal("Chilled-water loop 1");
  loop.kind = ccap::LoopKind::ChilledWater;
  loop.transport_limit = ccap::ThermalPower::from_milliwatts(kLoopTransportLimit);
  loop.transport_limit_declared = true;
  loop.equipment = {chiller_one.id, chiller_two.id, chiller_three.id};
  loop.redundancy = ccap::RedundancyClass::NPlusOne;
  loop.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(loop));

  ccap::ZoneRecord zone;
  zone.id = ccap::ZoneId::literal("hall-chw");
  zone.facility = facility.id;
  zone.label = ccap::BoundedText::literal("Chilled-water hall");
  zone.media.insert(ccap::CoolingMedium::Liquid);
  zone.compatibility = {ccap::CompatibilityClass::LiquidColdPlate};
  zone.loops = {loop.id};
  zone.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(zone));
  return ccap::Result<void>();
}

ccap::Result<void> run() {
  CCAP_TRY_DECLARE(start, ccap::parse_timestamp("2026-05-11T08:30:00.000Z"));
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
  CCAP_TRY_DECLARE(loop, analyzer.loop_capacity(ccap::LoopId::literal("loop-chw-1")));
  CCAP_TRY_DECLARE(zone, analyzer.zone_capacity(ccap::ZoneId::literal("hall-chw"),
                                                ccap::CoolingMedium::Liquid,
                                                ccap::CompatibilityClass::LiquidColdPlate));

  // --- the derate, recomputed exactly as the library computes it -------------
  CCAP_TRY_DECLARE(curve, ccap::Fraction::parse_percent(kDeratePercent));
  const Milliwatts parts = curve.parts_per_million();
  const Milliwatts base = kChillerOneValidated;
  const Milliwatts product = base * parts;
  const Milliwatts expected_derated = product / ccap::Fraction::kScale;
  const Milliwatts discarded = product % ccap::Fraction::kScale;
  const ccap::EquipmentContribution* derated_unit =
      find_unit(loop, ccap::EquipmentId::literal("chiller-1"));
  if (derated_unit == nullptr) {
    return ccap::Error(ccap::ErrorCode::NotFound, "chiller-1 is not on the loop")
        .with("loop", loop.loop.str());
  }
  CCAP_TRY(expect_known(derated_unit->derated, expected_derated, "chiller-1 derated"));
  if (discarded == 0) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation,
                       "this example is meant to show a derate that has to round down");
  }

  // --- the N+1 group formula, recomputed from the member capacities ----------
  std::vector<Milliwatts> member_usable;
  for (const ccap::EquipmentContribution& unit : loop.equipment) {
    member_usable.push_back(unit.usable.amount_or_zero().milliwatts());
  }
  std::sort(member_usable.begin(), member_usable.end(),
            [](Milliwatts lhs, Milliwatts rhs) { return lhs > rhs; });
  Milliwatts total = 0;
  for (const Milliwatts amount : member_usable) {
    total += amount;
  }
  const Milliwatts largest = member_usable.empty() ? 0 : member_usable.front();
  const Milliwatts expected_group = total - largest;
  CCAP_TRY(expect_known(loop.breakdown.usable, expected_group, "loop usable"));

  if (loop.redundancy.declared != ccap::RedundancyClass::NPlusOne ||
      loop.redundancy.effective != ccap::RedundancyClass::NPlusOne ||
      loop.redundancy.counted != 3U || loop.redundancy.excluded != 0U) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation,
                       "the redundancy posture is not the expected one")
        .with("declared", std::string(ccap::to_string(loop.redundancy.declared)))
        .with("effective", std::string(ccap::to_string(loop.redundancy.effective)))
        .with("counted", std::to_string(loop.redundancy.counted))
        .with("excluded", std::to_string(loop.redundancy.excluded));
  }
  CCAP_TRY(expect_known(zone.offered, expected_group, "zone offered"));

  std::cout << "Cooling Capacity example 03: redundancy and derating\n";
  std::cout << "three chillers in an N+1 group on one chilled-water loop\n\n";
  std::cout << std::left << std::setw(30) << "generation" << snapshot->generation().to_string()
            << "\n";
  std::cout << std::left << std::setw(30) << "evaluated at" << clock.now().to_string() << "\n";
  std::cout << std::left << std::setw(30) << "loop" << loop.loop.str() << "\n";
  std::cout << std::left << std::setw(30) << "declared redundancy"
            << ccap::to_string(loop.redundancy.declared) << "\n";
  std::cout << std::left << std::setw(30) << "effective redundancy"
            << ccap::to_string(loop.redundancy.effective) << "\n";
  std::cout << std::left << std::setw(30) << "members / counted / excluded"
            << std::to_string(loop.redundancy.members) + " / " +
                   std::to_string(loop.redundancy.counted) + " / " +
                   std::to_string(loop.redundancy.excluded)
            << "\n\n";

  std::cout << "equipment chain\n";
  for (const ccap::EquipmentContribution& unit : loop.equipment) {
    std::cout << unit.equipment.str() << " (" << ccap::to_string(unit.state) << ", basis "
              << ccap::to_string(unit.basis) << ")\n";
    std::cout << "  nominal   " << unit.nominal.to_string() << "\n";
    std::cout << "  validated " << unit.validated.to_string() << "\n";
    std::cout << "  derated   " << unit.derated.to_string() << "\n";
    std::cout << "  degraded  " << unit.degraded.to_string() << "\n";
    std::cout << "  usable    " << unit.usable.to_string() << "\n";
    std::cout << "  cumulative derate " << unit.cumulative_derate.to_string() << "\n";
    if (&unit == derated_unit) {
      std::cout << "  exact integer arithmetic:\n";
      std::cout << "    floor(" << std::to_string(base) << " mW * " << std::to_string(parts)
                << " / " << std::to_string(ccap::Fraction::kScale)
                << ") = " << ccap::ThermalPower::from_milliwatts(expected_derated).to_string()
                << "\n";
      std::cout << "    the remainder " << std::to_string(discarded) << " / "
                << std::to_string(ccap::Fraction::kScale)
                << " of a milliwatt is discarded, never rounded up\n";
      std::cout << ccap::render_steps(unit.explanation);
    }
  }

  std::cout << "\nN+1 group formula\n";
  std::cout << "  counted units          " << std::to_string(member_usable.size()) << "\n";
  std::cout << "  total of counted units " << ccap::ThermalPower::from_milliwatts(total).to_string()
            << "\n";
  std::cout << "  largest unit released  "
            << ccap::ThermalPower::from_milliwatts(largest).to_string() << "\n";
  std::cout << "  usable = total - largest = "
            << ccap::ThermalPower::from_milliwatts(expected_group).to_string() << "\n";
  std::cout << "  transport limit        "
            << ccap::ThermalPower::from_milliwatts(kLoopTransportLimit).to_string()
            << " (not binding)\n\n";

  std::cout << "loop and zone\n";
  print_figure("loop nominal", loop.breakdown.nominal);
  print_figure("loop validated", loop.breakdown.validated);
  print_figure("loop derated", loop.breakdown.derated);
  print_figure("loop usable (N+1, transport)", loop.breakdown.usable);
  print_figure("zone usable", zone.breakdown.usable);
  print_figure("zone offered", zone.offered);

  std::cout << "\nexplanation of the loop answer\n";
  std::cout << ccap::render_steps(loop.explanation);
  return ccap::Result<void>();
}

}  // namespace

int main() {
  const ccap::Result<void> outcome = run();
  if (!outcome.ok()) {
    return failure(outcome.error());
  }
  std::cout << "\nexample 03 finished: ok\n";
  return 0;
}
