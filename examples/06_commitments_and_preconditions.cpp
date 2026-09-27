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

// 06 - commitments and preconditions.
//
// A commitment is the authority to occupy cooling capacity. Only the held and
// committed states consume capacity; a plan consumes nothing, and release,
// expiry and supersession are terminal.
//
// The example walks one load through hold, commit and release in memory and then
// exercises the two preconditions every commitment request carries:
//
//   * idempotency is keyed on the attempt identifier, so a retried request whose
//     content and resulting state match what was already applied is a replay
//     that writes nothing, while a reused attempt with other content is a
//     conflict;
//   * a request names the generation it was formed against, and a request formed
//     against an older generation is refused rather than merged into the current
//     one.
//
// Both rules are the rules CoolingCapacityEngine::apply enforces. The engine
// needs a durable store, so this example states the rules against hand-built
// generations instead and prints exactly what the engine would decide.

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
constexpr Milliwatts kLoopTransportLimit = 700'000'000;
constexpr Milliwatts kFirstLoad = 200'000'000;
constexpr Milliwatts kSecondLoad = 300'000'000;

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

// The engine's idempotency rule, stated as a decision: an attempt that was never
// seen is applied; an attempt that was already applied with the same content and
// left in the state this operation produces is a replay; anything else is a
// conflict, never a silent overwrite.
enum class AttemptDecision { NotSeen, Replay, Conflict };

AttemptDecision decide_attempt(const ccap::CoolingSnapshot& snapshot,
                               const ccap::CommitmentRequest& request) {
  for (const ccap::CommitmentRecord& existing : snapshot.commitments()) {
    if (existing.attempt != request.attempt ||
        existing.state == ccap::CommitmentState::Superseded) {
      continue;
    }
    // The probe carries the loop the engine chose, because a request that names
    // no loop accepts whichever loop the engine selected.
    ccap::CommitmentRecord probe;
    probe.attempt = request.attempt;
    probe.zone = request.zone;
    probe.pinned_loop = existing.pinned_loop;
    probe.medium = request.medium;
    probe.compatibility = request.compatibility;
    probe.thermal = request.thermal;
    probe.expires_at = request.expires_at;
    probe.note = request.note;
    const bool same_content = existing.same_request_as(probe);
    const bool same_state = existing.state == ccap::resulting_state(request.operation);
    return same_content && same_state ? AttemptDecision::Replay : AttemptDecision::Conflict;
  }
  return AttemptDecision::NotSeen;
}

// The engine's generation precondition: the request names the generation it was
// formed against, and nothing is written when the catalog has moved on.
ccap::Result<void> check_generation(const ccap::CoolingSnapshot& snapshot,
                                    const ccap::CommitmentRequest& request) {
  if (snapshot.generation() != request.expected_generation) {
    return ccap::Error::stale_generation("the request was formed against an older generation",
                                         request.expected_generation.value(),
                                         snapshot.generation().value());
  }
  return ccap::Result<void>();
}

std::string decision_text(AttemptDecision decision) {
  switch (decision) {
    case AttemptDecision::NotSeen:
      return "not seen before, so the operation is applied";
    case AttemptDecision::Replay:
      return "replay, so nothing is written and the original outcome is returned";
    case AttemptDecision::Conflict:
      return "conflict: this attempt identifier was already used for a different request";
  }
  return "unrecognised";
}

// One air loop with two CRAH units, and one zone served by it.
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

ccap::CommitmentRecord make_commitment(const char* id, const char* attempt, Milliwatts thermal,
                                       ccap::CommitmentState state, ccap::RecordRevision revision,
                                       ccap::CapacityGeneration formed_against,
                                       ccap::Timestamp instant) {
  ccap::CommitmentRecord record;
  record.id = ccap::CommitmentId::literal(id);
  record.attempt = ccap::AttemptId::literal(attempt);
  record.zone = ccap::ZoneId::literal("hall-1");
  record.pinned_loop = ccap::LoopId::literal("loop-air-1");
  record.medium = ccap::CoolingMedium::Air;
  record.compatibility = ccap::CompatibilityClass::AirConvection;
  record.thermal = ccap::ThermalPower::from_milliwatts(thermal);
  record.state = state;
  record.actor = ccap::ActorId::literal("operator-1");
  record.created_at = instant;
  record.created_generation = formed_against;
  record.revision = revision;
  return record;
}

ccap::Result<ccap::ZoneCapacity> zone_view(const ccap::CoolingSnapshot& snapshot,
                                           ccap::Timestamp instant) {
  const ccap::CoolingAnalyzer analyzer(snapshot, instant, ccap::AnalysisBasis::Fresh);
  return analyzer.zone_capacity(ccap::ZoneId::literal("hall-1"), ccap::CoolingMedium::Air,
                                ccap::CompatibilityClass::AirConvection);
}

// Builds one immutable generation of the catalog: the model, plus at most one
// commitment record.
ccap::Result<std::shared_ptr<const ccap::CoolingSnapshot>> build_generation(
    std::uint64_t generation, ccap::Timestamp instant, const ccap::CommitmentRecord* commitment) {
  ccap::SnapshotBuilder builder;
  builder.set_generation(ccap::CapacityGeneration::from_value(generation));
  builder.set_constructed_at(instant);
  CCAP_TRY(add_model(builder));
  if (commitment != nullptr) {
    CCAP_TRY(builder.add(*commitment));
  }
  ccap::ValidationReport report;
  CCAP_TRY_DECLARE(snapshot, builder.build(&report));
  if (!report.ok()) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation,
                       "a generated catalog failed validation")
        .with("generation", std::to_string(generation))
        .with("findings", report.render());
  }
  return snapshot;
}

ccap::Result<void> print_generation(const ccap::CoolingSnapshot& snapshot,
                                    ccap::Timestamp instant) {
  CCAP_TRY_DECLARE(view, zone_view(snapshot, instant));
  std::cout << std::left << std::setw(30) << "  generation" << snapshot.generation().to_string()
            << "\n";
  if (snapshot.commitments().empty()) {
    std::cout << std::left << std::setw(30) << "  commitment"
              << "none"
              << "\n";
  } else {
    const ccap::CommitmentRecord& commitment = snapshot.commitments().front();
    std::cout << std::left << std::setw(30) << "  commitment"
              << commitment.id.str() + " " + std::string(ccap::to_string(commitment.state)) +
                     " revision " + commitment.revision.to_string()
              << "\n";
  }
  print_figure("  usable", view.breakdown.usable);
  print_figure("  committed", view.breakdown.committed);
  print_figure("  reserve", view.breakdown.reserve);
  print_figure("  offered", view.offered);
  std::cout << "\n";
  return ccap::Result<void>();
}

ccap::Result<void> expect_generation(const ccap::CoolingSnapshot& snapshot,
                                     ccap::Timestamp instant, Milliwatts committed,
                                     Milliwatts offered, const char* committed_what,
                                     const char* offered_what) {
  CCAP_TRY_DECLARE(view, zone_view(snapshot, instant));
  CCAP_TRY(expect_known(view.breakdown.committed, committed, committed_what));
  CCAP_TRY(expect_known(view.offered, offered, offered_what));
  return ccap::Result<void>();
}

ccap::Result<void> run() {
  CCAP_TRY_DECLARE(start, ccap::parse_timestamp("2026-08-03T07:15:00.000Z"));
  ccap::ManualClock clock(start);

  std::cout << "Cooling Capacity example 06: commitments and preconditions\n\n";
  std::cout << "the commitment state machine\n";
  std::cout << std::left << std::setw(14) << "operation" << std::setw(16) << "state"
            << "consumes capacity\n";
  const ccap::CommitmentOperation operations[] = {
      ccap::CommitmentOperation::Plan, ccap::CommitmentOperation::Hold,
      ccap::CommitmentOperation::Commit, ccap::CommitmentOperation::Release,
      ccap::CommitmentOperation::Expire};
  for (const ccap::CommitmentOperation operation : operations) {
    const ccap::CommitmentState state = ccap::resulting_state(operation);
    std::cout << std::left << std::setw(14) << ccap::to_string(operation) << std::setw(16)
              << ccap::to_string(state) << (ccap::consumes_capacity(state) ? "yes" : "no") << "\n";
  }
  std::cout << "\n";

  // --- generation 1: the model, with nothing committed yet -------------------
  CCAP_TRY_DECLARE(gen1, build_generation(1, clock.now(), nullptr));
  CCAP_TRY(expect_generation(*gen1, clock.now(), 0, kLoopTransportLimit, "gen1 committed",
                             "gen1 offered"));
  std::cout << "generation 1: the model, with nothing committed yet\n";
  CCAP_TRY(print_generation(*gen1, clock.now()));

  ccap::CommitmentRequest hold;
  hold.operation = ccap::CommitmentOperation::Hold;
  hold.id = ccap::CommitmentId::literal("load-900");
  hold.attempt = ccap::AttemptId::literal("attempt-900");
  hold.zone = ccap::ZoneId::literal("hall-1");
  hold.medium = ccap::CoolingMedium::Air;
  hold.compatibility = ccap::CompatibilityClass::AirConvection;
  hold.thermal = ccap::ThermalPower::from_milliwatts(kFirstLoad);
  hold.actor = ccap::ActorId::literal("operator-1");
  hold.requested_at = clock.now();
  hold.expected_generation = gen1->generation();
  // A request must carry an identifier, an attempt, an actor, a zone, a
  // positive load and the generation it was formed against.
  CCAP_TRY(hold.validate());
  CCAP_TRY(check_generation(*gen1, hold));
  std::cout << "hold attempt-900 for " << hold.thermal.to_string() << " against generation 1\n";
  std::cout << "  " << decision_text(decide_attempt(*gen1, hold)) << "\n\n";

  // --- generation 2: the load is held ---------------------------------------
  CCAP_TRY_DECLARE(revision_two, ccap::RecordRevision::first().next());
  const ccap::CommitmentRecord held =
      make_commitment("load-900", "attempt-900", kFirstLoad, ccap::CommitmentState::Held,
                      ccap::RecordRevision::first(), gen1->generation(), clock.now());
  CCAP_TRY_DECLARE(gen2, build_generation(2, clock.now(), &held));
  CCAP_TRY(expect_generation(*gen2, clock.now(), kFirstLoad, 500'000'000, "gen2 committed",
                             "gen2 offered"));
  std::cout << "generation 2: the load is held, and a held load consumes capacity\n";
  CCAP_TRY(print_generation(*gen2, clock.now()));

  // --- a retried attempt replays --------------------------------------------
  ccap::CommitmentRequest retry = hold;
  retry.expected_generation = gen2->generation();
  CCAP_TRY(check_generation(*gen2, retry));
  std::cout << "retried attempt-900, formed against the current generation 2\n";
  std::cout << "  identical content: " << decision_text(decide_attempt(*gen2, retry)) << "\n";
  ccap::CommitmentRequest reused = retry;
  reused.thermal = ccap::ThermalPower::from_milliwatts(250'000'000);
  std::cout << "  the same attempt for 250000000 mW instead: "
            << decision_text(decide_attempt(*gen2, reused)) << "\n";
  CCAP_TRY(expect_generation(*gen2, clock.now(), kFirstLoad, 500'000'000, "retry committed",
                             "retry offered"));
  std::cout << "  after both retries the catalog is unchanged: generation "
            << gen2->generation().to_string() << ", committed "
            << ccap::ThermalPower::from_milliwatts(kFirstLoad).to_string() << "\n\n";

  // --- generation 3: the load is committed ----------------------------------
  const ccap::CommitmentRecord committed =
      make_commitment("load-900", "attempt-900", kFirstLoad, ccap::CommitmentState::Committed,
                      revision_two, gen1->generation(), clock.now());
  CCAP_TRY_DECLARE(gen3, build_generation(3, clock.now(), &committed));
  CCAP_TRY(expect_generation(*gen3, clock.now(), kFirstLoad, 500'000'000, "gen3 committed",
                             "gen3 offered"));
  std::cout << "generation 3: the load is committed, and still consumes capacity\n";
  CCAP_TRY(print_generation(*gen3, clock.now()));

  // --- generation 4: the load is released -----------------------------------
  CCAP_TRY_DECLARE(revision_three, revision_two.next());
  const ccap::CommitmentRecord released =
      make_commitment("load-900", "attempt-900", kFirstLoad, ccap::CommitmentState::Released,
                      revision_three, gen1->generation(), clock.now());
  CCAP_TRY_DECLARE(gen4, build_generation(4, clock.now(), &released));
  CCAP_TRY(expect_generation(*gen4, clock.now(), 0, kLoopTransportLimit, "gen4 committed",
                             "gen4 offered"));
  std::cout << "generation 4: the load is released, so it consumes nothing again\n";
  CCAP_TRY(print_generation(*gen4, clock.now()));
  std::cout << "  consumes_capacity(released) is "
            << (ccap::consumes_capacity(ccap::CommitmentState::Released) ? "true" : "false")
            << ", so the capacity is back with the zone\n\n";

  // --- a stale-generation request is refused --------------------------------
  ccap::CommitmentRequest second;
  second.operation = ccap::CommitmentOperation::Hold;
  second.id = ccap::CommitmentId::literal("load-903");
  second.attempt = ccap::AttemptId::literal("attempt-903");
  second.zone = ccap::ZoneId::literal("hall-1");
  second.medium = ccap::CoolingMedium::Air;
  second.compatibility = ccap::CompatibilityClass::AirConvection;
  second.thermal = ccap::ThermalPower::from_milliwatts(kSecondLoad);
  second.actor = ccap::ActorId::literal("operator-1");
  second.requested_at = clock.now();
  second.expected_generation = ccap::CapacityGeneration::first();
  CCAP_TRY(second.validate());

  std::cout << "a request formed against generation 1 while the catalog is at generation 4\n";
  const ccap::Result<void> refused = check_generation(*gen4, second);
  if (refused.ok()) {
    return ccap::Error(ccap::ErrorCode::InvariantViolation,
                       "a request formed against an older generation must be refused");
  }
  std::cout << "  refused: " << refused.error().to_string() << "\n";
  std::cout << "  code " << ccap::to_string(refused.error().code()) << " in category "
            << ccap::to_string(refused.error().category()) << "\n";
  std::cout << "  nothing was written and the catalog still holds generation "
            << gen4->generation().to_string() << "\n\n";

  second.expected_generation = gen4->generation();
  CCAP_TRY(check_generation(*gen4, second));
  std::cout << "the same request re-formed against the current generation 4\n";
  std::cout << "  " << decision_text(decide_attempt(*gen4, second)) << "\n\n";

  // --- generation 5: the second load is held --------------------------------
  const ccap::CommitmentRecord second_held =
      make_commitment("load-903", "attempt-903", kSecondLoad, ccap::CommitmentState::Held,
                      ccap::RecordRevision::first(), gen4->generation(), clock.now());
  CCAP_TRY_DECLARE(gen5, build_generation(5, clock.now(), &second_held));
  CCAP_TRY(expect_generation(*gen5, clock.now(), kSecondLoad, 400'000'000, "gen5 committed",
                             "gen5 offered"));
  std::cout << "generation 5: the second load is held\n";
  CCAP_TRY(print_generation(*gen5, clock.now()));

  // --- the whole lifecycle on one page --------------------------------------
  struct Stage {
    const ccap::CoolingSnapshot* snapshot;
    const char* event;
  };
  const Stage stages[] = {{gen1.get(), "model published"},
                          {gen2.get(), "hold applied"},
                          {gen3.get(), "commit applied"},
                          {gen4.get(), "release applied"},
                          {gen5.get(), "second hold applied"}};
  std::cout << "lifecycle summary\n";
  std::cout << std::left << std::setw(12) << "generation" << std::setw(21) << "event"
            << std::setw(14) << "commitment" << std::setw(16) << "committed" << std::setw(16)
            << "reserve"
            << "offered\n";
  for (const Stage& stage : stages) {
    CCAP_TRY_DECLARE(view, zone_view(*stage.snapshot, clock.now()));
    const std::string state =
        stage.snapshot->commitments().empty()
            ? std::string("-")
            : std::string(ccap::to_string(stage.snapshot->commitments().front().state));
    std::cout << std::left << std::setw(12) << stage.snapshot->generation().to_string()
              << std::setw(21) << stage.event << std::setw(14) << state << std::setw(16)
              << view.breakdown.committed.to_string() << std::setw(16)
              << view.breakdown.reserve.to_string() << view.offered.to_string() << "\n";
  }
  std::cout << "\n";
  return ccap::Result<void>();
}

}  // namespace

int main() {
  const ccap::Result<void> outcome = run();
  if (!outcome.ok()) {
    return failure(outcome.error());
  }
  std::cout << "example 06 finished: ok\n";
  return 0;
}
