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

#include "cooling_capacity/reconciliation.hpp"

#include <cstdlib>

#include "cooling_capacity/snapshot.hpp"
#include "enum_table.hpp"

namespace cooling_capacity {

namespace {

constexpr detail::EnumName<LoadStatus> kLoadStatusNames[] = {
    {LoadStatus::Consistent, "consistent"},
    {LoadStatus::OverCommitted, "over-committed"},
    {LoadStatus::UnderCommitted, "under-committed"},
    {LoadStatus::Unknown, "unknown"},
};

std::int64_t absolute(std::int64_t value) { return value < 0 ? -value : value; }

LoadReconciliation evaluate_entry(const CoolingAnalyzer& analyzer, std::optional<ZoneId> zone,
                                  std::optional<LoopId> loop, CoolingMedium medium,
                                  const CapacityBreakdown& breakdown,
                                  std::vector<EvidenceId> observations) {
  const CoolingPolicy& policy = analyzer.snapshot().policy();
  LoadReconciliation entry;
  entry.zone = std::move(zone);
  entry.loop = std::move(loop);
  entry.medium = medium;
  entry.committed = breakdown.committed;
  entry.observed = breakdown.observed;
  entry.observations = std::move(observations);

  if (!entry.committed.is_known() || !entry.observed.is_known()) {
    entry.status = LoadStatus::Unknown;
    entry.delta = CapacityValue::unknown(
        entry.committed.is_known() ? entry.observed.reason() : entry.committed.reason());
    const std::string scope = entry.loop.has_value() ? "loop " + entry.loop->str()
                                                     : "zone " + entry.zone->str();
    entry.explanation.emplace_back(
        ExplanationCode::NoObservation,
        std::vector<ExplanationParam>{param("scope", scope),
                                      param("medium", std::string(to_string(medium)))});
    return entry;
  }

  const std::int64_t committed = entry.committed.amount_or_zero().milliwatts();
  const std::int64_t observed = entry.observed.amount_or_zero().milliwatts();
  const std::int64_t difference = committed - observed;
  entry.delta = CapacityValue::known(ThermalPower::from_milliwatts(difference));

  const Result<ThermalPower> tolerance =
      scale(ThermalPower::from_milliwatts(committed), policy.reconciliation_tolerance);
  const std::int64_t tolerance_raw =
      tolerance.ok() ? tolerance.value().milliwatts() : 0;

  if (absolute(difference) <= tolerance_raw) {
    entry.status = LoadStatus::Consistent;
  } else if (difference > 0) {
    entry.status = LoadStatus::OverCommitted;
  } else {
    entry.status = LoadStatus::UnderCommitted;
  }

  const std::string scope = entry.loop.has_value() ? "loop " + entry.loop->str()
                                                   : "zone " + entry.zone->str();
  entry.explanation.emplace_back(
      ExplanationCode::ObservationCharged,
      std::vector<ExplanationParam>{
          param("scope", scope), param("observed", entry.observed.to_string()),
          param("evidence", entry.observations.empty() ? std::string("none")
                                                       : entry.observations.front().str())});
  entry.explanation.emplace_back(
      ExplanationCode::CommittedLoadCharged,
      std::vector<ExplanationParam>{param("scope", scope),
                                    param("committed", entry.committed.to_string()),
                                    param("states", std::string("held,committed"))});
  return entry;
}

}  // namespace

std::string_view to_string(LoadStatus status) noexcept {
  return detail::name_from_table(kLoadStatusNames, status);
}

bool operator==(const LoadReconciliation& lhs, const LoadReconciliation& rhs) {
  return lhs.zone == rhs.zone && lhs.loop == rhs.loop && lhs.medium == rhs.medium &&
         lhs.committed == rhs.committed && lhs.observed == rhs.observed &&
         lhs.delta == rhs.delta && lhs.status == rhs.status &&
         lhs.observations == rhs.observations && lhs.explanation == rhs.explanation;
}

Result<ReconciliationReport> reconcile(const CoolingAnalyzer& analyzer) {
  ReconciliationReport report;
  report.generation = analyzer.snapshot().generation();
  report.policy = analyzer.snapshot().policy().id;
  report.policy_revision = analyzer.snapshot().policy().revision;
  report.evaluated_at = analyzer.evaluated_at();
  report.basis = analyzer.basis();
  report.explanation.emplace_back(
      ExplanationCode::SnapshotBasis,
      std::vector<ExplanationParam>{
          param("generation", analyzer.snapshot().generation().value()),
          param("basis", std::string(to_string(analyzer.basis()))),
          param("evaluated_at", analyzer.evaluated_at().to_string())});

  for (const LoopRecord& loop : analyzer.snapshot().loops()) {
    CCAP_TRY_DECLARE(view, analyzer.loop_capacity(loop.id));
    std::vector<EvidenceId> observations;
    for (const EvidenceRecord& record : analyzer.snapshot().evidence()) {
      if (record.kind != EvidenceKind::LoadMeasurement && record.kind != EvidenceKind::Telemetry) {
        continue;
      }
      const auto* id = std::get_if<LoopId>(&record.subject);
      if (id != nullptr && *id == loop.id) {
        observations.push_back(record.id);
      }
    }
    report.loops.push_back(evaluate_entry(analyzer, std::nullopt, loop.id, view.medium,
                                          view.breakdown, std::move(observations)));
  }

  for (const ZoneRecord& zone : analyzer.snapshot().zones()) {
    for (const CompatibilityClass value : zone.compatibility) {
      CCAP_TRY_DECLARE(view, analyzer.zone_capacity(zone.id, medium_of(value), value));
      std::vector<EvidenceId> observations;
      for (const EvidenceRecord& record : analyzer.snapshot().evidence()) {
        if (record.kind != EvidenceKind::LoadMeasurement &&
            record.kind != EvidenceKind::Telemetry) {
          continue;
        }
        const auto* id = std::get_if<ZoneId>(&record.subject);
        if (id != nullptr && *id == zone.id) {
          observations.push_back(record.id);
        }
      }
      report.zones.push_back(evaluate_entry(analyzer, zone.id, std::nullopt, view.medium,
                                            view.breakdown, std::move(observations)));
    }
  }
  return report;
}

}  // namespace cooling_capacity
