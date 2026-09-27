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

// Concurrency: readers never take a lock and writers are serialised by one
// engine mutex. The test drives real threads against a real engine on a real
// directory, records every observation in a per-thread structure and asserts on
// them after joining, so no check ever runs on a worker thread and no assertion
// depends on how the operating system schedules the threads.

#include "fixtures.hpp"
#include "test_harness.hpp"
#include "test_process.hpp"

using namespace ccap_test;

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using cooling_capacity::AnalysisBasis;
using cooling_capacity::CandidateEvaluation;
using cooling_capacity::CandidateLoad;
using cooling_capacity::CommitReceipt;
using cooling_capacity::CoolingAnalyzer;
using cooling_capacity::CoolingCapacityEngine;
using cooling_capacity::CoolingMedium;
using cooling_capacity::CoolingSnapshot;
using cooling_capacity::EngineOptions;
using cooling_capacity::ErrorCode;
using cooling_capacity::ManualClock;
using cooling_capacity::SnapshotBuilder;
using cooling_capacity::ZoneCapacity;

constexpr std::size_t kReaderThreads = 4;
constexpr std::size_t kPublishedGenerations = 50;
constexpr std::size_t kReaderIterationCap = 4000;
constexpr std::size_t kPublisherAttempts = 10;
constexpr std::int64_t kOfferedMilliwatts = 400000000;

struct Harness {
  std::string directory;
  ManualClock clock{fixture_now()};
  EngineOptions options;
  std::unique_ptr<CoolingCapacityEngine> engine;
};

std::unique_ptr<Harness> open_harness(const std::string& name) {
  auto harness = std::make_unique<Harness>();
  const Result<std::string> directory = fresh_directory(name);
  if (!directory.ok()) {
    std::abort();
  }
  harness->directory = directory.value();
  harness->options.actor = actor_id("operator");
  harness->options.clock = &harness->clock;
  harness->options.store.access = cooling_capacity::StoreAccess::ReadWrite;
  harness->options.store.create_if_missing = true;
  Result<std::unique_ptr<CoolingCapacityEngine>> engine =
      CoolingCapacityEngine::open(harness->directory, harness->options);
  if (!engine.ok()) {
    std::abort();
  }
  harness->engine = std::move(engine).value();

  Result<SnapshotBuilder> builder = harness->engine->begin_mutation();
  if (!builder.ok()) {
    std::abort();
  }
  if (!add_simple_facility(builder.value(), simple_facility()).ok()) {
    std::abort();
  }
  if (!harness->engine->publish(builder.value()).ok()) {
    std::abort();
  }
  return harness;
}

// A fresh measurement about the zone. Each publish adds one, so every
// generation differs from its predecessor.
EvidenceRecord pulse(const std::string& id) {
  EvidenceRecord record;
  record.id = evidence_id(id.c_str());
  record.kind = EvidenceKind::LoadMeasurement;
  record.source = EvidenceSource::BmsTelemetry;
  record.subject = zone_id("hall-1");
  record.medium = CoolingMedium::Air;
  record.value = milliwatts(1000);
  record.observed_at = fixture_now();
  record.provenance.actor = actor_id("bms");
  record.provenance.reference = cooling_capacity::DocumentRef::literal("bms-1");
  record.revision = RecordRevision::first();
  return record;
}

CandidateLoad small_load() {
  CandidateLoad load;
  load.zone = zone_id("hall-1");
  load.medium = CoolingMedium::Air;
  load.compatibility = CompatibilityClass::AirConvection;
  load.thermal = milliwatts(1000);
  load.actor = actor_id("operator");
  load.requested_at = fixture_now();
  return load;
}

// What one reader observed. Written only by its own thread and read after the
// join, so no synchronisation is needed beyond the join itself.
struct ReaderResult {
  std::size_t observations = 0;
  std::uint64_t first_generation = 0;
  std::uint64_t last_generation = 0;
  std::size_t regressions = 0;
  std::size_t null_snapshots = 0;
  std::size_t malformed = 0;
  std::size_t capacity_errors = 0;
  std::size_t capacity_mismatches = 0;
  std::size_t evaluation_errors = 0;
  std::size_t evaluation_not_admitted = 0;
};

void reader_body(const CoolingCapacityEngine& engine, const std::atomic<bool>& go,
                 const std::atomic<bool>& stop, const std::atomic<std::size_t>& published,
                 const CandidateLoad& load, ReaderResult& result) {
  while (!go.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  // Wait until at least one new generation exists, so every reader definitely
  // reads while writers are running. This is a wait on a fact the main thread
  // guarantees, not a sleep.
  while (published.load(std::memory_order_acquire) == 0U) {
    std::this_thread::yield();
  }

  std::uint64_t previous = 0;
  for (std::size_t iteration = 0; iteration < kReaderIterationCap; ++iteration) {
    if (stop.load(std::memory_order_acquire)) {
      break;
    }
    const std::shared_ptr<const CoolingSnapshot> snapshot = engine.snapshot();
    if (snapshot == nullptr) {
      ++result.null_snapshots;
      continue;
    }
    const std::uint64_t generation = snapshot->generation().value();
    if (generation < previous) {
      ++result.regressions;
    }
    previous = generation;
    if (result.observations == 0U) {
      result.first_generation = generation;
    }
    result.last_generation = generation;
    ++result.observations;

    // A snapshot is a whole immutable generation: its identity, its record count
    // and its contents must agree with one another.
    if (generation == 0U || snapshot->record_count() == 0U ||
        snapshot->digest().to_hex().size() != 64U ||
        snapshot->find_zone(zone_id("hall-1")) == nullptr ||
        snapshot->find_loop(loop_id("air-loop-1")) == nullptr ||
        snapshot->record_count() !=
            snapshot->sites().size() + snapshot->facilities().size() +
                snapshot->domains().size() + snapshot->manifolds().size() +
                snapshot->equipment().size() + snapshot->loops().size() +
                snapshot->plants().size() + snapshot->zones().size() +
                snapshot->evidence().size() + snapshot->commitments().size()) {
      ++result.malformed;
    }

    // The capacity answer of the generation the reader holds does not depend on
    // which generation that is.
    const CoolingAnalyzer analyzer(*snapshot, fixture_now(), AnalysisBasis::Fresh);
    const Result<ZoneCapacity> zone = analyzer.zone_capacity(
        zone_id("hall-1"), CoolingMedium::Air, CompatibilityClass::AirConvection);
    if (!zone.ok()) {
      ++result.capacity_errors;
    } else if (!zone.value().offered.is_known() ||
               zone.value().offered.amount_or_zero().milliwatts() != kOfferedMilliwatts) {
      ++result.capacity_mismatches;
    }

    const Result<CandidateEvaluation> evaluation = engine.evaluate(load);
    if (!evaluation.ok()) {
      ++result.evaluation_errors;
    } else if (!evaluation.value().admitted()) {
      ++result.evaluation_not_admitted;
    }
  }
}

}  // namespace

CCAP_TEST(readers_never_see_a_torn_snapshot_while_generations_are_published) {
  std::unique_ptr<Harness> harness = open_harness("ccap-test-concurrency");
  CoolingCapacityEngine& engine = *harness->engine;
  // Generation one is committed when the store is created, generation two is the
  // published facility.
  const CapacityGeneration initial = engine.snapshot()->generation();
  CCAP_CHECK_EQ(initial, CapacityGeneration::from_value(2));

  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::atomic<std::size_t> published{0};
  const CandidateLoad load = small_load();
  std::vector<ReaderResult> results(kReaderThreads);
  std::vector<std::thread> readers;
  readers.reserve(kReaderThreads);
  for (std::size_t index = 0; index < kReaderThreads; ++index) {
    readers.emplace_back(reader_body, std::cref(engine), std::cref(go), std::cref(stop),
                         std::cref(published), std::cref(load), std::ref(results[index]));
  }

  go.store(true, std::memory_order_release);
  for (std::size_t index = 0; index < kPublishedGenerations; ++index) {
    Result<SnapshotBuilder> builder = engine.begin_mutation();
    CCAP_CHECK(builder.ok());
    if (!builder.ok()) {
      break;
    }
    const std::string name = "pulse-" + std::to_string(index);
    CCAP_CHECK_VOID(builder.value().add(pulse(name)));
    const Result<CommitReceipt> receipt = engine.publish(builder.value());
    CCAP_CHECK(receipt.ok());
    if (!receipt.ok()) {
      break;
    }
    CCAP_CHECK_EQ(receipt.value().generation,
                  CapacityGeneration::from_value(initial.value() + index + 1U));
    published.fetch_add(1U, std::memory_order_release);
  }
  stop.store(true, std::memory_order_release);
  for (std::thread& reader : readers) {
    reader.join();
  }

  const std::shared_ptr<const CoolingSnapshot> final_snapshot = engine.snapshot();
  CCAP_CHECK(final_snapshot != nullptr);
  const std::uint64_t final_generation = final_snapshot->generation().value();
  CCAP_CHECK_EQ(final_generation, initial.value() + kPublishedGenerations);
  CCAP_CHECK_EQ(published.load(), kPublishedGenerations);
  CCAP_CHECK_EQ(final_snapshot->evidence().size(), kPublishedGenerations);
  CCAP_CHECK_EQ(final_snapshot->record_count(), 6U + kPublishedGenerations);

  std::size_t total_observations = 0;
  for (std::size_t index = 0; index < kReaderThreads; ++index) {
    const ReaderResult& result = results[index];
    CCAP_CHECK(result.observations > 0U);
    CCAP_CHECK_EQ(result.null_snapshots, 0U);
    CCAP_CHECK_EQ(result.regressions, 0U);
    CCAP_CHECK_EQ(result.malformed, 0U);
    CCAP_CHECK_EQ(result.capacity_errors, 0U);
    CCAP_CHECK_EQ(result.capacity_mismatches, 0U);
    CCAP_CHECK_EQ(result.evaluation_errors, 0U);
    CCAP_CHECK_EQ(result.evaluation_not_admitted, 0U);
    // Every reader started after the first publication, so it observed a
    // generation at or beyond the second one, and never one beyond the head.
    CCAP_CHECK(result.first_generation >= initial.value() + 1U);
    CCAP_CHECK(result.last_generation >= result.first_generation);
    CCAP_CHECK(result.last_generation <= final_generation);
    total_observations += result.observations;
  }
  CCAP_CHECK(total_observations >= kReaderThreads);

  // Dropping the engine releases the writer lock and the open files, so the
  // directory can actually be removed.
  const std::string directory = harness->directory;
  harness.reset();
}

CCAP_TEST(two_concurrent_publishers_serialise_on_the_head_generation) {
  std::unique_ptr<Harness> harness = open_harness("ccap-test-concurrency-publish");
  CoolingCapacityEngine& engine = *harness->engine;
  const CapacityGeneration base = engine.snapshot()->generation();
  CCAP_CHECK_EQ(base, CapacityGeneration::from_value(2));

  std::atomic<std::size_t> successes{0};
  std::atomic<std::size_t> unexpected{0};
  const auto publisher = [&engine, &successes, &unexpected](std::size_t thread_index) {
    for (std::size_t attempt = 0; attempt < kPublisherAttempts; ++attempt) {
      for (;;) {
        Result<SnapshotBuilder> builder = engine.begin_mutation();
        if (!builder.ok()) {
          unexpected.fetch_add(1U, std::memory_order_relaxed);
          break;
        }
        const std::string name = "publisher-" + std::to_string(thread_index) + "-" +
                                 std::to_string(attempt);
        if (!builder.value().add(pulse(name)).ok()) {
          unexpected.fetch_add(1U, std::memory_order_relaxed);
          break;
        }
        const Result<CommitReceipt> receipt = engine.publish(builder.value());
        if (receipt.ok()) {
          successes.fetch_add(1U, std::memory_order_relaxed);
          break;
        }
        // Another writer moved the head between reading it and committing. The
        // refusal is the serialisation working, so the attempt is retried
        // against the generation that is current now.
        const ErrorCode code = receipt.error().code();
        if (code != ErrorCode::Conflict && code != ErrorCode::StaleGeneration) {
          unexpected.fetch_add(1U, std::memory_order_relaxed);
          break;
        }
      }
    }
  };

  std::thread first(publisher, 0U);
  std::thread second(publisher, 1U);
  first.join();
  second.join();

  const std::size_t published = successes.load();
  CCAP_CHECK_EQ(unexpected.load(), 0U);
  // Both writers make progress: a serialised pair of producers cannot lose all
  // of its attempts.
  CCAP_CHECK(published >= kPublisherAttempts);
  CCAP_CHECK(published <= 2U * kPublisherAttempts);

  // The head generation equals the number of successful publishes on top of the
  // generation the two writers started from, which is exactly the statement that
  // concurrent publishes serialise without losing or double-applying one.
  const std::shared_ptr<const CoolingSnapshot> final_snapshot = engine.snapshot();
  CCAP_CHECK(final_snapshot != nullptr);
  CCAP_CHECK_EQ(final_snapshot->generation().value(), base.value() + published);
  CCAP_CHECK_EQ(final_snapshot->evidence().size(), published);
  CCAP_CHECK_EQ(final_snapshot->record_count(), 6U + published);

  // The durable head agrees with the in-memory one, and the artifact on disk
  // still decodes, so a concurrent publish cannot leave a damaged generation.
  CCAP_CHECK_OK(generations, engine.generations());
  CCAP_CHECK_FALSE(generations.empty());
  if (!generations.empty()) {
    CCAP_CHECK_EQ(generations.back().generation, final_snapshot->generation());
    CCAP_CHECK(generations.back().is_head);
    CCAP_CHECK(generations.back().readable);
  }
  CCAP_CHECK_OK(reloaded, engine.load_generation(final_snapshot->generation()));
  CCAP_CHECK_EQ(reloaded->digest(), final_snapshot->digest());
  CCAP_CHECK_EQ(reloaded->record_count(), final_snapshot->record_count());

  // The surviving capacity answer is still the one the model describes.
  CCAP_CHECK_OK(zone, engine.zone_capacity(zone_id("hall-1"), CoolingMedium::Air,
                                           CompatibilityClass::AirConvection));
  CCAP_CHECK_EQ(zone.offered.amount_or_zero().milliwatts(), kOfferedMilliwatts);
  CCAP_CHECK_EQ(zone.loops.size(), 1U);

  // Dropping the engine releases the writer lock and the open files, so the
  // directory can actually be removed.
  const std::string directory = harness->directory;
  harness.reset();
}