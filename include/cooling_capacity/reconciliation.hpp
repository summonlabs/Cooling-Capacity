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

#ifndef COOLING_CAPACITY_RECONCILIATION_HPP
#define COOLING_CAPACITY_RECONCILIATION_HPP

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

// How the committed thermal load compares with the load actually observed.
// This is an internal cross-check of the accounting against evidence, not a
// facility-wide reconciliation service: it answers "does what we have handed
// out match what the sensors see?" and nothing else.
enum class LoadStatus : std::uint8_t {
  Consistent = 0,
  OverCommitted = 1,
  UnderCommitted = 2,
  Unknown = 3,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(LoadStatus status) noexcept;

struct LoadReconciliation {
  std::optional<ZoneId> zone;
  std::optional<LoopId> loop;
  CoolingMedium medium = CoolingMedium::Air;
  CapacityValue committed;
  CapacityValue observed;
  CapacityValue delta;
  LoadStatus status = LoadStatus::Unknown;
  std::vector<EvidenceId> observations;
  std::vector<ExplanationStep> explanation;

  friend bool operator==(const LoadReconciliation& lhs, const LoadReconciliation& rhs);
};

struct ReconciliationReport {
  CapacityGeneration generation;
  PolicyId policy;
  PolicyRevision policy_revision;
  Timestamp evaluated_at;
  AnalysisBasis basis = AnalysisBasis::Fresh;
  std::vector<LoadReconciliation> loops;
  std::vector<LoadReconciliation> zones;
  std::vector<ExplanationStep> explanation;
};

// Cross-checks the committed load against the load actually observed, per loop
// and per zone. This is an accounting integrity check inside the cooling
// capacity model; it is not a facility-wide reconciliation service.
[[nodiscard]] CCAP_EXPORT Result<ReconciliationReport> reconcile(const CoolingAnalyzer& analyzer);

}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_RECONCILIATION_HPP
