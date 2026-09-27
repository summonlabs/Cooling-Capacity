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
using cooling_capacity::CoolingMedium;
using cooling_capacity::EngineOptions;
using cooling_capacity::ManualClock;
using cooling_capacity::SnapshotBuilder;
using cooling_capacity::StoreAccess;

EngineOptions write_options(const ManualClock& clock) {
  EngineOptions options;
  options.actor = actor_id("operator");
  options.clock = &clock;
  options.store.access = StoreAccess::ReadWrite;
  options.store.create_if_missing = true;
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

// A real process restart: the parent writes and exits its engine, then a
// separate operating-system process opens the same store and reports what it
// finds. Nothing here is a thread pretending to be a process.
CCAP_TEST(a_separate_process_reads_the_generation_the_parent_committed) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-restart"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const ManualClock clock(fixture_now());
  cooling_capacity::Digest committed_digest;
  {
    CCAP_CHECK_OK(engine, CoolingCapacityEngine::open(directory, write_options(clock)));
    publish_facility(engine);
    committed_digest = engine->snapshot()->digest();
    CCAP_CHECK_EQ(engine->snapshot()->generation().value(), 2U);
  }

  const std::string output = directory + "/head.txt";
  CCAP_CHECK_VOID(remove_path(output));
  CCAP_CHECK_VOID(remove_path(output));
  CCAP_CHECK_OK(child, spawn({"--child", "read-head", directory, output}));
  CCAP_CHECK_OK(code, wait_for_exit(child, 60000U));
  CCAP_CHECK_EQ(code, 0UL);
  CCAP_CHECK_OK(report, read_text(output));
  const std::string expected = "2\n" + committed_digest.to_hex() + "\n";
  CCAP_CHECK_EQ(report, expected);
}

CCAP_TEST(a_restarted_writer_advances_the_epoch_and_keeps_the_history) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-restart-epoch"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const ManualClock clock(fixture_now());
  cooling_capacity::WriterEpoch first_epoch;
  {
    CCAP_CHECK_OK(engine, CoolingCapacityEngine::open(directory, write_options(clock)));
    CCAP_CHECK_OK(epoch, engine->acquire_writer());
    first_epoch = epoch;
    publish_facility(engine);
  }
  CCAP_CHECK_OK(second, CoolingCapacityEngine::open(directory, write_options(clock)));
  CCAP_CHECK_OK(second_epoch, second->acquire_writer());
  CCAP_CHECK(first_epoch < second_epoch);
  CCAP_CHECK_OK(generations, second->generations());
  CCAP_CHECK_EQ(generations.size(), 2U);
  CCAP_CHECK_EQ(second->snapshot()->generation().value(), 2U);

  // A fresh publish after the restart continues the sequence rather than
  // restarting it.
  publish_facility(second);
  CCAP_CHECK_EQ(second->snapshot()->generation().value(), 3U);
  CCAP_CHECK_OK(store, cooling_capacity::CoolingCapacityStore::open(directory, [] {
    cooling_capacity::StoreOpenOptions options;
    options.access = StoreAccess::ReadOnly;
    return options;
  }()));
  CCAP_CHECK_OK(verification, store->verify());
  CCAP_CHECK(verification.ok);
}

CCAP_TEST(a_child_process_reads_a_store_while_the_parent_holds_only_read_authority) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-restart-readonly"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const ManualClock clock(fixture_now());
  CCAP_CHECK_OK(engine, CoolingCapacityEngine::open(directory, write_options(clock)));
  publish_facility(engine);
  // The parent keeps the writer lock; a read-only child must still work.
  const std::string output = directory + "/head.txt";
  CCAP_CHECK_VOID(remove_path(output));
  CCAP_CHECK_OK(child, spawn({"--child", "read-head", directory, output}));
  CCAP_CHECK_OK(code, wait_for_exit(child, 60000U));
  CCAP_CHECK_EQ(code, 0UL);
  CCAP_CHECK_OK(report, read_text(output));
  CCAP_CHECK(report.find("2\n") == 0U);
}