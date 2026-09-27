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

#include <chrono>
#include <memory>
#include <thread>

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

// Waits until the marker file reports at least `target` published generations.
// Exhausting the budget is a failure, never a pass.
Result<void> wait_for_publications(const std::string& marker, unsigned long target) {
  const auto start = std::chrono::steady_clock::now();
  while (true) {
    if (file_exists(marker)) {
      const Result<std::string> text = read_text(marker);
      if (text.ok()) {
        std::string trimmed = text.value();
        while (!trimmed.empty() &&
               (trimmed.back() == '\n' || trimmed.back() == '\r' || trimmed.back() == ' ')) {
          trimmed.pop_back();
        }
        const Result<std::int64_t> count = cooling_capacity::parse_int64(trimmed);
        if (count.ok() && count.value() >= static_cast<std::int64_t>(target)) {
          return Result<void>();
        }
      }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    if (elapsed > 60000) {
      return cooling_capacity::Error(ErrorCode::ProcessFailure,
                                     "the child did not publish enough generations in time")
          .with("marker", marker)
          .with("target", std::to_string(target));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

}  // namespace

// A writer killed in the middle of its work must leave exactly one whole
// authoritative state behind: a head that reads, every generation that reads,
// and no staging residue.
CCAP_TEST(a_writer_killed_during_commit_leaves_a_whole_store) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-crash"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const std::string marker = directory + "-marker.txt";
  for (int round = 0; round < 3; ++round) {
    CCAP_CHECK_VOID(remove_path(marker));
    CCAP_CHECK_OK(child, spawn({"--child", "commit-loop", directory, marker}));
    const Result<void> published_enough = wait_for_publications(marker, 5U);
    if (!published_enough.ok()) {
      // The child failing early is the usual cause, so its exit code is
      // reported rather than leaving the failure unexplained.
      CCAP_CHECK_OK(child_code, wait_for_exit(child, 60000U));
      std::fprintf(stderr, "commit-loop child exited with code %lu\n", child_code);
    }
    CCAP_CHECK_VOID(published_enough);
    CCAP_CHECK_OK(code, terminate(child));
    CCAP_CHECK_NE(code, 0UL);

    CCAP_CHECK_OK(store, CoolingCapacityStore::open(directory, writable()));
    CCAP_CHECK_OK(verification, store->verify());
    if (!verification.ok) {
      std::fprintf(stderr, "%s\n", verification.render().c_str());
    }
    CCAP_CHECK(verification.ok);
    CCAP_CHECK(verification.head_readable);
    CCAP_CHECK_OK(head, store->head_generation());
    CCAP_CHECK(head.value() >= 5U);
    CCAP_CHECK_OK(loaded, store->load_head());
    CCAP_CHECK_EQ(loaded->generation(), head);
    CCAP_CHECK(loaded->find_zone(zone_id("hall-1")) == nullptr ||
               loaded->find_equipment(equipment_id("crah-a")) != nullptr);
    CCAP_CHECK_VOID(store->close());
  }
  CCAP_CHECK_VOID(remove_path(marker));
}

CCAP_TEST(a_truncated_generation_file_is_refused_rather_than_half_read) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-crash-truncated"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const std::string marker = directory + "-marker.txt";
  CCAP_CHECK_OK(child, spawn({"--child", "commit-loop", directory, marker}));
  CCAP_CHECK_VOID(wait_for_publications(marker, 3U));
  CCAP_CHECK_OK(code, terminate(child));
  CCAP_CHECK_NE(code, 0UL);

  // Chop the tail off the highest generation file, leaving the manifest
  // pointing at a generation that can no longer be read in full.
  CCAP_CHECK_OK(store, CoolingCapacityStore::open(directory, writable()));
  CCAP_CHECK_OK(head, store->head_generation());
  CCAP_CHECK_VOID(store->close());
  const std::string path =
      directory + "/gen-" + std::string(20U - head.to_string().size(), '0') +
      head.to_string() + ".ccap";
  CCAP_CHECK(file_exists(path));
  CCAP_CHECK_OK(contents, read_text(path));
  CCAP_CHECK(contents.size() > 64U);
  CCAP_CHECK_VOID(write_text(path, contents.substr(0, contents.size() / 2U)));

  CCAP_CHECK_OK(damaged, CoolingCapacityStore::open(directory, writable()));
  CCAP_CHECK_ERR(damaged->load_head(), ErrorCode::Truncated);
  CCAP_CHECK_OK(verification, damaged->verify());
  CCAP_CHECK_FALSE(verification.ok);
  CCAP_CHECK_FALSE(verification.problems.empty());
}

CCAP_TEST(a_partial_staging_file_is_never_mistaken_for_a_generation) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-crash-partial"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  CCAP_CHECK_OK(store, CoolingCapacityStore::open(directory, writable()));
  CCAP_CHECK_OK(epoch, store->acquire_writer(actor_id("operator")));
  (void)epoch;
  // Write a plausible but incomplete artifact under a staging name.
  CCAP_CHECK_VOID(write_text(directory + "/staging-3-9.tmp", "CCAPGEN1 partial"));
  // It is not a generation and must not be adopted.
  CCAP_CHECK_ERR(store->load_head(), ErrorCode::NotFound);
  CCAP_CHECK_OK(generations, store->generations());
  CCAP_CHECK_EQ(generations.size(), 0U);
  CCAP_CHECK_VOID(store->close());

  CCAP_CHECK_OK(reopened, CoolingCapacityStore::open(directory, writable()));
  CCAP_CHECK_FALSE(file_exists(directory + "/staging-3-9.tmp"));
  CCAP_CHECK_EQ(reopened->recovery().removed_staging_files.size(), 1U);
  CCAP_CHECK_ERR(reopened->load_head(), ErrorCode::NotFound);
}

CCAP_TEST(a_generation_file_renamed_to_the_wrong_number_is_refused) {
  CCAP_CHECK_OK(directory, fresh_directory("ccap-test-crash-renamed"));
  const ::ccap_test::Cleanup cleanup_guard(directory);
  const ManualClock test_clock(fixture_now());
  {
    cooling_capacity::EngineOptions options;
    options.actor = actor_id("operator");
    options.clock = &test_clock;
    options.store.access = StoreAccess::ReadWrite;
    options.store.create_if_missing = true;
    CCAP_CHECK_OK(engine, cooling_capacity::CoolingCapacityEngine::open(directory, options));
    CCAP_CHECK_OK(builder, engine->begin_mutation());
    const SimpleFacility facility = simple_facility();
    CCAP_CHECK_VOID(add_simple_facility(builder, facility));
    CCAP_CHECK_OK(receipt, engine->publish(builder));
    (void)receipt;
  }
  const std::string source = directory + "/gen-00000000000000000002.ccap";
  const std::string target = directory + "/gen-00000000000000000007.ccap";
  CCAP_CHECK(file_exists(source));
  CCAP_CHECK_OK(contents, read_text(source));
  CCAP_CHECK_VOID(write_text(target, contents));

  CCAP_CHECK_OK(store, CoolingCapacityStore::open(directory, writable()));
  CCAP_CHECK_ERR(store->load_generation(cooling_capacity::CapacityGeneration::from_value(7)),
                 ErrorCode::Corruption);
  CCAP_CHECK_OK(generations, store->generations());
  // The two real generations plus the renamed copy.
  CCAP_CHECK_EQ(generations.size(), 3U);
  CCAP_CHECK(generations[0].readable);
  CCAP_CHECK(generations[1].readable);
  CCAP_CHECK_FALSE(generations[2].readable);
}