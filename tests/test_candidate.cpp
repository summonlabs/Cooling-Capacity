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

namespace {

using cooling_capacity::AnalysisBasis;
using cooling_capacity::CandidateDisposition;
using cooling_capacity::CandidateEvaluation;
using cooling_capacity::CandidateLoad;
using cooling_capacity::CandidateReason;
using cooling_capacity::CoolingAnalyzer;
using cooling_capacity::CoolingMedium;
using cooling_capacity::CoolingSnapshot;
using cooling_capacity::RedundancyClass;
using cooling_capacity::SnapshotBuilder;

SnapshotBuilder facility_builder(const SimpleFacility& facility) {
  SnapshotBuilder builder;
  builder.set_generation(CapacityGeneration::first());
  builder.set_constructed_at(fixture_now());
  const Result<void> added = add_simple_facility(builder, facility);
  CCAP_CHECK_VOID(added);
  return builder;
}

CandidateLoad request(std::int64_t thermal_milliwatts) {
  CandidateLoad load;
  load.zone = zone_id("hall-1");
  load.medium = CoolingMedium::Air;
  load.compatibility = CompatibilityClass::AirConvection;
  load.thermal = milliwatts(thermal_milliwatts);
  load.actor = actor_id("operator");
  return load;
}

CandidateEvaluation evaluate(const CoolingSnapshot& snapshot, const CandidateLoad& load) {
  const CoolingAnalyzer analyzer(snapshot, fixture_now(), AnalysisBasis::Fresh);
  const Result<CandidateEvaluation> result = cooling_capacity::evaluate_candidate(analyzer, load);
  if (!result.ok()) {
    std::fprintf(stderr, "evaluation failed: %s\n", result.error().to_string().c_str());
    std::abort();
  }
  return result.value();
}

}  // namespace

CCAP_TEST(a_load_within_capacity_is_admitted) {
  SnapshotBuilder builder = facility_builder(simple_facility());
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CandidateEvaluation evaluation = evaluate(*snapshot, request(300000000));
  CCAP_CHECK_EQ(evaluation.disposition, CandidateDisposition::Admitted);
  CCAP_CHECK_EQ(evaluation.reason, CandidateReason::None);
  CCAP_CHECK(evaluation.admitted());
  CCAP_CHECK_EQ(evaluation.available.milliwatts(), 400000000);
}

CCAP_TEST(a_load_above_capacity_is_rejected_not_indeterminate) {
  SnapshotBuilder builder = facility_builder(simple_facility());
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CandidateEvaluation evaluation = evaluate(*snapshot, request(500000000));
  CCAP_CHECK_EQ(evaluation.disposition, CandidateDisposition::Rejected);
  CCAP_CHECK_EQ(evaluation.reason, CandidateReason::InsufficientLocalCapacity);
}

CCAP_TEST(a_load_exactly_at_capacity_is_admitted) {
  SnapshotBuilder builder = facility_builder(simple_facility());
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CandidateEvaluation evaluation = evaluate(*snapshot, request(400000000));
  CCAP_CHECK_EQ(evaluation.disposition, CandidateDisposition::Admitted);
}

CCAP_TEST(a_zero_or_negative_load_is_refused_as_an_argument) {
  SnapshotBuilder builder = facility_builder(simple_facility());
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer(*snapshot, fixture_now(), AnalysisBasis::Fresh);
  CCAP_CHECK_ERR(cooling_capacity::evaluate_candidate(analyzer, request(0)),
                 cooling_capacity::ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(cooling_capacity::evaluate_candidate(analyzer, request(-5)),
                 cooling_capacity::ErrorCode::InvalidArgument);
}

CCAP_TEST(a_mismatched_medium_and_class_is_refused) {
  CandidateLoad load = request(1000);
  load.medium = CoolingMedium::Liquid;
  load.compatibility = CompatibilityClass::AirConvection;
  SnapshotBuilder builder = facility_builder(simple_facility());
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer(*snapshot, fixture_now(), AnalysisBasis::Fresh);
  CCAP_CHECK_ERR(cooling_capacity::evaluate_candidate(analyzer, load),
                 cooling_capacity::ErrorCode::IncompatibleMedium);
}

CCAP_TEST(an_unsupported_medium_is_a_definite_no) {
  SnapshotBuilder builder = facility_builder(simple_facility());
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  CandidateLoad load = request(1000);
  load.medium = CoolingMedium::Liquid;
  load.compatibility = CompatibilityClass::LiquidColdPlate;
  const CandidateEvaluation evaluation = evaluate(*snapshot, load);
  CCAP_CHECK_EQ(evaluation.disposition, CandidateDisposition::Rejected);
  CCAP_CHECK_EQ(evaluation.reason, CandidateReason::MediumUnsupported);
}

CCAP_TEST(a_missing_zone_is_rejected_and_an_unknown_may_not_be_used) {
  SnapshotBuilder builder = facility_builder(simple_facility());
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  CandidateLoad load = request(1000);
  load.zone = zone_id("hall-9");
  const CandidateEvaluation evaluation = evaluate(*snapshot, load);
  CCAP_CHECK_EQ(evaluation.disposition, CandidateDisposition::Rejected);
  CCAP_CHECK_EQ(evaluation.reason, CandidateReason::ZoneNotFound);
}

CCAP_TEST(unknown_capacity_makes_a_candidate_indeterminate_not_rejected) {
  SimpleFacility facility = simple_facility();
  facility.loop.transport_limit_declared = false;
  SnapshotBuilder builder = facility_builder(facility);
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CandidateEvaluation evaluation = evaluate(*snapshot, request(1000));
  CCAP_CHECK_EQ(evaluation.disposition, CandidateDisposition::Indeterminate);
  CCAP_CHECK_EQ(evaluation.reason, CandidateReason::LoopTransportUndeclared);
  CCAP_CHECK_EQ(evaluation.available.milliwatts(), 0);
}

CCAP_TEST(stale_evidence_makes_a_candidate_indeterminate) {
  SimpleFacility facility = simple_facility();
  facility.crah_a.validated = watts(400000);
  facility.crah_a.validated_evidence = evidence_id("commissioning-report");
  facility.crah_b.validated = watts(400000);
  facility.crah_b.validated_evidence = evidence_id("commissioning-report");

  EvidenceRecord report;
  report.id = evidence_id("commissioning-report");
  report.kind = EvidenceKind::CapacityValidation;
  report.source = EvidenceSource::CommissioningTool;
  report.subject = facility.crah_a.id;
  report.medium = CoolingMedium::Air;
  report.value = milliwatts(400000000);
  report.observed_at = Timestamp::from_unix_milliseconds(fixture_now().unix_milliseconds() -
                                                         400LL * 24LL * 3600LL * 1000LL);
  report.provenance.actor = actor_id("commissioner");
  report.provenance.reference = cooling_capacity::DocumentRef::literal("report-1");
  report.revision = RecordRevision::first();

  SnapshotBuilder builder = facility_builder(facility);
  CCAP_CHECK_VOID(builder.add(report));
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CandidateEvaluation evaluation = evaluate(*snapshot, request(1000));
  CCAP_CHECK_EQ(evaluation.disposition, CandidateDisposition::Indeterminate);
  CCAP_CHECK_EQ(evaluation.reason, CandidateReason::StaleEvidence);
}

CCAP_TEST(with_an_explicit_override_stale_evidence_may_be_used) {
  SimpleFacility facility = simple_facility();
  facility.crah_a.validated = watts(400000);
  facility.crah_a.validated_evidence = evidence_id("commissioning-report");
  facility.crah_b.validated = watts(400000);
  facility.crah_b.validated_evidence = evidence_id("commissioning-report");

  EvidenceRecord report;
  report.id = evidence_id("commissioning-report");
  report.kind = EvidenceKind::CapacityValidation;
  report.source = EvidenceSource::CommissioningTool;
  report.subject = facility.crah_a.id;
  report.medium = CoolingMedium::Air;
  report.value = milliwatts(400000000);
  report.observed_at = Timestamp::from_unix_milliseconds(fixture_now().unix_milliseconds() -
                                                         400LL * 24LL * 3600LL * 1000LL);
  report.provenance.actor = actor_id("commissioner");
  report.provenance.reference = cooling_capacity::DocumentRef::literal("report-1");
  report.revision = RecordRevision::first();

  SnapshotBuilder builder = facility_builder(facility);
  cooling_capacity::CoolingPolicy policy = cooling_capacity::CoolingPolicy::defaults();
  policy.stale_action = cooling_capacity::StaleEvidenceAction::Use;
  builder.set_policy(policy);
  CCAP_CHECK_VOID(builder.add(report));
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CandidateEvaluation evaluation = evaluate(*snapshot, request(1000));
  CCAP_CHECK_EQ(evaluation.disposition, CandidateDisposition::Admitted);
}

CCAP_TEST(a_redundancy_requirement_can_reject_a_candidate_that_fits) {
  SimpleFacility facility = simple_facility();
  facility.loop.redundancy = RedundancyClass::None;
  SnapshotBuilder builder = facility_builder(facility);
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  CandidateLoad load = request(100000000);
  load.required_redundancy = RedundancyClass::NPlusOne;
  const CandidateEvaluation evaluation = evaluate(*snapshot, load);
  CCAP_CHECK_EQ(evaluation.disposition, CandidateDisposition::Rejected);
  CCAP_CHECK_EQ(evaluation.reason, CandidateReason::InsufficientRedundancy);
}

CCAP_TEST(a_redundancy_requirement_that_is_met_is_admitted) {
  SnapshotBuilder builder = facility_builder(simple_facility());
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  CandidateLoad load = request(100000000);
  load.required_redundancy = RedundancyClass::NPlusOne;
  const CandidateEvaluation evaluation = evaluate(*snapshot, load);
  CCAP_CHECK_EQ(evaluation.disposition, CandidateDisposition::Admitted);
}

CCAP_TEST(a_pinned_loop_must_serve_the_zone) {
  SnapshotBuilder builder = facility_builder(simple_facility());
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  CandidateLoad load = request(1000);
  load.pinned_loop = loop_id("air-loop-9");
  const CandidateEvaluation missing = evaluate(*snapshot, load);
  CCAP_CHECK_EQ(missing.disposition, CandidateDisposition::Rejected);
  CCAP_CHECK_EQ(missing.reason, CandidateReason::LoopNotFound);

  CandidateLoad other = request(1000);
  other.pinned_loop = loop_id("air-loop-1");
  CCAP_CHECK_EQ(evaluate(*snapshot, other).disposition, CandidateDisposition::Admitted);
}

CCAP_TEST(a_recovered_snapshot_may_not_admit_until_it_is_revalidated) {
  SnapshotBuilder builder = facility_builder(simple_facility());
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const std::shared_ptr<const CoolingSnapshot> recovered = snapshot->with_provenance(
      cooling_capacity::SnapshotOrigin::RecoveredFromStore, std::nullopt);
  const CoolingAnalyzer analyzer(*recovered, fixture_now(),
                                 AnalysisBasis::RecoveredPendingRevalidation);
  CCAP_CHECK_OK(evaluation, cooling_capacity::evaluate_candidate(analyzer, request(1000)));
  CCAP_CHECK_EQ(evaluation.disposition, CandidateDisposition::Indeterminate);
  CCAP_CHECK_EQ(evaluation.reason, CandidateReason::RecoveredPendingRevalidation);

  // Read-only reporting still works on recovered state.
  CCAP_CHECK_OK(zone, analyzer.zone_capacity(zone_id("hall-1"), CoolingMedium::Air,
                                             CompatibilityClass::AirConvection));
  CCAP_CHECK(zone.offered.is_known());
}

CCAP_TEST(every_evaluation_carries_its_generation_and_explanation) {
  SnapshotBuilder builder = facility_builder(simple_facility());
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CandidateEvaluation evaluation = evaluate(*snapshot, request(1000));
  CCAP_CHECK_EQ(evaluation.generation, snapshot->generation());
  CCAP_CHECK_EQ(evaluation.policy.str(), std::string("default-policy"));
  CCAP_CHECK_EQ(evaluation.evaluated_at, fixture_now());
  CCAP_CHECK_FALSE(evaluation.explanation.empty());
  const std::string rendered = cooling_capacity::render_steps(evaluation.explanation);
  CCAP_CHECK(rendered.find("candidate-disposition") != std::string::npos);
  CCAP_CHECK(rendered.find("<missing:") == std::string::npos);
}
