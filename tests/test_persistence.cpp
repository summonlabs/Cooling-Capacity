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

#include "fixtures.hpp"
#include "test_harness.hpp"
#include "test_process.hpp"

#include <memory>

using namespace ccap_test;

namespace {

using cooling_capacity::CapacityGeneration;
using cooling_capacity::CoolingCapacityEngine;
using cooling_capacity::CoolingCapacityStore;
using cooling_capacity::CoolingMedium;
using cooling_capacity::EngineOptions;
using cooling_capacity::ErrorCode;
using cooling_capacity::HeadRecovery;
using cooling_capacity::ManualClock;
using cooling_capacity::SnapshotBuilder;
using cooling_capacity::SnapshotOrigin;
using cooling_capacity::StoreAccess;
using cooling_capacity::StoreOpenOptions;

EngineOptions write_options(const ManualClock& clock) {
  EngineOptions options;
  options.actor = actor_id("operator");
  options.clock = &clock;
  options.store.access = StoreAccess::ReadWrite;
  options.store.create_if_missing = true;
  return options;
}

EngineOptions read_options(const ManualClock& clock) {
  EngineOptions options;
  options.actor = actor_id("reader");
  options.clock = &clock;
  options.store.access = StoreAccess::ReadOnly;
  options.store.create_if_missing = false;
  return options;
}

void publish_facility(const std::unique_ptr<CoolingCapacityEngine>& engine) {
  Result<SnapshotBuilder> builder = engine->begin_mutation();
  if (!builder.ok()) {
    std::abort();
  }
  const SimpleFacility facility = simple_facility();
  if (!upsert_simple_facility(builder.value(), facility).ok()) {
    std::abort();
  }
  if (!engine->publish(builder.value()).ok()) {
    std::abort();
  }
}

}  // namespace

CCAP_TEST(a_published_generation_survives_close_and_reopen) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-persistence"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const ManualClock clock(fixture_now());
  cooling_capacity::Digest digest;
  {
    CCAP_CHECK_OK(engine, CoolingCapacityEngine::open(directory, write_options(clock)));
    publish_facility(engine);
    digest = engine->snapshot()->digest();
    CCAP_CHECK_EQ(engine->snapshot()->generation().value(), 2U);
  }  // the engine, and with it the writer lock, is released here

  CCAP_CHECK_OK(reopened, CoolingCapacityEngine::open(directory, read_options(clock)));
  const std::shared_ptr<const cooling_capacity::CoolingSnapshot> snapshot = reopened->snapshot();
  CCAP_CHECK_EQ(snapshot->generation().value(), 2U);
  CCAP_CHECK_EQ(snapshot->digest(), digest);
  CCAP_CHECK_EQ(snapshot->record_count(), 6U);
  CCAP_CHECK(snapshot->find_zone(zone_id("hall-1")) != nullptr);
  CCAP_CHECK_OK(zone, reopened->zone_capacity(zone_id("hall-1"), CoolingMedium::Air,
                                              CompatibilityClass::AirConvection));
  CCAP_CHECK_EQ(zone.offered.amount_or_zero().milliwatts(), 400000000);
}

CCAP_TEST(a_reopened_snapshot_is_recovered_not_fresh) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-persistence-basis"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const ManualClock clock(fixture_now());
  {
    CCAP_CHECK_OK(engine, CoolingCapacityEngine::open(directory, write_options(clock)));
    publish_facility(engine);
    CCAP_CHECK_EQ(engine->basis(), cooling_capacity::AnalysisBasis::Fresh);
  }
  CCAP_CHECK_OK(reopened, CoolingCapacityEngine::open(directory, read_options(clock)));
  CCAP_CHECK_EQ(reopened->snapshot()->origin(), SnapshotOrigin::RecoveredFromStore);
  CCAP_CHECK_EQ(reopened->basis(), cooling_capacity::AnalysisBasis::RecoveredPendingRevalidation);

  cooling_capacity::CandidateLoad load;
  load.zone = zone_id("hall-1");
  load.medium = CoolingMedium::Air;
  load.compatibility = CompatibilityClass::AirConvection;
  load.thermal = milliwatts(1000);
  load.actor = actor_id("operator");
  CCAP_CHECK_OK(evaluation, reopened->evaluate(load));
  CCAP_CHECK_EQ(evaluation.disposition, cooling_capacity::CandidateDisposition::Indeterminate);
  CCAP_CHECK_EQ(evaluation.reason, cooling_capacity::CandidateReason::RecoveredPendingRevalidation);

  CCAP_CHECK_OK(generation, reopened->reload());
  CCAP_CHECK_EQ(generation.value(), 2U);
}

CCAP_TEST(every_stored_generation_is_individually_readable) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-persistence-history"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const ManualClock clock(fixture_now());
  CCAP_CHECK_OK(engine, CoolingCapacityEngine::open(directory, write_options(clock)));
  publish_facility(engine);
  for (int round = 0; round < 3; ++round) {
    CCAP_CHECK_OK(builder, engine->begin_mutation());
    EquipmentRecord unit = make_unit("crah-a", EquipmentKind::Crah,
                                     400000 + (round * 10000));
    CCAP_CHECK_VOID(builder.replace(unit));
    CCAP_CHECK_OK(receipt, engine->publish(builder));
    (void)receipt;
  }
  CCAP_CHECK_EQ(engine->snapshot()->generation().value(), 5U);

  CCAP_CHECK_OK(generations, engine->generations());
  CCAP_CHECK_EQ(generations.size(), 5U);
  for (const cooling_capacity::GenerationInfo& info : generations) {
    CCAP_CHECK(info.readable);
    CCAP_CHECK_OK(loaded, engine->load_generation(info.generation));
    CCAP_CHECK_EQ(loaded->generation(), info.generation);
  }
}

CCAP_TEST(the_store_verifies_every_generation_it_holds) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-persistence-verify"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const ManualClock clock(fixture_now());
  CCAP_CHECK_OK(engine, CoolingCapacityEngine::open(directory, write_options(clock)));
  publish_facility(engine);

  StoreOpenOptions options;
  options.access = StoreAccess::ReadOnly;
  CCAP_CHECK_OK(store, CoolingCapacityStore::open(directory, options));
  CCAP_CHECK_OK(verification, store->verify());
  CCAP_CHECK(verification.ok);
  CCAP_CHECK(verification.head_readable);
  CCAP_CHECK_EQ(verification.unreadable_generations, 0U);
  CCAP_CHECK_EQ(verification.stray_files, 0U);
  CCAP_CHECK_EQ(verification.generations.size(), 2U);
  CCAP_CHECK(verification.validation.ok());
}

CCAP_TEST(a_missing_store_directory_is_not_created_implicitly) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-persistence-missing"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  StoreOpenOptions options;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = false;
  const std::string absent = directory + "-absent";
  CCAP_CHECK_VOID(remove_tree(absent));
  CCAP_CHECK_ERR(CoolingCapacityStore::open(absent, options), ErrorCode::NotFound);
  CCAP_CHECK_FALSE(file_exists(absent));
}

CCAP_TEST(retiring_a_generation_removes_only_that_generation) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-persistence-retire"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const ManualClock clock(fixture_now());
  CCAP_CHECK_OK(engine, CoolingCapacityEngine::open(directory, write_options(clock)));
  publish_facility(engine);
  // The engine holds the process-wide writer lock, so it has to yield it before
  // another store object can take authority over the same directory.
  CCAP_CHECK_VOID(engine->release_writer());

  StoreOpenOptions options;
  options.access = StoreAccess::ReadWrite;
  CCAP_CHECK_OK(store, CoolingCapacityStore::open(directory, options));
  CCAP_CHECK_OK(epoch, store->acquire_writer(actor_id("operator")));
  (void)epoch;
  CCAP_CHECK_ERR(store->retire(CapacityGeneration::from_value(2)), ErrorCode::PreconditionFailed);
  CCAP_CHECK_VOID(store->retire(CapacityGeneration::first()));
  CCAP_CHECK_OK(generations, store->generations());
  CCAP_CHECK_EQ(generations.size(), 1U);
  CCAP_CHECK_EQ(generations[0].generation.value(), 2U);
  CCAP_CHECK_ERR(store->retire(CapacityGeneration::from_value(9)), ErrorCode::NotFound);
}

CCAP_TEST(the_retention_window_bounds_how_many_generations_are_kept) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-persistence-retention"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const ManualClock clock(fixture_now());
  EngineOptions options = write_options(clock);
  options.store.limits.max_generations_retained = 3;
  CCAP_CHECK_OK(engine, CoolingCapacityEngine::open(directory, options));
  publish_facility(engine);
  for (int round = 0; round < 5; ++round) {
    CCAP_CHECK_OK(builder, engine->begin_mutation());
    EquipmentRecord unit = make_unit("crah-a", EquipmentKind::Crah, 400000 + (round * 1000));
    CCAP_CHECK_VOID(builder.replace(unit));
    CCAP_CHECK_OK(receipt, engine->publish(builder));
    (void)receipt;
  }
  CCAP_CHECK_OK(generations, engine->generations());
  CCAP_CHECK(generations.size() <= 3U);
  CCAP_CHECK_EQ(engine->snapshot()->generation().value(), 7U);
  CCAP_CHECK_OK(head, engine->load_generation(CapacityGeneration::from_value(7)));
}

CCAP_TEST(a_store_whose_manifest_is_damaged_refuses_to_open_by_default) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-persistence-manifest"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const ManualClock clock(fixture_now());
  {
    CCAP_CHECK_OK(engine, CoolingCapacityEngine::open(directory, write_options(clock)));
    publish_facility(engine);
  }
  const std::string manifest = directory + "/MANIFEST";
  CCAP_CHECK(file_exists(manifest));
  const Result<std::string> contents = read_text(manifest);
  CCAP_CHECK(contents.ok());
  std::string damaged = contents.value();
  damaged[40] = static_cast<char>(damaged[40] ^ 0xFF);
  CCAP_CHECK_VOID(write_text(manifest, damaged));

  StoreOpenOptions strict;
  strict.access = StoreAccess::ReadOnly;
  CCAP_CHECK_ERR(CoolingCapacityStore::open(directory, strict), ErrorCode::DigestMismatch);

  // Explicit recovery reconstructs the head from the highest generation that
  // verifies end to end, and says so.
  StoreOpenOptions recovering;
  recovering.access = StoreAccess::ReadWrite;
  recovering.head_recovery = HeadRecovery::ReconstructFromGenerations;
  CCAP_CHECK_OK(store, CoolingCapacityStore::open(directory, recovering));
  CCAP_CHECK(store->recovery().head_reconstructed);
  CCAP_CHECK_FALSE(store->recovery().warnings.empty());
  CCAP_CHECK_OK(head, store->head_generation());
  CCAP_CHECK_EQ(head.value(), 2U);
  CCAP_CHECK_OK(loaded, store->load_head());
  CCAP_CHECK_EQ(loaded->generation().value(), 2U);
}

CCAP_TEST(staging_files_left_by_an_interrupted_commit_are_removed_on_open) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-persistence-staging"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const ManualClock clock(fixture_now());
  {
    CCAP_CHECK_OK(engine, CoolingCapacityEngine::open(directory, write_options(clock)));
    publish_facility(engine);
  }
  CCAP_CHECK_VOID(write_text(directory + "/staging-9-9.tmp", "leftover"));
  CCAP_CHECK(file_exists(directory + "/staging-9-9.tmp"));

  StoreOpenOptions options;
  options.access = StoreAccess::ReadWrite;
  CCAP_CHECK_OK(store, CoolingCapacityStore::open(directory, options));
  CCAP_CHECK_FALSE(file_exists(directory + "/staging-9-9.tmp"));
  CCAP_CHECK_EQ(store->recovery().removed_staging_files.size(), 1U);
}

CCAP_TEST(a_published_but_unreferenced_generation_stays_readable_and_harmless) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-persistence-orphan"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const ManualClock clock(fixture_now());
  CCAP_CHECK_OK(engine, CoolingCapacityEngine::open(directory, write_options(clock)));
  publish_facility(engine);

  // A second handle opened after the commit sees the same head, and a
  // generation number that was never published is a plain not-found.
  StoreOpenOptions options;
  options.access = StoreAccess::ReadOnly;
  CCAP_CHECK_OK(store, CoolingCapacityStore::open(directory, options));
  CCAP_CHECK_OK(head, store->head_generation());
  CCAP_CHECK_EQ(head.value(), 2U);
  CCAP_CHECK_OK(loaded, store->load_generation(CapacityGeneration::from_value(2)));
  CCAP_CHECK_EQ(loaded->record_count(), 6U);
  CCAP_CHECK_ERR(store->load_generation(CapacityGeneration::from_value(99)),
                 ErrorCode::NotFound);
}