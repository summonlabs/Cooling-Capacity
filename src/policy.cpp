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

#include "cooling_capacity/policy.hpp"

#include "enum_table.hpp"

namespace cooling_capacity {

namespace {

constexpr detail::EnumName<StaleEvidenceAction> kStaleActionNames[] = {
    {StaleEvidenceAction::Reject, "reject"},
    {StaleEvidenceAction::Use, "use"},
};

constexpr detail::EnumName<UnknownCapacityAction> kUnknownActionNames[] = {
    {UnknownCapacityAction::FailClosed, "fail-closed"},
    {UnknownCapacityAction::TreatAsZero, "treat-as-zero"},
};

constexpr detail::EnumName<CapacityBasisRequirement> kBasisRequirementNames[] = {
    {CapacityBasisRequirement::NominalAllowed, "nominal-allowed"},
    {CapacityBasisRequirement::ValidatedRequired, "validated-required"},
};

constexpr std::int64_t kMinuteMs = 60 * 1000;
constexpr std::int64_t kHourMs = 60 * kMinuteMs;
constexpr std::int64_t kDayMs = 24 * kHourMs;

}  // namespace

std::string_view to_string(StaleEvidenceAction action) noexcept {
  return detail::name_from_table(kStaleActionNames, action);
}

Result<StaleEvidenceAction> parse_stale_evidence_action(std::string_view text) {
  return detail::parse_from_table("stale evidence action", kStaleActionNames, text);
}

std::string_view to_string(UnknownCapacityAction action) noexcept {
  return detail::name_from_table(kUnknownActionNames, action);
}

Result<UnknownCapacityAction> parse_unknown_capacity_action(std::string_view text) {
  return detail::parse_from_table("unknown capacity action", kUnknownActionNames, text);
}

std::string_view to_string(CapacityBasisRequirement requirement) noexcept {
  return detail::name_from_table(kBasisRequirementNames, requirement);
}

Result<CapacityBasisRequirement> parse_capacity_basis_requirement(std::string_view text) {
  return detail::parse_from_table("capacity basis requirement", kBasisRequirementNames, text);
}

CoolingPolicy CoolingPolicy::defaults() {
  CoolingPolicy policy;
  policy.id = PolicyId::literal("default-policy");
  policy.revision = PolicyRevision::first();
  policy.freshness = {
      FreshnessRule{EvidenceKind::Nameplate, DurationMs::no_expiry()},
      FreshnessRule{EvidenceKind::CapacityValidation, DurationMs::from_milliseconds(365 * kDayMs)},
      FreshnessRule{EvidenceKind::LoadMeasurement, DurationMs::from_milliseconds(5 * kMinuteMs)},
      FreshnessRule{EvidenceKind::Telemetry, DurationMs::from_milliseconds(5 * kMinuteMs)},
      FreshnessRule{EvidenceKind::MaintenanceOrder, DurationMs::from_milliseconds(kDayMs)},
      FreshnessRule{EvidenceKind::EnvironmentalConstraint, DurationMs::from_milliseconds(kDayMs)},
      FreshnessRule{EvidenceKind::TransportSurvey, DurationMs::from_milliseconds(365 * kDayMs)},
      FreshnessRule{EvidenceKind::DegradationAssessment, DurationMs::from_milliseconds(30 * kDayMs)},
      FreshnessRule{EvidenceKind::RedundancyDeclaration, DurationMs::no_expiry()},
      FreshnessRule{EvidenceKind::OperatorAssertion, DurationMs::from_milliseconds(kDayMs)},
      FreshnessRule{EvidenceKind::VendorBulletin, DurationMs::no_expiry()},
  };
  return policy;
}

Result<DurationMs> CoolingPolicy::max_age_for(EvidenceKind kind) const {
  for (const FreshnessRule& rule : freshness) {
    if (rule.kind == kind) {
      return rule.max_age;
    }
  }
  return Error(ErrorCode::NotFound, "policy has no freshness rule for this evidence kind")
      .with("evidence_kind", std::string(to_string(kind)));
}

Result<void> CoolingPolicy::validate() const {
  if (id.empty()) {
    return Error(ErrorCode::InvalidArgument, "policy has no identifier");
  }
  if (revision.value() == 0) {
    return Error(ErrorCode::InvalidArgument, "policy revision must be at least one")
        .with("policy", id.str());
  }
  if (freshness.size() != kEvidenceKindCount) {
    return Error(ErrorCode::InvalidArgument,
                 "policy must carry exactly one freshness rule per evidence kind")
        .with("policy", id.str())
        .with("rules", std::to_string(freshness.size()))
        .with("expected", std::to_string(kEvidenceKindCount));
  }
  for (std::size_t index = 0; index < freshness.size(); ++index) {
    const FreshnessRule& rule = freshness[index];
    if (static_cast<std::size_t>(rule.kind) != index) {
      return Error(ErrorCode::InvalidArgument,
                   "policy freshness rules must be ordered by evidence kind with no duplicates")
          .with("policy", id.str())
          .with("position", std::to_string(index));
    }
    if (rule.max_age.is_negative()) {
      return Error(ErrorCode::InvalidArgument, "policy freshness window cannot be negative")
          .with("evidence_kind", std::string(to_string(rule.kind)));
    }
  }
  if (!minimum_reserve.in_unit_interval()) {
    return Error(ErrorCode::InvalidArgument, "policy minimum reserve is outside [0, 1]")
        .with("value", minimum_reserve.to_string());
  }
  if (!minimum_cumulative_derate.in_unit_interval()) {
    return Error(ErrorCode::InvalidArgument,
                 "policy minimum cumulative derate is outside [0, 1]")
        .with("value", minimum_cumulative_derate.to_string());
  }
  if (!shared_pool_reserve.in_unit_interval()) {
    return Error(ErrorCode::InvalidArgument, "policy shared pool reserve is outside [0, 1]")
        .with("value", shared_pool_reserve.to_string());
  }
  if (!reconciliation_tolerance.in_unit_interval()) {
    return Error(ErrorCode::InvalidArgument, "policy reconciliation tolerance is outside [0, 1]")
        .with("value", reconciliation_tolerance.to_string());
  }
  return Result<void>();
}

bool operator==(const CoolingPolicy& lhs, const CoolingPolicy& rhs) {
  return lhs.id == rhs.id && lhs.revision == rhs.revision && lhs.freshness == rhs.freshness &&
         lhs.stale_action == rhs.stale_action && lhs.unknown_action == rhs.unknown_action &&
         lhs.basis_requirement == rhs.basis_requirement &&
         lhs.minimum_reserve == rhs.minimum_reserve &&
         lhs.minimum_cumulative_derate == rhs.minimum_cumulative_derate &&
         lhs.shared_pool_reserve == rhs.shared_pool_reserve &&
         lhs.require_transport_declaration == rhs.require_transport_declaration &&
         lhs.require_revalidation_after_recovery == rhs.require_revalidation_after_recovery &&
         lhs.reconciliation_tolerance == rhs.reconciliation_tolerance;
}

}  // namespace cooling_capacity
