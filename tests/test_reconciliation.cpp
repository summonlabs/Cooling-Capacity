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

#include "fixtures.hpp"
#include "test_harness.hpp"

using namespace ccap_test;

#include <optional>
#include <string>
#include <vector>

namespace {

using cooling_capacity::AnalysisBasis;
using cooling_capacity::CapacityValue;
using cooling_capacity::CommitmentRecord;
using cooling_capacity::CommitmentState;
using cooling_capacity::CoolingAnalyzer;
using cooling_capacity::CoolingMedium;
using cooling_capacity::CoolingSnapshot;
using cooling_capacity::EvidenceRecord;
using cooling_capacity::ExplanationCode;
using cooling_capacity::LoadReconciliation;
using cooling_capacity::LoadStatus;
using cooling_capacity::ReconciliationReport;
using cooling_capacity::SnapshotBuilder;

SnapshotBuilder facility_builder(const SimpleFacility& facility) {
  SnapshotBuilder builder;
  builder.set_generation(CapacityGeneration::first());
  builder.set_constructed_at(fixture_now());
  CCAP_CHECK_VOID(add_simple_facility(builder, facility));
  return builder;
}

EvidenceRecord load_measurement(const char* id, std::int64_t thermal_milliwatts) {
  EvidenceRecord record;
  record.id = evidence_id(id);
  record.kind = EvidenceKind::LoadMeasurement;
  record.source = EvidenceSource::BmsTelemetry;
  record.subject = zone_id("hall-1");
  record.medium = CoolingMedium::Air;
  record.value = milliwatts(thermal_milliwatts);
  record.observed_at = fixture_now();
  record.provenance.actor = actor_id("bms");
  record.provenance.reference = cooling_capacity::DocumentRef::literal("bms-1");
  record.revision = RecordRevision::first();
  return record;
}

CommitmentRecord commitment(const char* id, const char* attempt, std::int64_t thermal_milliwatts) {
  CommitmentRecord record;
  record.id = cooling_capacity::CommitmentId::literal(id);
  record.attempt = cooling_capacity::AttemptId::literal(attempt);
  record.zone = zone_id("hall-1");
  record.pinned_loop = loop_id("air-loop-1");
  record.medium = CoolingMedium::Air;
  record.compatibility = CompatibilityClass::AirConvection;
  record.thermal = milliwatts(thermal_milliwatts);
  record.state = CommitmentState::Committed;
  record.actor = actor_id("operator");
  record.created_at = fixture_now();
  record.created_generation = CapacityGeneration::first();
  record.revision = RecordRevision::first();
  return record;
}

Result<ReconciliationReport> reconcile_snapshot(const CoolingSnapshot& snapshot) {
  const CoolingAnalyzer analyzer(snapshot, fixture_now(), AnalysisBasis::Fresh);
  return cooling_capacity::reconcile(analyzer);
}

const LoadReconciliation* zone_entry(const ReconciliationReport& report) {
  for (const LoadReconciliation& entry : report.zones) {
    if (entry.loop.has_value()) {
      continue;
    }
    return &entry;
  }
  return nullptr;
}

const LoadReconciliation* loop_entry(const ReconciliationReport& report) {
  for (const LoadReconciliation& entry : report.loops) {
    if (!entry.loop.has_value()) {
      continue;
    }
    return &entry;
  }
  return nullptr;
}

}  // namespace

CCAP_TEST(a_matching_commitment_and_observation_reconcile_as_consistent) {
  SnapshotBuilder builder = facility_builder(simple_facility());
  CCAP_CHECK_VOID(builder.add(commitment("c-1", "a-1", 120000000)));
  CCAP_CHECK_VOID(builder.add(load_measurement("load-1", 120000000)));
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);

  CCAP_CHECK_OK(report, reconcile_snapshot(*snapshot));
  CCAP_CHECK_EQ(report.generation, snapshot->generation());
  CCAP_CHECK_EQ(report.policy.str(), std::string("default-policy"));
  CCAP_CHECK_EQ(report.policy_revision, snapshot->policy().revision);
  CCAP_CHECK_EQ(report.evaluated_at, fixture_now());
  CCAP_CHECK_EQ(report.basis, AnalysisBasis::Fresh);
  CCAP_CHECK_EQ(report.explanation.size(), 1U);
  CCAP_CHECK_EQ(report.explanation.front().code(), ExplanationCode::SnapshotBasis);
  CCAP_CHECK_EQ(report.loops.size(), 1U);
  CCAP_CHECK_EQ(report.zones.size(), 1U);

  const LoadReconciliation* zone = zone_entry(report);
  CCAP_CHECK(zone != nullptr);
  if (zone != nullptr) {
    CCAP_CHECK(zone->zone.has_value());
    CCAP_CHECK_EQ(zone->zone->str(), std::string("hall-1"));
    CCAP_CHECK_FALSE(zone->loop.has_value());
    CCAP_CHECK_EQ(zone->medium, CoolingMedium::Air);
    CCAP_CHECK_EQ(zone->status, LoadStatus::Consistent);
    CCAP_CHECK(zone->committed.is_known());
    CCAP_CHECK_EQ(zone->committed.amount_or_zero().milliwatts(), 120000000);
    CCAP_CHECK(zone->observed.is_known());
    CCAP_CHECK_EQ(zone->observed.amount_or_zero().milliwatts(), 120000000);
    CCAP_CHECK(zone->delta.is_known());
    CCAP_CHECK_EQ(zone->delta.amount_or_zero().milliwatts(), 0);
    CCAP_CHECK_EQ(zone->observations.size(), 1U);
    if (!zone->observations.empty()) {
      CCAP_CHECK_EQ(zone->observations.front().str(), std::string("load-1"));
    }
    const std::string rendered = cooling_capacity::render_steps(zone->explanation);
    CCAP_CHECK(rendered.find("observation-charged:") != std::string::npos);
    CCAP_CHECK(rendered.find("committed-load-charged:") != std::string::npos);
    CCAP_CHECK(rendered.find("<missing:") == std::string::npos);
  }

  // The loop carries the same commitment, but nothing has observed it.
  const LoadReconciliation* loop = loop_entry(report);
  CCAP_CHECK(loop != nullptr);
  if (loop != nullptr) {
    CCAP_CHECK(loop->loop.has_value());
    CCAP_CHECK_EQ(loop->loop->str(), std::string("air-loop-1"));
    CCAP_CHECK_FALSE(loop->zone.has_value());
    CCAP_CHECK_EQ(loop->committed.amount_or_zero().milliwatts(), 120000000);
    CCAP_CHECK_EQ(loop->status, LoadStatus::Unknown);
    CCAP_CHECK_EQ(loop->observations.size(), 0U);
  }
  CCAP_CHECK_EQ(cooling_capacity::to_string(LoadStatus::Consistent), std::string_view("consistent"));
  CCAP_CHECK_EQ(cooling_capacity::to_string(LoadStatus::UnderCommitted),
                std::string_view("under-committed"));
}

CCAP_TEST(no_observation_makes_the_entry_unknown_not_zero) {
  SnapshotBuilder builder = facility_builder(simple_facility());
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);

  CCAP_CHECK_OK(report, reconcile_snapshot(*snapshot));
  const LoadReconciliation* zone = zone_entry(report);
  CCAP_CHECK(zone != nullptr);
  if (zone != nullptr) {
    CCAP_CHECK_EQ(zone->status, LoadStatus::Unknown);
    CCAP_CHECK_FALSE(zone->observed.is_known());
    CCAP_CHECK_EQ(zone->observed.status(), cooling_capacity::CapacityStatus::Unknown);
    CCAP_CHECK_EQ(zone->observed.reason(), cooling_capacity::CapacityReason::NoObservation);
    CCAP_CHECK_FALSE(zone->delta.is_known());
    CCAP_CHECK(zone->committed.is_known());
    CCAP_CHECK_EQ(zone->committed.amount_or_zero().milliwatts(), 0);
    CCAP_CHECK_EQ(zone->observations.size(), 0U);

    CCAP_CHECK_EQ(zone->explanation.size(), 1U);
    CCAP_CHECK_EQ(zone->explanation.front().code(), ExplanationCode::NoObservation);
    const std::string rendered = cooling_capacity::render_steps(zone->explanation);
    CCAP_CHECK(rendered.find("no-observation:") != std::string::npos);
    CCAP_CHECK(rendered.find("no fresh observation exists for zone hall-1 on medium air") !=
               std::string::npos);
    CCAP_CHECK(rendered.find("<missing:") == std::string::npos);
  }

  const LoadReconciliation* loop = loop_entry(report);
  CCAP_CHECK(loop != nullptr);
  if (loop != nullptr) {
    CCAP_CHECK_EQ(loop->status, LoadStatus::Unknown);
    CCAP_CHECK_EQ(loop->explanation.size(), 1U);
  }
}

CCAP_TEST(an_observation_above_the_commitment_is_under_committed) {
  SnapshotBuilder builder = facility_builder(simple_facility());
  CCAP_CHECK_VOID(builder.add(load_measurement("load-1", 150000000)));
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);

  CCAP_CHECK_OK(report, reconcile_snapshot(*snapshot));
  const LoadReconciliation* zone = zone_entry(report);
  CCAP_CHECK(zone != nullptr);
  if (zone != nullptr) {
    CCAP_CHECK_EQ(zone->status, LoadStatus::UnderCommitted);
    CCAP_CHECK_EQ(zone->committed.amount_or_zero().milliwatts(), 0);
    CCAP_CHECK_EQ(zone->observed.amount_or_zero().milliwatts(), 150000000);
    // The observed load exceeds what was handed out, so the delta is negative.
    CCAP_CHECK(zone->delta.is_known());
    CCAP_CHECK_EQ(zone->delta.amount_or_zero().milliwatts(), -150000000);
    CCAP_CHECK_EQ(zone->explanation.size(), 2U);
    CCAP_CHECK_EQ(zone->explanation.front().code(), ExplanationCode::ObservationCharged);
  }
}

CCAP_TEST(an_observation_below_the_commitment_is_over_committed) {
  SnapshotBuilder builder = facility_builder(simple_facility());
  CCAP_CHECK_VOID(builder.add(commitment("c-1", "a-1", 500000000)));
  CCAP_CHECK_VOID(builder.add(load_measurement("load-1", 100000000)));
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);

  CCAP_CHECK_OK(report, reconcile_snapshot(*snapshot));
  const LoadReconciliation* zone = zone_entry(report);
  CCAP_CHECK(zone != nullptr);
  if (zone != nullptr) {
    CCAP_CHECK_EQ(zone->status, LoadStatus::OverCommitted);
    CCAP_CHECK_EQ(zone->committed.amount_or_zero().milliwatts(), 500000000);
    CCAP_CHECK_EQ(zone->observed.amount_or_zero().milliwatts(), 100000000);
    CCAP_CHECK_EQ(zone->delta.amount_or_zero().milliwatts(), 400000000);
  }
  const LoadReconciliation* loop = loop_entry(report);
  CCAP_CHECK(loop != nullptr);
  if (loop != nullptr) {
    CCAP_CHECK_EQ(loop->committed.amount_or_zero().milliwatts(), 500000000);
  }
}

CCAP_TEST(a_difference_inside_the_policy_tolerance_is_consistent) {
  SnapshotBuilder builder = facility_builder(simple_facility());
  CCAP_CHECK_VOID(builder.add(commitment("c-1", "a-1", 100000000)));
  // Ten percent of 100 MW is 10 MW, and the observation is only five above.
  CCAP_CHECK_VOID(builder.add(load_measurement("load-1", 105000000)));
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);

  CCAP_CHECK_OK(report, reconcile_snapshot(*snapshot));
  const LoadReconciliation* zone = zone_entry(report);
  CCAP_CHECK(zone != nullptr);
  if (zone != nullptr) {
    CCAP_CHECK_EQ(zone->status, LoadStatus::Consistent);
    CCAP_CHECK_EQ(zone->delta.amount_or_zero().milliwatts(), -5000000);
    CCAP_CHECK_EQ(snapshot->policy().reconciliation_tolerance, ppm(100000));
  }
}

CCAP_TEST(an_observation_of_another_medium_does_not_reconcile_the_air_load) {
  // A liquid observation about an air zone is listed as evidence about the
  // subject, but it is not an observation of the air load: the entry stays
  // Unknown rather than reading the liquid figure as zero.
  SnapshotBuilder builder = facility_builder(simple_facility());
  EvidenceRecord liquid = load_measurement("load-liquid", 150000000);
  liquid.medium = CoolingMedium::Liquid;
  CCAP_CHECK_VOID(builder.add(liquid));
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);

  CCAP_CHECK_OK(report, reconcile_snapshot(*snapshot));
  const LoadReconciliation* zone = zone_entry(report);
  CCAP_CHECK(zone != nullptr);
  if (zone != nullptr) {
    CCAP_CHECK_EQ(zone->status, LoadStatus::Unknown);
    CCAP_CHECK_EQ(zone->observed.reason(), cooling_capacity::CapacityReason::NoObservation);
    CCAP_CHECK_EQ(zone->observations.size(), 1U);
  }
}