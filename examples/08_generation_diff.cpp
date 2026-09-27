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

// 08 - generation diff.
//
// Two generations of the same facility are built and compared. The diff is
// deterministic: changes are ordered by record kind and then by identifier, and
// the field deltas inside a record are ordered by field name, so the same two
// generations always produce byte-identical output no matter how the records
// were inserted or stored.
//
// The example prints the summary, every changed record with its field deltas in
// canonical rendering, the explanation step, and the capacity consequence of the
// change.

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

constexpr Milliwatts kCrahNominal = 400'000'000;
constexpr Milliwatts kSpareNominal = 250'000'000;
constexpr Milliwatts kFirstTransportLimit = 700'000'000;
constexpr Milliwatts kSecondTransportLimit = 900'000'000;
constexpr Milliwatts kRaisedNominal = 420'000'000;

int failure(const ccap::Error& error) {
  std::cout << "FAILED: " << error.to_string() << "\n";
  return 1;
}

ccap::Result<void> expect_count(std::size_t actual, std::size_t expected, const char* what) {
  if (actual != expected) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation, "the diff count is not the expected one")
        .with("count", what)
        .with("actual", std::to_string(actual))
        .with("expected", std::to_string(expected));
  }
  return ccap::Result<void>();
}

ccap::Result<void> add_site_and_facility(ccap::SnapshotBuilder& builder) {
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
  return ccap::Result<void>();
}

ccap::Result<void> add_crah(ccap::SnapshotBuilder& builder, const ccap::FacilityId& facility,
                            const char* id, const char* label, Milliwatts nominal,
                            ccap::OperatingState state, std::uint64_t revision) {
  ccap::EquipmentRecord record;
  record.id = ccap::EquipmentId::literal(id);
  record.facility = facility;
  record.label = ccap::BoundedText::literal(label);
  record.kind = ccap::EquipmentKind::Crah;
  record.state = state;
  record.nominal = ccap::ThermalPower::from_milliwatts(nominal);
  record.revision = ccap::RecordRevision::from_value(revision);
  return builder.add(record);
}

ccap::Result<void> add_loop(ccap::SnapshotBuilder& builder, const ccap::FacilityId& facility,
                            Milliwatts transport_limit, std::uint64_t revision,
                            const std::vector<ccap::EquipmentId>& equipment) {
  ccap::LoopRecord loop;
  loop.id = ccap::LoopId::literal("loop-air-1");
  loop.facility = facility;
  loop.label = ccap::BoundedText::literal("Air loop 1");
  loop.kind = ccap::LoopKind::AirSupply;
  loop.transport_limit = ccap::ThermalPower::from_milliwatts(transport_limit);
  loop.transport_limit_declared = true;
  loop.equipment = equipment;
  loop.redundancy = ccap::RedundancyClass::NPlusOne;
  loop.revision = ccap::RecordRevision::from_value(revision);
  return builder.add(loop);
}

ccap::Result<void> add_zone(ccap::SnapshotBuilder& builder, const ccap::FacilityId& facility,
                            const char* id, const char* label, std::uint64_t revision) {
  ccap::ZoneRecord zone;
  zone.id = ccap::ZoneId::literal(id);
  zone.facility = facility;
  zone.label = ccap::BoundedText::literal(label);
  zone.media.insert(ccap::CoolingMedium::Air);
  zone.compatibility = {ccap::CompatibilityClass::AirConvection};
  zone.loops = {ccap::LoopId::literal("loop-air-1")};
  zone.revision = ccap::RecordRevision::from_value(revision);
  return builder.add(zone);
}

// Generation 1: two units in N+1 behind a 700 kW path, and a standby spare that
// no loop uses.
ccap::Result<void> build_first_generation(ccap::SnapshotBuilder& builder) {
  const ccap::FacilityId facility = ccap::FacilityId::literal("dc-1");
  CCAP_TRY(add_site_and_facility(builder));
  CCAP_TRY(add_crah(builder, facility, "crah-1", "CRAH 1", kCrahNominal,
                    ccap::OperatingState::InService, 1));
  CCAP_TRY(add_crah(builder, facility, "crah-2", "CRAH 2", kCrahNominal,
                    ccap::OperatingState::InService, 1));
  CCAP_TRY(add_crah(builder, facility, "crah-spare", "CRAH spare", kSpareNominal,
                    ccap::OperatingState::Standby, 1));
  CCAP_TRY(add_loop(builder, facility, kFirstTransportLimit, 1,
                    {ccap::EquipmentId::literal("crah-1"), ccap::EquipmentId::literal("crah-2")}));
  CCAP_TRY(add_zone(builder, facility, "hall-1", "Hall 1", 1));
  return ccap::Result<void>();
}

// Generation 2: CRAH 1 is re-rated, a third unit is added to the loop, the
// transport survey is revised, the spare is retired, and a second hall is added.
ccap::Result<void> build_second_generation(ccap::SnapshotBuilder& builder) {
  const ccap::FacilityId facility = ccap::FacilityId::literal("dc-1");
  CCAP_TRY(add_site_and_facility(builder));
  CCAP_TRY(add_crah(builder, facility, "crah-1", "CRAH 1", kRaisedNominal,
                    ccap::OperatingState::InService, 2));
  CCAP_TRY(add_crah(builder, facility, "crah-2", "CRAH 2", kCrahNominal,
                    ccap::OperatingState::InService, 1));
  CCAP_TRY(add_crah(builder, facility, "crah-3", "CRAH 3", kCrahNominal,
                    ccap::OperatingState::InService, 1));
  CCAP_TRY(add_loop(builder, facility, kSecondTransportLimit, 2,
                    {ccap::EquipmentId::literal("crah-1"), ccap::EquipmentId::literal("crah-2"),
                     ccap::EquipmentId::literal("crah-3")}));
  CCAP_TRY(add_zone(builder, facility, "hall-1", "Hall 1", 1));
  CCAP_TRY(add_zone(builder, facility, "hall-2", "Hall 2", 1));
  return ccap::Result<void>();
}

ccap::Result<std::shared_ptr<const ccap::CoolingSnapshot>> build(
    std::uint64_t generation, ccap::Timestamp instant, bool second) {
  ccap::SnapshotBuilder builder;
  builder.set_generation(ccap::CapacityGeneration::from_value(generation));
  builder.set_constructed_at(instant);
  CCAP_TRY(second ? build_second_generation(builder) : build_first_generation(builder));
  ccap::ValidationReport report;
  CCAP_TRY_DECLARE(snapshot, builder.build(&report));
  if (!report.ok()) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation, "a generation failed validation")
        .with("generation", std::to_string(generation))
        .with("findings", report.render());
  }
  return snapshot;
}

ccap::Result<ccap::CapacityValue> offered_at(const ccap::CoolingSnapshot& snapshot,
                                             ccap::Timestamp instant) {
  const ccap::CoolingAnalyzer analyzer(snapshot, instant, ccap::AnalysisBasis::Fresh);
  CCAP_TRY_DECLARE(view, analyzer.zone_capacity(ccap::ZoneId::literal("hall-1"),
                                                ccap::CoolingMedium::Air,
                                                ccap::CompatibilityClass::AirConvection));
  return view.offered;
}

ccap::Result<void> run() {
  CCAP_TRY_DECLARE(start, ccap::parse_timestamp("2026-10-05T16:20:00.000Z"));
  ccap::ManualClock clock(start);

  CCAP_TRY_DECLARE(generation_one, build(1, clock.now(), false));
  CCAP_TRY_DECLARE(generation_two, build(2, clock.now(), true));

  const ccap::GenerationDiff first = ccap::diff(*generation_one, *generation_two);
  const ccap::GenerationDiff again = ccap::diff(*generation_one, *generation_two);

  CCAP_TRY(expect_count(first.added, 2U, "added"));
  CCAP_TRY(expect_count(first.removed, 1U, "removed"));
  CCAP_TRY(expect_count(first.modified, 2U, "modified"));
  CCAP_TRY(expect_count(first.change_count(), 5U, "changes"));
  if (first.identical) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation,
                       "the two generations differ, so the diff cannot be identical");
  }
  if (!(first.changes == again.changes)) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation,
                       "two diffs of the same generations must be identical");
  }

  CCAP_TRY_DECLARE(offered_one, offered_at(*generation_one, clock.now()));
  CCAP_TRY_DECLARE(offered_two, offered_at(*generation_two, clock.now()));

  std::cout << "Cooling Capacity example 08: generation diff\n\n";
  std::cout << std::left << std::setw(24) << "from generation" << first.from.to_string() << "\n";
  std::cout << std::left << std::setw(24) << "to generation" << first.to.to_string() << "\n";
  std::cout << std::left << std::setw(24) << "from digest" << first.from_digest.to_hex() << "\n";
  std::cout << std::left << std::setw(24) << "to digest" << first.to_digest.to_hex() << "\n";
  std::cout << std::left << std::setw(24) << "identical"
            << (first.identical ? "true" : "false") << "\n";
  std::cout << std::left << std::setw(24) << "added/removed/modified"
            << std::to_string(first.added) + " / " + std::to_string(first.removed) + " / " +
                   std::to_string(first.modified)
            << "\n\n";

  std::cout << "changes, ordered by record kind and then by identifier\n";
  for (const ccap::RecordChange& change : first.changes) {
    std::cout << "  " << change.to_string() << "\n";
  }

  std::cout << "\ncapacity consequence for zone hall-1\n";
  std::cout << std::left << std::setw(30) << "  offered in generation 1" << offered_one.to_string()
            << "\n";
  std::cout << std::left << std::setw(30) << "  offered in generation 2" << offered_two.to_string()
            << "\n";

  std::cout << "\nexplanation\n";
  std::cout << ccap::render_steps(first.explanation);

  std::cout << "the diff is deterministic: a second computation produced the same "
            << std::to_string(again.change_count()) << " changes\n";
  return ccap::Result<void>();
}

}  // namespace

int main() {
  const ccap::Result<void> outcome = run();
  if (!outcome.ok()) {
    return failure(outcome.error());
  }
  std::cout << "\nexample 08 finished: ok\n";
  return 0;
}
