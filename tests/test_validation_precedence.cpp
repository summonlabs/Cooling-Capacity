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

#include <memory>
#include <string>

namespace {

using cooling_capacity::CommitmentRecord;
using cooling_capacity::CoolingPolicy;
using cooling_capacity::CoolingSnapshot;
using cooling_capacity::EntityKind;
using cooling_capacity::EvidenceRecord;
using cooling_capacity::SnapshotBuilder;
using cooling_capacity::ValidationCode;
using cooling_capacity::ValidationFinding;
using cooling_capacity::ValidationReport;
using cooling_capacity::ValidationSeverity;

SnapshotBuilder fixture_builder() {
  SnapshotBuilder builder;
  builder.set_generation(CapacityGeneration::first());
  builder.set_constructed_at(fixture_now());
  return builder;
}

// The first error-severity finding of a candidate generation, which is what a
// caller must fix first. Warnings never precede it in the report.
const ValidationFinding* first_error(const ValidationReport& report) {
  for (const ValidationFinding& finding : report.findings()) {
    if (finding.severity == ValidationSeverity::Error) {
      return &finding;
    }
  }
  return nullptr;
}

void expect_first_error(const SnapshotBuilder& builder, ValidationCode code, EntityKind kind) {
  const ValidationReport report = builder.validate();
  CCAP_CHECK_FALSE(report.ok());
  CCAP_CHECK(report.error_count() >= 1U);
  const ValidationFinding* first = first_error(report);
  CCAP_CHECK(first != nullptr);
  if (first != nullptr) {
    CCAP_CHECK_EQ(first->code, code);
    CCAP_CHECK_EQ(first->kind, kind);
    CCAP_CHECK_EQ(first->severity, ValidationSeverity::Error);
    CCAP_CHECK_FALSE(first->subject.empty());
    CCAP_CHECK_FALSE(first->detail.empty());
  }
}

// Every record of the simple facility except the site, so that a test can leave
// the site out and observe exactly one defect.
void add_facility_records(SnapshotBuilder& builder, const SimpleFacility& facility) {
  CCAP_CHECK_VOID(builder.add(facility.facility));
  CCAP_CHECK_VOID(builder.add(facility.crah_a));
  CCAP_CHECK_VOID(builder.add(facility.crah_b));
  CCAP_CHECK_VOID(builder.add(facility.loop));
  CCAP_CHECK_VOID(builder.add(facility.zone));
}

}  // namespace

CCAP_TEST(a_facility_with_a_missing_site_is_reported_first) {
  const SimpleFacility facility = simple_facility();
  SnapshotBuilder builder = fixture_builder();
  add_facility_records(builder, facility);
  expect_first_error(builder, ValidationCode::SiteMissing, EntityKind::Facility);

  CCAP_CHECK_OK(snapshot, builder.build_unvalidated());
  const ValidationReport report = cooling_capacity::validate_snapshot(*snapshot);
  CCAP_CHECK(report.has(ValidationCode::SiteMissing));
  CCAP_CHECK_FALSE(report.ok());
}

CCAP_TEST(a_zone_with_a_missing_loop_is_reported_first) {
  SimpleFacility facility = simple_facility();
  facility.zone.loops = {loop_id("air-loop-9")};
  SnapshotBuilder builder = fixture_builder();
  CCAP_CHECK_VOID(builder.add(facility.site));
  add_facility_records(builder, facility);
  expect_first_error(builder, ValidationCode::ZoneLoopMissing, EntityKind::Zone);
}

CCAP_TEST(a_loop_with_missing_equipment_is_reported_first) {
  SimpleFacility facility = simple_facility();
  facility.loop.equipment = {facility.crah_a.id, equipment_id("crah-z")};
  SnapshotBuilder builder = fixture_builder();
  CCAP_CHECK_VOID(builder.add(facility.site));
  add_facility_records(builder, facility);
  expect_first_error(builder, ValidationCode::LoopEquipmentMissing, EntityKind::Loop);
}

CCAP_TEST(a_loop_with_no_equipment_is_reported_first) {
  SimpleFacility facility = simple_facility();
  facility.loop.equipment = {};
  SnapshotBuilder builder = fixture_builder();
  CCAP_CHECK_VOID(builder.add(facility.site));
  add_facility_records(builder, facility);
  expect_first_error(builder, ValidationCode::LoopWithoutEquipment, EntityKind::Loop);
}

CCAP_TEST(a_zone_medium_without_a_class_is_reported_first) {
  SimpleFacility facility = simple_facility();
  facility.zone.media.insert(CoolingMedium::Liquid);
  SnapshotBuilder builder = fixture_builder();
  CCAP_CHECK_VOID(builder.add(facility.site));
  add_facility_records(builder, facility);
  expect_first_error(builder, ValidationCode::ZoneMediumWithoutClass, EntityKind::Zone);
}

CCAP_TEST(a_negative_nominal_capacity_is_reported_first) {
  SimpleFacility facility = simple_facility();
  facility.crah_a.nominal = ThermalPower::from_milliwatts(-5000);
  SnapshotBuilder builder = fixture_builder();
  CCAP_CHECK_VOID(builder.add(facility.site));
  add_facility_records(builder, facility);
  expect_first_error(builder, ValidationCode::NominalCapacityNegative, EntityKind::Equipment);
}

CCAP_TEST(a_validated_capacity_above_the_nominal_is_reported_first) {
  SimpleFacility facility = simple_facility();
  facility.crah_a.validated = watts(500000);
  SnapshotBuilder builder = fixture_builder();
  CCAP_CHECK_VOID(builder.add(facility.site));
  add_facility_records(builder, facility);
  expect_first_error(builder, ValidationCode::ValidatedCapacityAboveNominal,
                     EntityKind::Equipment);
}

CCAP_TEST(a_derate_above_one_is_reported_first) {
  SimpleFacility facility = simple_facility();
  facility.crah_a.derates = {DerateFactor{identifier("ambient"), ppm(1200000),
                                          cooling_capacity::DerateReason::AmbientDesign,
                                          std::nullopt}};
  SnapshotBuilder builder = fixture_builder();
  CCAP_CHECK_VOID(builder.add(facility.site));
  add_facility_records(builder, facility);
  expect_first_error(builder, ValidationCode::DerateOutOfRange, EntityKind::Equipment);
}

CCAP_TEST(a_commitment_pinned_to_a_loop_that_does_not_serve_its_zone_is_reported_first) {
  const SimpleFacility facility = simple_facility();
  LoopRecord other = make_loop("air-loop-2", cooling_capacity::LoopKind::AirSupply,
                               {facility.crah_b.id}, 500000,
                               cooling_capacity::RedundancyClass::None);
  CommitmentRecord commitment;
  commitment.id = cooling_capacity::CommitmentId::literal("c-1");
  commitment.attempt = cooling_capacity::AttemptId::literal("a-1");
  commitment.zone = zone_id("hall-1");
  commitment.pinned_loop = other.id;
  commitment.medium = CoolingMedium::Air;
  commitment.compatibility = CompatibilityClass::AirConvection;
  commitment.thermal = milliwatts(1000);
  commitment.state = cooling_capacity::CommitmentState::Committed;
  commitment.actor = actor_id("operator");
  commitment.created_at = fixture_now();
  commitment.created_generation = CapacityGeneration::first();
  commitment.revision = RecordRevision::first();

  SnapshotBuilder builder = fixture_builder();
  CCAP_CHECK_VOID(builder.add(facility.site));
  add_facility_records(builder, facility);
  CCAP_CHECK_VOID(builder.add(other));
  CCAP_CHECK_VOID(builder.add(commitment));
  expect_first_error(builder, ValidationCode::CommitmentLoopNotServingZone,
                     EntityKind::Commitment);
}

CCAP_TEST(an_evidence_record_about_a_missing_subject_is_reported_first) {
  const SimpleFacility facility = simple_facility();
  EvidenceRecord evidence;
  evidence.id = evidence_id("nameplate-crah-z");
  evidence.kind = EvidenceKind::Nameplate;
  evidence.source = EvidenceSource::FacilityRegistry;
  evidence.subject = equipment_id("crah-z");
  evidence.medium = CoolingMedium::Air;
  evidence.value = milliwatts(400000);
  evidence.observed_at = fixture_now();
  evidence.provenance.actor = actor_id("registry");
  evidence.provenance.reference = cooling_capacity::DocumentRef::literal("registry-1");
  evidence.revision = RecordRevision::first();

  SnapshotBuilder builder = fixture_builder();
  CCAP_CHECK_VOID(builder.add(facility.site));
  add_facility_records(builder, facility);
  CCAP_CHECK_VOID(builder.add(evidence));
  expect_first_error(builder, ValidationCode::EvidenceSubjectMissing, EntityKind::Evidence);
}

CCAP_TEST(a_broken_policy_is_reported_before_any_record) {
  const SimpleFacility facility = simple_facility();
  CoolingPolicy policy = CoolingPolicy::defaults();
  policy.freshness.clear();

  SnapshotBuilder builder = fixture_builder();
  builder.set_policy(policy);
  CCAP_CHECK_VOID(builder.add(facility.site));
  add_facility_records(builder, facility);
  expect_first_error(builder, ValidationCode::PolicyInvalid, EntityKind::Policy);
}

CCAP_TEST(the_findings_order_is_deterministic) {
  const auto broken_model = [] {
    SimpleFacility facility = simple_facility();
    facility.crah_a.nominal = ThermalPower::from_milliwatts(-1);
    facility.zone.loops = {loop_id("air-loop-9")};
    SnapshotBuilder builder = fixture_builder();
    add_facility_records(builder, facility);
    return builder;
  };

  const std::string first = broken_model().validate().render();
  const std::string second = broken_model().validate().render();
  CCAP_CHECK_FALSE(first.empty());
  CCAP_CHECK_EQ(first, second);

  // Facilities are validated before equipment, which is validated before zones,
  // so those three defects always appear in that order.
  CCAP_CHECK(first.find("site-missing") != std::string::npos);
  CCAP_CHECK(first.find("nominal-capacity-negative") != std::string::npos);
  CCAP_CHECK(first.find("zone-loop-missing") != std::string::npos);
  CCAP_CHECK_LT(first.find("site-missing"), first.find("nominal-capacity-negative"));
  CCAP_CHECK_LT(first.find("nominal-capacity-negative"), first.find("zone-loop-missing"));

  expect_first_error(broken_model(), ValidationCode::SiteMissing, EntityKind::Facility);

  // The same generation, built a second time through the public build path,
  // reports the same first defect.
  SnapshotBuilder builder = broken_model();
  ValidationReport report;
  const Result<std::shared_ptr<const CoolingSnapshot>> result = builder.build(&report);
  CCAP_CHECK_FALSE(result.ok());
  const ValidationFinding* first_finding = first_error(report);
  CCAP_CHECK(first_finding != nullptr);
  if (first_finding != nullptr) {
    CCAP_CHECK_EQ(first_finding->code, ValidationCode::SiteMissing);
    CCAP_CHECK_EQ(first_finding->kind, EntityKind::Facility);
    CCAP_CHECK_EQ(first_finding->detail, std::string("site site-a is not present"));
  }
}