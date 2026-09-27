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

#include "test_harness.hpp"

#include "cooling_capacity/policy.hpp"

namespace {

using cooling_capacity::CapacityBasisRequirement;
using cooling_capacity::CoolingPolicy;
using cooling_capacity::DurationMs;
using cooling_capacity::ErrorCode;
using cooling_capacity::EvidenceKind;
using cooling_capacity::FreshnessRule;
using cooling_capacity::StaleEvidenceAction;
using cooling_capacity::UnknownCapacityAction;

}  // namespace

CCAP_TEST(policy_defaults_are_complete_and_valid) {
  const CoolingPolicy policy = CoolingPolicy::defaults();
  CCAP_CHECK_VOID(policy.validate());
  CCAP_CHECK_EQ(policy.freshness.size(), cooling_capacity::kEvidenceKindCount);
  CCAP_CHECK_EQ(policy.stale_action, StaleEvidenceAction::Reject);
  CCAP_CHECK_EQ(policy.unknown_action, UnknownCapacityAction::FailClosed);
  CCAP_CHECK_EQ(policy.basis_requirement, CapacityBasisRequirement::NominalAllowed);
  CCAP_CHECK(policy.minimum_reserve.is_zero());
  CCAP_CHECK(policy.require_transport_declaration);
  CCAP_CHECK(policy.require_revalidation_after_recovery);
  for (std::size_t index = 0; index < cooling_capacity::kEvidenceKindCount; ++index) {
    CCAP_CHECK_OK(window, policy.max_age_for(cooling_capacity::evidence_kind_at(index)));
    (void)window;
  }
}

CCAP_TEST(policy_nameplate_never_expires_but_telemetry_does) {
  const CoolingPolicy policy = CoolingPolicy::defaults();
  CCAP_CHECK_OK(nameplate, policy.max_age_for(EvidenceKind::Nameplate));
  CCAP_CHECK(nameplate.never_expires());
  CCAP_CHECK_OK(telemetry, policy.max_age_for(EvidenceKind::Telemetry));
  CCAP_CHECK_FALSE(telemetry.never_expires());
  CCAP_CHECK(telemetry < DurationMs::from_milliseconds(3600000));
}

CCAP_TEST(policy_rejects_missing_freshness_rules) {
  CoolingPolicy policy = CoolingPolicy::defaults();
  policy.freshness.pop_back();
  CCAP_CHECK_ERR(policy.validate(), ErrorCode::InvalidArgument);
}

CCAP_TEST(policy_rejects_unordered_or_duplicated_rules) {
  CoolingPolicy policy = CoolingPolicy::defaults();
  std::swap(policy.freshness[0], policy.freshness[1]);
  CCAP_CHECK_ERR(policy.validate(), ErrorCode::InvalidArgument);

  CoolingPolicy duplicated = CoolingPolicy::defaults();
  duplicated.freshness[1] = duplicated.freshness[0];
  CCAP_CHECK_ERR(duplicated.validate(), ErrorCode::InvalidArgument);
}

CCAP_TEST(policy_rejects_out_of_range_fractions) {
  CoolingPolicy policy = CoolingPolicy::defaults();
  policy.minimum_reserve = cooling_capacity::Fraction::from_parts_per_million(1000001);
  CCAP_CHECK_ERR(policy.validate(), ErrorCode::InvalidArgument);

  CoolingPolicy negative = CoolingPolicy::defaults();
  negative.shared_pool_reserve = cooling_capacity::Fraction::from_parts_per_million(-1);
  CCAP_CHECK_ERR(negative.validate(), ErrorCode::InvalidArgument);

  CoolingPolicy tolerance = CoolingPolicy::defaults();
  tolerance.reconciliation_tolerance =
      cooling_capacity::Fraction::from_parts_per_million(2000000);
  CCAP_CHECK_ERR(tolerance.validate(), ErrorCode::InvalidArgument);
}

CCAP_TEST(policy_rejects_an_empty_identity_or_zero_revision) {
  CoolingPolicy policy = CoolingPolicy::defaults();
  policy.id = cooling_capacity::PolicyId();
  CCAP_CHECK_ERR(policy.validate(), ErrorCode::InvalidArgument);

  CoolingPolicy unversioned = CoolingPolicy::defaults();
  unversioned.revision = cooling_capacity::PolicyRevision::from_value(0);
  CCAP_CHECK_ERR(unversioned.validate(), ErrorCode::InvalidArgument);
}

CCAP_TEST(policy_rejects_a_negative_window) {
  CoolingPolicy policy = CoolingPolicy::defaults();
  policy.freshness[0].max_age = DurationMs::from_milliseconds(-1);
  CCAP_CHECK_ERR(policy.validate(), ErrorCode::InvalidArgument);
}

CCAP_TEST(policy_lookup_reports_a_missing_rule) {
  CoolingPolicy policy = CoolingPolicy::defaults();
  policy.freshness.clear();
  CCAP_CHECK_ERR(policy.max_age_for(EvidenceKind::Telemetry), ErrorCode::NotFound);
}

CCAP_TEST(policy_enum_names_round_trip) {
  CCAP_CHECK_OK(stale, cooling_capacity::parse_stale_evidence_action("reject"));
  CCAP_CHECK_EQ(stale, StaleEvidenceAction::Reject);
  CCAP_CHECK_OK(unknown, cooling_capacity::parse_unknown_capacity_action("fail-closed"));
  CCAP_CHECK_EQ(unknown, UnknownCapacityAction::FailClosed);
  CCAP_CHECK_OK(basis, cooling_capacity::parse_capacity_basis_requirement("validated-required"));
  CCAP_CHECK_EQ(basis, CapacityBasisRequirement::ValidatedRequired);
  CCAP_CHECK_ERR(cooling_capacity::parse_stale_evidence_action("Reject"),
                 ErrorCode::InvalidEnumValue);
}
