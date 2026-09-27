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

#include "cooling_capacity/rollup.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>

#include "cooling_capacity/snapshot.hpp"
#include "enum_table.hpp"

namespace cooling_capacity {

namespace {

constexpr detail::EnumName<AnalysisBasis> kBasisNames[] = {
    {AnalysisBasis::Fresh, "fresh"},
    {AnalysisBasis::RecoveredPendingRevalidation, "recovered-pending-revalidation"},
    {AnalysisBasis::Revalidated, "revalidated"},
};

constexpr detail::EnumName<BottleneckKind> kBottleneckNames[] = {
    {BottleneckKind::None, "none"},
    {BottleneckKind::ZoneCommittedLoad, "zone-committed-load"},
    {BottleneckKind::LoopTransport, "loop-transport"},
    {BottleneckKind::LoopEquipment, "loop-equipment"},
    {BottleneckKind::LoopCommittedLoad, "loop-committed-load"},
    {BottleneckKind::SharedPool, "shared-pool"},
    {BottleneckKind::MinimumReserve, "minimum-reserve"},
    {BottleneckKind::EvidenceFreshness, "evidence-freshness"},
    {BottleneckKind::UnknownCapacity, "unknown-capacity"},
    {BottleneckKind::ZoneMediumUnsupported, "zone-medium-unsupported"},
};

// Deterministic ordering used to break ties between equally binding
// constraints: the kind decides first, then the subject identifier.
bool bottleneck_before(const Bottleneck& lhs, const Bottleneck& rhs) {
  if (lhs.remaining != rhs.remaining) {
    return lhs.remaining < rhs.remaining;
  }
  const auto lhs_rank = static_cast<unsigned>(lhs.kind);
  const auto rhs_rank = static_cast<unsigned>(rhs.kind);
  if (lhs_rank != rhs_rank) {
    return lhs_rank < rhs_rank;
  }
  return lhs.subject < rhs.subject;
}

std::string subject_key(const EvidenceSubject& subject) {
  std::string key(subject_kind_name(subject));
  key.push_back('|');
  key.append(subject_text(subject));
  return key;
}

struct EvidenceIndex {
  std::multimap<std::string, const EvidenceRecord*> by_subject;

  void build(const CoolingSnapshot& snapshot) {
    by_subject.clear();
    for (const EvidenceRecord& record : snapshot.evidence()) {
      by_subject.emplace(subject_key(record.subject), &record);
    }
  }

  [[nodiscard]] std::vector<const EvidenceRecord*> for_subject(
      const EvidenceSubject& subject) const {
    std::vector<const EvidenceRecord*> found;
    const auto range = by_subject.equal_range(subject_key(subject));
    for (auto iterator = range.first; iterator != range.second; ++iterator) {
      found.push_back(iterator->second);
    }
    return found;
  }
};

// The per-query context. Built once per public operation so that the sharing
// index and the evidence index are computed once rather than per record.
struct AnalysisState {
  const CoolingSnapshot& snapshot;
  Timestamp evaluated_at;
  AnalysisBasis basis;
  std::unordered_map<std::string, std::uint32_t> loop_shares;
  EvidenceIndex evidence;

  AnalysisState(const CoolingSnapshot& snapshot_in, Timestamp evaluated_at_in,
                AnalysisBasis basis_in)
      : snapshot(snapshot_in), evaluated_at(evaluated_at_in), basis(basis_in) {
    evidence.build(snapshot);
    loop_shares.reserve(snapshot.equipment().size() * 2U + 1U);
    for (const LoopRecord& loop : snapshot.loops()) {
      for (const EquipmentId& unit : loop.equipment) {
        ++loop_shares[unit.str()];
      }
    }
  }

  [[nodiscard]] std::uint32_t shares_of(const EquipmentId& unit) const {
    const auto iterator = loop_shares.find(unit.str());
    if (iterator == loop_shares.end() || iterator->second == 0U) {
      return 1U;
    }
    return iterator->second;
  }

  [[nodiscard]] bool accepting_stale() const {
    return snapshot.policy().stale_action == StaleEvidenceAction::Use;
  }
};

EvidenceAssessment assess(const AnalysisState& state, const EvidenceRecord& record) {
  EvidenceAssessment assessment;
  assessment.id = record.id;
  assessment.observed_at = record.observed_at;
  assessment.assessed_at = state.evaluated_at;
  assessment.age = DurationMs::from_milliseconds(state.evaluated_at.unix_milliseconds() -
                                                  record.observed_at.unix_milliseconds());
  if (record.state == EvidenceState::Withdrawn) {
    assessment.freshness = EvidenceFreshness::Withdrawn;
    return assessment;
  }
  if (record.state == EvidenceState::Superseded) {
    assessment.freshness = EvidenceFreshness::Superseded;
    return assessment;
  }
  if (assessment.age.is_negative()) {
    assessment.freshness = EvidenceFreshness::FutureDated;
    return assessment;
  }
  if (record.valid_until.has_value() && state.evaluated_at > *record.valid_until) {
    assessment.freshness = EvidenceFreshness::ExpiredByValidity;
    return assessment;
  }
  const Result<DurationMs> window = state.snapshot.policy().max_age_for(record.kind);
  if (!window.ok()) {
    assessment.freshness = EvidenceFreshness::Stale;
    return assessment;
  }
  assessment.freshness = within_window(assessment.age, window.value())
                             ? EvidenceFreshness::Fresh
                             : EvidenceFreshness::Stale;
  return assessment;
}

// An age-based defect may be forgiven by an explicit policy override; an
// explicit retraction or a future-dated observation never is.
bool acceptable(const AnalysisState& state, const EvidenceAssessment& assessment) {
  if (assessment.freshness == EvidenceFreshness::Fresh) {
    return true;
  }
  if (!state.accepting_stale()) {
    return false;
  }
  return assessment.freshness == EvidenceFreshness::Stale ||
         assessment.freshness == EvidenceFreshness::ExpiredByValidity;
}

CapacityReason reason_for(EvidenceFreshness freshness) {
  switch (freshness) {
    case EvidenceFreshness::Stale:
    case EvidenceFreshness::ExpiredByValidity:
      return CapacityReason::EvidenceStale;
    case EvidenceFreshness::Withdrawn:
      return CapacityReason::EvidenceWithdrawn;
    case EvidenceFreshness::FutureDated:
      return CapacityReason::EvidenceFutureDated;
    case EvidenceFreshness::Superseded:
      return CapacityReason::EvidenceWithdrawn;
    case EvidenceFreshness::Fresh:
      return CapacityReason::None;
  }
  return CapacityReason::EvidenceMissing;
}

CapacityReason reason_for_state(OperatingState state) {
  switch (state) {
    case OperatingState::InService:
      return CapacityReason::None;
    case OperatingState::Standby:
      return CapacityReason::Standby;
    case OperatingState::Maintenance:
      return CapacityReason::Maintenance;
    case OperatingState::Faulted:
      return CapacityReason::Faulted;
    case OperatingState::Decommissioned:
      return CapacityReason::Decommissioned;
    case OperatingState::Unknown:
      return CapacityReason::OperatingStateUnknown;
  }
  return CapacityReason::OutOfService;
}

Result<CapacityValue> headroom_of(const CapacityValue& usable, const CapacityValue& committed) {
  if (!usable.is_known()) {
    return CapacityValue::unknown(usable.reason());
  }
  const std::int64_t usable_raw = usable.amount_or_zero().milliwatts();
  if (committed.is_known()) {
    const std::int64_t committed_raw = committed.amount_or_zero().milliwatts();
    const std::int64_t remaining = committed_raw >= usable_raw ? 0 : usable_raw - committed_raw;
    return CapacityValue::known(ThermalPower::from_milliwatts(remaining));
  }
  if (committed.status() == CapacityStatus::Unavailable) {
    return usable;
  }
  if (committed.status() == CapacityStatus::Unsupported) {
    return usable;
  }
  return CapacityValue::unknown(committed.reason());
}

// The exact equipment capacity chain. Every multiplication is integer and
// rounds down, so a derived figure is never larger than the exact value.
struct UnitCapacity {
  CapacityValue nominal;
  CapacityValue validated;
  CapacityValue derated;
  CapacityValue degraded;
  CapacityValue usable;
  CapacityBasis basis = CapacityBasis::None;
  Fraction cumulative_derate = Fraction::one();
  Fraction degradation = Fraction::one();
  std::vector<ExplanationStep> explanation;
};

void note(UnitCapacity& unit, ExplanationCode code, std::vector<ExplanationParam> params) {
  unit.explanation.emplace_back(code, std::move(params));
}

Result<UnitCapacity> compute_unit(const AnalysisState& state, const EquipmentRecord& record) {
  UnitCapacity unit;
  const CoolingPolicy& policy = state.snapshot.policy();
  const std::string id = record.id.str();

  unit.nominal = record.nominal.is_negative()
                     ? CapacityValue::unknown(CapacityReason::NoCapacityDeclared)
                     : CapacityValue::known(record.nominal);

  CapacityValue base = unit.nominal;
  unit.basis = CapacityBasis::Nominal;
  bool base_known = !record.nominal.is_negative();

  if (record.validated.has_value()) {
    if (record.validated_evidence.has_value()) {
      const EvidenceRecord* evidence = state.snapshot.find_evidence(*record.validated_evidence);
      if (evidence == nullptr) {
        base = CapacityValue::unknown(CapacityReason::EvidenceMissing);
        base_known = false;
        note(unit, ExplanationCode::EquipmentValidationMissing, {param("equipment", id)});
      } else {
        const EvidenceAssessment assessment = assess(state, *evidence);
        note(unit, ExplanationCode::EvidenceAssessed,
             {param("evidence", evidence->id.str()),
              param("kind", std::string(to_string(evidence->kind))),
              param("freshness", std::string(to_string(assessment.freshness))),
              param("age", assessment.age.to_string())});
        if (acceptable(state, assessment)) {
          base = CapacityValue::known(*record.validated);
          unit.basis = CapacityBasis::Validated;
          base_known = true;
        } else {
          note(unit, ExplanationCode::EvidenceRejected,
               {param("evidence", evidence->id.str()),
                param("reason", std::string(to_string(assessment.freshness)))});
          base = CapacityValue::unknown(reason_for(assessment.freshness));
          base_known = false;
        }
      }
    } else if (policy.basis_requirement == CapacityBasisRequirement::ValidatedRequired) {
      base = CapacityValue::unknown(CapacityReason::ValidationMissing);
      base_known = false;
      note(unit, ExplanationCode::EquipmentValidationMissing, {param("equipment", id)});
    } else {
      // A declared validation with no evidence cannot outrank the nameplate.
      base = unit.nominal;
      unit.basis = CapacityBasis::Nominal;
      base_known = !record.nominal.is_negative();
    }
  } else if (policy.basis_requirement == CapacityBasisRequirement::ValidatedRequired) {
    base = CapacityValue::unknown(CapacityReason::ValidationMissing);
    base_known = false;
    note(unit, ExplanationCode::EquipmentValidationMissing, {param("equipment", id)});
  }

  note(unit, ExplanationCode::EquipmentCapacitySource,
       {param("equipment", id), param("basis", std::string(to_string(unit.basis))),
        param("value", base.to_string())});
  unit.validated = base;

  if (!base_known) {
    unit.derated = base;
    unit.degraded = base;
    unit.usable = base;
    return unit;
  }

  CapacityValue derated = base;
  Fraction cumulative = Fraction::one();
  for (const DerateFactor& derate : record.derates) {
    CCAP_TRY_DECLARE(composed, compose(cumulative, derate.factor));
    cumulative = composed;
    CCAP_TRY_DECLARE(scaled, scale(derated.amount_or_zero(), derate.factor));
    derated = CapacityValue::known(scaled);
    note(unit, ExplanationCode::EquipmentDerated,
         {param("equipment", id), param("factor", derate.factor.to_string()),
          param("reason", std::string(to_string(derate.reason))),
          param("cumulative", cumulative.to_string())});
  }
  unit.cumulative_derate = cumulative;
  unit.derated = derated;

  if (!policy.minimum_cumulative_derate.is_zero() &&
      cumulative < policy.minimum_cumulative_derate) {
    note(unit, ExplanationCode::EquipmentDerateBelowFloor,
         {param("equipment", id), param("cumulative", cumulative.to_string()),
          param("floor", policy.minimum_cumulative_derate.to_string())});
    unit.degraded = CapacityValue::unavailable(CapacityReason::DerateBelowFloor);
    unit.usable = unit.degraded;
    return unit;
  }

  CapacityValue degraded = derated;
  const DegradationState& degradation = record.degradation;
  if (degradation.basis == DegradationBasis::None) {
    // Validation guarantees the factor is exactly one in this case.
    degraded = derated;
  } else {
    bool trusted = true;
    if (degradation.assessed_at.has_value()) {
      const DurationMs age =
          DurationMs::from_milliseconds(state.evaluated_at.unix_milliseconds() -
                                        degradation.assessed_at->unix_milliseconds());
      const Result<DurationMs> window =
          policy.max_age_for(EvidenceKind::DegradationAssessment);
      if (age.is_negative() || !window.ok() ||
          !within_window(age, window.value())) {
        trusted = false;
        degraded = CapacityValue::unknown(CapacityReason::EvidenceStale);
        note(unit, ExplanationCode::EvidenceRejected,
             {param("evidence", degradation.evidence.has_value()
                                    ? degradation.evidence->str()
                                    : std::string("degradation-assessment")),
              param("reason", "stale")});
      }
    }
    if (trusted && degradation.evidence.has_value()) {
      const EvidenceRecord* evidence = state.snapshot.find_evidence(*degradation.evidence);
      if (evidence == nullptr) {
        trusted = false;
        degraded = CapacityValue::unknown(CapacityReason::EvidenceMissing);
      } else {
        const EvidenceAssessment assessment = assess(state, *evidence);
        if (!acceptable(state, assessment)) {
          trusted = false;
          degraded = CapacityValue::unknown(reason_for(assessment.freshness));
        }
      }
    }
    if (trusted) {
      CCAP_TRY_DECLARE(scaled, scale(derated.amount_or_zero(), degradation.factor));
      degraded = CapacityValue::known(scaled);
      note(unit, ExplanationCode::EquipmentDegraded,
           {param("equipment", id), param("factor", degradation.factor.to_string()),
            param("basis", std::string(to_string(degradation.basis))),
            param("cumulative", degradation.factor.to_string())});
    }
  }
  unit.degradation = degradation.basis == DegradationBasis::None ? Fraction::one()
                                                                 : degradation.factor;
  unit.degraded = degraded;

  CapacityValue usable = degraded;
  if (record.state_evidence.has_value()) {
    const EvidenceRecord* evidence = state.snapshot.find_evidence(*record.state_evidence);
    if (evidence == nullptr) {
      usable = CapacityValue::unknown(CapacityReason::EvidenceMissing);
    } else {
      const EvidenceAssessment assessment = assess(state, *evidence);
      if (!acceptable(state, assessment)) {
        usable = CapacityValue::unknown(reason_for(assessment.freshness));
        note(unit, ExplanationCode::EvidenceRejected,
             {param("evidence", evidence->id.str()),
              param("reason", std::string(to_string(assessment.freshness)))});
      }
    }
  }

  if (usable.is_known()) {
    switch (record.state) {
      case OperatingState::InService:
        break;
      case OperatingState::Unknown:
        usable = CapacityValue::unknown(CapacityReason::OperatingStateUnknown);
        note(unit, ExplanationCode::EquipmentStateUnknown, {param("equipment", id)});
        break;
      default:
        usable = CapacityValue::unavailable(reason_for_state(record.state));
        note(unit, ExplanationCode::EquipmentOutOfService,
             {param("equipment", id), param("state", std::string(to_string(record.state))),
              param("reason", std::string(to_string(record.state)))});
        break;
    }
  }
  unit.usable = usable;
  return unit;
}

// A group's capacity after its declared redundancy is applied. The formula is
// authoritative; the effective redundancy class reported alongside it is a
// descriptive label derived from the same posture.
//
//   units are the capacity-contributing members of the group, with
//   `amount` = the unit's own usable capacity and `counts` = whether the unit
//   may be relied on at all (in service or standby).
//
//   N          sum of the in-service units
//   N+1        total of all counted units minus the largest
//   N+2        total minus the largest two
//   2N         half the total, rounded down
//   2N+1       half of (total minus the largest), rounded down
//   2(N+1)     half of (total minus the largest two), rounded down
struct GroupMember {
  ThermalPower amount;
  bool in_service = false;
  bool counted = false;
  bool unknown = false;
  CapacityReason unknown_reason = CapacityReason::EvidenceMissing;
  CapacityReason unavailable_reason = CapacityReason::OutOfService;
};

Result<CapacityValue> group_capacity(const std::vector<GroupMember>& members,
                                     RedundancyClass declared) {
  std::vector<ThermalPower> counted;
  bool have_unavailable_reason = false;
  CapacityReason unavailable_reason = CapacityReason::OutOfService;
  for (const GroupMember& member : members) {
    if (member.unknown) {
      return CapacityValue::unknown(member.unknown_reason);
    }
    if (member.counted) {
      counted.push_back(member.amount);
    } else if (!have_unavailable_reason) {
      // A group with nothing left to count is definitely zero. The reason
      // reported is the one that removed the first unit, which is more useful
      // than a generic "out of service" when the cause is, for example, a
      // derate below the policy floor.
      unavailable_reason = member.unavailable_reason;
      have_unavailable_reason = true;
    }
  }
  if (members.empty()) {
    return CapacityValue::unavailable(CapacityReason::EmptyGroup);
  }
  if (counted.empty()) {
    return CapacityValue::unavailable(unavailable_reason);
  }
  std::sort(counted.begin(), counted.end(),
            [](ThermalPower lhs, ThermalPower rhs) { return lhs > rhs; });

  std::int64_t total = 0;
  for (const ThermalPower amount : counted) {
    if (total > std::numeric_limits<std::int64_t>::max() - amount.milliwatts()) {
      return Error(ErrorCode::ArithmeticOverflow, "group capacity total overflowed");
    }
    total += amount.milliwatts();
  }
  std::int64_t in_service_total = 0;
  for (const GroupMember& member : members) {
    if (member.counted && member.in_service) {
      in_service_total += member.amount.milliwatts();
    }
  }
  const std::int64_t largest = counted.empty() ? 0 : counted[0].milliwatts();
  const std::int64_t second = counted.size() > 1 ? counted[1].milliwatts() : 0;

  std::int64_t usable = 0;
  switch (declared) {
    case RedundancyClass::None:
      usable = in_service_total;
      break;
    case RedundancyClass::NPlusOne:
      usable = total > largest ? total - largest : 0;
      break;
    case RedundancyClass::NPlusTwo:
      usable = total > largest + second ? total - largest - second : 0;
      break;
    case RedundancyClass::TwoN:
      usable = total / 2;
      break;
    case RedundancyClass::TwoNPlusOne:
      usable = (total > largest ? total - largest : 0) / 2;
      break;
    case RedundancyClass::TwoNPlusTwo:
      usable = (total > largest + second ? total - largest - second : 0) / 2;
      break;
  }
  return CapacityValue::known(ThermalPower::from_milliwatts(usable));
}

struct GroupResult {
  CapacityValue capacity;
  RedundancyPosture posture;
  std::vector<EquipmentContribution> contributions;
  std::vector<ExplanationStep> explanation;
};

Result<GroupResult> evaluate_group(const AnalysisState& state,
                                   const std::vector<EquipmentId>& members,
                                   RedundancyClass declared, const Identifier& group_name,
                                   EntityKind group_kind) {
  GroupResult result;
  result.posture.declared = declared;
  result.posture.members = members.size();

  std::vector<GroupMember> group_members;
  group_members.reserve(members.size());

  for (const EquipmentId& unit_id : members) {
    const EquipmentRecord* record = state.snapshot.find_equipment(unit_id);
    if (record == nullptr) {
      return Error(ErrorCode::NotFound, "equipment referenced by a group is not present")
          .with("group", group_name.str())
          .with("equipment", unit_id.str());
    }
    CCAP_TRY_DECLARE(unit, compute_unit(state, *record));
    const std::uint32_t shares = state.shares_of(unit_id);

    EquipmentContribution contribution;
    contribution.equipment = unit_id;
    contribution.kind = record->kind;
    contribution.state = record->state;
    contribution.basis = unit.basis;
    contribution.nominal = unit.nominal;
    contribution.validated = unit.validated;
    contribution.derated = unit.derated;
    contribution.degraded = unit.degraded;
    contribution.usable = unit.usable;
    contribution.cumulative_derate = unit.cumulative_derate;
    contribution.degradation = unit.degradation;
    contribution.sharing_loops = shares;
    contribution.shared = shares > 1U;
    contribution.explanation = unit.explanation;

    if (is_transport_only(record->kind)) {
      contribution.contributes_capacity = false;
      contribution.counted_for_redundancy = false;
      contribution.usable = CapacityValue::known(ThermalPower::from_milliwatts(0));
      result.explanation.emplace_back(
          ExplanationCode::TransportOnlyGroupMember,
          std::vector<ExplanationParam>{param("equipment", unit_id.str()),
                                        param("loop", group_name.str())});
      result.contributions.push_back(std::move(contribution));
      continue;
    }

    if (shares > 1U && unit.usable.is_known() &&
        !unit.usable.amount_or_zero().is_zero()) {
      const auto share_parts =
          static_cast<std::int64_t>(Fraction::kScale / static_cast<std::int64_t>(shares));
      CCAP_TRY_DECLARE(
          shared_amount,
          scale(unit.usable.amount_or_zero(), Fraction::from_parts_per_million(share_parts)));
      contribution.usable = CapacityValue::known(shared_amount);
      result.explanation.emplace_back(
          ExplanationCode::EquipmentSharedBetweenLoops,
          std::vector<ExplanationParam>{param("equipment", unit_id.str()),
                                        param("loops", static_cast<std::uint64_t>(shares)),
                                        param("share", shared_amount.to_string())});
    }

    GroupMember member;
    member.counted = is_countable_for_redundancy(record->state);
    member.in_service = is_available_now(record->state);
    if (record->state == OperatingState::Unknown) {
      member.unknown = true;
      member.unknown_reason = CapacityReason::OperatingStateUnknown;
    } else if (member.counted) {
      // A counted unit is one the redundancy formula may rely on. A unit in
      // service carries its usable capacity now; a standby unit carries none
      // now but still contributes its own capacity to the formula, so the
      // degraded figure is what the formula must see.
      const CapacityValue& amount_source =
          member.in_service ? contribution.usable : contribution.degraded;
      if (amount_source.is_known()) {
        member.amount = amount_source.amount_or_zero();
      } else if (amount_source.status() == CapacityStatus::Unavailable) {
        member.counted = false;
        member.in_service = false;
        member.unavailable_reason = amount_source.reason();
      } else {
        member.unknown = true;
        member.unknown_reason = amount_source.reason();
      }
    } else {
      member.counted = false;
      member.in_service = false;
      member.unavailable_reason = contribution.usable.status() == CapacityStatus::Unavailable
                                      ? contribution.usable.reason()
                                      : CapacityReason::OutOfService;
    }

    result.posture.capacity_units += 1U;
    if (member.counted) {
      result.posture.counted += 1U;
    } else {
      result.posture.excluded += 1U;
    }
    if (record->state == OperatingState::Unknown) {
      result.posture.unknown_member_state = true;
    }
    group_members.push_back(member);
    result.contributions.push_back(std::move(contribution));
  }

  result.posture.effective =
      degrade(declared, static_cast<unsigned>(result.posture.excluded));
  CCAP_TRY_DECLARE(capacity, group_capacity(group_members, declared));
  result.capacity = capacity;
  result.explanation.emplace_back(
      ExplanationCode::RedundancyApplied,
      std::vector<ExplanationParam>{
          param("group", group_name.str()), param("declared", std::string(to_string(declared))),
          param("effective", std::string(to_string(result.posture.effective))),
          param("counted", static_cast<std::uint64_t>(result.posture.counted)),
          param("members", static_cast<std::uint64_t>(result.posture.members)),
          param("excluded", static_cast<std::uint64_t>(result.posture.excluded))});
  (void)group_kind;
  return result;
}

// Sums the consuming commitments that a predicate accepts.
Result<CapacityValue> committed_for(const CoolingSnapshot& snapshot,
                                    const std::function<bool(const CommitmentRecord&)>& accept,
                                    std::vector<std::string>& states_out) {
  std::vector<CapacityValue> parts;
  std::vector<std::string> states;
  for (const CommitmentRecord& commitment : snapshot.commitments()) {
    if (!consumes_capacity(commitment.state) || !accept(commitment)) {
      continue;
    }
    parts.push_back(CapacityValue::known(commitment.thermal));
    states.push_back(std::string(to_string(commitment.state)));
  }
  states_out = std::move(states);
  if (parts.empty()) {
    return CapacityValue::known(ThermalPower::from_milliwatts(0));
  }
  return sum_values(parts);
}

// Sums the fresh observed load for a subject and medium.
Result<CapacityValue> observed_for(const AnalysisState& state, const EvidenceSubject& subject,
                                   CoolingMedium medium,
                                   std::vector<EvidenceId>& evidence_out) {
  std::vector<CapacityValue> parts;
  for (const EvidenceRecord* record : state.evidence.for_subject(subject)) {
    if (record->kind != EvidenceKind::LoadMeasurement && record->kind != EvidenceKind::Telemetry) {
      continue;
    }
    const auto* power = std::get_if<ThermalPower>(&record->value);
    if (power == nullptr || !record->medium.has_value() || *record->medium != medium) {
      continue;
    }
    const EvidenceAssessment assessment = assess(state, *record);
    if (!acceptable(state, assessment)) {
      continue;
    }
    parts.push_back(CapacityValue::known(*power));
    evidence_out.push_back(record->id);
  }
  if (parts.empty()) {
    return CapacityValue::unknown(CapacityReason::NoObservation);
  }
  return sum_values(parts);
}

void fill_breakdown_from_loops(CapacityBreakdown& breakdown,
                               const std::vector<const LoopCapacity*>& loops) {
  std::vector<CapacityValue> nominal;
  std::vector<CapacityValue> validated;
  std::vector<CapacityValue> derated;
  std::vector<CapacityValue> degraded;
  std::vector<CapacityValue> usable;
  bool any_validated = false;
  for (const LoopCapacity* loop : loops) {
    nominal.push_back(loop->breakdown.nominal);
    validated.push_back(loop->breakdown.validated);
    derated.push_back(loop->breakdown.derated);
    degraded.push_back(loop->breakdown.degraded);
    usable.push_back(loop->breakdown.usable);
    if (loop->breakdown.basis == CapacityBasis::Validated) {
      any_validated = true;
    }
  }
  const Result<CapacityValue> nominal_sum = sum_values(nominal);
  const Result<CapacityValue> validated_sum = sum_values(validated);
  const Result<CapacityValue> derated_sum = sum_values(derated);
  const Result<CapacityValue> degraded_sum = sum_values(degraded);
  const Result<CapacityValue> usable_sum = sum_values(usable);
  if (nominal_sum.ok()) {
    breakdown.nominal = nominal_sum.value();
  }
  if (validated_sum.ok()) {
    breakdown.validated = validated_sum.value();
  }
  if (derated_sum.ok()) {
    breakdown.derated = derated_sum.value();
  }
  if (degraded_sum.ok()) {
    breakdown.degraded = degraded_sum.value();
  }
  if (usable_sum.ok()) {
    breakdown.usable = usable_sum.value();
  }
  if (any_validated) {
    breakdown.basis = CapacityBasis::Validated;
  } else if (!loops.empty()) {
    breakdown.basis = CapacityBasis::Nominal;
  }
}

std::optional<Bottleneck> choose_bottleneck(std::vector<Bottleneck> candidates) {
  if (candidates.empty()) {
    return std::nullopt;
  }
  auto best = std::min_element(candidates.begin(), candidates.end(), bottleneck_before);
  if (best->kind == BottleneckKind::None) {
    return std::nullopt;
  }
  return *best;
}

}  // namespace

std::string_view to_string(AnalysisBasis basis) noexcept {
  return detail::name_from_table(kBasisNames, basis);
}

Result<AnalysisBasis> parse_analysis_basis(std::string_view text) {
  return detail::parse_from_table("analysis basis", kBasisNames, text);
}

std::string_view to_string(BottleneckKind kind) noexcept {
  return detail::name_from_table(kBottleneckNames, kind);
}

std::string Bottleneck::to_string() const {
  std::string out(cooling_capacity::to_string(kind));
  out.push_back(' ');
  out.append(subject.str());
  out.append(" limit=");
  out.append(limit.to_string());
  out.append(" consumed=");
  out.append(consumed.to_string());
  out.append(" remaining=");
  out.append(remaining.to_string());
  return out;
}

bool operator==(const EquipmentContribution& lhs, const EquipmentContribution& rhs) {
  return lhs.equipment == rhs.equipment && lhs.loop == rhs.loop && lhs.kind == rhs.kind &&
         lhs.state == rhs.state && lhs.basis == rhs.basis && lhs.nominal == rhs.nominal &&
         lhs.validated == rhs.validated && lhs.derated == rhs.derated &&
         lhs.degraded == rhs.degraded && lhs.usable == rhs.usable &&
         lhs.cumulative_derate == rhs.cumulative_derate &&
         lhs.degradation == rhs.degradation && lhs.sharing_loops == rhs.sharing_loops &&
         lhs.shared == rhs.shared && lhs.contributes_capacity == rhs.contributes_capacity &&
         lhs.counted_for_redundancy == rhs.counted_for_redundancy &&
         lhs.explanation == rhs.explanation;
}

bool operator==(const LoopCapacity& lhs, const LoopCapacity& rhs) {
  return lhs.loop == rhs.loop && lhs.kind == rhs.kind && lhs.medium == rhs.medium &&
         lhs.breakdown == rhs.breakdown && lhs.equipment == rhs.equipment &&
         lhs.redundancy == rhs.redundancy && lhs.bottleneck == rhs.bottleneck &&
         lhs.explanation == rhs.explanation;
}

bool operator==(const PlantCapacity& lhs, const PlantCapacity& rhs) {
  return lhs.plant == rhs.plant && lhs.medium == rhs.medium &&
         lhs.breakdown == rhs.breakdown && lhs.equipment == rhs.equipment &&
         lhs.redundancy == rhs.redundancy && lhs.served_loops == rhs.served_loops &&
         lhs.explanation == rhs.explanation;
}

bool operator==(const SharedPoolView& lhs, const SharedPoolView& rhs) {
  return lhs.plant == rhs.plant && lhs.medium == rhs.medium && lhs.usable == rhs.usable &&
         lhs.committed == rhs.committed && lhs.headroom == rhs.headroom &&
         lhs.served_loops == rhs.served_loops && lhs.redundancy == rhs.redundancy &&
         lhs.bottleneck == rhs.bottleneck;
}

bool operator==(const ZoneCapacity& lhs, const ZoneCapacity& rhs) {
  return lhs.zone == rhs.zone && lhs.medium == rhs.medium &&
         lhs.compatibility == rhs.compatibility && lhs.breakdown == rhs.breakdown &&
         lhs.local_headroom == rhs.local_headroom && lhs.shared_headroom == rhs.shared_headroom &&
         lhs.offered == rhs.offered && lhs.loops == rhs.loops &&
         lhs.shared_pools == rhs.shared_pools &&
         lhs.shared_headroom_is_shared == rhs.shared_headroom_is_shared &&
         lhs.bottleneck == rhs.bottleneck && lhs.explanation == rhs.explanation;
}

CoolingAnalyzer::CoolingAnalyzer(const CoolingSnapshot& snapshot, Timestamp evaluated_at,
                                 AnalysisBasis basis)
    : snapshot_(snapshot), evaluated_at_(evaluated_at), basis_(basis) {}

Result<LoopCapacity> CoolingAnalyzer::loop_capacity(LoopId loop_id) const {
  const LoopRecord* loop = snapshot_.find_loop(loop_id);
  if (loop == nullptr) {
    return Error(ErrorCode::NotFound, "loop is not present in this generation")
        .with("loop", loop_id.str())
        .with("generation", snapshot_.generation().to_string());
  }
  const AnalysisState state(snapshot_, evaluated_at_, basis_);

  LoopCapacity result;
  result.loop = loop->id;
  result.kind = loop->kind;
  result.medium = medium_of(loop->kind);
  result.explanation.emplace_back(
      ExplanationCode::SnapshotBasis,
      std::vector<ExplanationParam>{param("generation", snapshot_.generation().value()),
                                    param("basis", std::string(to_string(basis_))),
                                    param("evaluated_at", evaluated_at_.to_string())});
  result.explanation.emplace_back(
      ExplanationCode::LoopConsidered,
      std::vector<ExplanationParam>{param("loop", loop->id.str()),
                                    param("kind", std::string(to_string(loop->kind))),
                                    param("medium", std::string(to_string(result.medium)))});

  CCAP_TRY_DECLARE(group,
                   evaluate_group(state, loop->equipment, loop->redundancy, loop->id.value(),
                                  EntityKind::Loop));
  result.equipment = group.contributions;
  result.redundancy = group.posture;
  result.breakdown.medium = result.medium;
  result.breakdown.nominal = CapacityValue::known(ThermalPower::from_milliwatts(0));
  result.breakdown.validated = CapacityValue::known(ThermalPower::from_milliwatts(0));
  result.breakdown.derated = CapacityValue::known(ThermalPower::from_milliwatts(0));
  result.breakdown.degraded = CapacityValue::known(ThermalPower::from_milliwatts(0));
  result.breakdown.usable = group.capacity;
  for (const ExplanationStep& step : group.explanation) {
    result.explanation.push_back(step);
  }

  std::vector<CapacityValue> nominal_parts;
  std::vector<CapacityValue> validated_parts;
  std::vector<CapacityValue> derated_parts;
  std::vector<CapacityValue> degraded_parts;
  std::vector<CapacityValue> usable_parts;
  bool any_validated = false;
  for (const EquipmentContribution& contribution : result.equipment) {
    nominal_parts.push_back(contribution.nominal);
    validated_parts.push_back(contribution.validated);
    derated_parts.push_back(contribution.derated);
    degraded_parts.push_back(contribution.degraded);
    usable_parts.push_back(contribution.usable);
    if (contribution.basis == CapacityBasis::Validated) {
      any_validated = true;
    }
  }
  CCAP_TRY_DECLARE(nominal_sum, sum_values(nominal_parts));
  CCAP_TRY_DECLARE(validated_sum, sum_values(validated_parts));
  CCAP_TRY_DECLARE(derated_sum, sum_values(derated_parts));
  CCAP_TRY_DECLARE(degraded_sum, sum_values(degraded_parts));
  result.breakdown.nominal = nominal_sum;
  result.breakdown.validated = validated_sum;
  result.breakdown.derated = derated_sum;
  result.breakdown.degraded = degraded_sum;
  result.breakdown.basis =
      any_validated ? CapacityBasis::Validated : CapacityBasis::Nominal;

  // The transport path can never deliver more than it can carry.
  std::vector<CapacityValue> limits;
  limits.push_back(group.capacity);
  if (loop->transport_limit_declared) {
    CapacityValue transport = loop->transport_limit.is_negative()
                                  ? CapacityValue::unknown(CapacityReason::NoCapacityDeclared)
                                  : CapacityValue::known(loop->transport_limit);
    if (loop->transport_evidence.has_value()) {
      const EvidenceRecord* evidence = snapshot_.find_evidence(*loop->transport_evidence);
      if (evidence == nullptr) {
        transport = CapacityValue::unknown(CapacityReason::EvidenceMissing);
      } else {
        const EvidenceAssessment assessment = assess(state, *evidence);
        if (!acceptable(state, assessment)) {
          transport = CapacityValue::unknown(reason_for(assessment.freshness));
        }
      }
    }
    limits.push_back(transport);
    result.explanation.emplace_back(
        ExplanationCode::LoopTransportDeclared,
        std::vector<ExplanationParam>{param("loop", loop->id.str()),
                                      param("limit", transport.to_string())});
  } else {
    result.explanation.emplace_back(
        ExplanationCode::LoopTransportUndeclared,
        std::vector<ExplanationParam>{param("loop", loop->id.str())});
    if (snapshot_.policy().require_transport_declaration) {
      limits.push_back(CapacityValue::unknown(CapacityReason::TransportNotDeclared));
    }
  }
  CCAP_TRY_DECLARE(loop_usable, min_values(limits));
  result.breakdown.usable = loop_usable;

  std::vector<std::string> states;
  CCAP_TRY_DECLARE(
      committed,
      committed_for(snapshot_,
                    [&loop_id](const CommitmentRecord& commitment) {
                      return commitment.pinned_loop.has_value() &&
                             *commitment.pinned_loop == loop_id;
                    },
                    states));
  result.breakdown.committed = committed;

  std::vector<EvidenceId> observation_ids;
  CCAP_TRY_DECLARE(observed, observed_for(state, loop->id, result.medium, observation_ids));
  result.breakdown.observed = observed;

  CCAP_TRY(finalise_breakdown(result.breakdown));

  if (!loop_usable.is_known()) {
    result.bottleneck = Bottleneck{BottleneckKind::UnknownCapacity, loop->id.value(), loop_usable,
                                   result.breakdown.committed.amount_or_zero(),
                                   ThermalPower::from_milliwatts(0)};
  } else {
    const bool transport_binds =
        loop->transport_limit_declared && limits.size() > 1U &&
        limits[1].is_known() &&
        limits[1].amount_or_zero().milliwatts() <= group.capacity.amount_or_zero().milliwatts();
    const BottleneckKind kind =
        transport_binds ? BottleneckKind::LoopTransport : BottleneckKind::LoopEquipment;
    result.bottleneck =
        Bottleneck{kind, loop->id.value(), loop_usable, result.breakdown.committed.amount_or_zero(),
                   result.breakdown.reserve.amount_or_zero()};
  }
  result.explanation.emplace_back(
      ExplanationCode::LoopCapacity,
      std::vector<ExplanationParam>{
          param("loop", loop->id.str()), param("usable", result.breakdown.usable.to_string()),
          param("limited_by", result.bottleneck.has_value()
                                  ? std::string(to_string(result.bottleneck->kind))
                                  : std::string("none"))});
  if (!states.empty()) {
    std::string joined;
    for (std::size_t index = 0; index < states.size(); ++index) {
      if (index != 0) {
        joined.push_back(',');
      }
      joined.append(states[index]);
    }
    result.explanation.emplace_back(
        ExplanationCode::CommittedLoadCharged,
        std::vector<ExplanationParam>{param("scope", std::string("loop ") + loop->id.str()),
                                      param("committed", committed.to_string()),
                                      param("states", joined)});
  }
  if (result.breakdown.over_committed) {
    result.explanation.emplace_back(
        ExplanationCode::OverCommitted,
        std::vector<ExplanationParam>{param("scope", std::string("loop ") + loop->id.str()),
                                      param("deficit", result.breakdown.deficit.to_string())});
  }
  return result;
}

Result<PlantCapacity> CoolingAnalyzer::plant_capacity(PlantId plant_id) const {
  const PlantRecord* plant = snapshot_.find_plant(plant_id);
  if (plant == nullptr) {
    return Error(ErrorCode::NotFound, "plant is not present in this generation")
        .with("plant", plant_id.str())
        .with("generation", snapshot_.generation().to_string());
  }
  const AnalysisState state(snapshot_, evaluated_at_, basis_);

  PlantCapacity result;
  result.plant = plant->id;
  result.medium = plant->medium;
  result.explanation.emplace_back(
      ExplanationCode::SnapshotBasis,
      std::vector<ExplanationParam>{param("generation", snapshot_.generation().value()),
                                    param("basis", std::string(to_string(basis_))),
                                    param("evaluated_at", evaluated_at_.to_string())});

  CCAP_TRY_DECLARE(group,
                   evaluate_group(state, plant->equipment, plant->redundancy, plant->id.value(),
                                  EntityKind::Plant));
  result.equipment = group.contributions;
  result.redundancy = group.posture;
  result.breakdown.medium = result.medium;
  result.breakdown.usable = group.capacity;
  result.breakdown.nominal = CapacityValue::known(ThermalPower::from_milliwatts(0));
  result.breakdown.validated = CapacityValue::known(ThermalPower::from_milliwatts(0));
  result.breakdown.derated = CapacityValue::known(ThermalPower::from_milliwatts(0));
  result.breakdown.degraded = CapacityValue::known(ThermalPower::from_milliwatts(0));
  for (const ExplanationStep& step : group.explanation) {
    result.explanation.push_back(step);
  }

  std::vector<CapacityValue> nominal_parts;
  std::vector<CapacityValue> validated_parts;
  std::vector<CapacityValue> derated_parts;
  std::vector<CapacityValue> degraded_parts;
  bool any_validated = false;
  for (const EquipmentContribution& contribution : result.equipment) {
    nominal_parts.push_back(contribution.nominal);
    validated_parts.push_back(contribution.validated);
    derated_parts.push_back(contribution.derated);
    degraded_parts.push_back(contribution.degraded);
    if (contribution.basis == CapacityBasis::Validated) {
      any_validated = true;
    }
  }
  CCAP_TRY_DECLARE(nominal_sum, sum_values(nominal_parts));
  CCAP_TRY_DECLARE(validated_sum, sum_values(validated_parts));
  CCAP_TRY_DECLARE(derated_sum, sum_values(derated_parts));
  CCAP_TRY_DECLARE(degraded_sum, sum_values(degraded_parts));
  result.breakdown.nominal = nominal_sum;
  result.breakdown.validated = validated_sum;
  result.breakdown.derated = derated_sum;
  result.breakdown.degraded = degraded_sum;
  result.breakdown.basis = any_validated ? CapacityBasis::Validated : CapacityBasis::Nominal;

  // The plant's own commitments are exactly the commitments of every loop it
  // serves, counted once per plant. A loop fed by two plants charges both. The
  // served loops are collected first and the commitments then walked once, so a
  // plant query costs one pass over the commitments rather than one pass per
  // loop.
  std::vector<CapacityValue> committed_parts;
  std::vector<std::string> states;
  for (const LoopRecord& loop : snapshot_.loops()) {
    const bool primary = loop.primary_plant.has_value() && *loop.primary_plant == plant_id;
    const bool secondary = loop.secondary_plant.has_value() && *loop.secondary_plant == plant_id;
    if (primary || secondary) {
      result.served_loops.push_back(loop.id);
    }
  }
  std::sort(result.served_loops.begin(), result.served_loops.end());
  for (const CommitmentRecord& commitment : snapshot_.commitments()) {
    if (!consumes_capacity(commitment.state) || !commitment.pinned_loop.has_value()) {
      continue;
    }
    if (!std::binary_search(result.served_loops.begin(), result.served_loops.end(),
                            *commitment.pinned_loop)) {
      continue;
    }
    committed_parts.push_back(CapacityValue::known(commitment.thermal));
    states.push_back(std::string(to_string(commitment.state)));
  }
  result.breakdown.committed = CapacityValue::known(ThermalPower::from_milliwatts(0));
  if (!committed_parts.empty()) {
    CCAP_TRY_DECLARE(committed_sum, sum_values(committed_parts));
    result.breakdown.committed = committed_sum;
  }

  std::vector<EvidenceId> observation_ids;
  CCAP_TRY_DECLARE(observed, observed_for(state, plant->id, result.medium, observation_ids));
  result.breakdown.observed = observed;
  CCAP_TRY(finalise_breakdown(result.breakdown));

  result.explanation.emplace_back(
      ExplanationCode::PlantShared,
      std::vector<ExplanationParam>{
          param("plant", plant->id.str()),
          param("loops", static_cast<std::uint64_t>(result.served_loops.size())),
          param("usable", result.breakdown.usable.to_string()),
          param("committed", result.breakdown.committed.to_string()),
          param("headroom", result.breakdown.reserve.to_string())});
  if (result.breakdown.over_committed) {
    result.explanation.emplace_back(
        ExplanationCode::OverCommitted,
        std::vector<ExplanationParam>{param("scope", std::string("plant ") + plant->id.str()),
                                      param("deficit", result.breakdown.deficit.to_string())});
  }
  return result;
}

Result<ZoneCapacity> CoolingAnalyzer::zone_capacity(ZoneId zone_id, CoolingMedium medium,
                                                    CompatibilityClass compatibility) const {
  const ZoneRecord* zone = snapshot_.find_zone(zone_id);
  if (zone == nullptr) {
    return Error(ErrorCode::NotFound, "zone is not present in this generation")
        .with("zone", zone_id.str())
        .with("generation", snapshot_.generation().to_string());
  }
  if (medium_of(compatibility) != medium) {
    return Error(ErrorCode::IncompatibleMedium,
                 "compatibility class does not belong to the requested medium")
        .with("medium", std::string(to_string(medium)))
        .with("class", std::string(to_string(compatibility)));
  }
  const AnalysisState state(snapshot_, evaluated_at_, basis_);

  ZoneCapacity result;
  result.zone = zone->id;
  result.medium = medium;
  result.compatibility = compatibility;
  result.breakdown.medium = medium;
  result.explanation.emplace_back(
      ExplanationCode::SnapshotBasis,
      std::vector<ExplanationParam>{param("generation", snapshot_.generation().value()),
                                    param("basis", std::string(to_string(basis_))),
                                    param("evaluated_at", evaluated_at_.to_string())});

  std::string classes;
  for (std::size_t index = 0; index < zone->compatibility.size(); ++index) {
    if (index != 0) {
      classes.push_back(',');
    }
    classes.append(to_string(zone->compatibility[index]));
  }
  result.explanation.emplace_back(
      ExplanationCode::ZoneResolved,
      std::vector<ExplanationParam>{param("zone", zone->id.str()),
                                    param("facility", zone->facility.str()),
                                    param("media", zone->media.to_string()),
                                    param("classes", classes)});

  const auto unsupported = [&](CapacityReason reason) {
    result.breakdown.usable = CapacityValue::unsupported(reason);
    result.local_headroom = CapacityValue::unsupported(reason);
    result.offered = CapacityValue::unsupported(reason);
    result.breakdown.committed = CapacityValue::known(ThermalPower::from_milliwatts(0));
    result.breakdown.observed = CapacityValue::unknown(CapacityReason::NoObservation);
    result.bottleneck = Bottleneck{BottleneckKind::ZoneMediumUnsupported, zone->id.value(),
                                   result.breakdown.usable, ThermalPower::from_milliwatts(0),
                                   ThermalPower::from_milliwatts(0)};
    return result;
  };

  if (!zone->media.contains(medium)) {
    result.explanation.emplace_back(
        ExplanationCode::MediumUnsupported,
        std::vector<ExplanationParam>{param("zone", zone->id.str()),
                                      param("medium", std::string(to_string(medium)))});
    return unsupported(CapacityReason::MediumNotSupported);
  }
  if (std::find(zone->compatibility.begin(), zone->compatibility.end(), compatibility) ==
      zone->compatibility.end()) {
    result.explanation.emplace_back(
        ExplanationCode::ClassUnsupported,
        std::vector<ExplanationParam>{param("zone", zone->id.str()),
                                      param("class", std::string(to_string(compatibility))),
                                      param("medium", std::string(to_string(medium)))});
    return unsupported(CapacityReason::ClassNotSupported);
  }

  std::vector<LoopId> serving;
  for (const LoopId& loop_id : zone->loops) {
    const LoopRecord* loop = snapshot_.find_loop(loop_id);
    if (loop == nullptr) {
      continue;
    }
    if (!serves(loop->kind, compatibility)) {
      result.explanation.emplace_back(
          ExplanationCode::LoopClassMismatch,
          std::vector<ExplanationParam>{param("loop", loop_id.str()),
                                        param("class", std::string(to_string(compatibility)))});
      continue;
    }
    serving.push_back(loop_id);
  }
  if (serving.empty()) {
    result.explanation.emplace_back(
        ExplanationCode::MediumUnsupported,
        std::vector<ExplanationParam>{param("zone", zone->id.str()),
                                      param("medium", std::string(to_string(medium)))});
    return unsupported(CapacityReason::NotServedByAnyLoop);
  }

  std::vector<CapacityValue> local_parts;
  for (const LoopId& loop_id : serving) {
    CCAP_TRY_DECLARE(loop_capacity, this->loop_capacity(loop_id));
    CCAP_TRY_DECLARE(headroom, headroom_of(loop_capacity.breakdown.usable,
                                           loop_capacity.breakdown.committed));
    local_parts.push_back(headroom);
    result.loops.push_back(std::move(loop_capacity));
  }
  CCAP_TRY_DECLARE(local_headroom, sum_values(local_parts));
  result.local_headroom = local_headroom;

  std::vector<const LoopCapacity*> loop_views;
  loop_views.reserve(result.loops.size());
  for (const LoopCapacity& view : result.loops) {
    loop_views.push_back(&view);
  }
  fill_breakdown_from_loops(result.breakdown, loop_views);

  std::vector<std::string> states;
  CCAP_TRY_DECLARE(
      committed,
      committed_for(snapshot_,
                    [&zone_id, medium, compatibility](const CommitmentRecord& commitment) {
                      return commitment.zone == zone_id && commitment.medium == medium &&
                             commitment.compatibility == compatibility;
                    },
                    states));
  result.breakdown.committed = committed;

  std::vector<EvidenceId> observation_ids;
  CCAP_TRY_DECLARE(observed, observed_for(state, zone->id, medium, observation_ids));
  result.breakdown.observed = observed;
  CCAP_TRY(finalise_breakdown(result.breakdown));

  // Shared pools. A pool's headroom belongs to every zone drawing on it, so it
  // is reported once and the zone's offer is the smaller of its own headroom
  // and the tightest pool it depends on.
  std::vector<PlantId> pool_ids;
  for (const LoopCapacity& view : result.loops) {
    const LoopRecord* loop = snapshot_.find_loop(view.loop);
    if (loop == nullptr) {
      continue;
    }
    for (const std::optional<PlantId>& candidate : {loop->primary_plant, loop->secondary_plant}) {
      if (candidate.has_value() &&
          std::find(pool_ids.begin(), pool_ids.end(), *candidate) == pool_ids.end()) {
        pool_ids.push_back(*candidate);
      }
    }
    if (loop->secondary_plant.has_value() && loop->primary_plant.has_value()) {
      result.explanation.emplace_back(
          ExplanationCode::PlantSupplyAlternates,
          std::vector<ExplanationParam>{param("loop", loop->id.str()),
                                        param("primary", loop->primary_plant->str()),
                                        param("secondary", loop->secondary_plant->str())});
    }
  }
  std::sort(pool_ids.begin(), pool_ids.end());

  std::vector<CapacityValue> pool_headrooms;
  for (const PlantId& plant_id : pool_ids) {
    CCAP_TRY_DECLARE(plant_capacity, this->plant_capacity(plant_id));
    SharedPoolView pool;
    pool.plant = plant_id;
    pool.medium = plant_capacity.medium;
    pool.usable = plant_capacity.breakdown.usable;
    pool.committed = plant_capacity.breakdown.committed;
    pool.served_loops = plant_capacity.served_loops;
    pool.redundancy = plant_capacity.redundancy;
    CCAP_TRY_DECLARE(headroom, headroom_of(pool.usable, pool.committed));
    CCAP_TRY_DECLARE(offered_pool,
                     apply_reserve(headroom.is_known() ? headroom.amount_or_zero()
                                                       : ThermalPower::from_milliwatts(0),
                                   snapshot_.policy().shared_pool_reserve));
    if (headroom.is_known()) {
      pool.headroom = CapacityValue::known(offered_pool);
    } else {
      pool.headroom = headroom;
    }
    pool.bottleneck = Bottleneck{BottleneckKind::SharedPool, plant_id.value(), pool.usable,
                                 pool.committed.amount_or_zero(),
                                 pool.headroom.amount_or_zero()};
    pool_headrooms.push_back(pool.headroom);
    result.shared_pools.push_back(std::move(pool));
  }

  std::optional<CapacityValue> shared_headroom;
  if (!pool_headrooms.empty()) {
    CCAP_TRY_DECLARE(tightest, min_values(pool_headrooms));
    shared_headroom = tightest;
    result.shared_headroom = tightest;
    result.shared_headroom_is_shared = true;
    result.explanation.emplace_back(
        ExplanationCode::SharedHeadroomIsShared,
        std::vector<ExplanationParam>{param("plants", static_cast<std::uint64_t>(pool_ids.size()))});
  }

  std::vector<CapacityValue> offers;
  offers.push_back(local_headroom);
  if (shared_headroom.has_value()) {
    offers.push_back(*shared_headroom);
  }
  CCAP_TRY_DECLARE(raw_offer, min_values(offers));

  Fraction effective_reserve = snapshot_.policy().minimum_reserve;
  if (zone->reserve_floor > effective_reserve) {
    effective_reserve = zone->reserve_floor;
  }
  if (raw_offer.is_known()) {
    CCAP_TRY_DECLARE(offered, apply_reserve(raw_offer.amount_or_zero(), effective_reserve));
    result.offered = CapacityValue::known(offered);
    if (!effective_reserve.is_zero()) {
      result.explanation.emplace_back(
          ExplanationCode::ReserveHeld,
          std::vector<ExplanationParam>{param("scope", std::string("zone ") + zone->id.str()),
                                        param("reserve", effective_reserve.to_string()),
                                        param("before", raw_offer.to_string()),
                                        param("after", result.offered.to_string())});
    }
  } else {
    result.offered = raw_offer;
  }

  result.explanation.emplace_back(
      ExplanationCode::ZoneLocalHeadroom,
      std::vector<ExplanationParam>{
          param("zone", zone->id.str()), param("local", local_headroom.to_string()),
          param("loops", static_cast<std::uint64_t>(serving.size()))});

  std::vector<Bottleneck> candidates;
  if (!raw_offer.is_known()) {
    candidates.push_back(Bottleneck{BottleneckKind::UnknownCapacity, zone->id.value(), raw_offer,
                                    ThermalPower::from_milliwatts(0),
                                    ThermalPower::from_milliwatts(0)});
  } else {
    for (const LoopCapacity& view : result.loops) {
      CCAP_TRY_DECLARE(headroom,
                       headroom_of(view.breakdown.usable, view.breakdown.committed));
      const BottleneckKind kind = view.breakdown.committed.is_known() &&
                                          !view.breakdown.committed.amount_or_zero().is_zero()
                                      ? BottleneckKind::LoopCommittedLoad
                                      : (view.bottleneck.has_value() ? view.bottleneck->kind
                                                                     : BottleneckKind::LoopEquipment);
      candidates.push_back(Bottleneck{kind, view.loop.value(), view.breakdown.usable,
                                      view.breakdown.committed.amount_or_zero(),
                                      headroom.is_known() ? headroom.amount_or_zero()
                                                          : ThermalPower::from_milliwatts(0)});
    }
    for (const SharedPoolView& pool : result.shared_pools) {
      candidates.push_back(Bottleneck{BottleneckKind::SharedPool, pool.plant.value(), pool.usable,
                                      pool.committed.amount_or_zero(),
                                      pool.headroom.amount_or_zero()});
    }
    result.bottleneck = choose_bottleneck(candidates);
    if (result.bottleneck.has_value()) {
      result.explanation.emplace_back(
          ExplanationCode::Bottleneck,
          std::vector<ExplanationParam>{
              param("kind", std::string(to_string(result.bottleneck->kind))),
              param("subject", result.bottleneck->subject.str()),
              param("limit", result.bottleneck->limit.to_string()),
              param("consumed", result.bottleneck->consumed.to_string()),
              param("remaining", result.bottleneck->remaining.to_string())});
    }
  }

  if (!states.empty()) {
    std::string joined;
    for (std::size_t index = 0; index < states.size(); ++index) {
      if (index != 0) {
        joined.push_back(',');
      }
      joined.append(states[index]);
    }
    result.explanation.emplace_back(
        ExplanationCode::CommittedLoadCharged,
        std::vector<ExplanationParam>{param("scope", std::string("zone ") + zone->id.str()),
                                      param("committed", committed.to_string()),
                                      param("states", joined)});
  }
  if (result.breakdown.over_committed) {
    result.explanation.emplace_back(
        ExplanationCode::OverCommitted,
        std::vector<ExplanationParam>{param("scope", std::string("zone ") + zone->id.str()),
                                      param("deficit", result.breakdown.deficit.to_string())});
  }
  return result;
}

Result<CoolingCapacityReport> CoolingAnalyzer::report() const {
  CoolingCapacityReport report;
  report.generation = snapshot_.generation();
  report.policy = snapshot_.policy().id;
  report.policy_revision = snapshot_.policy().revision;
  report.evaluated_at = evaluated_at_;
  report.basis = basis_;
  report.explanation.emplace_back(
      ExplanationCode::SnapshotBasis,
      std::vector<ExplanationParam>{param("generation", snapshot_.generation().value()),
                                    param("basis", std::string(to_string(basis_))),
                                    param("evaluated_at", evaluated_at_.to_string())});
  report.explanation.emplace_back(
      ExplanationCode::PolicyApplied,
      std::vector<ExplanationParam>{
          param("policy", snapshot_.policy().id.str()),
          param("revision", snapshot_.policy().revision.value()),
          param("stale_action", std::string(to_string(snapshot_.policy().stale_action))),
          param("unknown_action", std::string(to_string(snapshot_.policy().unknown_action)))});

  for (const PlantRecord& plant : snapshot_.plants()) {
    CCAP_TRY_DECLARE(view, plant_capacity(plant.id));
    report.plants.push_back(std::move(view));
  }
  for (const LoopRecord& loop : snapshot_.loops()) {
    CCAP_TRY_DECLARE(view, loop_capacity(loop.id));
    report.loops.push_back(std::move(view));
  }
  for (const ZoneRecord& zone : snapshot_.zones()) {
    for (const CompatibilityClass value : zone.compatibility) {
      CCAP_TRY_DECLARE(view, zone_capacity(zone.id, medium_of(value), value));
      report.zones.push_back(std::move(view));
    }
  }
  return report;
}

}  // namespace cooling_capacity
