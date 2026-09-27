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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using cooling_capacity::CommitmentRecord;
using cooling_capacity::CoolingSnapshot;
using cooling_capacity::ErrorCode;
using cooling_capacity::Limits;
using cooling_capacity::SnapshotBuilder;

constexpr std::uint64_t kHardArtifactCeiling = 1ULL << 34U;

SnapshotBuilder fixture_builder(std::uint64_t generation) {
  SnapshotBuilder builder;
  builder.set_generation(CapacityGeneration::from_value(generation));
  builder.set_constructed_at(fixture_now());
  return builder;
}

// The simple facility plus an unused second site, so a decoder limited to one
// site must refuse the artifact.
std::shared_ptr<const CoolingSnapshot> snapshot_with_two_sites() {
  SnapshotBuilder builder = fixture_builder(1);
  CCAP_CHECK_VOID(builder.add(make_site("site-b")));
  CCAP_CHECK_VOID(add_simple_facility(builder, simple_facility()));
  return build_snapshot(builder);
}

// The simple facility plus a second loop that also serves the zone.
std::shared_ptr<const CoolingSnapshot> snapshot_with_two_loops() {
  const SimpleFacility facility = simple_facility();
  LoopRecord second = make_loop("air-loop-2", cooling_capacity::LoopKind::AirSupply,
                                {facility.crah_b.id}, 500000,
                                cooling_capacity::RedundancyClass::None);
  ZoneRecord zone = facility.zone;
  zone.loops = {facility.loop.id, second.id};

  SnapshotBuilder builder = fixture_builder(1);
  CCAP_CHECK_VOID(builder.add(facility.site));
  CCAP_CHECK_VOID(builder.add(facility.facility));
  CCAP_CHECK_VOID(builder.add(facility.crah_a));
  CCAP_CHECK_VOID(builder.add(facility.crah_b));
  CCAP_CHECK_VOID(builder.add(facility.loop));
  CCAP_CHECK_VOID(builder.add(second));
  CCAP_CHECK_VOID(builder.add(zone));
  return build_snapshot(builder);
}

// A generation large enough that its artifact cannot fit in the smallest
// permitted max_artifact_bytes.
std::shared_ptr<const CoolingSnapshot> snapshot_with_many_units(std::size_t units) {
  SnapshotBuilder builder = fixture_builder(1);
  const SimpleFacility facility = simple_facility();
  CCAP_CHECK_VOID(builder.add(facility.site));
  CCAP_CHECK_VOID(builder.add(facility.facility));
  CCAP_CHECK_VOID(builder.add(facility.crah_a));
  CCAP_CHECK_VOID(builder.add(facility.crah_b));
  for (std::size_t index = 0; index < units; ++index) {
    EquipmentRecord unit = make_unit("crah-extra", EquipmentKind::Crah, 100000);
    if (index != 0) {
      const std::string name = "crah-extra-" + std::to_string(index);
      unit.id = equipment_id(name.c_str());
      unit.label = BoundedText::literal(name);
    }
    CCAP_CHECK_VOID(builder.add(unit));
  }
  CCAP_CHECK_VOID(builder.add(facility.loop));
  CCAP_CHECK_VOID(builder.add(facility.zone));
  return build_snapshot(builder);
}

void expect_invalid(const Limits& limits) {
  CCAP_CHECK_ERR(limits.validate(), ErrorCode::InvalidArgument);
}

}  // namespace

CCAP_TEST(the_default_limits_are_valid) {
  CCAP_CHECK_VOID(Limits::defaults().validate());
  Limits limits = Limits::defaults();
  CCAP_CHECK(limits.max_artifact_bytes > 4096ULL);
  CCAP_CHECK_LT(limits.max_artifact_bytes, kHardArtifactCeiling + 1ULL);
}

CCAP_TEST(a_zero_bound_is_rejected) {
  const Limits zero_sites = [] {
    Limits limits;
    limits.max_sites = 0;
    return limits;
  }();
  expect_invalid(zero_sites);

  Limits limits = Limits::defaults();
  limits.max_sites = 0;
  const Result<void> result = limits.validate();
  CCAP_CHECK_FALSE(result.ok());
  if (!result.ok()) {
    CCAP_CHECK_EQ(result.error().code(), ErrorCode::InvalidArgument);
    CCAP_CHECK_EQ(result.error().context().size(), 1U);
    if (!result.error().context().empty()) {
      CCAP_CHECK_EQ(result.error().context().front().key, std::string("field"));
      CCAP_CHECK_EQ(result.error().context().front().value, std::string("max_sites"));
    }
  }

  const auto reject = [](auto mutate) {
    Limits candidate = Limits::defaults();
    mutate(candidate);
    expect_invalid(candidate);
  };
  reject([](Limits& value) { value.max_sites = 0; });
  reject([](Limits& value) { value.max_facilities = 0; });
  reject([](Limits& value) { value.max_zones = 0; });
  reject([](Limits& value) { value.max_loops = 0; });
  reject([](Limits& value) { value.max_plants = 0; });
  reject([](Limits& value) { value.max_manifolds = 0; });
  reject([](Limits& value) { value.max_domains = 0; });
  reject([](Limits& value) { value.max_equipment = 0; });
  reject([](Limits& value) { value.max_evidence = 0; });
  reject([](Limits& value) { value.max_commitments = 0; });
  reject([](Limits& value) { value.max_points_of_connection = 0; });
  reject([](Limits& value) { value.max_loops_per_zone = 0; });
  reject([](Limits& value) { value.max_equipment_per_loop = 0; });
  reject([](Limits& value) { value.max_derate_factors = 0; });
  reject([](Limits& value) { value.max_freshness_rules = 0; });
  reject([](Limits& value) { value.max_generations_retained = 0; });
  reject([](Limits& value) { value.max_writer_epoch = 0; });
}

CCAP_TEST(an_implausible_artifact_ceiling_is_rejected) {
  const auto with_ceiling = [](std::uint64_t bytes) {
    Limits limits = Limits::defaults();
    limits.max_artifact_bytes = bytes;
    return limits;
  };

  // The boundary values are accepted; anything outside them is not.
  CCAP_CHECK_VOID(with_ceiling(4096ULL).validate());
  CCAP_CHECK_VOID(with_ceiling(kHardArtifactCeiling).validate());
  expect_invalid(with_ceiling(4095ULL));
  expect_invalid(with_ceiling(0ULL));
  expect_invalid(with_ceiling(kHardArtifactCeiling + 1ULL));
  expect_invalid(with_ceiling(0xFFFFFFFFFFFFFFFFULL));

  Limits retained = Limits::defaults();
  retained.max_generations_retained = 4096;
  CCAP_CHECK_VOID(retained.validate());
  retained.max_generations_retained = 4097;
  expect_invalid(retained);
}

CCAP_TEST(a_builder_with_small_limits_refuses_extra_records) {
  Limits limits = Limits::defaults();
  limits.max_sites = 1;
  limits.max_zones = 1;
  limits.max_loops = 1;
  limits.max_equipment = 2;
  limits.max_evidence = 1;
  limits.max_commitments = 1;
  CCAP_CHECK_VOID(limits.validate());

  SnapshotBuilder builder = fixture_builder(1);
  builder.set_limits(limits);
  CCAP_CHECK_VOID(builder.add(make_site("site-a")));
  CCAP_CHECK_ERR(builder.add(make_site("site-b")), ErrorCode::LimitExceeded);

  const SimpleFacility facility = simple_facility();
  CCAP_CHECK_VOID(builder.add(facility.facility));
  CCAP_CHECK_VOID(builder.add(facility.crah_a));
  CCAP_CHECK_VOID(builder.add(facility.crah_b));
  EquipmentRecord third = make_unit("crah-c", EquipmentKind::Crah, 100000);
  CCAP_CHECK_ERR(builder.add(third), ErrorCode::LimitExceeded);
  CCAP_CHECK_VOID(builder.add(facility.loop));
  LoopRecord second_loop = make_loop("air-loop-2", cooling_capacity::LoopKind::AirSupply,
                                     {facility.crah_a.id}, 100000,
                                     cooling_capacity::RedundancyClass::None);
  CCAP_CHECK_ERR(builder.add(second_loop), ErrorCode::LimitExceeded);
  CCAP_CHECK_VOID(builder.add(facility.zone));

  EvidenceRecord evidence;
  evidence.id = evidence_id("e-1");
  evidence.kind = EvidenceKind::OperatorAssertion;
  evidence.subject = zone_id("hall-1");
  evidence.observed_at = fixture_now();
  evidence.provenance.actor = actor_id("operator");
  evidence.provenance.reference = cooling_capacity::DocumentRef::literal("note-1");
  evidence.revision = RecordRevision::first();
  CCAP_CHECK_VOID(builder.add(evidence));
  EvidenceRecord other = evidence;
  other.id = evidence_id("e-2");
  CCAP_CHECK_ERR(builder.add(other), ErrorCode::LimitExceeded);

  CommitmentRecord commitment;
  commitment.id = cooling_capacity::CommitmentId::literal("c-1");
  commitment.attempt = cooling_capacity::AttemptId::literal("a-1");
  commitment.zone = zone_id("hall-1");
  commitment.created_generation = CapacityGeneration::first();
  commitment.revision = RecordRevision::first();
  CCAP_CHECK_VOID(builder.add(commitment));
  CCAP_CHECK_ERR(builder.add(commitment), ErrorCode::LimitExceeded);

  // What was accepted is still a coherent generation.
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  CCAP_CHECK_EQ(snapshot->sites().size(), 1U);
  CCAP_CHECK_EQ(snapshot->equipment().size(), 2U);
  CCAP_CHECK_EQ(snapshot->evidence().size(), 1U);
}

CCAP_TEST(decoding_an_artifact_whose_declared_count_exceeds_the_limits_is_refused) {
  const Limits encode_limits = Limits::defaults();
  const std::shared_ptr<const CoolingSnapshot> two_sites = snapshot_with_two_sites();
  CCAP_CHECK_OK(bytes, cooling_capacity::encode_snapshot(*two_sites, encode_limits));
  CCAP_CHECK_EQ(two_sites->sites().size(), 2U);

  Limits one_site = Limits::defaults();
  one_site.max_sites = 1;
  CCAP_CHECK_VOID(one_site.validate());
  CCAP_CHECK_ERR(cooling_capacity::decode_snapshot(bytes, one_site), ErrorCode::LimitExceeded);
  // The same bytes decode once the bound is large enough, so the refusal is the
  // limit and not the artifact.
  CCAP_CHECK_OK(decoded, cooling_capacity::decode_snapshot(bytes, Limits::defaults()));
  CCAP_CHECK_EQ(decoded->sites().size(), 2U);

  const std::shared_ptr<const CoolingSnapshot> two_loops = snapshot_with_two_loops();
  CCAP_CHECK_OK(loop_bytes, cooling_capacity::encode_snapshot(*two_loops, encode_limits));
  Limits one_loop_per_zone = Limits::defaults();
  one_loop_per_zone.max_loops_per_zone = 1;
  CCAP_CHECK_VOID(one_loop_per_zone.validate());
  CCAP_CHECK_ERR(cooling_capacity::decode_snapshot(loop_bytes, one_loop_per_zone),
                 ErrorCode::LimitExceeded);

  Limits one_unit_per_loop = Limits::defaults();
  one_unit_per_loop.max_equipment_per_loop = 1;
  CCAP_CHECK_VOID(one_unit_per_loop.validate());
  CCAP_CHECK_ERR(cooling_capacity::decode_snapshot(loop_bytes, one_unit_per_loop),
                 ErrorCode::LimitExceeded);

  const std::shared_ptr<const CoolingSnapshot> simple = snapshot_with_two_sites();
  CCAP_CHECK_OK(simple_bytes, cooling_capacity::encode_snapshot(*simple, encode_limits));
  Limits one_equipment = Limits::defaults();
  one_equipment.max_equipment = 1;
  CCAP_CHECK_VOID(one_equipment.validate());
  CCAP_CHECK_ERR(cooling_capacity::decode_snapshot(simple_bytes, one_equipment),
                 ErrorCode::LimitExceeded);
}

CCAP_TEST(an_artifact_above_the_configured_ceiling_is_refused_on_both_paths) {
  const std::shared_ptr<const CoolingSnapshot> large = snapshot_with_many_units(200);
  CCAP_CHECK_EQ(large->equipment().size(), 202U);
  CCAP_CHECK_EQ(large->loops().size(), 1U);

  // Encoding refuses to produce an artifact above the ceiling.
  Limits small = Limits::defaults();
  small.max_artifact_bytes = 4096;
  CCAP_CHECK_VOID(small.validate());
  CCAP_CHECK_ERR(cooling_capacity::encode_snapshot(*large, small), ErrorCode::Oversized);

  // Decoding refuses an artifact above the ceiling before it reads a record.
  CCAP_CHECK_OK(bytes, cooling_capacity::encode_snapshot(*large, Limits::defaults()));
  CCAP_CHECK_LT(4096U, bytes.size());
  CCAP_CHECK_OK(manifest, cooling_capacity::read_artifact_manifest(bytes));
  CCAP_CHECK_LT(4096ULL, manifest.payload_bytes);
  CCAP_CHECK_ERR(cooling_capacity::decode_snapshot(bytes, small), ErrorCode::Oversized);
  CCAP_CHECK_OK(decoded, cooling_capacity::decode_snapshot(bytes, Limits::defaults()));
  CCAP_CHECK_EQ(decoded->record_count(), large->record_count());
}