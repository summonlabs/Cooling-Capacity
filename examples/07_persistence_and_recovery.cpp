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

// 07 - persistence and recovery.
//
// The engine is the composition of a durable store and an analysis view. This
// example publishes a generation into a store on disk, closes the engine,
// reopens it, and shows what recovery does and does not grant:
//
//   * the reopened snapshot is readable and its capacity figures are reported;
//   * its analysis basis is RecoveredPendingRevalidation, which is not the same
//     authority as a snapshot that was just built;
//   * a candidate load is Indeterminate until revalidate() has recorded that the
//     recovered state was checked against current evidence.
//
// The store lives in the current working directory under example07-store and is
// removed at the end.

#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>

#include "cooling_capacity/cooling_capacity.hpp"

namespace ccap = cooling_capacity;

namespace {

using Milliwatts = std::int64_t;

// The store directory this example creates and removes.
constexpr const char* kStoreDirectory = "example07-store";

constexpr Milliwatts kCrahNominal = 400'000'000;
constexpr Milliwatts kLoopTransportLimit = 600'000'000;
constexpr Milliwatts kCandidateLoad = 300'000'000;

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

// Removes the store directory, whether or not a previous run left one behind.
void remove_store() {
  std::error_code ignored;
  std::filesystem::remove_all(kStoreDirectory, ignored);
}

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
  return ccap::Result<void>();
}

ccap::CandidateLoad candidate_load(ccap::Timestamp instant) {
  ccap::CandidateLoad load;
  load.zone = ccap::ZoneId::literal("hall-1");
  load.medium = ccap::CoolingMedium::Air;
  load.compatibility = ccap::CompatibilityClass::AirConvection;
  load.thermal = ccap::ThermalPower::from_milliwatts(kCandidateLoad);
  load.actor = ccap::ActorId::literal("example-07");
  load.requested_at = instant;
  return load;
}

ccap::Result<void> run() {
  CCAP_TRY_DECLARE(start, ccap::parse_timestamp("2026-09-14T06:00:00.000Z"));
  ccap::ManualClock clock(start);

  // A failed earlier run must not make this one fail: start from nothing.
  remove_store();

  ccap::EngineOptions options;
  options.actor = ccap::ActorId::literal("example-07");
  options.store.access = ccap::StoreAccess::ReadWrite;
  options.store.create_if_missing = true;
  options.clock = &clock;

  std::cout << "Cooling Capacity example 07: persistence and recovery\n\n";
  std::cout << "store directory  " << kStoreDirectory
            << " (created and removed by this example)\n";
  std::cout << std::left << std::setw(30) << "evaluated at" << clock.now().to_string() << "\n\n";

  ccap::CapacityGeneration published = ccap::CapacityGeneration::from_value(0);
  {
    CCAP_TRY_DECLARE(engine, ccap::CoolingCapacityEngine::open(kStoreDirectory, options));
    std::cout << "open\n";
    std::cout << std::left << std::setw(30) << "  generation"
              << engine->snapshot()->generation().to_string() << " (empty catalog)\n";
    std::cout << std::left << std::setw(30) << "  basis" << ccap::to_string(engine->basis())
              << "\n\n";

    // A published generation goes through the whole store protocol: validate,
    // reserve, write staging, flush, verify, publish atomically, replace the
    // manifest.
    CCAP_TRY_DECLARE(builder, engine->begin_mutation());
    CCAP_TRY(add_model(builder));
    CCAP_TRY_DECLARE(receipt, engine->publish(builder));
    published = receipt.generation;
    std::cout << "publish\n";
    std::cout << std::left << std::setw(30) << "  generation" << receipt.generation.to_string()
              << "\n";
    std::cout << std::left << std::setw(30) << "  digest" << receipt.digest.to_hex() << "\n";
    std::cout << std::left << std::setw(30) << "  committed at" << receipt.committed_at.to_string()
              << "\n";
    std::cout << std::left << std::setw(30) << "  artifact bytes"
              << std::to_string(receipt.artifact_bytes) << "\n";
    std::cout << std::left << std::setw(30) << "  manifest bytes"
              << std::to_string(receipt.manifest_bytes) << "\n";
    std::cout << std::left << std::setw(30) << "  head replaced"
              << (receipt.head_replaced ? "true" : "false") << "\n";
    std::cout << std::left << std::setw(30) << "  basis" << ccap::to_string(engine->basis())
              << "\n\n";

    CCAP_TRY_DECLARE(view, engine->zone_capacity(ccap::ZoneId::literal("hall-1"),
                                                 ccap::CoolingMedium::Air,
                                                 ccap::CompatibilityClass::AirConvection));
    CCAP_TRY(expect_known(view.offered, kLoopTransportLimit, "offered before recovery"));
    std::cout << "capacity before recovery\n";
    print_figure("  usable", view.breakdown.usable);
    print_figure("  offered", view.offered);
    CCAP_TRY_DECLARE(before, engine->evaluate(candidate_load(clock.now())));
    std::cout << std::left << std::setw(30) << "  candidate" << before.to_string() << "\n\n";

    // Releasing the writer is explicit here; destroying the engine would do it
    // as well, because the lock belongs to the operating system and not to a
    // flag in memory.
    CCAP_TRY(engine->release_writer());
    std::cout << "close: the writer lock is released and the engine is destroyed\n\n";
  }

  {
    CCAP_TRY_DECLARE(engine, ccap::CoolingCapacityEngine::open(kStoreDirectory, options));
    const std::shared_ptr<const ccap::CoolingSnapshot> recovered = engine->snapshot();
    if (recovered->generation() != published) {
      return ccap::Error(ccap::ErrorCode::InvariantViolation,
                         "the reopened engine did not adopt the published generation")
          .with("published", published.to_string())
          .with("reopened", recovered->generation().to_string());
    }
    if (recovered->origin() != ccap::SnapshotOrigin::RecoveredFromStore) {
      return ccap::Error(ccap::ErrorCode::InvariantViolation,
                         "a recovered snapshot must say where it came from")
          .with("origin", std::string(ccap::to_string(recovered->origin())));
    }
    if (engine->basis() != ccap::AnalysisBasis::RecoveredPendingRevalidation) {
      return ccap::Error(ccap::ErrorCode::InvariantViolation,
                         "recovered state is not current authority")
          .with("basis", std::string(ccap::to_string(engine->basis())));
    }

    std::cout << "reopen\n";
    std::cout << std::left << std::setw(30) << "  generation"
              << recovered->generation().to_string() << "\n";
    std::cout << std::left << std::setw(30) << "  origin"
              << ccap::to_string(recovered->origin()) << "\n";
    std::cout << std::left << std::setw(30) << "  basis" << ccap::to_string(engine->basis())
              << "\n";
    std::cout << std::left << std::setw(30) << "  revalidated at"
              << (recovered->revalidated_at().has_value()
                      ? recovered->revalidated_at()->to_string()
                      : std::string("never"))
              << "\n";
    std::cout << std::left << std::setw(30) << "  records"
              << std::to_string(recovered->record_count()) << "\n\n";

    // The figures are still reported: recovery is about authority, not about
    // hiding what is stored.
    CCAP_TRY_DECLARE(view, engine->zone_capacity(ccap::ZoneId::literal("hall-1"),
                                                 ccap::CoolingMedium::Air,
                                                 ccap::CompatibilityClass::AirConvection));
    CCAP_TRY(expect_known(view.offered, kLoopTransportLimit, "offered after recovery"));
    std::cout << "capacity after recovery\n";
    print_figure("  usable", view.breakdown.usable);
    print_figure("  offered", view.offered);
    CCAP_TRY_DECLARE(pending, engine->evaluate(candidate_load(clock.now())));
    if (pending.disposition != ccap::CandidateDisposition::Indeterminate ||
        pending.reason != ccap::CandidateReason::RecoveredPendingRevalidation) {
      return ccap::Error(ccap::ErrorCode::InvariantViolation,
                         "a recovered generation must not admit a load before revalidation")
          .with("evaluation", pending.to_string());
    }
    std::cout << std::left << std::setw(30) << "  candidate" << pending.to_string() << "\n";
    std::cout << "  the load cannot be placed: persisted state does not become current by\n"
              << "  being loaded. The offer in that answer is the default value, because the\n"
              << "  evaluation stopped at the recovery precondition before any capacity was\n"
              << "  computed.\n\n";

    // Revalidation records that the recovered state was examined at this instant
    // and publishes a new generation. It does not make stale evidence fresh.
    clock.advance(ccap::DurationMs::from_milliseconds(60'000));
    CCAP_TRY_DECLARE(revalidated, engine->revalidate(clock));
    if (engine->basis() != ccap::AnalysisBasis::Revalidated) {
      return ccap::Error(ccap::ErrorCode::InvariantViolation,
                         "revalidation must change the analysis basis")
          .with("basis", std::string(ccap::to_string(engine->basis())));
    }
    std::cout << "revalidate\n";
    std::cout << std::left << std::setw(30) << "  published generation"
              << revalidated.to_string() << "\n";
    std::cout << std::left << std::setw(30) << "  basis" << ccap::to_string(engine->basis())
              << "\n";
    std::cout << std::left << std::setw(30) << "  revalidated at" << clock.now().to_string()
              << "\n";
    std::cout << std::left << std::setw(30) << "  origin"
              << ccap::to_string(engine->snapshot()->origin()) << "\n";
    std::cout << std::left << std::setw(30) << "  revalidated_at recorded"
              << (engine->snapshot()->revalidated_at().has_value() ? "yes" : "no") << "\n\n";

    CCAP_TRY_DECLARE(after, engine->evaluate(candidate_load(clock.now())));
    if (!after.admitted()) {
      return ccap::Error(ccap::ErrorCode::InvariantViolation,
                         "the candidate should be admitted after revalidation")
          .with("evaluation", after.to_string());
    }
    std::cout << "capacity after revalidation\n";
    std::cout << std::left << std::setw(30) << "  candidate" << after.to_string() << "\n\n";
  }

  // An independent read-only handle verifies the store end to end: manifest,
  // every generation file, the decode and the validation of each one.
  {
    ccap::StoreOpenOptions read_only;
    read_only.access = ccap::StoreAccess::ReadOnly;
    CCAP_TRY_DECLARE(store, ccap::CoolingCapacityStore::open(kStoreDirectory, read_only));
    CCAP_TRY_DECLARE(verification, store->verify());
    std::cout << "verify\n";
    std::cout << std::left << std::setw(30) << "  ok" << (verification.ok ? "true" : "false")
              << "\n";
    std::cout << std::left << std::setw(30) << "  head generation"
              << verification.head_generation.to_string() << "\n";
    std::cout << std::left << std::setw(30) << "  unreadable generations"
              << std::to_string(verification.unreadable_generations) << "\n";
    if (!verification.ok) {
      return ccap::Error(ccap::ErrorCode::InvariantViolation, "the store did not verify")
          .with("report", verification.render());
    }
    CCAP_TRY_DECLARE(generations, store->generations());
    for (const ccap::GenerationInfo& info : generations) {
      std::cout << std::left << std::setw(30) << "  generation"
                << info.generation.to_string() + (info.is_head ? " (head)" : "") + ", " +
                       std::to_string(info.bytes) + " bytes, readable " +
                       (info.readable ? "true" : "false")
                << "\n";
    }
    CCAP_TRY(store->close());
  }

  remove_store();
  std::cout << "\nremoved the store directory " << kStoreDirectory << "\n";
  return ccap::Result<void>();
}

}  // namespace

int main() {
  const ccap::Result<void> outcome = run();
  if (!outcome.ok()) {
    return failure(outcome.error());
  }
  std::cout << "example 07 finished: ok\n";
  return 0;
}
