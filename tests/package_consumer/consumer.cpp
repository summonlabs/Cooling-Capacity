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

// An out-of-tree consumer of the Cooling Capacity package.
//
// It uses nothing but the public API and the imported CMake target:
//
//   * build a snapshot in memory and validate it;
//   * run a zone capacity query and check the exact figures;
//   * evaluate a candidate load and check the disposition;
//   * print the version string of the library it linked against.
//
// It exits 0 only when every expected value matches, so a packaging mistake -
// a missing header, a stale library, a broken ABI - fails the build or the test
// instead of passing silently.

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include "cooling_capacity/cooling_capacity.hpp"

namespace ccap = cooling_capacity;

namespace {

using Milliwatts = std::int64_t;

constexpr Milliwatts kUnitNominal = 400'000'000;   // 400 kW per CRAH unit
constexpr Milliwatts kTransportLimit = 600'000'000;  // 600 kW of transport
constexpr Milliwatts kCandidateLoad = 250'000'000;   // 250 kW requested
constexpr Milliwatts kExpectedUsable = 600'000'000;
constexpr Milliwatts kExpectedOffered = 600'000'000;

int fail(const std::string& message) {
  std::cout << "consumer: FAILED: " << message << "\n";
  return 1;
}

ccap::Result<void> expect_known(const ccap::CapacityValue& value, Milliwatts expected,
                                const char* what) {
  if (!value.is_known()) {
    return ccap::Error(ccap::ErrorCode::Indeterminate, "the figure is not known")
        .with("figure", what)
        .with("value", value.to_string());
  }
  if (value.amount_or_zero().milliwatts() != expected) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation, "the figure is not the expected one")
        .with("figure", what)
        .with("actual", value.to_string())
        .with("expected", ccap::ThermalPower::from_milliwatts(expected).to_string());
  }
  return ccap::Result<void>();
}

// One facility with one air loop of two CRAH units and one zone served by it.
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
  crah_one.nominal = ccap::ThermalPower::from_milliwatts(kUnitNominal);
  crah_one.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(crah_one));

  ccap::EquipmentRecord crah_two = crah_one;
  crah_two.id = ccap::EquipmentId::literal("crah-2");
  crah_two.label = ccap::BoundedText::literal("CRAH 2");
  CCAP_TRY(builder.add(crah_two));

  ccap::LoopRecord loop;
  loop.id = ccap::LoopId::literal("loop-air-1");
  loop.facility = facility.id;
  loop.label = ccap::BoundedText::literal("Air loop 1");
  loop.kind = ccap::LoopKind::AirSupply;
  loop.transport_limit = ccap::ThermalPower::from_milliwatts(kTransportLimit);
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
  return ccap::Result<void>();
}

}  // namespace

int main() {
  std::cout << "consumer: cooling_capacity " << ccap::version_string()
            << " artifact format " << ccap::artifact_format_version() << "\n";

  // --- build a snapshot in memory -------------------------------------------
  const ccap::Result<ccap::Timestamp> parsed = ccap::parse_timestamp("2026-12-01T08:00:00.000Z");
  if (!parsed.ok()) {
    return fail(parsed.error().to_string());
  }
  const ccap::Timestamp instant = parsed.value();
  ccap::SnapshotBuilder builder;
  builder.set_generation(ccap::CapacityGeneration::first());
  builder.set_constructed_at(instant);
  {
    const ccap::Result<void> added = add_model(builder);
    if (!added.ok()) {
      return fail(added.error().to_string());
    }
  }

  ccap::ValidationReport report;
  const ccap::Result<std::shared_ptr<const ccap::CoolingSnapshot>> built = builder.build(&report);
  if (!built.ok()) {
    return fail(built.error().to_string());
  }
  const std::shared_ptr<const ccap::CoolingSnapshot> snapshot = built.value();
  std::cout << "consumer: generation " << snapshot->generation().to_string() << " with "
            << snapshot->record_count() << " records\n";

  // --- validate it ----------------------------------------------------------
  const ccap::ValidationReport validation = ccap::validate_snapshot(*snapshot);
  if (!validation.ok()) {
    return fail("the snapshot did not validate: " + validation.render());
  }
  std::cout << "consumer: validation ok (" << validation.error_count() << " errors, "
            << validation.warning_count() << " warnings)\n";

  // --- run a zone capacity query --------------------------------------------
  const ccap::CoolingAnalyzer analyzer(*snapshot, instant, ccap::AnalysisBasis::Fresh);
  const ccap::Result<ccap::ZoneCapacity> zone = analyzer.zone_capacity(
      ccap::ZoneId::literal("hall-1"), ccap::CoolingMedium::Air,
      ccap::CompatibilityClass::AirConvection);
  if (!zone.ok()) {
    return fail(zone.error().to_string());
  }
  const ccap::Result<void> usable =
      expect_known(zone.value().breakdown.usable, kExpectedUsable, "zone usable");
  if (!usable.ok()) {
    return fail(usable.error().to_string());
  }
  const ccap::Result<void> offered =
      expect_known(zone.value().offered, kExpectedOffered, "zone offered");
  if (!offered.ok()) {
    return fail(offered.error().to_string());
  }
  std::cout << "consumer: zone hall-1 usable " << zone.value().breakdown.usable.to_string()
            << ", offered " << zone.value().offered.to_string() << "\n";

  // --- evaluate a candidate load --------------------------------------------
  ccap::CandidateLoad load;
  load.zone = ccap::ZoneId::literal("hall-1");
  load.medium = ccap::CoolingMedium::Air;
  load.compatibility = ccap::CompatibilityClass::AirConvection;
  load.thermal = ccap::ThermalPower::from_milliwatts(kCandidateLoad);
  load.actor = ccap::ActorId::literal("consumer");
  load.requested_at = instant;
  const ccap::Result<ccap::CandidateEvaluation> evaluation =
      ccap::evaluate_candidate(analyzer, load);
  if (!evaluation.ok()) {
    return fail(evaluation.error().to_string());
  }
  if (!evaluation.value().admitted()) {
    return fail("the candidate load should have been admitted: " +
                evaluation.value().to_string());
  }
  std::cout << "consumer: candidate " << load.thermal.to_string() << " is "
            << ccap::to_string(evaluation.value().disposition) << "\n";

  std::cout << "consumer: ok\n";
  return 0;
}
