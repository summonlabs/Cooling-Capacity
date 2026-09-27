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

// Capacity benchmarks.
//
// Four operations are measured over a SYNTHETIC facility: they are not a model
// of any real data centre and their figures are only a shape to compute over.
//
//   * a zone capacity rollup through CoolingAnalyzer::zone_capacity;
//   * a candidate load evaluation through evaluate_candidate;
//   * a full durable commit: the engine.publish() call end to end, including
//     the staging write, the flush, the atomic publish and the manifest
//     replacement;
//   * a generation diff between two generations.
//
// Only completed operations are timed: the clock is started immediately before
// the operation and stopped immediately after it returned a successful result,
// so setup, error paths and reporting are never counted. The benchmark exits
// non-zero if any measured operation fails.
//
// Usage:
//   benchmark_capacity           full run
//   benchmark_capacity --quick   reduced iteration counts (used by the smoke test)

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "cooling_capacity/cooling_capacity.hpp"

namespace ccap = cooling_capacity;

namespace {

using Milliwatts = std::int64_t;
using SteadyClock = std::chrono::steady_clock;

// The store directory this benchmark creates and removes.
constexpr const char* kStoreDirectory = "benchmark-store";

// The synthetic facility: eight air loops of four CRAH units each, one zone per
// loop.
constexpr std::size_t kLoopCount = 8;
constexpr std::size_t kUnitsPerLoop = 4;
constexpr Milliwatts kUnitNominal = 250'000'000;
constexpr Milliwatts kTransportLimit = 800'000'000;
constexpr Milliwatts kCandidateThermal = 300'000'000;

struct Iterations {
  std::size_t rollup;
  std::size_t candidate;
  std::size_t diff;
  std::size_t commit;
};

constexpr Iterations kQuickIterations{50, 100, 25, 5};
constexpr Iterations kFullIterations{500, 1000, 250, 25};

// Accumulates elapsed time for operations that completed. `start` is called
// before the operation and `stop` only after it succeeded.
class Timer {
 public:
  void start() { started_ = SteadyClock::now(); }

  void stop() {
    const std::chrono::duration<double, std::micro> elapsed = SteadyClock::now() - started_;
    elapsed_microseconds_ += elapsed.count();
    ++operations_;
  }

  [[nodiscard]] double microseconds() const { return elapsed_microseconds_; }
  [[nodiscard]] std::size_t operations() const { return operations_; }

 private:
  SteadyClock::time_point started_{};
  double elapsed_microseconds_ = 0.0;
  std::size_t operations_ = 0;
};

void report(const char* title, const char* operation, const Timer& timer) {
  const double per_operation = timer.microseconds() / static_cast<double>(timer.operations());
  const double per_second = per_operation > 0.0 ? 1'000'000.0 / per_operation : 0.0;
  std::cout << title << "\n";
  std::cout << std::left << std::setw(24) << "  operation" << operation << "\n";
  std::cout << std::left << std::setw(24) << "  completed operations" << timer.operations() << "\n";
  std::cout << std::left << std::setw(24) << "  total elapsed" << std::fixed
            << std::setprecision(3) << timer.microseconds() / 1000.0 << " ms\n";
  std::cout << std::left << std::setw(24) << "  per operation" << per_operation << " us\n";
  std::cout << std::left << std::setw(24) << "  operations per second" << per_second << "\n\n";
}

// Removes the benchmark store however the run ends, including on a failure.
struct StoreDirectoryGuard {
  ~StoreDirectoryGuard() {
    std::error_code ignored;
    std::filesystem::remove_all(kStoreDirectory, ignored);
  }
};

// Adds the SYNTHETIC model to a builder. When `revised` is set, one unit is
// re-rated so that two generations differ in exactly one record.
ccap::Result<void> add_synthetic_model(ccap::SnapshotBuilder& builder, bool revised) {
  ccap::SiteRecord site;
  site.id = ccap::SiteId::literal("site-synthetic");
  site.label = ccap::BoundedText::literal("Synthetic site");
  site.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(site));

  ccap::FacilityRecord facility;
  facility.id = ccap::FacilityId::literal("dc-synthetic");
  facility.site = site.id;
  facility.label = ccap::BoundedText::literal("Synthetic data center");
  facility.revision = ccap::RecordRevision::first();
  CCAP_TRY(builder.add(facility));

  for (std::size_t loop_index = 0; loop_index < kLoopCount; ++loop_index) {
    const std::string loop_tag = std::to_string(loop_index);
    std::vector<ccap::EquipmentId> units;
    units.reserve(kUnitsPerLoop);
    for (std::size_t unit_index = 0; unit_index < kUnitsPerLoop; ++unit_index) {
      ccap::EquipmentRecord unit;
      unit.id = ccap::EquipmentId::literal("crah-" + loop_tag + "-" +
                                           std::to_string(unit_index));
      unit.facility = facility.id;
      unit.label = ccap::BoundedText::literal("Synthetic CRAH " + loop_tag + "-" +
                                              std::to_string(unit_index));
      unit.kind = ccap::EquipmentKind::Crah;
      unit.state = ccap::OperatingState::InService;
      const bool re_rated = revised && loop_index == 0U && unit_index == 0U;
      unit.nominal = ccap::ThermalPower::from_milliwatts(re_rated ? kUnitNominal + 1'000'000
                                                                  : kUnitNominal);
      unit.revision = re_rated ? ccap::RecordRevision::from_value(2)
                               : ccap::RecordRevision::first();
      units.push_back(unit.id);
      CCAP_TRY(builder.add(unit));
    }

    ccap::LoopRecord loop;
    loop.id = ccap::LoopId::literal("loop-air-" + loop_tag);
    loop.facility = facility.id;
    loop.label = ccap::BoundedText::literal("Synthetic air loop " + loop_tag);
    loop.kind = ccap::LoopKind::AirSupply;
    loop.transport_limit = ccap::ThermalPower::from_milliwatts(kTransportLimit);
    loop.transport_limit_declared = true;
    loop.equipment = units;
    loop.redundancy = ccap::RedundancyClass::NPlusOne;
    loop.revision = ccap::RecordRevision::first();
    CCAP_TRY(builder.add(loop));

    ccap::ZoneRecord zone;
    zone.id = ccap::ZoneId::literal("hall-" + loop_tag);
    zone.facility = facility.id;
    zone.label = ccap::BoundedText::literal("Synthetic hall " + loop_tag);
    zone.media.insert(ccap::CoolingMedium::Air);
    zone.compatibility = {ccap::CompatibilityClass::AirConvection};
    zone.loops = {loop.id};
    zone.revision = ccap::RecordRevision::first();
    CCAP_TRY(builder.add(zone));
  }
  return ccap::Result<void>();
}

ccap::Result<std::shared_ptr<const ccap::CoolingSnapshot>> build_synthetic(
    std::uint64_t generation, ccap::Timestamp instant, bool revised) {
  ccap::SnapshotBuilder builder;
  builder.set_generation(ccap::CapacityGeneration::from_value(generation));
  builder.set_constructed_at(instant);
  CCAP_TRY(add_synthetic_model(builder, revised));
  ccap::ValidationReport report;
  CCAP_TRY_DECLARE(snapshot, builder.build(&report));
  if (!report.ok()) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation,
                       "the synthetic model failed validation")
        .with("findings", report.render());
  }
  return snapshot;
}

std::vector<ccap::ZoneId> synthetic_zones() {
  std::vector<ccap::ZoneId> zones;
  zones.reserve(kLoopCount);
  for (std::size_t index = 0; index < kLoopCount; ++index) {
    zones.push_back(ccap::ZoneId::literal("hall-" + std::to_string(index)));
  }
  return zones;
}

// Measures a zone capacity rollup over the SYNTHETIC facility.
ccap::Result<Milliwatts> measure_rollup(const ccap::CoolingSnapshot& snapshot,
                                        const std::vector<ccap::ZoneId>& zones,
                                        ccap::Timestamp instant, std::size_t iterations,
                                        Timer& timer) {
  const ccap::CoolingAnalyzer analyzer(snapshot, instant, ccap::AnalysisBasis::Fresh);
  Milliwatts sink = 0;
  for (std::size_t index = 0; index < iterations; ++index) {
    const ccap::ZoneId& zone = zones[index % zones.size()];
    timer.start();
    CCAP_TRY_DECLARE(view, analyzer.zone_capacity(zone, ccap::CoolingMedium::Air,
                                                  ccap::CompatibilityClass::AirConvection));
    timer.stop();
    sink += view.offered.amount_or_zero().milliwatts();
  }
  return sink;
}

// Measures a candidate load evaluation over the SYNTHETIC facility.
ccap::Result<Milliwatts> measure_candidate(const ccap::CoolingSnapshot& snapshot,
                                           const std::vector<ccap::ZoneId>& zones,
                                           ccap::Timestamp instant, std::size_t iterations,
                                           Timer& timer) {
  const ccap::CoolingAnalyzer analyzer(snapshot, instant, ccap::AnalysisBasis::Fresh);
  ccap::CandidateLoad load;
  load.zone = zones.front();
  load.medium = ccap::CoolingMedium::Air;
  load.compatibility = ccap::CompatibilityClass::AirConvection;
  load.thermal = ccap::ThermalPower::from_milliwatts(kCandidateThermal);
  load.actor = ccap::ActorId::literal("benchmark");
  load.requested_at = instant;
  CCAP_TRY(load.validate());

  Milliwatts sink = 0;
  for (std::size_t index = 0; index < iterations; ++index) {
    timer.start();
    CCAP_TRY_DECLARE(evaluation, ccap::evaluate_candidate(analyzer, load));
    timer.stop();
    if (!evaluation.admitted()) {
      return ccap::Error(ccap::ErrorCode::InvariantViolation,
                         "the benchmark candidate should be admitted")
          .with("evaluation", evaluation.to_string());
    }
    sink += evaluation.available.milliwatts();
  }
  return sink;
}

// Measures a generation diff between two generations.
ccap::Result<std::size_t> measure_diff(const ccap::CoolingSnapshot& before,
                                       const ccap::CoolingSnapshot& after,
                                       std::size_t iterations, Timer& timer) {
  std::size_t sink = 0;
  for (std::size_t index = 0; index < iterations; ++index) {
    timer.start();
    const ccap::GenerationDiff result = ccap::diff(before, after);
    timer.stop();
    sink += result.change_count();
  }
  return sink;
}

ccap::Result<void> run(bool quick) {
  const Iterations iterations = quick ? kQuickIterations : kFullIterations;

  CCAP_TRY_DECLARE(start, ccap::parse_timestamp("2026-11-02T09:00:00.000Z"));
  ccap::ManualClock clock(start);

  CCAP_TRY_DECLARE(snapshot, build_synthetic(1, clock.now(), false));
  CCAP_TRY_DECLARE(revised, build_synthetic(2, clock.now(), true));
  const std::vector<ccap::ZoneId> zones = synthetic_zones();

  std::cout << "Cooling Capacity benchmark\n";
  std::cout << std::left << std::setw(24) << "  library version" << ccap::version_string() << "\n";
  std::cout << std::left << std::setw(24) << "  mode"
            << (quick ? "quick: reduced iteration counts" : "full") << "\n";
  std::cout << std::left << std::setw(24) << "  model"
            << "SYNTHETIC: " + std::to_string(kLoopCount) + " loops, " +
                   std::to_string(kLoopCount) + " zones, " +
                   std::to_string(kLoopCount * kUnitsPerLoop) + " CRAH units, " +
                   std::to_string(snapshot->record_count()) + " records"
            << "\n";
  std::cout << std::left << std::setw(24) << "  store"
            << std::string(kStoreDirectory) + ": created and removed by this run" << "\n\n";

  // 1. Zone capacity rollup over the synthetic facility.
  Timer rollup_timer;
  CCAP_TRY_DECLARE(rollup_sink, measure_rollup(*snapshot, zones, clock.now(), iterations.rollup,
                                               rollup_timer));
  report("SYNTHETIC zone capacity rollup", "CoolingAnalyzer::zone_capacity", rollup_timer);

  // 2. Candidate load evaluation.
  Timer candidate_timer;
  CCAP_TRY_DECLARE(candidate_sink,
                   measure_candidate(*snapshot, zones, clock.now(), iterations.candidate,
                                     candidate_timer));
  report("SYNTHETIC candidate load evaluation", "evaluate_candidate", candidate_timer);

  // 3. A full durable commit: the whole publish path, including the flush.
  StoreDirectoryGuard guard;
  Timer commit_timer;
  {
    ccap::EngineOptions options;
    options.actor = ccap::ActorId::literal("benchmark");
    options.store.access = ccap::StoreAccess::ReadWrite;
    options.store.create_if_missing = true;
    options.clock = &clock;
    CCAP_TRY_DECLARE(engine, ccap::CoolingCapacityEngine::open(kStoreDirectory, options));

    // Seeding the store is setup, not a measured operation.
    CCAP_TRY_DECLARE(seed_builder, engine->begin_mutation());
    CCAP_TRY(add_synthetic_model(seed_builder, false));
    CCAP_TRY_DECLARE(seed_receipt, engine->publish(seed_builder));
    std::cout << "durable store seeded at generation " << seed_receipt.generation.to_string()
              << " (" << seed_receipt.artifact_bytes << " bytes per artifact)\n\n";

    const ccap::EquipmentId target = ccap::EquipmentId::literal("crah-0-0");
    std::uint64_t artifact_bytes = 0;
    for (std::size_t index = 0; index < iterations.commit; ++index) {
      // Preparing the candidate generation is setup; only publish is measured.
      CCAP_TRY_DECLARE(builder, engine->begin_mutation());
      const ccap::EquipmentRecord* current = engine->snapshot()->find_equipment(target);
      if (current == nullptr) {
        return ccap::Error(ccap::ErrorCode::NotFound, "the benchmark unit is not in the catalog")
            .with("equipment", target.str());
      }
      ccap::EquipmentRecord updated = *current;
      updated.nominal = ccap::ThermalPower::from_milliwatts(updated.nominal.milliwatts() + 1'000);
      CCAP_TRY_DECLARE(revision, updated.revision.next());
      updated.revision = revision;
      CCAP_TRY(builder.replace(updated));

      commit_timer.start();
      CCAP_TRY_DECLARE(receipt, engine->publish(builder));
      commit_timer.stop();
      artifact_bytes += receipt.artifact_bytes;
    }
    report("full durable commit", "CoolingCapacityEngine::publish (flush included)", commit_timer);
    std::cout << std::left << std::setw(24) << "  artifact bytes" << artifact_bytes << "\n\n";
  }

  // 4. Generation diff between two generations.
  Timer diff_timer;
  CCAP_TRY_DECLARE(diff_sink, measure_diff(*snapshot, *revised, iterations.diff, diff_timer));
  report("generation diff", "diff(previous, current)", diff_timer);

  // The sinks are printed so that no measured work can be optimised away.
  std::cout << "sinks (a checksum of everything the measured operations returned)\n";
  std::cout << std::left << std::setw(24) << "  rollup" << rollup_sink << "\n";
  std::cout << std::left << std::setw(24) << "  candidate" << candidate_sink << "\n";
  std::cout << std::left << std::setw(24) << "  diff" << diff_sink << "\n";
  std::cout << "\niteration counts\n";
  std::cout << std::left << std::setw(24) << "  rollup" << iterations.rollup << "\n";
  std::cout << std::left << std::setw(24) << "  candidate" << iterations.candidate << "\n";
  std::cout << std::left << std::setw(24) << "  commit" << iterations.commit << "\n";
  std::cout << std::left << std::setw(24) << "  diff" << iterations.diff << "\n";
  return ccap::Result<void>();
}

}  // namespace

int main(int argc, char** argv) {
  bool quick = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--quick") {
      quick = true;
      continue;
    }
    std::cout << "usage: benchmark_capacity [--quick]\n";
    return 1;
  }

  const ccap::Result<void> outcome = run(quick);
  if (!outcome.ok()) {
    std::cout << "FAILED: " << outcome.error().to_string() << "\n";
    return 1;
  }
  std::cout << "\nbenchmark finished: ok (" << (quick ? "quick" : "full") << ")\n";
  return 0;
}
