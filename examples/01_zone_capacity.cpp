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

// 01 - zone capacity.
//
// One facility, one zone, one air loop, two CRAH units. The example prints the
// whole accounting chain for the zone - nominal, validated, usable, committed,
// reserve and offered - and checks every derived figure against the value the
// exact integer accounting must produce.
//
// Every authoritative quantity is an exact integer in milliwatts; floating
// point never takes part. The evaluation instant comes from a ManualClock, so
// the output is identical on every run.

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "cooling_capacity/cooling_capacity.hpp"

namespace ccap = cooling_capacity;

namespace {

using Milliwatts = std::int64_t;

// 400 kW nameplate per CRAH unit, in the canonical unit.
constexpr Milliwatts kCrahNominal = 400'000'000;
// The commissioning figure for CRAH 1. It is below the nameplate.
constexpr Milliwatts kCrahOneValidated = 380'000'000;
// What the transport path can actually carry.
constexpr Milliwatts kLoopTransportLimit = 700'000'000;
// A load already held against the zone.
constexpr Milliwatts kHeldLoad = 250'000'000;

// Prints the error that stopped the example.
int failure(const ccap::Error& error) {
  std::cout << "FAILED: " << error.to_string() << "\n";
  return 1;
}

// Prints one labelled figure with the value column aligned.
void print_figure(const std::string& label, const ccap::CapacityValue& value) {
  std::cout << std::left << std::setw(22) << label << value.to_string() << "\n";
}

void print_quantity(const std::string& label, const ccap::ThermalPower& value) {
  std::cout << std::left << std::setw(22) << label << value.to_string() << "\n";
}

// Checks a derived figure against the exact value this example expects, so that
// a surprise is reported as a failure rather than printed as if it were right.
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

// Builds the whole model into one candidate generation: one site, one facility,
// two CRAH units on one air loop, one zone served by that loop, and one load
// already held against the zone.
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

  // The commissioning record that backs the validated figure of CRAH 1. Without
  // it the nameplate would have to be used, and the accounting would say so.
  ccap::EvidenceRecord validation;
  validation.id = ccap::EvidenceId::literal("ev-crah-1-validation");
  validation.kind = ccap::EvidenceKind::CapacityValidation;
  validation.source = ccap::EvidenceSource::CommissioningTool;
  validation.subject = ccap::EquipmentId::literal("crah-1");
  validation.medium = ccap::CoolingMedium::Air;
  validation.value = ccap::ThermalPower::from_milliwatts(kCrahOneValidated);
  validation.observed_at = instant;
  validation.provenance.actor = ccap::ActorId::literal("commissioning-tool");
  validation.provenance.reference = ccap::DocumentRef::literal("CX-2026-001");
  validation.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(validation));

  ccap::EquipmentRecord crah_one;
  crah_one.id = ccap::EquipmentId::literal("crah-1");
  crah_one.facility = facility.id;
  crah_one.label = ccap::BoundedText::literal("CRAH 1");
  crah_one.kind = ccap::EquipmentKind::Crah;
  crah_one.state = ccap::OperatingState::InService;
  crah_one.nominal = ccap::ThermalPower::from_milliwatts(kCrahNominal);
  crah_one.validated = ccap::ThermalPower::from_milliwatts(kCrahOneValidated);
  crah_one.validated_evidence = validation.id;
  crah_one.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(crah_one));

  // CRAH 2 has no commissioning figure, so its nameplate is used and the
  // capacity basis for that unit stays nominal.
  ccap::EquipmentRecord crah_two = crah_one;
  crah_two.id = ccap::EquipmentId::literal("crah-2");
  crah_two.label = ccap::BoundedText::literal("CRAH 2");
  crah_two.validated = std::nullopt;
  crah_two.validated_evidence = std::nullopt;
  CCAP_TRY(builder.add(crah_two));

  ccap::LoopRecord loop;
  loop.id = ccap::LoopId::literal("loop-air-1");
  loop.facility = facility.id;
  loop.label = ccap::BoundedText::literal("Air loop 1");
  loop.kind = ccap::LoopKind::AirSupply;
  loop.transport_limit = ccap::ThermalPower::from_milliwatts(kLoopTransportLimit);
  loop.transport_limit_declared = true;
  loop.equipment = {crah_one.id, crah_two.id};
  loop.redundancy = ccap::RedundancyClass::None;
  loop.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(loop));

  ccap::ZoneRecord zone;
  zone.id = ccap::ZoneId::literal("hall-1");
  zone.facility = facility.id;
  zone.label = ccap::BoundedText::literal("Hall 1");
  zone.media.insert(ccap::CoolingMedium::Air);
  zone.compatibility = {ccap::CompatibilityClass::AirConvection};
  zone.loops = {loop.id};
  zone.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(zone));

  // A load already held against the zone. A held commitment consumes capacity,
  // which is what makes committed and reserve differ from usable.
  ccap::CommitmentRecord held;
  held.id = ccap::CommitmentId::literal("load-42");
  held.attempt = ccap::AttemptId::literal("attempt-load-42");
  held.zone = zone.id;
  held.pinned_loop = loop.id;
  held.medium = ccap::CoolingMedium::Air;
  held.compatibility = ccap::CompatibilityClass::AirConvection;
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
  // A ManualClock keeps the evaluation instant, and therefore every freshness
  // decision and every printed timestamp, deterministic.
  CCAP_TRY_DECLARE(start, ccap::parse_timestamp("2026-03-02T09:00:00.000Z"));
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
  CCAP_TRY_DECLARE(zone, analyzer.zone_capacity(ccap::ZoneId::literal("hall-1"),
                                                ccap::CoolingMedium::Air,
                                                ccap::CompatibilityClass::AirConvection));

  // Every figure below is the exact result of the integer accounting:
  //   nominal   = 400000000 + 400000000
  //   validated = 380000000 + 400000000
  //   usable    = min(validated, transport limit 700000000)
  //   committed = the held load
  //   reserve   = usable - committed
  //   offered   = local headroom - the zone's effective reserve
  CCAP_TRY(expect_known(zone.breakdown.nominal, 800'000'000, "zone nominal"));
  CCAP_TRY(expect_known(zone.breakdown.validated, 780'000'000, "zone validated"));
  CCAP_TRY(expect_known(zone.breakdown.usable, 700'000'000, "zone usable"));
  CCAP_TRY(expect_known(zone.breakdown.committed, kHeldLoad, "zone committed"));
  CCAP_TRY(expect_known(zone.breakdown.reserve, 450'000'000, "zone reserve"));
  CCAP_TRY(expect_known(zone.offered, 450'000'000, "zone offered"));
  if (zone.breakdown.basis != ccap::CapacityBasis::Validated) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation,
                       "the zone basis should be validated because CRAH 1 was validated")
        .with("basis", std::string(ccap::to_string(zone.breakdown.basis)));
  }

  std::cout << "Cooling Capacity example 01: zone capacity\n";
  std::cout << "one facility, one zone, one air loop, two CRAH units\n\n";
  std::cout << std::left << std::setw(22) << "generation" << snapshot->generation().to_string()
            << "\n";
  std::cout << std::left << std::setw(22) << "policy"
            << snapshot->policy().id.str() + " revision " + snapshot->policy().revision.to_string()
            << "\n";
  std::cout << std::left << std::setw(22) << "evaluated at" << clock.now().to_string() << "\n";
  std::cout << std::left << std::setw(22) << "analysis basis" << ccap::to_string(analyzer.basis())
            << "\n";
  std::cout << std::left << std::setw(22) << "validation" << std::to_string(report.error_count()) +
                                                                   " error(s), " +
                                                                   std::to_string(
                                                                       report.warning_count()) +
                                                                   " warning(s)"
            << "\n";
  std::cout << std::left << std::setw(22) << "zone"
            << zone.zone.str() + " (" + std::string(ccap::to_string(zone.medium)) + ", " +
                   std::string(ccap::to_string(zone.compatibility)) + ")"
            << "\n\n";

  std::cout << "zone capacity chain\n";
  print_figure("nominal", zone.breakdown.nominal);
  print_figure("validated", zone.breakdown.validated);
  print_figure("derated", zone.breakdown.derated);
  print_figure("degraded", zone.breakdown.degraded);
  print_figure("usable", zone.breakdown.usable);
  print_figure("committed", zone.breakdown.committed);
  print_figure("reserve", zone.breakdown.reserve);
  print_figure("deficit", zone.breakdown.deficit);
  print_figure("observed", zone.breakdown.observed);
  print_figure("local headroom", zone.local_headroom);
  print_figure("offered", zone.offered);
  print_quantity("held load (record)", ccap::ThermalPower::from_milliwatts(kHeldLoad));
  std::cout << std::left << std::setw(22) << "capacity basis"
            << ccap::to_string(zone.breakdown.basis) << "\n";
  std::cout << std::left << std::setw(22) << "over committed"
            << (zone.breakdown.over_committed ? "true" : "false") << "\n";

  std::cout << "\nloops serving the zone\n";
  for (const ccap::LoopCapacity& loop : zone.loops) {
    std::cout << "loop " << loop.loop.str() << " (" << ccap::to_string(loop.kind) << ")\n";
    std::cout << "  usable " << loop.breakdown.usable.to_string() << ", committed "
              << loop.breakdown.committed.to_string() << ", reserve "
              << loop.breakdown.reserve.to_string() << ", bottleneck "
              << (loop.bottleneck.has_value() ? loop.bottleneck->to_string() : std::string("none"))
              << "\n";
    for (const ccap::EquipmentContribution& unit : loop.equipment) {
      std::cout << "  unit " << unit.equipment.str() << " (" << ccap::to_string(unit.kind)
                << ", " << ccap::to_string(unit.state) << ")\n";
      std::cout << "    basis " << ccap::to_string(unit.basis) << ", nominal "
                << unit.nominal.to_string() << ", validated " << unit.validated.to_string()
                << ", derated " << unit.derated.to_string() << ", usable "
                << unit.usable.to_string() << "\n";
    }
  }

  std::cout << "\nzone bottleneck: "
            << (zone.bottleneck.has_value() ? zone.bottleneck->to_string() : std::string("none"))
            << "\n";

  // The same loop figures are reported by the loop query, because a loop is a
  // first-class accounting point and not only an ingredient of a zone.
  CCAP_TRY_DECLARE(loop_view, analyzer.loop_capacity(ccap::LoopId::literal("loop-air-1")));
  CCAP_TRY(expect_known(loop_view.breakdown.usable, 700'000'000, "loop usable"));
  CCAP_TRY(expect_known(loop_view.breakdown.committed, kHeldLoad, "loop committed"));
  CCAP_TRY(expect_known(loop_view.breakdown.reserve, 450'000'000, "loop reserve"));
  CCAP_TRY(expect_quantity(loop_view.breakdown.committed.amount_or_zero(), kHeldLoad,
                           "loop committed"));

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
  std::cout << "\nexample 01 finished: ok\n";
  return 0;
}
