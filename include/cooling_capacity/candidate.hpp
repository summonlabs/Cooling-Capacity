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

#ifndef COOLING_CAPACITY_CANDIDATE_HPP
#define COOLING_CAPACITY_CANDIDATE_HPP

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "cooling_capacity/accounting.hpp"
#include "cooling_capacity/explanation.hpp"
#include "cooling_capacity/ids.hpp"
#include "cooling_capacity/medium.hpp"
#include "cooling_capacity/rollup.hpp"
#include "cooling_capacity/units.hpp"

namespace cooling_capacity {

// A proposed thermal load asking whether a zone can carry it. This is the only
// placement-shaped question the library answers: it evaluates a load the caller
// has already chosen. It does not search for a zone.
struct CandidateLoad {
  ZoneId zone;
  CoolingMedium medium = CoolingMedium::Air;
  CompatibilityClass compatibility = CompatibilityClass::AirConvection;
  ThermalPower thermal;
  RedundancyClass required_redundancy = RedundancyClass::None;
  std::optional<LoopId> pinned_loop;
  ActorId actor;
  Timestamp requested_at;

  [[nodiscard]] Result<void> validate() const;
};

// Admitted, definitively rejected, or indeterminate because the accounting
// cannot be established. Indeterminate is not a soft rejection: it means the
// honest answer is "not known", and a caller that must decide has to supply
// fresh evidence rather than treat it as a yes.
enum class CandidateDisposition : std::uint8_t {
  Admitted = 0,
  Rejected = 1,
  Indeterminate = 2,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(CandidateDisposition disposition) noexcept;
[[nodiscard]] CCAP_EXPORT Result<CandidateDisposition> parse_candidate_disposition(
    std::string_view text);

enum class CandidateReason : std::uint8_t {
  None = 0,
  ZeroOrNegativeLoad = 1,
  ZoneNotFound = 2,
  MediumUnsupported = 3,
  ClassUnsupported = 4,
  LoopNotFound = 5,
  PinnedLoopNotServing = 6,
  PinnedLoopClassMismatch = 7,
  InsufficientLocalCapacity = 8,
  InsufficientSharedCapacity = 9,
  InsufficientRedundancy = 10,
  OverCommitted = 11,
  ReserveViolation = 12,
  UnknownCapacity = 13,
  UnknownOperatingState = 14,
  StaleEvidence = 15,
  RecoveredPendingRevalidation = 16,
  LoopTransportUndeclared = 17,
  FacilityMediumMismatch = 18,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(CandidateReason reason) noexcept;
[[nodiscard]] CCAP_EXPORT Result<CandidateReason> parse_candidate_reason(std::string_view text);

struct CandidateEvaluation {
  CandidateDisposition disposition = CandidateDisposition::Indeterminate;
  CandidateReason reason = CandidateReason::None;
  CandidateLoad request;
  CapacityGeneration generation;
  PolicyId policy;
  PolicyRevision policy_revision;
  Timestamp evaluated_at;
  AnalysisBasis basis = AnalysisBasis::Fresh;
  ThermalPower requested;
  ThermalPower available;
  CapacityValue offered;
  std::optional<Bottleneck> binding;
  std::vector<LoopId> serving_loops;
  std::vector<ExplanationStep> explanation;

  [[nodiscard]] bool admitted() const noexcept {
    return disposition == CandidateDisposition::Admitted;
  }
  [[nodiscard]] std::string to_string() const;
};

// Evaluates one candidate load against one analyzer. The analyzer carries the
// snapshot, the evaluation instant and the analysis basis, so the same request
// against the same generation and instant always produces the same answer.
[[nodiscard]] CCAP_EXPORT Result<CandidateEvaluation> evaluate_candidate(
    const CoolingAnalyzer& analyzer, const CandidateLoad& candidate);

}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_CANDIDATE_HPP
