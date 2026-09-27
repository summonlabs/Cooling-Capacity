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

// Adversarial input: identifiers and text that try to escape their grammar, a
// snapshot that claims two records with one identity, evidence that contradicts
// itself, and engine requests that reuse an attempt or an old generation. Every
// case must be refused with the specific code that names the defect.

#include "fixtures.hpp"
#include "test_harness.hpp"
#include "test_process.hpp"

using namespace ccap_test;

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

using cooling_capacity::AttemptId;
using cooling_capacity::BoundedText;
using cooling_capacity::CommitmentId;
using cooling_capacity::CommitmentOperation;
using cooling_capacity::CommitmentOutcome;
using cooling_capacity::CommitmentRecord;
using cooling_capacity::CommitmentRequest;
using cooling_capacity::CoolingCapacityEngine;
using cooling_capacity::CoolingMedium;
using cooling_capacity::CoolingSnapshot;
using cooling_capacity::EngineOptions;
using cooling_capacity::ErrorCode;
using cooling_capacity::Identifier;
using cooling_capacity::ManualClock;
using cooling_capacity::SnapshotBuilder;
using cooling_capacity::ValidationCode;
using cooling_capacity::ValidationReport;

SnapshotBuilder fixture_builder() {
  SnapshotBuilder builder;
  builder.set_generation(CapacityGeneration::first());
  builder.set_constructed_at(fixture_now());
  return builder;
}

// Adds the same record twice under two identifiers and expects the canonical
// form to refuse the duplicate identity.
template <class Record>
void expect_duplicate_identity(const Record& first, const Record& second) {
  SnapshotBuilder builder = fixture_builder();
  CCAP_CHECK_VOID(builder.add(first));
  CCAP_CHECK_VOID(builder.add(second));
  CCAP_CHECK_ERR(builder.build_unvalidated(), ErrorCode::DuplicateIdentity);
  CCAP_CHECK_ERR(builder.build(nullptr), ErrorCode::DuplicateIdentity);
}

EvidenceRecord nameplate(const char* id, const EquipmentId& subject, std::int64_t thermal_milliwatts) {
  EvidenceRecord record;
  record.id = evidence_id(id);
  record.kind = EvidenceKind::Nameplate;
  record.source = EvidenceSource::FacilityRegistry;
  record.subject = subject;
  record.medium = CoolingMedium::Air;
  record.value = milliwatts(thermal_milliwatts);
  record.observed_at = fixture_now();
  record.provenance.actor = actor_id("registry");
  record.provenance.reference = cooling_capacity::DocumentRef::literal("registry-1");
  record.revision = RecordRevision::first();
  return record;
}

// A real engine on a real directory, with the simple facility already published.
struct Harness {
  std::string directory;
  ManualClock clock{fixture_now()};
  EngineOptions options;
  std::unique_ptr<CoolingCapacityEngine> engine;
};

std::unique_ptr<Harness> open_harness(const std::string& name) {
  auto harness = std::make_unique<Harness>();
  const Result<std::string> directory = fresh_directory(name);
  if (!directory.ok()) {
    std::abort();
  }
  harness->directory = directory.value();
  harness->options.actor = actor_id("operator");
  harness->options.clock = &harness->clock;
  harness->options.store.access = cooling_capacity::StoreAccess::ReadWrite;
  harness->options.store.create_if_missing = true;
  Result<std::unique_ptr<CoolingCapacityEngine>> engine =
      CoolingCapacityEngine::open(harness->directory, harness->options);
  if (!engine.ok()) {
    std::abort();
  }
  harness->engine = std::move(engine).value();

  Result<SnapshotBuilder> builder = harness->engine->begin_mutation();
  if (!builder.ok()) {
    std::abort();
  }
  if (!add_simple_facility(builder.value(), simple_facility()).ok()) {
    std::abort();
  }
  if (!harness->engine->publish(builder.value()).ok()) {
    std::abort();
  }
  return harness;
}

CommitmentRequest make_request(CommitmentOperation operation, const char* commitment,
                               const char* attempt, std::int64_t thermal_milliwatts,
                               const CoolingCapacityEngine& engine) {
  CommitmentRequest request;
  request.operation = operation;
  request.id = CommitmentId::literal(commitment);
  request.attempt = AttemptId::literal(attempt);
  request.zone = zone_id("hall-1");
  request.medium = CoolingMedium::Air;
  request.compatibility = CompatibilityClass::AirConvection;
  request.thermal = milliwatts(thermal_milliwatts);
  request.actor = actor_id("operator");
  request.requested_at = fixture_now();
  request.expected_generation = engine.snapshot()->generation();
  return request;
}

}  // namespace

CCAP_TEST(identifiers_reject_traversal_and_control_characters) {
  const char* rejected[] = {
      "",           ".",           "..",         "../etc",      "..\\..\\windows", "a/b",
      "a\\b",       "a b",         " hall-1",    "hall-1 ",     "-leading",        ":leading",
      ".hidden",    "x\ty",        "x\ny",       "x\rz",        "\x01",            "x\x7f",
      "caf\xc3\xa9", "hall#1",     "hall@1",     "hall+1",      "hall;1",          "*",
  };
  for (const char* text : rejected) {
    CCAP_CHECK_ERR(Identifier::parse(text), ErrorCode::InvalidIdentifier);
    CCAP_CHECK_ERR(cooling_capacity::EquipmentId::parse(text), ErrorCode::InvalidIdentifier);
  }

  const std::string too_long(64U, 'a');
  CCAP_CHECK_ERR(Identifier::parse(too_long), ErrorCode::InvalidIdentifier);
  const std::string at_limit(63U, 'a');
  CCAP_CHECK_OK(accepted, Identifier::parse(at_limit));
  CCAP_CHECK_EQ(accepted.str().size(), 63U);

  const char* allowed[] = {"a", "0", "A1", "air-loop-1", "hall.1", "zone:2", "crah_a"};
  for (const char* text : allowed) {
    CCAP_CHECK_OK(parsed, Identifier::parse(text));
    CCAP_CHECK_EQ(parsed.str(), std::string(text));
  }
}

CCAP_TEST(bounded_text_rejects_control_characters_and_over_length_text) {
  const char* rejected[] = {"line\nbreak", "tab\there", "\x01", "\x7f", "bell\x07",
                            "\xc2\x80", "\xc2\x9f"};
  for (const char* text : rejected) {
    CCAP_CHECK_ERR(BoundedText::parse(text), ErrorCode::InvalidText);
  }

  const std::string over_length(513U, 'a');
  CCAP_CHECK_ERR(BoundedText::parse(over_length), ErrorCode::InvalidText);
  const std::string at_limit(512U, 'a');
  CCAP_CHECK_OK(accepted, BoundedText::parse(at_limit));
  CCAP_CHECK_EQ(accepted.str().size(), 512U);

  // Malformed UTF-8 is reported as an encoding defect, not as a control
  // character.
  const std::string invalid_utf8 = "\xC3\x28";
  CCAP_CHECK_ERR(BoundedText::parse(invalid_utf8), ErrorCode::InvalidUtf8);
  const std::string overlong = "\xC0\xAF";
  CCAP_CHECK_ERR(BoundedText::parse(overlong), ErrorCode::InvalidUtf8);
  const std::string lone_continuation = "\x80";
  CCAP_CHECK_ERR(BoundedText::parse(lone_continuation), ErrorCode::InvalidUtf8);

  // Printable non-ASCII text is annotation, not an attack.
  CCAP_CHECK_OK(unicode, BoundedText::parse("cooling hall caf\xC3\xA9 \xE2\x82\xAC"));
  CCAP_CHECK_FALSE(unicode.empty());
}

CCAP_TEST(a_snapshot_refuses_a_duplicate_identity_in_each_collection) {
  expect_duplicate_identity(make_site("site-a"), make_site("site-a"));
  expect_duplicate_identity(make_facility("dc-1", site_id("site-a")),
                            make_facility("dc-1", site_id("site-a")));
  expect_duplicate_identity(make_unit("crah-a", EquipmentKind::Crah, 400000),
                            make_unit("crah-a", EquipmentKind::Crah, 400000));
  expect_duplicate_identity(make_loop("air-loop-1", cooling_capacity::LoopKind::AirSupply, {},
                                      700000, cooling_capacity::RedundancyClass::None),
                            make_loop("air-loop-1", cooling_capacity::LoopKind::AirSupply, {},
                                      700000, cooling_capacity::RedundancyClass::None));
  expect_duplicate_identity(make_zone("hall-1", {}, {CompatibilityClass::AirConvection}),
                            make_zone("hall-1", {}, {CompatibilityClass::AirConvection}));
  expect_duplicate_identity(make_plant("plant-1", CoolingMedium::Air, {},
                                       cooling_capacity::RedundancyClass::None),
                            make_plant("plant-1", CoolingMedium::Air, {},
                                       cooling_capacity::RedundancyClass::None));

  cooling_capacity::ManifoldRecord manifold;
  manifold.id = cooling_capacity::ManifoldId::literal("manifold-1");
  manifold.facility = facility_id("dc-1");
  manifold.medium = CoolingMedium::Air;
  manifold.label = BoundedText::literal("manifold-1");
  manifold.transport_limit = watts(100000);
  manifold.revision = RecordRevision::first();
  expect_duplicate_identity(manifold, manifold);

  cooling_capacity::DomainRecord domain;
  domain.id = cooling_capacity::DomainId::literal("domain-1");
  domain.facility = facility_id("dc-1");
  domain.medium = CoolingMedium::Air;
  domain.label = BoundedText::literal("domain-1");
  domain.revision = RecordRevision::first();
  expect_duplicate_identity(domain, domain);

  expect_duplicate_identity(nameplate("e-1", equipment_id("crah-a"), 400000),
                            nameplate("e-1", equipment_id("crah-a"), 400000));

  CommitmentRecord commitment;
  commitment.id = CommitmentId::literal("c-1");
  commitment.attempt = AttemptId::literal("a-1");
  commitment.zone = zone_id("hall-1");
  commitment.medium = CoolingMedium::Air;
  commitment.compatibility = CompatibilityClass::AirConvection;
  commitment.thermal = milliwatts(1000000);
  commitment.actor = actor_id("operator");
  commitment.created_at = fixture_now();
  commitment.created_generation = CapacityGeneration::first();
  commitment.revision = RecordRevision::first();
  expect_duplicate_identity(commitment, commitment);

  // The same equipment listed twice inside one loop is a duplicate identity too.
  SimpleFacility facility = simple_facility();
  facility.loop.equipment = {facility.crah_a.id, facility.crah_a.id};
  SnapshotBuilder builder = fixture_builder();
  CCAP_CHECK_VOID(builder.add(facility.site));
  CCAP_CHECK_VOID(builder.add(facility.facility));
  CCAP_CHECK_VOID(builder.add(facility.crah_a));
  CCAP_CHECK_VOID(builder.add(facility.loop));
  CCAP_CHECK_VOID(builder.add(facility.zone));
  CCAP_CHECK_ERR(builder.build_unvalidated(), ErrorCode::DuplicateIdentity);
}

CCAP_TEST(an_evidence_record_that_supersedes_itself_is_refused) {
  const SimpleFacility facility = simple_facility();
  EvidenceRecord record = nameplate("e-1", facility.crah_a.id, 400000);
  record.provenance.supersedes = record.id;

  SnapshotBuilder builder = fixture_builder();
  CCAP_CHECK_VOID(builder.add(facility.site));
  CCAP_CHECK_VOID(builder.add(facility.facility));
  CCAP_CHECK_VOID(builder.add(facility.crah_a));
  CCAP_CHECK_VOID(builder.add(facility.crah_b));
  CCAP_CHECK_VOID(builder.add(facility.loop));
  CCAP_CHECK_VOID(builder.add(facility.zone));
  CCAP_CHECK_VOID(builder.add(record));

  const ValidationReport report = builder.validate();
  CCAP_CHECK_FALSE(report.ok());
  CCAP_CHECK(report.has(ValidationCode::EvidenceSupersedesSelf));
  ValidationReport build_report;
  CCAP_CHECK_ERR(builder.build(&build_report), ErrorCode::InvariantViolation);
  CCAP_CHECK(build_report.has(ValidationCode::EvidenceSupersedesSelf));
}

CCAP_TEST(evidence_validity_that_ends_before_it_was_observed_is_refused) {
  const SimpleFacility facility = simple_facility();
  EvidenceRecord record = nameplate("e-1", facility.crah_a.id, 400000);
  record.valid_until =
      Timestamp::from_unix_milliseconds(fixture_now().unix_milliseconds() - 1000LL);

  SnapshotBuilder builder = fixture_builder();
  CCAP_CHECK_VOID(builder.add(facility.site));
  CCAP_CHECK_VOID(builder.add(facility.facility));
  CCAP_CHECK_VOID(builder.add(facility.crah_a));
  CCAP_CHECK_VOID(builder.add(facility.crah_b));
  CCAP_CHECK_VOID(builder.add(facility.loop));
  CCAP_CHECK_VOID(builder.add(facility.zone));
  CCAP_CHECK_VOID(builder.add(record));

  const ValidationReport report = builder.validate();
  CCAP_CHECK_FALSE(report.ok());
  CCAP_CHECK(report.has(ValidationCode::EvidenceValidityInverted));

  // The window ending exactly at the observation instant is not inverted.
  record.valid_until = record.observed_at;
  CCAP_CHECK_VOID(builder.replace(record));
  const ValidationReport accepted = builder.validate();
  CCAP_CHECK_FALSE(accepted.has(ValidationCode::EvidenceValidityInverted));
  CCAP_CHECK(accepted.ok());
}

CCAP_TEST(two_active_nameplate_records_for_one_subject_are_refused) {
  const SimpleFacility facility = simple_facility();
  const EvidenceRecord first = nameplate("e-1", facility.crah_a.id, 400000);
  const EvidenceRecord second = nameplate("e-2", facility.crah_a.id, 410000);

  SnapshotBuilder builder = fixture_builder();
  CCAP_CHECK_VOID(builder.add(facility.site));
  CCAP_CHECK_VOID(builder.add(facility.facility));
  CCAP_CHECK_VOID(builder.add(facility.crah_a));
  CCAP_CHECK_VOID(builder.add(facility.crah_b));
  CCAP_CHECK_VOID(builder.add(facility.loop));
  CCAP_CHECK_VOID(builder.add(facility.zone));
  CCAP_CHECK_VOID(builder.add(first));
  CCAP_CHECK_VOID(builder.add(second));

  const ValidationReport report = builder.validate();
  CCAP_CHECK_FALSE(report.ok());
  CCAP_CHECK(report.has(ValidationCode::EvidenceDuplicateActiveKind));
  CCAP_CHECK_EQ(report.error_count(), 1U);

  // Withdrawing the second record removes the conflict: only active record of a
  // declarative kind has to be unique.
  EvidenceRecord withdrawn = second;
  withdrawn.state = cooling_capacity::EvidenceState::Withdrawn;
  CCAP_CHECK_VOID(builder.replace(withdrawn));
  const ValidationReport accepted = builder.validate();
  CCAP_CHECK(accepted.ok());
  CCAP_CHECK_FALSE(accepted.has(ValidationCode::EvidenceDuplicateActiveKind));
}

CCAP_TEST(a_reused_attempt_identifier_with_different_content_is_a_conflict) {
  std::unique_ptr<Harness> harness = open_harness("ccap-test-adversarial");
  CCAP_CHECK(harness->engine->holds_writer());

  CCAP_CHECK_OK(first, harness->engine->apply(make_request(CommitmentOperation::Commit, "c-1",
                                                           "a-1", 100000000, *harness->engine)));
  CCAP_CHECK_EQ(first.state, cooling_capacity::CommitmentState::Committed);

  // The same attempt with a different commitment and a different load.
  CCAP_CHECK_ERR(harness->engine->apply(make_request(CommitmentOperation::Commit, "c-2", "a-1",
                                                     50000000, *harness->engine)),
                 ErrorCode::IdempotencyConflict);
  // Nothing was written by the refused request.
  CCAP_CHECK_EQ(harness->engine->snapshot()->commitments().size(), 1U);

  // A genuine retry of the same request is a replay, not a conflict, so the
  // refusal above is about the content and not about the attempt identifier.
  CommitmentRequest retry = make_request(CommitmentOperation::Commit, "c-1", "a-1", 100000000,
                                         *harness->engine);
  CCAP_CHECK_OK(replayed, harness->engine->apply(retry));
  CCAP_CHECK(replayed.replayed);
  CCAP_CHECK_EQ(replayed.id.str(), std::string("c-1"));
  CCAP_CHECK_EQ(harness->engine->snapshot()->commitments().size(), 1U);

  // Releasing the engine releases the writer lock, so the directory can go.
  const std::string directory = harness->directory;
  harness.reset();
}

CCAP_TEST(a_commitment_request_formed_against_a_stale_generation_is_refused) {
  std::unique_ptr<Harness> harness = open_harness("ccap-test-adversarial-stale");
  // Opening an empty store commits generation one, and the harness publishes the
  // facility as generation two.
  const CapacityGeneration opened = harness->engine->snapshot()->generation();
  CCAP_CHECK_EQ(opened, CapacityGeneration::from_value(2));

  CCAP_CHECK_OK(first, harness->engine->apply(make_request(CommitmentOperation::Commit, "c-1",
                                                           "a-1", 100000000, *harness->engine)));
  CCAP_CHECK_EQ(first.generation, CapacityGeneration::from_value(3));
  CCAP_CHECK_EQ(harness->engine->snapshot()->generation(), CapacityGeneration::from_value(3));

  CommitmentRequest stale = make_request(CommitmentOperation::Commit, "c-2", "a-2", 100000000,
                                         *harness->engine);
  stale.expected_generation = opened;
  CCAP_CHECK_ERR(harness->engine->apply(stale), ErrorCode::StaleGeneration);
  CCAP_CHECK_EQ(harness->engine->snapshot()->commitments().size(), 1U);

  // A request that names no generation at all is refused as an argument.
  CommitmentRequest unnamed = make_request(CommitmentOperation::Commit, "c-3", "a-3", 1000000,
                                           *harness->engine);
  unnamed.expected_generation = CapacityGeneration::from_value(0);
  CCAP_CHECK_ERR(harness->engine->apply(unnamed), ErrorCode::InvalidArgument);

  const std::string directory = harness->directory;
  harness.reset();
}

CCAP_TEST(an_artifact_with_a_trailing_byte_is_refused) {
  SnapshotBuilder builder = fixture_builder();
  CCAP_CHECK_VOID(add_simple_facility(builder, simple_facility()));
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const cooling_capacity::Limits limits = cooling_capacity::Limits::defaults();
  CCAP_CHECK_OK(bytes, cooling_capacity::encode_snapshot(*snapshot, limits));

  std::string trailing = bytes;
  trailing.push_back('\0');
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(trailing), ErrorCode::Truncated);
  CCAP_CHECK_ERR(cooling_capacity::decode_snapshot(trailing, limits), ErrorCode::Truncated);

  std::string trailing_text = bytes;
  trailing_text.append("extra");
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(trailing_text), ErrorCode::Truncated);
  CCAP_CHECK_ERR(cooling_capacity::decode_snapshot(trailing_text, limits), ErrorCode::Truncated);

  // A leading byte is not a magic prefix either.
  std::string leading = std::string("x") + bytes;
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(leading),
                 ErrorCode::UnsupportedFormat);

  // The untouched artifact still verifies.
  CCAP_CHECK_OK(manifest, cooling_capacity::read_artifact_manifest(bytes));
  CCAP_CHECK(manifest.verified);
}