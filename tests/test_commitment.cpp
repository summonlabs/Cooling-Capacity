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

using cooling_capacity::AttemptId;
using cooling_capacity::CommitmentId;
using cooling_capacity::CommitmentOperation;
using cooling_capacity::CommitmentOutcome;
using cooling_capacity::CommitmentRequest;
using cooling_capacity::CommitmentState;
using cooling_capacity::CoolingCapacityEngine;
using cooling_capacity::CoolingMedium;
using cooling_capacity::EngineOptions;
using cooling_capacity::ErrorCode;
using cooling_capacity::ManualClock;

struct Harness {
  ~Harness() {
    engine.reset();
    const Result<void> removed = remove_tree(directory);
    (void)removed;
  }
  std::string directory;
  ManualClock clock{fixture_now()};
  EngineOptions options;
  std::unique_ptr<CoolingCapacityEngine> engine;
};

std::unique_ptr<Harness> open_harness(const std::string& name) {
  auto harness = std::make_unique<Harness>();
  const Result<std::string> directory = fresh_directory(name);
  if (!directory.ok()) {
    std::fprintf(stderr, "could not prepare the store directory\n");
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
    std::fprintf(stderr, "could not open the engine: %s\n", engine.error().to_string().c_str());
    std::abort();
  }
  harness->engine = std::move(engine).value();

  Result<cooling_capacity::SnapshotBuilder> builder = harness->engine->begin_mutation();
  if (!builder.ok()) {
    std::abort();
  }
  const SimpleFacility facility = simple_facility();
  if (!upsert_simple_facility(builder.value(), facility).ok()) {
    std::abort();
  }
  if (!harness->engine->publish(builder.value()).ok()) {
    std::abort();
  }
  return harness;
}

CommitmentRequest make_request(CommitmentOperation operation, const char* commitment,
                               const char* attempt, std::int64_t thermal_milliwatts,
                               const CoolingCapacityEngine& engine) {
  CommitmentRequest request;
  request.operation = operation;
  request.id = CommitmentId::literal(commitment);
  request.attempt = AttemptId::literal(attempt);
  request.zone = zone_id("hall-1");
  request.medium = CoolingMedium::Air;
  request.compatibility = CompatibilityClass::AirConvection;
  request.thermal = milliwatts(thermal_milliwatts);
  request.actor = actor_id("operator");
  request.requested_at = fixture_now();
  request.expected_generation = engine.snapshot()->generation();
  return request;
}

}  // namespace

CCAP_TEST(a_commitment_moves_through_plan_hold_and_commit) {
  const std::unique_ptr<Harness> harness = open_harness("ccap-test-commitment");
  CCAP_CHECK(harness->engine->holds_writer());

  // Each request carries its own attempt identifier: an attempt names one
  // request, so the three steps of the lifecycle are three attempts against one
  // commitment. Reusing an attempt for a different request is a conflict, which
  // the next test covers.
  CCAP_CHECK_OK(planned, harness->engine->apply(make_request(CommitmentOperation::Plan, "c-1",
                                                             "a-plan", 100000000,
                                                             *harness->engine)));
  CCAP_CHECK_EQ(planned.state, CommitmentState::Planned);
  CCAP_CHECK_EQ(planned.committed_after.milliwatts(), 0);
  CCAP_CHECK_FALSE(planned.replayed);

  CCAP_CHECK_OK(held, harness->engine->apply(make_request(CommitmentOperation::Hold, "c-1",
                                                          "a-hold", 100000000, *harness->engine)));
  CCAP_CHECK_EQ(held.state, CommitmentState::Held);
  CCAP_CHECK_EQ(held.committed_after.milliwatts(), 100000000);
  CCAP_CHECK_EQ(held.reserve_after.milliwatts(), 300000000);

  CCAP_CHECK_OK(committed, harness->engine->apply(make_request(CommitmentOperation::Commit, "c-1",
                                                               "a-commit", 100000000,
                                                               *harness->engine)));
  CCAP_CHECK_EQ(committed.state, CommitmentState::Committed);
  CCAP_CHECK_EQ(committed.reserve_after.milliwatts(), 300000000);

}

CCAP_TEST(a_retried_attempt_replays_instead_of_committing_twice) {
  const std::unique_ptr<Harness> harness = open_harness("ccap-test-replay");
  const CommitmentRequest request = make_request(CommitmentOperation::Commit, "c-1", "a-1",
                                                 100000000, *harness->engine);
  CCAP_CHECK_OK(first, harness->engine->apply(request));
  CCAP_CHECK_FALSE(first.replayed);

  CommitmentRequest retry = request;
  retry.expected_generation = harness->engine->snapshot()->generation();
  CCAP_CHECK_OK(second, harness->engine->apply(retry));
  CCAP_CHECK(second.replayed);
  CCAP_CHECK_EQ(second.id.str(), std::string("c-1"));
  CCAP_CHECK_EQ(second.committed_after.milliwatts(), 100000000);
  CCAP_CHECK_EQ(harness->engine->snapshot()->generation().value(), first.generation.value());

}

CCAP_TEST(the_same_attempt_with_different_content_is_a_conflict) {
  const std::unique_ptr<Harness> harness = open_harness("ccap-test-idempotency");
  CCAP_CHECK_OK(first, harness->engine->apply(make_request(CommitmentOperation::Commit, "c-1",
                                                           "a-1", 100000000,
                                                           *harness->engine)));
  (void)first;
  CommitmentRequest different = make_request(CommitmentOperation::Commit, "c-2", "a-1",
                                             50000000, *harness->engine);
  CCAP_CHECK_ERR(harness->engine->apply(different), ErrorCode::IdempotencyConflict);
  CCAP_CHECK_EQ(harness->engine->snapshot()->commitments().size(), 1U);

}

CCAP_TEST(a_request_formed_against_an_older_generation_is_refused) {
  const std::unique_ptr<Harness> harness = open_harness("ccap-test-stale");
  CommitmentRequest request = make_request(CommitmentOperation::Commit, "c-1", "a-1", 100000000,
                                           *harness->engine);
  const CapacityGeneration formed_against = request.expected_generation;
  CCAP_CHECK_OK(first, harness->engine->apply(request));
  CCAP_CHECK(first.generation != formed_against);

  // The same request, formed again against the generation it was originally
  // written for, is refused rather than merged into the current one.
  CommitmentRequest stale = make_request(CommitmentOperation::Commit, "c-2", "a-2", 100000000,
                                         *harness->engine);
  stale.expected_generation = formed_against;
  CCAP_CHECK_ERR(harness->engine->apply(stale), ErrorCode::StaleGeneration);
  CCAP_CHECK_EQ(harness->engine->snapshot()->commitments().size(), 1U);

}

CCAP_TEST(a_load_that_does_not_fit_is_refused_before_it_becomes_authoritative) {
  const std::unique_ptr<Harness> harness = open_harness("ccap-test-capacity");
  CCAP_CHECK_ERR(harness->engine->apply(make_request(CommitmentOperation::Commit, "c-1", "a-1",
                                                     500000000, *harness->engine)),
                 ErrorCode::CapacityExceeded);
  CCAP_CHECK_EQ(harness->engine->snapshot()->commitments().size(), 0U);

}

CCAP_TEST(a_released_commitment_stops_consuming_capacity) {
  const std::unique_ptr<Harness> harness = open_harness("ccap-test-release");
  CCAP_CHECK_OK(committed, harness->engine->apply(make_request(CommitmentOperation::Commit, "c-1",
                                                               "a-1", 100000000,
                                                               *harness->engine)));
  CCAP_CHECK_EQ(committed.committed_after.milliwatts(), 100000000);

  CommitmentRequest release;
  release.operation = CommitmentOperation::Release;
  release.id = CommitmentId::literal("c-1");
  release.attempt = AttemptId::literal("a-2");
  release.actor = actor_id("operator");
  release.requested_at = fixture_now();
  release.expected_generation = harness->engine->snapshot()->generation();
  release.target = CommitmentId::literal("c-1");
  release.zone = zone_id("hall-1");
  release.medium = CoolingMedium::Air;
  release.compatibility = CompatibilityClass::AirConvection;
  CCAP_CHECK_OK(outcome, harness->engine->apply(release));
  CCAP_CHECK_EQ(outcome.state, CommitmentState::Released);
  CCAP_CHECK_EQ(outcome.committed_after.milliwatts(), 0);
  CCAP_CHECK_EQ(outcome.reserve_after.milliwatts(), 400000000);

}

CCAP_TEST(releasing_a_missing_commitment_is_not_found) {
  const std::unique_ptr<Harness> harness = open_harness("ccap-test-release-missing");
  CommitmentRequest release;
  release.operation = CommitmentOperation::Release;
  release.id = CommitmentId::literal("c-9");
  release.attempt = AttemptId::literal("a-9");
  release.actor = actor_id("operator");
  release.requested_at = fixture_now();
  release.expected_generation = harness->engine->snapshot()->generation();
  release.target = CommitmentId::literal("c-9");
  release.zone = zone_id("hall-1");
  release.medium = CoolingMedium::Air;
  release.compatibility = CompatibilityClass::AirConvection;
  CCAP_CHECK_ERR(harness->engine->apply(release), ErrorCode::NotFound);
}

CCAP_TEST(a_request_without_a_generation_precondition_is_refused) {
  const std::unique_ptr<Harness> harness = open_harness("ccap-test-precondition");
  CommitmentRequest request = make_request(CommitmentOperation::Commit, "c-1", "a-1", 1000000,
                                           *harness->engine);
  request.expected_generation = CapacityGeneration::from_value(0);
  CCAP_CHECK_ERR(harness->engine->apply(request), ErrorCode::InvalidArgument);
}