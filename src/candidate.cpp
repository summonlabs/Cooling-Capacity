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

#include "cooling_capacity/candidate.hpp"

#include <algorithm>

#include "cooling_capacity/snapshot.hpp"
#include "enum_table.hpp"

namespace cooling_capacity {

namespace {

constexpr detail::EnumName<CandidateDisposition> kDispositionNames[] = {
    {CandidateDisposition::Admitted, "admitted"},
    {CandidateDisposition::Rejected, "rejected"},
    {CandidateDisposition::Indeterminate, "indeterminate"},
};

constexpr detail::EnumName<CandidateReason> kReasonNames[] = {
    {CandidateReason::None, "none"},
    {CandidateReason::ZeroOrNegativeLoad, "zero-or-negative-load"},
    {CandidateReason::ZoneNotFound, "zone-not-found"},
    {CandidateReason::MediumUnsupported, "medium-unsupported"},
    {CandidateReason::ClassUnsupported, "class-unsupported"},
    {CandidateReason::LoopNotFound, "loop-not-found"},
    {CandidateReason::PinnedLoopNotServing, "pinned-loop-not-serving"},
    {CandidateReason::PinnedLoopClassMismatch, "pinned-loop-class-mismatch"},
    {CandidateReason::InsufficientLocalCapacity, "insufficient-local-capacity"},
    {CandidateReason::InsufficientSharedCapacity, "insufficient-shared-capacity"},
    {CandidateReason::InsufficientRedundancy, "insufficient-redundancy"},
    {CandidateReason::OverCommitted, "over-committed"},
    {CandidateReason::ReserveViolation, "reserve-violation"},
    {CandidateReason::UnknownCapacity, "unknown-capacity"},
    {CandidateReason::UnknownOperatingState, "unknown-operating-state"},
    {CandidateReason::StaleEvidence, "stale-evidence"},
    {CandidateReason::RecoveredPendingRevalidation, "recovered-pending-revalidation"},
    {CandidateReason::LoopTransportUndeclared, "loop-transport-undeclared"},
    {CandidateReason::FacilityMediumMismatch, "facility-medium-mismatch"},
};

// Maps a reason attached to an unknown capacity onto the more specific
// candidate reason, so an operator learns what to fix.
CandidateReason indeterminate_reason(CapacityReason reason) {
  switch (reason) {
    case CapacityReason::TransportNotDeclared:
      return CandidateReason::LoopTransportUndeclared;
    case CapacityReason::EvidenceStale:
    case CapacityReason::EvidenceWithdrawn:
    case CapacityReason::EvidenceFutureDated:
    case CapacityReason::EvidenceMissing:
      return CandidateReason::StaleEvidence;
    case CapacityReason::OperatingStateUnknown:
      return CandidateReason::UnknownOperatingState;
    case CapacityReason::RecoveredPendingRevalidation:
      return CandidateReason::RecoveredPendingRevalidation;
    default:
      return CandidateReason::UnknownCapacity;
  }
}

}  // namespace

std::string_view to_string(CandidateDisposition disposition) noexcept {
  return detail::name_from_table(kDispositionNames, disposition);
}

Result<CandidateDisposition> parse_candidate_disposition(std::string_view text) {
  return detail::parse_from_table("candidate disposition", kDispositionNames, text);
}

std::string_view to_string(CandidateReason reason) noexcept {
  return detail::name_from_table(kReasonNames, reason);
}

Result<CandidateReason> parse_candidate_reason(std::string_view text) {
  return detail::parse_from_table("candidate reason", kReasonNames, text);
}

Result<void> CandidateLoad::validate() const {
  if (zone.empty()) {
    return Error(ErrorCode::InvalidArgument, "candidate load has no zone");
  }
  if (thermal.is_negative() || thermal.is_zero()) {
    return Error(ErrorCode::InvalidArgument, "candidate load must be a positive thermal power")
        .with("thermal_milliwatts", std::to_string(thermal.milliwatts()));
  }
  if (medium_of(compatibility) != medium) {
    return Error(ErrorCode::IncompatibleMedium,
                 "candidate compatibility class does not belong to the requested medium")
        .with("medium", std::string(to_string(medium)))
        .with("class", std::string(to_string(compatibility)));
  }
  return Result<void>();
}

std::string CandidateEvaluation::to_string() const {
  std::string out(cooling_capacity::to_string(disposition));
  out.append(": ");
  out.append(cooling_capacity::to_string(reason));
  out.append(": requested=");
  out.append(requested.to_string());
  out.append(" available=");
  out.append(available.to_string());
  out.append(" offered=");
  out.append(offered.to_string());
  out.append(" generation=");
  out.append(generation.to_string());
  return out;
}

Result<CandidateEvaluation> evaluate_candidate(const CoolingAnalyzer& analyzer,
                                               const CandidateLoad& candidate) {
  CCAP_TRY(candidate.validate());

  const CoolingSnapshot& snapshot = analyzer.snapshot();
  CandidateEvaluation evaluation;
  evaluation.request = candidate;
  evaluation.requested = candidate.thermal;
  evaluation.generation = snapshot.generation();
  evaluation.policy = snapshot.policy().id;
  evaluation.policy_revision = snapshot.policy().revision;
  evaluation.evaluated_at = analyzer.evaluated_at();
  evaluation.basis = analyzer.basis();
  evaluation.explanation.emplace_back(
      ExplanationCode::CandidateRequest,
      std::vector<ExplanationParam>{
          param("thermal", candidate.thermal.to_string()),
          param("medium", std::string(to_string(candidate.medium))),
          param("class", std::string(to_string(candidate.compatibility))),
          param("zone", candidate.zone.str())});
  evaluation.explanation.emplace_back(
      ExplanationCode::SnapshotBasis,
      std::vector<ExplanationParam>{param("generation", snapshot.generation().value()),
                                    param("basis", std::string(to_string(analyzer.basis()))),
                                    param("evaluated_at", analyzer.evaluated_at().to_string())});
  evaluation.explanation.emplace_back(
      ExplanationCode::PolicyApplied,
      std::vector<ExplanationParam>{
          param("policy", snapshot.policy().id.str()),
          param("revision", snapshot.policy().revision.value()),
          param("stale_action", std::string(to_string(snapshot.policy().stale_action))),
          param("unknown_action", std::string(to_string(snapshot.policy().unknown_action)))});

  const auto conclude = [&evaluation](CandidateDisposition disposition, CandidateReason reason) {
    evaluation.disposition = disposition;
    evaluation.reason = reason;
    evaluation.explanation.emplace_back(
        ExplanationCode::CandidateDisposition,
        std::vector<ExplanationParam>{param("disposition", std::string(to_string(disposition))),
                                      param("reason", std::string(to_string(reason)))});
    return evaluation;
  };

  if (analyzer.basis() == AnalysisBasis::RecoveredPendingRevalidation &&
      snapshot.policy().require_revalidation_after_recovery) {
    evaluation.explanation.emplace_back(
        ExplanationCode::SnapshotRecovered,
        std::vector<ExplanationParam>{param("generation", snapshot.generation().value()),
                                      param("loaded_at", analyzer.evaluated_at().to_string())});
    return conclude(CandidateDisposition::Indeterminate,
                    CandidateReason::RecoveredPendingRevalidation);
  }

  if (snapshot.find_zone(candidate.zone) == nullptr) {
    evaluation.explanation.emplace_back(
        ExplanationCode::ZoneNotFound,
        std::vector<ExplanationParam>{param("zone", candidate.zone.str())});
    return conclude(CandidateDisposition::Rejected, CandidateReason::ZoneNotFound);
  }

  Result<ZoneCapacity> zone = analyzer.zone_capacity(candidate.zone, candidate.medium,
                                                     candidate.compatibility);
  if (!zone.ok()) {
    if (zone.error().code() == ErrorCode::NotFound) {
      evaluation.explanation.emplace_back(
          ExplanationCode::ZoneNotFound,
          std::vector<ExplanationParam>{param("zone", candidate.zone.str())});
      return conclude(CandidateDisposition::Rejected, CandidateReason::ZoneNotFound);
    }
    return zone.error();
  }
  ZoneCapacity view = std::move(zone).value();
  for (const ExplanationStep& step : view.explanation) {
    evaluation.explanation.push_back(step);
  }
  for (const LoopCapacity& loop : view.loops) {
    evaluation.serving_loops.push_back(loop.loop);
  }
  evaluation.binding = view.bottleneck;
  evaluation.offered = view.offered;
  evaluation.available = view.offered.is_known() ? view.offered.amount_or_zero()
                                                 : ThermalPower::from_milliwatts(0);

  if (view.offered.status() == CapacityStatus::Unsupported) {
    const CandidateReason reason = view.offered.reason() == CapacityReason::MediumNotSupported
                                       ? CandidateReason::MediumUnsupported
                                       : (view.offered.reason() == CapacityReason::ClassNotSupported
                                              ? CandidateReason::ClassUnsupported
                                              : CandidateReason::MediumUnsupported);
    return conclude(CandidateDisposition::Rejected, reason);
  }

  if (candidate.pinned_loop.has_value()) {
    const auto pinned = std::find(evaluation.serving_loops.begin(), evaluation.serving_loops.end(),
                                  *candidate.pinned_loop);
    if (pinned == evaluation.serving_loops.end()) {
      if (snapshot.find_loop(*candidate.pinned_loop) == nullptr) {
        return conclude(CandidateDisposition::Rejected, CandidateReason::LoopNotFound);
      }
      return conclude(CandidateDisposition::Rejected,
                      CandidateReason::PinnedLoopNotServing);
    }
  }

  // The redundancy requirement filters the loops that may carry the load. An
  // unpinned load may land on any serving loop, so every serving loop has to
  // meet the requirement; a pinned load only has to satisfy its own loop.
  const unsigned required_rank = redundancy_rank(candidate.required_redundancy);
  if (required_rank > 0U) {
    for (const LoopCapacity& loop : view.loops) {
      if (candidate.pinned_loop.has_value() && loop.loop != *candidate.pinned_loop) {
        continue;
      }
      if (redundancy_rank(loop.redundancy.effective) < required_rank) {
        evaluation.explanation.emplace_back(
            ExplanationCode::RedundancyRequirementUnmet,
            std::vector<ExplanationParam>{
                param("group", loop.loop.str()),
                param("effective", std::string(to_string(loop.redundancy.effective))),
                param("required", std::string(to_string(candidate.required_redundancy)))});
        return conclude(CandidateDisposition::Rejected,
                        CandidateReason::InsufficientRedundancy);
      }
      evaluation.explanation.emplace_back(
          ExplanationCode::RedundancyRequirementMet,
          std::vector<ExplanationParam>{
              param("group", loop.loop.str()),
              param("declared", std::string(to_string(loop.redundancy.declared))),
              param("required", std::string(to_string(candidate.required_redundancy)))});
    }
  }

  if (view.breakdown.over_committed) {
    evaluation.explanation.emplace_back(
        ExplanationCode::OverCommitted,
        std::vector<ExplanationParam>{param("scope", std::string("zone ") + candidate.zone.str()),
                                      param("deficit", view.breakdown.deficit.to_string())});
    return conclude(CandidateDisposition::Rejected, CandidateReason::OverCommitted);
  }

  if (view.offered.is_known()) {
    if (view.offered.amount_or_zero().milliwatts() >= candidate.thermal.milliwatts()) {
      return conclude(CandidateDisposition::Admitted, CandidateReason::None);
    }
    const CandidateReason reason =
        view.bottleneck.has_value() && view.bottleneck->kind == BottleneckKind::SharedPool
            ? CandidateReason::InsufficientSharedCapacity
            : CandidateReason::InsufficientLocalCapacity;
    evaluation.explanation.emplace_back(
        ExplanationCode::CandidateDisposition,
        std::vector<ExplanationParam>{
            param("disposition", std::string(to_string(CandidateDisposition::Rejected))),
            param("reason", std::string(to_string(reason)))});
    evaluation.disposition = CandidateDisposition::Rejected;
    evaluation.reason = reason;
    return evaluation;
  }

  // Unknown capacity is not zero capacity.
  return conclude(CandidateDisposition::Indeterminate,
                  indeterminate_reason(view.offered.reason()));
}

}  // namespace cooling_capacity
