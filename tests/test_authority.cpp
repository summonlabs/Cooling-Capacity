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
#include <thread>
#include <vector>

using namespace ccap_test;

namespace {

using cooling_capacity::CoolingCapacityEngine;
using cooling_capacity::CoolingCapacityStore;
using cooling_capacity::CoolingMedium;
using cooling_capacity::EngineOptions;
using cooling_capacity::ErrorCode;
using cooling_capacity::GenerationInfo;
using cooling_capacity::HeadRecovery;
using cooling_capacity::ManualClock;
using cooling_capacity::SnapshotBuilder;
using cooling_capacity::StoreAccess;
using cooling_capacity::StoreOpenOptions;
using cooling_capacity::WriterEpoch;

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

}  // namespace

CCAP_TEST(an_empty_store_starts_at_generation_one) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-authority-empty"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const ManualClock clock(fixture_now());
  CCAP_CHECK_OK(engine, CoolingCapacityEngine::open(directory, write_options(clock)));
  CCAP_CHECK_EQ(engine->snapshot()->generation(), CapacityGeneration::first());
  CCAP_CHECK_EQ(engine->snapshot()->record_count(), 0U);
  CCAP_CHECK(engine->holds_writer());
}

CCAP_TEST(a_read_only_engine_cannot_write) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-authority-readonly"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const ManualClock clock(fixture_now());
  {
    CCAP_CHECK_OK(engine, CoolingCapacityEngine::open(directory, write_options(clock)));
    CCAP_CHECK_OK(builder, engine->begin_mutation());
    const SimpleFacility facility = simple_facility();
    CCAP_CHECK_VOID(add_simple_facility(builder, facility));
    CCAP_CHECK_OK(receipt, engine->publish(builder));
    CCAP_CHECK_EQ(receipt.generation.value(), 2U);
  }
  CCAP_CHECK_OK(reader, CoolingCapacityEngine::open(directory, read_options(clock)));
  CCAP_CHECK_FALSE(reader->holds_writer());
  // Reloading is a read and stays available; publishing is not.
  CCAP_CHECK_OK(reloaded, reader->reload());
  CCAP_CHECK_EQ(reloaded.value(), 2U);
  SnapshotBuilder read_only_builder;
  read_only_builder.set_generation(CapacityGeneration::from_value(9));
  CCAP_CHECK_ERR(reader->publish(read_only_builder), ErrorCode::ReadOnly);
}

CCAP_TEST(a_second_writer_is_refused_while_the_first_holds_authority) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-authority-contention"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const ManualClock clock(fixture_now());
  CCAP_CHECK_OK(first, CoolingCapacityEngine::open(directory, write_options(clock)));
  CCAP_CHECK(first->holds_writer());
  CCAP_CHECK_ERR(CoolingCapacityEngine::open(directory, write_options(clock)),
                 ErrorCode::LockConflict);
  CCAP_CHECK(first->holds_writer());

  // Releasing authority lets the next writer in, with a higher epoch.
  CCAP_CHECK_OK(epoch_before, first->acquire_writer());
  CCAP_CHECK_VOID(first->release_writer());
  CCAP_CHECK_OK(second, CoolingCapacityEngine::open(directory, write_options(clock)));
  CCAP_CHECK(second->holds_writer());
  CCAP_CHECK_OK(store, CoolingCapacityStore::open(directory, StoreOpenOptions{}));
  CCAP_CHECK_OK(epoch_after, store->writer_epoch());
  CCAP_CHECK(epoch_before < epoch_after);
}

CCAP_TEST(a_generation_commit_advances_the_head_and_the_epoch_stamps_it) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-authority-commit"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const ManualClock clock(fixture_now());
  CCAP_CHECK_OK(engine, CoolingCapacityEngine::open(directory, write_options(clock)));
  CCAP_CHECK_OK(builder, engine->begin_mutation());
  const SimpleFacility facility = simple_facility();
  CCAP_CHECK_VOID(add_simple_facility(builder, facility));
  CCAP_CHECK_OK(receipt, engine->publish(builder));
  CCAP_CHECK_OK(held_epoch, engine->acquire_writer());
  CCAP_CHECK_EQ(receipt.epoch, held_epoch);
  CCAP_CHECK(receipt.head_replaced);

  CCAP_CHECK_OK(store, CoolingCapacityStore::open(directory, StoreOpenOptions{}));
  CCAP_CHECK_OK(head, store->head_generation());
  CCAP_CHECK_EQ(head, receipt.generation);
  CCAP_CHECK_OK(generations, store->generations());
  CCAP_CHECK_EQ(generations.size(), 2U);
  CCAP_CHECK(generations[1].is_head);
  CCAP_CHECK(generations[0].readable);
}

CCAP_TEST(a_commit_whose_precondition_moved_is_refused_and_writes_nothing) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-authority-precondition"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const ManualClock clock(fixture_now());
  CCAP_CHECK_OK(engine, CoolingCapacityEngine::open(directory, write_options(clock)));
  CCAP_CHECK_OK(builder, engine->begin_mutation());
  const SimpleFacility facility = simple_facility();
  CCAP_CHECK_VOID(add_simple_facility(builder, facility));
  CCAP_CHECK_OK(receipt, engine->publish(builder));

  // The engine holds the process-wide writer lock, so it has to yield it before
  // another store object can take authority over the same directory.
  CCAP_CHECK_VOID(engine->release_writer());
  CCAP_CHECK_OK(store, CoolingCapacityStore::open(directory, StoreOpenOptions{}));
  const std::shared_ptr<const cooling_capacity::CoolingSnapshot> snapshot = engine->snapshot();
  cooling_capacity::CommitOptions options;
  options.expected_generation = CapacityGeneration::first();
  options.committed_at = fixture_now();
  CCAP_CHECK_ERR(store->commit(*snapshot, options), ErrorCode::WriterBusy);

  CCAP_CHECK_OK(writer_epoch, store->acquire_writer(actor_id("other")));
  (void)writer_epoch;
  cooling_capacity::CommitOptions stale;
  stale.expected_generation = CapacityGeneration::first();
  stale.committed_at = fixture_now();
  CCAP_CHECK_ERR(store->commit(*snapshot, stale), ErrorCode::StaleGeneration);
  CCAP_CHECK_OK(head, store->head_generation());
  CCAP_CHECK_EQ(head, receipt.generation);
}

CCAP_TEST(revalidation_produces_a_new_generation_and_a_revalidated_basis) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-authority-revalidate"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  ManualClock clock(fixture_now());
  CCAP_CHECK_OK(engine, CoolingCapacityEngine::open(directory, write_options(clock)));
  CCAP_CHECK_OK(builder, engine->begin_mutation());
  const SimpleFacility facility = simple_facility();
  CCAP_CHECK_VOID(add_simple_facility(builder, facility));
  CCAP_CHECK_OK(receipt, engine->publish(builder));

  clock.advance(cooling_capacity::DurationMs::from_milliseconds(1000));
  CCAP_CHECK_OK(generation, engine->revalidate(clock));
  CCAP_CHECK_EQ(generation.value(), receipt.generation.value() + 1U);
  CCAP_CHECK_EQ(engine->basis(), cooling_capacity::AnalysisBasis::Revalidated);
  CCAP_CHECK(engine->snapshot()->revalidated_at().has_value());
}

CCAP_TEST(a_key_that_cannot_be_acquired_is_reported_as_a_lock_conflict) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-authority-lock"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  StoreOpenOptions options;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  CCAP_CHECK_OK(first, CoolingCapacityStore::open(directory, options));
  CCAP_CHECK_OK(second, CoolingCapacityStore::open(directory, options));
  CCAP_CHECK_OK(epoch, first->acquire_writer(actor_id("a")));
  CCAP_CHECK_EQ(epoch.value(), 1U);
  CCAP_CHECK_ERR(second->acquire_writer(actor_id("b")), ErrorCode::LockConflict);
  CCAP_CHECK_VOID(first->release_writer());
  CCAP_CHECK_OK(second_epoch, second->acquire_writer(actor_id("b")));
  CCAP_CHECK_EQ(second_epoch.value(), 2U);
}