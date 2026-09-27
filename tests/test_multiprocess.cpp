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

using cooling_capacity::CoolingCapacityStore;
using cooling_capacity::ErrorCode;
using cooling_capacity::StoreAccess;
using cooling_capacity::StoreOpenOptions;

StoreOpenOptions writable() {
  StoreOpenOptions options;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  return options;
}

StoreOpenOptions read_only() {
  StoreOpenOptions options;
  options.access = StoreAccess::ReadOnly;
  return options;
}

}  // namespace

// Writer authority is an operating-system lock, so two independent processes
// genuinely contend for it. Neither side is a thread standing in for a process.
CCAP_TEST(a_second_process_is_refused_while_the_first_holds_writer_authority) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-multiprocess-refusal"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  CCAP_CHECK_OK(store, CoolingCapacityStore::open(directory, writable()));
  CCAP_CHECK_OK(epoch, store->acquire_writer(actor_id("parent")));
  CCAP_CHECK_EQ(epoch.value(), 1U);

  const std::string output = directory + "-probe.txt";
  CCAP_CHECK_VOID(remove_path(output));
  CCAP_CHECK_OK(child, spawn({"--child", "try-lock", directory, output}));
  CCAP_CHECK_OK(code, wait_for_exit(child, 60000U));
  CCAP_CHECK_EQ(code, 0UL);
  CCAP_CHECK_OK(report, read_text(output));
  CCAP_CHECK_EQ(report, std::string("refused lock_conflict\n"));
  CCAP_CHECK_VOID(store->release_writer());
}

CCAP_TEST(a_child_process_can_take_authority_once_the_parent_releases_it) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-multiprocess-handover"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  CCAP_CHECK_OK(store, CoolingCapacityStore::open(directory, writable()));
  CCAP_CHECK_OK(epoch, store->acquire_writer(actor_id("parent")));
  CCAP_CHECK_EQ(epoch.value(), 1U);
  CCAP_CHECK_VOID(store->release_writer());

  const std::string output = directory + "-probe.txt";
  CCAP_CHECK_VOID(remove_path(output));
  CCAP_CHECK_OK(child, spawn({"--child", "try-lock", directory, output}));
  CCAP_CHECK_OK(code, wait_for_exit(child, 60000U));
  CCAP_CHECK_EQ(code, 0UL);
  CCAP_CHECK_OK(report, read_text(output));
  CCAP_CHECK_EQ(report, std::string("acquired 2\n"));

  // A second child takes authority afterwards and advances the epoch again.
  CCAP_CHECK_VOID(remove_path(directory + "-probe2.txt"));
  CCAP_CHECK_OK(child_two, spawn({"--child", "try-lock", directory,
                                  directory + "-probe2.txt"}));
  CCAP_CHECK_OK(code_two, wait_for_exit(child_two, 60000U));
  CCAP_CHECK_EQ(code_two, 0UL);
  CCAP_CHECK_OK(report_two, read_text(directory + "-probe2.txt"));
  CCAP_CHECK_EQ(report_two, std::string("acquired 3\n"));
  CCAP_CHECK_OK(epoch_after, store->acquire_writer(actor_id("parent")));
  CCAP_CHECK(epoch_after.value() >= 4U);
  CCAP_CHECK_VOID(store->release_writer());
}

CCAP_TEST(a_holding_process_blocks_the_parent_and_yields_when_it_exits) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-multiprocess-hold"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const std::string ready = directory + "-ready.txt";
  const std::string stop = directory + "-stop.txt";
  CCAP_CHECK_VOID(write_text(stop, ""));  // placeholder, removed below

  // A stale coordination file from an earlier run would let the wait below
  // succeed before the child has done anything, so both are cleared first.
  CCAP_CHECK_VOID(remove_path(ready));
  CCAP_CHECK_VOID(remove_path(stop));
  CCAP_CHECK_OK(child, spawn({"--child", "hold-lock", directory, ready, stop}));
  CCAP_CHECK_VOID(wait_for_file(ready, 60000U));

  CCAP_CHECK_OK(probe, CoolingCapacityStore::open(directory, writable()));
  CCAP_CHECK_ERR(probe->acquire_writer(actor_id("parent")), ErrorCode::LockConflict);

  CCAP_CHECK_VOID(write_text(stop, "stop\n"));
  CCAP_CHECK_OK(code, wait_for_exit(child, 60000U));
  CCAP_CHECK_EQ(code, 0UL);

  CCAP_CHECK_OK(epoch, probe->acquire_writer(actor_id("parent")));
  CCAP_CHECK(epoch.value() >= 2U);
  CCAP_CHECK_VOID(probe->release_writer());
}

// Proves that process death relinquishes authority: the operating system drops
// the lock when the holder is killed, and the epoch advances so that nothing the
// dead process might still believe can be committed.
CCAP_TEST(killing_the_holder_relinquishes_authority) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-multiprocess-death"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const std::string ready = directory + "-ready.txt";
  const std::string stop = directory + "-stop.txt";
  // A stale coordination file from an earlier run would let the wait below
  // succeed before the child has done anything, so both are cleared first.
  CCAP_CHECK_VOID(remove_path(ready));
  CCAP_CHECK_VOID(remove_path(stop));
  CCAP_CHECK_OK(child, spawn({"--child", "hold-lock", directory, ready, stop}));
  CCAP_CHECK_VOID(wait_for_file(ready, 60000U));

  CCAP_CHECK_OK(probe, CoolingCapacityStore::open(directory, writable()));
  CCAP_CHECK_ERR(probe->acquire_writer(actor_id("parent")), ErrorCode::LockConflict);

  CCAP_CHECK_OK(code, terminate(child));
  CCAP_CHECK_NE(code, 0UL);

  CCAP_CHECK_OK(epoch, probe->acquire_writer(actor_id("parent")));
  CCAP_CHECK(epoch.value() >= 2U);
  CCAP_CHECK_VOID(probe->release_writer());

  // The store is still whole after the kill.
  CCAP_CHECK_OK(verification, probe->verify());
  if (!verification.ok) {
    std::fprintf(stderr, "%s\n", verification.render().c_str());
  }
  CCAP_CHECK(verification.ok);
}

CCAP_TEST(a_read_only_handle_is_never_granted_writer_authority) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-multiprocess-readonly"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  CCAP_CHECK_OK(store, CoolingCapacityStore::open(directory, writable()));
  CCAP_CHECK_VOID(store->close());
  CCAP_CHECK_OK(reader, CoolingCapacityStore::open(directory, read_only()));
  CCAP_CHECK_ERR(reader->acquire_writer(actor_id("reader")), ErrorCode::ReadOnly);
}