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

#include "cooling_capacity/engine.hpp"

#include <algorithm>
#include <utility>

namespace cooling_capacity {

namespace {

// The loop that will carry a new load: the serving loop with the most headroom,
// ties broken by identifier so that the choice is reproducible.
Result<LoopId> choose_loop(const ZoneCapacity& zone, const CandidateLoad& candidate) {
  if (candidate.pinned_loop.has_value()) {
    const auto found = std::find_if(zone.loops.begin(), zone.loops.end(),
                                    [&candidate](const LoopCapacity& loop) {
                                      return loop.loop == *candidate.pinned_loop;
                                    });
    if (found == zone.loops.end()) {
      return Error(ErrorCode::NotFound, "the pinned loop does not serve this zone and class")
          .with("loop", candidate.pinned_loop->str())
          .with("zone", candidate.zone.str());
    }
    return found->loop;
  }
  const LoopCapacity* best = nullptr;
  for (const LoopCapacity& loop : zone.loops) {
    if (!loop.breakdown.reserve.is_known()) {
      continue;
    }
    if (best == nullptr || loop.breakdown.reserve.amount_or_zero() >
                               best->breakdown.reserve.amount_or_zero() ||
        (loop.breakdown.reserve.amount_or_zero() ==
             best->breakdown.reserve.amount_or_zero() &&
         loop.loop < best->loop)) {
      best = &loop;
    }
  }
  if (best == nullptr) {
    return Error(ErrorCode::Unknown,
                 "no serving loop has an established reserve, so no loop can be selected")
        .with("zone", candidate.zone.str());
  }
  return best->loop;
}

Error candidate_rejection(const CandidateEvaluation& evaluation) {
  switch (evaluation.reason) {
    case CandidateReason::InsufficientLocalCapacity:
    case CandidateReason::InsufficientSharedCapacity:
    case CandidateReason::OverCommitted:
    case CandidateReason::ReserveViolation:
      return Error(ErrorCode::CapacityExceeded, "the zone cannot carry the requested load")
          .with("zone", evaluation.request.zone.str())
          .with("requested", evaluation.requested.to_string())
          .with("available", evaluation.available.to_string())
          .with("reason", std::string(to_string(evaluation.reason)));
    case CandidateReason::InsufficientRedundancy:
      return Error(ErrorCode::RedundancyUnsatisfied,
                   "no serving loop meets the requested redundancy class")
          .with("zone", evaluation.request.zone.str())
          .with("requested", std::string(to_string(evaluation.request.required_redundancy)));
    case CandidateReason::ClassUnsupported:
      return Error(ErrorCode::IncompatibleClass,
                   "the zone does not support the requested compatibility class")
          .with("zone", evaluation.request.zone.str())
          .with("class", std::string(to_string(evaluation.request.compatibility)));
    case CandidateReason::MediumUnsupported:
      return Error(ErrorCode::IncompatibleMedium, "the zone does not carry the requested medium")
          .with("zone", evaluation.request.zone.str())
          .with("medium", std::string(to_string(evaluation.request.medium)));
    case CandidateReason::ZoneNotFound:
      return Error(ErrorCode::NotFound, "the zone is not present in this generation")
          .with("zone", evaluation.request.zone.str());
    default:
      return Error(ErrorCode::Indeterminate,
                   "the cooling capacity of the zone cannot be established")
          .with("zone", evaluation.request.zone.str())
          .with("reason", std::string(to_string(evaluation.reason)));
  }
}

}  // namespace

CoolingCapacityEngine::CoolingCapacityEngine(std::unique_ptr<CoolingCapacityStore> store,
                                             EngineOptions options)
    : store_(std::move(store)), options_(std::move(options)) {
  snapshot_.store(std::shared_ptr<const CoolingSnapshot>(), std::memory_order_relaxed);
  basis_.store(AnalysisBasis::RecoveredPendingRevalidation, std::memory_order_relaxed);
}

CoolingCapacityEngine::~CoolingCapacityEngine() = default;

Result<std::unique_ptr<CoolingCapacityEngine>> CoolingCapacityEngine::open(
    const std::string& directory, const EngineOptions& options) {
  const Result<void> policy_check = options.policy.validate();
  if (!policy_check.ok()) {
    return policy_check.error();
  }
  if (options.actor.empty() && options.store.access == StoreAccess::ReadWrite) {
    return Error(ErrorCode::InvalidArgument,
                 "a writable engine needs an actor identity for the records it writes");
  }

  CCAP_TRY_DECLARE(store, CoolingCapacityStore::open(directory, options.store));
  std::unique_ptr<CoolingCapacityEngine> engine(
      new CoolingCapacityEngine(std::move(store), options));
  if (options.clock == nullptr) {
    engine->owned_clock_ = std::make_unique<SystemClock>();
  }
  const Clock& clock = options.clock != nullptr ? *options.clock : *engine->owned_clock_;

  if (options.store.access == StoreAccess::ReadWrite) {
    CCAP_TRY(engine->store_->acquire_writer(options.actor));
  }

  const Result<std::shared_ptr<const CoolingSnapshot>> loaded = engine->store_->load_head();
  if (loaded.ok()) {
    CCAP_TRY(engine->adopt(loaded.value(), AnalysisBasis::RecoveredPendingRevalidation));
    return engine;
  }
  if (loaded.error().code() != ErrorCode::NotFound) {
    return loaded.error();
  }
  if (options.store.access == StoreAccess::ReadOnly) {
    return loaded.error();
  }

  // An empty store starts at generation one with the configured policy and no
  // records of any kind. Nothing is inferred about a facility that has not been
  // declared.
  SnapshotBuilder builder;
  builder.set_generation(CapacityGeneration::first());
  builder.set_policy(options.policy);
  builder.set_constructed_at(clock.now());
  builder.set_limits(options.store.limits);
  ValidationReport report;
  CCAP_TRY_DECLARE(initial, builder.build(&report));
  CCAP_TRY_DECLARE(receipt, engine->commit_candidate(initial));
  (void)receipt;
  CCAP_TRY(engine->adopt(initial, AnalysisBasis::Fresh));
  return engine;
}

Result<void> CoolingCapacityEngine::adopt(std::shared_ptr<const CoolingSnapshot> snapshot,
                                          AnalysisBasis basis) {
  if (snapshot == nullptr) {
    return Error(ErrorCode::InvariantViolation, "the engine was handed a null snapshot");
  }
  snapshot_.store(std::move(snapshot), std::memory_order_release);
  basis_.store(basis, std::memory_order_release);
  return Result<void>();
}

std::shared_ptr<const CoolingSnapshot> CoolingCapacityEngine::snapshot() const {
  return snapshot_.load(std::memory_order_acquire);
}

AnalysisBasis CoolingCapacityEngine::basis() const {
  return basis_.load(std::memory_order_acquire);
}

Result<std::shared_ptr<const CoolingSnapshot>> CoolingCapacityEngine::load_generation(
    const CapacityGeneration& generation) const {
  return store_->load_generation(generation);
}

Result<std::vector<GenerationInfo>> CoolingCapacityEngine::generations() const {
  return store_->generations();
}

Result<CapacityGeneration> CoolingCapacityEngine::reload() {
  std::lock_guard<std::mutex> guard(mutation_mutex_);
  CCAP_TRY_DECLARE(loaded, store_->load_head());
  CCAP_TRY(adopt(loaded, AnalysisBasis::RecoveredPendingRevalidation));
  return loaded->generation();
}

Result<LoopCapacity> CoolingCapacityEngine::loop_capacity(const LoopId& loop) const {
  const std::shared_ptr<const CoolingSnapshot> current = snapshot();
  const CoolingAnalyzer analyzer(*current, clock_now(), basis());
  return analyzer.loop_capacity(loop);
}

Result<PlantCapacity> CoolingCapacityEngine::plant_capacity(const PlantId& plant) const {
  const std::shared_ptr<const CoolingSnapshot> current = snapshot();
  const CoolingAnalyzer analyzer(*current, clock_now(), basis());
  return analyzer.plant_capacity(plant);
}

Result<ZoneCapacity> CoolingCapacityEngine::zone_capacity(const ZoneId& zone, CoolingMedium medium,
                                                          CompatibilityClass compatibility) const {
  const std::shared_ptr<const CoolingSnapshot> current = snapshot();
  const CoolingAnalyzer analyzer(*current, clock_now(), basis());
  return analyzer.zone_capacity(zone, medium, compatibility);
}

Result<CapacityValue> CoolingCapacityEngine::available(const ZoneId& zone, CoolingMedium medium,
                                                       CompatibilityClass compatibility) const {
  CCAP_TRY_DECLARE(view, zone_capacity(zone, medium, compatibility));
  return view.offered;
}

Result<CoolingCapacityReport> CoolingCapacityEngine::report() const {
  const std::shared_ptr<const CoolingSnapshot> current = snapshot();
  const CoolingAnalyzer analyzer(*current, clock_now(), basis());
  return analyzer.report();
}

Result<CandidateEvaluation> CoolingCapacityEngine::evaluate(const CandidateLoad& candidate) const {
  const std::shared_ptr<const CoolingSnapshot> current = snapshot();
  const CoolingAnalyzer analyzer(*current, clock_now(), basis());
  return evaluate_candidate(analyzer, candidate);
}

Result<ReconciliationReport> CoolingCapacityEngine::reconcile() const {
  const std::shared_ptr<const CoolingSnapshot> current = snapshot();
  const CoolingAnalyzer analyzer(*current, clock_now(), basis());
  return cooling_capacity::reconcile(analyzer);
}

Result<GenerationDiff> CoolingCapacityEngine::diff(const CapacityGeneration& from,
                                                   const CapacityGeneration& to) const {
  const std::shared_ptr<const CoolingSnapshot> current = snapshot();
  std::shared_ptr<const CoolingSnapshot> left;
  std::shared_ptr<const CoolingSnapshot> right;
  if (from == current->generation()) {
    left = current;
  } else {
    CCAP_TRY_DECLARE(loaded, store_->load_generation(from));
    left = loaded;
  }
  if (to == current->generation()) {
    right = current;
  } else {
    CCAP_TRY_DECLARE(loaded, store_->load_generation(to));
    right = loaded;
  }
  return cooling_capacity::diff(*left, *right);
}

Result<SnapshotBuilder> CoolingCapacityEngine::builder_from(
    const CoolingSnapshot& snapshot) const {
  SnapshotBuilder builder;
  builder.set_generation(snapshot.generation());
  builder.set_policy(snapshot.policy());
  builder.set_constructed_at(snapshot.constructed_at());
  builder.set_limits(snapshot.limits());
  for (const SiteRecord& record : snapshot.sites()) {
    CCAP_TRY(builder.add(record));
  }
  for (const FacilityRecord& record : snapshot.facilities()) {
    CCAP_TRY(builder.add(record));
  }
  for (const DomainRecord& record : snapshot.domains()) {
    CCAP_TRY(builder.add(record));
  }
  for (const ManifoldRecord& record : snapshot.manifolds()) {
    CCAP_TRY(builder.add(record));
  }
  for (const EquipmentRecord& record : snapshot.equipment()) {
    CCAP_TRY(builder.add(record));
  }
  for (const LoopRecord& record : snapshot.loops()) {
    CCAP_TRY(builder.add(record));
  }
  for (const PlantRecord& record : snapshot.plants()) {
    CCAP_TRY(builder.add(record));
  }
  for (const ZoneRecord& record : snapshot.zones()) {
    CCAP_TRY(builder.add(record));
  }
  for (const EvidenceRecord& record : snapshot.evidence()) {
    CCAP_TRY(builder.add(record));
  }
  for (const CommitmentRecord& record : snapshot.commitments()) {
    CCAP_TRY(builder.add(record));
  }
  return builder;
}

Result<SnapshotBuilder> CoolingCapacityEngine::begin_mutation() const {
  const std::shared_ptr<const CoolingSnapshot> current = snapshot();
  CCAP_TRY_DECLARE(builder, builder_from(*current));
  CCAP_TRY_DECLARE(next, current->generation().next());
  builder.set_generation(next);
  return builder;
}

Result<CommitReceipt> CoolingCapacityEngine::commit_candidate(
    std::shared_ptr<const CoolingSnapshot> candidate) {
  if (options_.store.access != StoreAccess::ReadWrite) {
    return Error(ErrorCode::ReadOnly, "the engine was opened read-only")
        .with("directory", store_->directory());
  }
  CommitOptions options;
  const Result<CapacityGeneration> head = store_->head_generation();
  options.expected_generation =
      head.ok() ? head.value() : CapacityGeneration::from_value(0);
  options.committed_at = clock_now();
  return store_->commit(*candidate, options);
}

Result<CommitReceipt> CoolingCapacityEngine::publish(SnapshotBuilder& builder) {
  std::lock_guard<std::mutex> guard(mutation_mutex_);
  ValidationReport report;
  CCAP_TRY_DECLARE(candidate, builder.build(&report));
  CCAP_TRY_DECLARE(receipt, commit_candidate(candidate));
  CCAP_TRY(adopt(candidate, AnalysisBasis::Fresh));
  return receipt;
}

Result<CapacityGeneration> CoolingCapacityEngine::revalidate(const Clock& clock) {
  std::lock_guard<std::mutex> guard(mutation_mutex_);
  const std::shared_ptr<const CoolingSnapshot> current = snapshot();
  CCAP_TRY_DECLARE(builder, builder_from(*current));
  CCAP_TRY_DECLARE(next, current->generation().next());
  builder.set_generation(next);
  builder.set_constructed_at(clock.now());
  ValidationReport report;
  CCAP_TRY_DECLARE(candidate, builder.build(&report));
  CCAP_TRY_DECLARE(receipt, commit_candidate(candidate));
  std::shared_ptr<const CoolingSnapshot> stamped =
      candidate->with_provenance(SnapshotOrigin::Constructed, clock.now());
  CCAP_TRY(adopt(stamped, AnalysisBasis::Revalidated));
  return receipt.generation;
}

Result<WriterEpoch> CoolingCapacityEngine::acquire_writer() {
  std::lock_guard<std::mutex> guard(mutation_mutex_);
  return store_->acquire_writer(options_.actor);
}

Result<void> CoolingCapacityEngine::release_writer() {
  std::lock_guard<std::mutex> guard(mutation_mutex_);
  return store_->release_writer();
}

bool CoolingCapacityEngine::holds_writer() const noexcept { return store_->holds_writer(); }

Result<CommitmentOutcome> CoolingCapacityEngine::apply(const CommitmentRequest& request) {
  CCAP_TRY(request.validate());
  std::lock_guard<std::mutex> guard(mutation_mutex_);

  const std::shared_ptr<const CoolingSnapshot> current = snapshot();
  if (current->generation() != request.expected_generation) {
    return Error::stale_generation("the request was formed against an older generation",
                                   request.expected_generation.value(),
                                   current->generation().value());
  }

  // Idempotency is keyed on the attempt, so a retried request is recognised
  // even when it names a different commitment. A replay returns the original
  // outcome and writes nothing; a reused attempt with different content is a
  // conflict, never a silent overwrite.
  for (const CommitmentRecord& existing : current->commitments()) {
    if (existing.attempt != request.attempt || existing.state == CommitmentState::Superseded) {
      continue;
    }
    const bool consuming = request.operation == CommitmentOperation::Plan ||
                           request.operation == CommitmentOperation::Hold ||
                           request.operation == CommitmentOperation::Commit;
    if (consuming) {
      CommitmentRecord probe;
      probe.attempt = request.attempt;
      probe.zone = request.zone;
      // A request that names no loop accepts whichever loop the engine chose,
      // because that choice is deterministic for a given generation.
      probe.pinned_loop = existing.pinned_loop;
      probe.medium = request.medium;
      probe.compatibility = request.compatibility;
      probe.thermal = request.thermal;
      probe.expires_at = request.expires_at;
      probe.note = request.note;
      if (existing.same_request_as(probe) &&
          existing.state == resulting_state(request.operation)) {
        CommitmentOutcome outcome;
        outcome.id = existing.id;
        outcome.attempt = existing.attempt;
        outcome.state = existing.state;
        outcome.replayed = true;
        outcome.generation = current->generation();
        const CoolingAnalyzer analyzer(*current, clock_now(), basis());
        CCAP_TRY_DECLARE(view, analyzer.zone_capacity(request.zone, request.medium,
                                                      request.compatibility));
        outcome.committed_after = view.breakdown.committed.amount_or_zero();
        outcome.usable_after = view.breakdown.usable.amount_or_zero();
        outcome.reserve_after = view.breakdown.reserve.amount_or_zero();
        return outcome;
      }
    }
    return Error(ErrorCode::IdempotencyConflict,
                 "this attempt identifier was already used for a different request")
        .with("attempt", request.attempt.str())
        .with("commitment", existing.id.str());
  }

  CCAP_TRY_DECLARE(builder, builder_from(*current));
  CCAP_TRY_DECLARE(next, current->generation().next());
  builder.set_generation(next);

  const CoolingAnalyzer analyzer(*current, clock_now(), basis());

  if (request.operation == CommitmentOperation::Release ||
      request.operation == CommitmentOperation::Expire) {
    const CommitmentRecord* target =
        request.target.has_value() ? current->find_commitment(*request.target) : nullptr;
    if (target == nullptr) {
      return Error(ErrorCode::NotFound, "the commitment to release is not present")
          .with("commitment", request.target.has_value() ? request.target->str() : "<none>");
    }
    CommitmentRecord updated = *target;
    updated.state = resulting_state(request.operation);
    CCAP_TRY_DECLARE(revision, updated.revision.next());
    updated.revision = revision;
    CCAP_TRY(builder.replace(updated));

    ValidationReport report;
    CCAP_TRY_DECLARE(candidate, builder.build(&report));
    CCAP_TRY_DECLARE(receipt, commit_candidate(candidate));
    CCAP_TRY(adopt(candidate, AnalysisBasis::Fresh));

    const CoolingAnalyzer after(*candidate, clock_now(), basis());
    CCAP_TRY_DECLARE(view, after.zone_capacity(updated.zone, updated.medium,
                                               updated.compatibility));
    CommitmentOutcome outcome;
    outcome.id = updated.id;
    outcome.attempt = updated.attempt;
    outcome.state = updated.state;
    outcome.generation = receipt.generation;
    outcome.committed_after = view.breakdown.committed.amount_or_zero();
    outcome.usable_after = view.breakdown.usable.amount_or_zero();
    outcome.reserve_after = view.breakdown.reserve.amount_or_zero();
    return outcome;
  }

  CandidateLoad load;
  load.zone = request.zone;
  load.medium = request.medium;
  load.compatibility = request.compatibility;
  load.thermal = request.thermal;
  load.pinned_loop = request.pinned_loop;
  load.actor = request.actor;
  load.requested_at = request.requested_at;
  CCAP_TRY(load.validate());

  CCAP_TRY_DECLARE(view, analyzer.zone_capacity(request.zone, request.medium,
                                                request.compatibility));
  if (view.offered.status() == CapacityStatus::Unsupported) {
    return Error(ErrorCode::Unsupported,
                 "the zone cannot serve this medium or compatibility class")
        .with("zone", request.zone.str())
        .with("reason", std::string(to_string(view.offered.reason())));
  }
  CCAP_TRY_DECLARE(selected, choose_loop(view, load));
  load.pinned_loop = selected;

  if (request.operation != CommitmentOperation::Plan) {
    CCAP_TRY_DECLARE(evaluation, evaluate_candidate(analyzer, load));
    if (!evaluation.admitted()) {
      return candidate_rejection(evaluation);
    }
  } else {
    // A plan is an intent: it is recorded, and it consumes nothing. Whether it
    // could be admitted is answered by `evaluate`, not by `apply`.
    CCAP_TRY_DECLARE(evaluation, evaluate_candidate(analyzer, load));
    if (evaluation.disposition == CandidateDisposition::Rejected) {
      return candidate_rejection(evaluation);
    }
  }

  CommitmentRecord record;
  record.id = request.id;
  record.attempt = request.attempt;
  record.zone = request.zone;
  record.pinned_loop = selected;
  record.medium = request.medium;
  record.compatibility = request.compatibility;
  record.thermal = request.thermal;
  record.state = resulting_state(request.operation);
  record.actor = request.actor;
  record.created_at = request.requested_at;
  record.expires_at = request.expires_at;
  record.created_generation = current->generation();
  record.supersedes = request.target;
  record.note = request.note;
  record.revision = RecordRevision::first();

  // A request that names an existing commitment moves it along its lifecycle
  // rather than creating a second record with the same identity. The creation
  // instant and the generation the load was first accepted in are preserved:
  // they are what the load's history is anchored to.
  const CommitmentRecord* existing = current->find_commitment(request.id);
  if (existing != nullptr) {
    if (existing->zone != request.zone) {
      return Error(ErrorCode::Conflict,
                   "the named commitment already exists for a different zone")
          .with("commitment", request.id.str())
          .with("existing_zone", existing->zone.str())
          .with("requested_zone", request.zone.str());
    }
    record.created_at = existing->created_at;
    record.created_generation = existing->created_generation;
    record.supersedes = existing->supersedes;
    CCAP_TRY_DECLARE(revision, existing->revision.next());
    record.revision = revision;
    CCAP_TRY(builder.replace(record));
  } else {
    CCAP_TRY(builder.add(record));
  }

  ValidationReport report;
  CCAP_TRY_DECLARE(candidate, builder.build(&report));
  CCAP_TRY_DECLARE(receipt, commit_candidate(candidate));
  CCAP_TRY(adopt(candidate, AnalysisBasis::Fresh));

  const CoolingAnalyzer after(*candidate, clock_now(), basis());
  CCAP_TRY_DECLARE(after_view, after.zone_capacity(request.zone, request.medium,
                                                   request.compatibility));
  CommitmentOutcome outcome;
  outcome.id = record.id;
  outcome.attempt = record.attempt;
  outcome.state = record.state;
  outcome.generation = receipt.generation;
  outcome.committed_after = after_view.breakdown.committed.amount_or_zero();
  outcome.usable_after = after_view.breakdown.usable.amount_or_zero();
  outcome.reserve_after = after_view.breakdown.reserve.amount_or_zero();
  return outcome;
}

Timestamp CoolingCapacityEngine::clock_now() const {
  const Clock& clock = options_.clock != nullptr ? *options_.clock : *owned_clock_;
  return clock.now();
}

}  // namespace cooling_capacity
