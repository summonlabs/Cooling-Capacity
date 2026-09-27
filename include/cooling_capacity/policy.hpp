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

#ifndef COOLING_CAPACITY_POLICY_HPP
#define COOLING_CAPACITY_POLICY_HPP

#include <cstdint>
#include <string_view>
#include <vector>

#include "cooling_capacity/evidence.hpp"
#include "cooling_capacity/ids.hpp"
#include "cooling_capacity/text.hpp"
#include "cooling_capacity/units.hpp"

namespace cooling_capacity {

// What to do with evidence that has left its freshness window. `Reject` is the
// default and the conservative answer: an expired measurement makes the
// quantity Unknown. `Use` is an explicit operator override, recorded in the
// explanation of every answer it affects.
enum class StaleEvidenceAction : std::uint8_t {
  Reject = 0,
  Use = 1,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(StaleEvidenceAction action) noexcept;
[[nodiscard]] CCAP_EXPORT Result<StaleEvidenceAction> parse_stale_evidence_action(
    std::string_view text);

// What to do when a capacity constituent cannot be determined. `FailClosed`
// keeps the answer Unknown, which makes a candidate load Indeterminate rather
// than admitted. `TreatAsZero` is the explicit alternative for operators who
// would rather under-count than refuse to answer.
enum class UnknownCapacityAction : std::uint8_t {
  FailClosed = 0,
  TreatAsZero = 1,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(UnknownCapacityAction action) noexcept;
[[nodiscard]] CCAP_EXPORT Result<UnknownCapacityAction> parse_unknown_capacity_action(
    std::string_view text);

// Which capacity figure may be used as the basis of a usable capacity.
enum class CapacityBasisRequirement : std::uint8_t {
  // The nameplate value may be used when no validation exists.
  NominalAllowed = 0,
  // Only a commissioning- or validation-backed figure may be used; equipment
  // without one contributes Unknown.
  ValidatedRequired = 1,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(CapacityBasisRequirement requirement) noexcept;
[[nodiscard]] CCAP_EXPORT Result<CapacityBasisRequirement> parse_capacity_basis_requirement(
    std::string_view text);

// One freshness window. A window of `DurationMs::no_expiry()` means the kind
// never ages out, which is the right answer for a nameplate.
struct FreshnessRule {
  EvidenceKind kind = EvidenceKind::OperatorAssertion;
  DurationMs max_age = DurationMs::no_expiry();

  friend bool operator==(const FreshnessRule& lhs, const FreshnessRule& rhs) {
    return lhs.kind == rhs.kind && lhs.max_age == rhs.max_age;
  }
};

// The complete set of assumptions under which capacity is computed. A policy is
// a record in the snapshot, with an identity and a revision, so an answer can
// always name the policy it used.
struct CoolingPolicy {
  PolicyId id;
  PolicyRevision revision;
  std::vector<FreshnessRule> freshness;
  StaleEvidenceAction stale_action = StaleEvidenceAction::Reject;
  UnknownCapacityAction unknown_action = UnknownCapacityAction::FailClosed;
  CapacityBasisRequirement basis_requirement = CapacityBasisRequirement::NominalAllowed;
  // Fraction of headroom held back at every zone before capacity is offered.
  Fraction minimum_reserve = Fraction::zero();
  // When greater than zero, equipment whose combined derate falls below this
  // fraction is treated as Unavailable rather than deeply derated.
  Fraction minimum_cumulative_derate = Fraction::zero();
  // Fraction of a shared pool held back before shared headroom is offered.
  Fraction shared_pool_reserve = Fraction::zero();
  // When true, a loop without a declared transport limit has Unknown capacity.
  bool require_transport_declaration = true;
  // When true, a snapshot recovered from a durable store may be read and
  // reported but may not admit a new load until it has been revalidated against
  // current evidence. Persisted state never becomes current by being loaded.
  bool require_revalidation_after_recovery = true;
  // How far the observed load may differ from the committed load, as a fraction
  // of the committed load, before the two are reported as inconsistent.
  Fraction reconciliation_tolerance = Fraction::from_parts_per_million(100000);

  friend bool operator==(const CoolingPolicy& lhs, const CoolingPolicy& rhs);

  // A policy with a rule for every evidence kind and conservative defaults.
  [[nodiscard]] static CoolingPolicy defaults();

  [[nodiscard]] Result<DurationMs> max_age_for(EvidenceKind kind) const;
  [[nodiscard]] Result<void> validate() const;
};

}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_POLICY_HPP
