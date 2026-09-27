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
#include <string_view>

namespace {

using cooling_capacity::ArtifactManifest;
using cooling_capacity::CoolingSnapshot;
using cooling_capacity::Digest;
using cooling_capacity::Limits;
using cooling_capacity::SnapshotBuilder;
using cooling_capacity::SnapshotOrigin;

std::shared_ptr<const CoolingSnapshot> fixture_snapshot(std::uint64_t generation) {
  SnapshotBuilder builder;
  builder.set_generation(CapacityGeneration::from_value(generation));
  builder.set_constructed_at(fixture_now());
  CCAP_CHECK_VOID(add_simple_facility(builder, simple_facility()));
  return build_snapshot(builder);
}

std::string flip_byte(std::string bytes, std::size_t offset) {
  bytes[offset] = static_cast<char>(static_cast<unsigned char>(bytes[offset]) ^ 0x01U);
  return bytes;
}

}  // namespace

CCAP_TEST(an_encoded_artifact_round_trips_with_the_same_digest) {
  const std::shared_ptr<const CoolingSnapshot> snapshot = fixture_snapshot(1);
  const Limits limits = Limits::defaults();

  CCAP_CHECK_OK(bytes, cooling_capacity::encode_snapshot(*snapshot, limits));
  CCAP_CHECK_OK(decoded, cooling_capacity::decode_snapshot(bytes, limits));

  CCAP_CHECK_EQ(decoded->generation(), snapshot->generation());
  CCAP_CHECK_EQ(decoded->digest(), snapshot->digest());
  CCAP_CHECK_EQ(decoded->record_count(), snapshot->record_count());
  CCAP_CHECK_EQ(decoded->constructed_at(), snapshot->constructed_at());
  CCAP_CHECK_EQ(decoded->sites().size(), snapshot->sites().size());
  CCAP_CHECK_EQ(decoded->facilities().size(), snapshot->facilities().size());
  CCAP_CHECK_EQ(decoded->equipment().size(), snapshot->equipment().size());
  CCAP_CHECK_EQ(decoded->loops().size(), snapshot->loops().size());
  CCAP_CHECK_EQ(decoded->zones().size(), snapshot->zones().size());
  CCAP_CHECK_EQ(decoded->evidence().size(), snapshot->evidence().size());
  CCAP_CHECK_EQ(decoded->commitments().size(), snapshot->commitments().size());
  CCAP_CHECK(decoded->find_zone(zone_id("hall-1")) != nullptr);
  CCAP_CHECK(decoded->find_equipment(equipment_id("crah-a")) != nullptr);
  CCAP_CHECK(decoded->policy() == snapshot->policy());
  CCAP_CHECK_EQ(decoded->limits().max_sites, limits.max_sites);

  // A decoded artifact is data: the caller decides how much authority to give
  // it, so the decoder never stamps it as recovered.
  CCAP_CHECK_EQ(decoded->origin(), SnapshotOrigin::Constructed);
  CCAP_CHECK_FALSE(decoded->revalidated_at().has_value());

  CCAP_CHECK_OK(manifest, cooling_capacity::read_artifact_manifest(bytes));
  CCAP_CHECK_EQ(bytes.size(), cooling_capacity::kArtifactHeaderSize +
                                  static_cast<std::size_t>(manifest.payload_bytes) + Digest::kSize);

  // The decodable artifact is exactly the canonical encoding of what it holds.
  CCAP_CHECK_OK(reencoded, cooling_capacity::encode_snapshot(*decoded, limits));
  CCAP_CHECK(reencoded == bytes);
}

CCAP_TEST(encoding_twice_produces_identical_bytes) {
  const std::shared_ptr<const CoolingSnapshot> snapshot = fixture_snapshot(1);
  const Limits limits = Limits::defaults();

  CCAP_CHECK_OK(first, cooling_capacity::encode_snapshot(*snapshot, limits));
  CCAP_CHECK_OK(second, cooling_capacity::encode_snapshot(*snapshot, limits));
  CCAP_CHECK_EQ(first.size(), second.size());
  CCAP_CHECK(first == second);
  CCAP_CHECK(first.size() > cooling_capacity::kArtifactHeaderSize + Digest::kSize);

  // The encoding is a pure function of the contents: a second generation with
  // the same records but a different generation number encodes differently, and
  // the same generation always encodes the same way.
  const std::shared_ptr<const CoolingSnapshot> other = fixture_snapshot(2);
  CCAP_CHECK_OK(third, cooling_capacity::encode_snapshot(*other, limits));
  CCAP_CHECK_FALSE(third == first);
  CCAP_CHECK_EQ(third.size(), first.size());
  CCAP_CHECK_OK(repeat, cooling_capacity::encode_snapshot(*other, limits));
  CCAP_CHECK(repeat == third);
}

CCAP_TEST(the_manifest_describes_the_artifact_without_decoding_it) {
  const std::shared_ptr<const CoolingSnapshot> snapshot = fixture_snapshot(3);
  const Limits limits = Limits::defaults();
  CCAP_CHECK_OK(bytes, cooling_capacity::encode_snapshot(*snapshot, limits));

  CCAP_CHECK_OK(manifest, cooling_capacity::read_artifact_manifest(bytes));
  CCAP_CHECK_EQ(manifest.format_version, cooling_capacity::kArtifactFormatVersion);
  CCAP_CHECK_EQ(manifest.generation, CapacityGeneration::from_value(3));
  CCAP_CHECK_EQ(manifest.generation, snapshot->generation());
  CCAP_CHECK_EQ(manifest.payload_bytes,
                static_cast<std::uint64_t>(bytes.size() - cooling_capacity::kArtifactHeaderSize -
                                           Digest::kSize));
  CCAP_CHECK(manifest.verified);
  CCAP_CHECK_EQ(manifest.digest, snapshot->digest());
  CCAP_CHECK_EQ(manifest.digest, cooling_capacity::canonical_digest(*snapshot));

  // The trailer really is the digest of everything before it.
  const std::string_view body(bytes.data(), bytes.size() - Digest::kSize);
  CCAP_CHECK_EQ(Digest::of(body), manifest.digest);
  CCAP_CHECK_FALSE(manifest.digest.is_zero());
  CCAP_CHECK_EQ(manifest.digest.to_hex().size(), 64U);
}

CCAP_TEST(flipping_one_payload_byte_is_a_digest_mismatch) {
  const std::shared_ptr<const CoolingSnapshot> snapshot = fixture_snapshot(1);
  const Limits limits = Limits::defaults();
  CCAP_CHECK_OK(bytes, cooling_capacity::encode_snapshot(*snapshot, limits));
  CCAP_CHECK_OK(manifest, cooling_capacity::read_artifact_manifest(bytes));
  CCAP_CHECK(manifest.verified);

  // A single flipped bit inside the payload, and another inside the header,
  // both break the recorded digest rather than being read as data.
  const std::size_t payload_offset = cooling_capacity::kArtifactHeaderSize + 8U;
  CCAP_CHECK_LT(payload_offset, bytes.size() - Digest::kSize);
  const std::string payload_flip = flip_byte(bytes, payload_offset);
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(payload_flip),
                 cooling_capacity::ErrorCode::DigestMismatch);
  CCAP_CHECK_ERR(cooling_capacity::decode_snapshot(payload_flip, limits),
                 cooling_capacity::ErrorCode::DigestMismatch);

  const std::string header_flip = flip_byte(bytes, 32U);
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(header_flip),
                 cooling_capacity::ErrorCode::DigestMismatch);
  CCAP_CHECK_ERR(cooling_capacity::decode_snapshot(header_flip, limits),
                 cooling_capacity::ErrorCode::DigestMismatch);

  const std::string trailer_flip = flip_byte(bytes, bytes.size() - 1U);
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(trailer_flip),
                 cooling_capacity::ErrorCode::DigestMismatch);

  // The untouched artifact still verifies, so the failures above are the flip
  // and not a damaged fixture.
  CCAP_CHECK_OK(still_valid, cooling_capacity::read_artifact_manifest(bytes));
  CCAP_CHECK(still_valid.verified);
}

CCAP_TEST(the_text_rendering_names_the_generation_and_the_digest) {
  const std::shared_ptr<const CoolingSnapshot> snapshot = fixture_snapshot(1);
  const std::string text = cooling_capacity::render_snapshot_text(*snapshot);

  CCAP_CHECK(text.find("cooling-capacity snapshot generation 1\n") != std::string::npos);
  CCAP_CHECK(text.find("generation " + snapshot->generation().to_string() + "\n") !=
             std::string::npos);
  CCAP_CHECK(text.find("digest " + snapshot->digest().to_hex() + "\n") != std::string::npos);
  CCAP_CHECK(text.find("constructed_at " + snapshot->constructed_at().to_string() + "\n") !=
             std::string::npos);
  CCAP_CHECK(text.find("policy default-policy 1\n") != std::string::npos);
  CCAP_CHECK(text.find("records 6\n") != std::string::npos);
  CCAP_CHECK(text.find("site site-a\n") != std::string::npos);
  CCAP_CHECK(text.find("equipment crah-a\n") != std::string::npos);
  CCAP_CHECK(text.find("loop air-loop-1\n") != std::string::npos);
  CCAP_CHECK(text.find("zone hall-1\n") != std::string::npos);
  CCAP_CHECK_EQ(cooling_capacity::render_snapshot_text(*snapshot), text);

  // A round trip through the canonical encoding preserves the rendering, so the
  // text form is a faithful projection of the persisted state.
  const Limits limits = Limits::defaults();
  CCAP_CHECK_OK(bytes, cooling_capacity::encode_snapshot(*snapshot, limits));
  CCAP_CHECK_OK(decoded, cooling_capacity::decode_snapshot(bytes, limits));
  CCAP_CHECK_EQ(cooling_capacity::render_snapshot_text(*decoded), text);
  CCAP_CHECK_NE(cooling_capacity::render_snapshot_text(*fixture_snapshot(2)), text);
}

CCAP_TEST(canonical_digest_is_stable_and_bound_to_the_contents) {
  const std::shared_ptr<const CoolingSnapshot> snapshot = fixture_snapshot(1);
  const Digest first = cooling_capacity::canonical_digest(*snapshot);
  const Digest second = cooling_capacity::canonical_digest(*snapshot);
  CCAP_CHECK_EQ(first, second);
  CCAP_CHECK_EQ(first, snapshot->digest());

  // The digest a snapshot carries is the digest its artifact records.
  const Limits limits = Limits::defaults();
  CCAP_CHECK_OK(bytes, cooling_capacity::encode_snapshot(*snapshot, limits));
  CCAP_CHECK_OK(manifest, cooling_capacity::read_artifact_manifest(bytes));
  CCAP_CHECK_EQ(first, manifest.digest);
  const std::string_view body(bytes.data(), bytes.size() - Digest::kSize);
  CCAP_CHECK_EQ(first, Digest::of(body));

  // A different generation with identical records still has a different digest,
  // so a digest always names one generation.
  const std::shared_ptr<const CoolingSnapshot> other = fixture_snapshot(2);
  CCAP_CHECK_NE(cooling_capacity::canonical_digest(*other), first);
  CCAP_CHECK_NE(other->digest(), snapshot->digest());

  // And a record-level change is visible without renumbering the generation.
  SimpleFacility facility = simple_facility();
  facility.crah_a.nominal = watts(410000);
  SnapshotBuilder builder;
  builder.set_generation(CapacityGeneration::from_value(1));
  builder.set_constructed_at(fixture_now());
  CCAP_CHECK_VOID(add_simple_facility(builder, facility));
  const std::shared_ptr<const CoolingSnapshot> changed = build_snapshot(builder);
  CCAP_CHECK_NE(cooling_capacity::canonical_digest(*changed), first);
}