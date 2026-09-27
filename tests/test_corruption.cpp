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

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace {

using cooling_capacity::Digest;
using cooling_capacity::ErrorCode;
using cooling_capacity::Limits;
using cooling_capacity::SnapshotBuilder;

constexpr std::size_t kHeaderSize = cooling_capacity::kArtifactHeaderSize;
constexpr std::size_t kVersionOffset = 8;
constexpr std::size_t kEndianOffset = 12;
constexpr std::size_t kPayloadBytesOffset = 24;
// The marker a big-endian host would write, decoded as little-endian.
constexpr std::uint32_t kOppositeEndianMarker = 0x04030201U;

std::shared_ptr<const CoolingSnapshot> fixture_snapshot() {
  SnapshotBuilder builder;
  builder.set_generation(CapacityGeneration::first());
  builder.set_constructed_at(fixture_now());
  CCAP_CHECK_VOID(add_simple_facility(builder, simple_facility()));
  return build_snapshot(builder);
}

std::string valid_artifact() {
  const Limits limits = Limits::defaults();
  const std::shared_ptr<const CoolingSnapshot> snapshot = fixture_snapshot();
  Result<std::string> encoded = cooling_capacity::encode_snapshot(*snapshot, limits);
  if (!encoded.ok()) {
    CCAP_CHECK_VOID(encoded);
    return std::string();
  }
  return std::move(encoded).value();
}

void put_u32_at(std::string& bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned index = 0; index < 4U; ++index) {
    bytes[offset + index] = static_cast<char>((value >> (8U * index)) & 0xFFU);
  }
}

void put_u64_at(std::string& bytes, std::size_t offset, std::uint64_t value) {
  for (unsigned index = 0; index < 8U; ++index) {
    bytes[offset + index] = static_cast<char>((value >> (8U * index)) & 0xFFU);
  }
}

std::uint64_t read_u64_at(const std::string& bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8U; ++index) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + index]))
             << (8U * index);
  }
  return value;
}

// Appends the integrity trailer for a body of header and payload bytes, which is
// exactly what the encoder writes.
std::string with_trailer(std::string body) {
  const Digest digest = Digest::of(body);
  const std::array<std::uint8_t, Digest::kSize>& raw = digest.bytes();
  body.append(reinterpret_cast<const char*>(raw.data()), raw.size());
  return body;
}

Result<std::uint64_t> declared_payload_bytes(const std::string& bytes) {
  if (bytes.size() < kHeaderSize) {
    return cooling_capacity::Error(ErrorCode::Truncated, "artifact is shorter than its header");
  }
  return read_u64_at(bytes, kPayloadBytesOffset);
}

}  // namespace

CCAP_TEST(truncated_artifacts_are_refused_as_truncated) {
  const std::string bytes = valid_artifact();
  CCAP_CHECK(bytes.size() > kHeaderSize + Digest::kSize);
  const Limits limits = Limits::defaults();

  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(bytes.substr(0, bytes.size() / 2U)),
                 ErrorCode::Truncated);
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(bytes.substr(0, kHeaderSize - 1U)),
                 ErrorCode::Truncated);
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(bytes.substr(0, bytes.size() - 1U)),
                 ErrorCode::Truncated);
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(std::string_view()),
                 ErrorCode::Truncated);

  CCAP_CHECK_ERR(cooling_capacity::decode_snapshot(bytes.substr(0, bytes.size() / 2U), limits),
                 ErrorCode::Truncated);
  CCAP_CHECK_ERR(cooling_capacity::decode_snapshot(bytes.substr(0, kHeaderSize - 1U), limits),
                 ErrorCode::Truncated);
  CCAP_CHECK_ERR(cooling_capacity::decode_snapshot(bytes.substr(0, bytes.size() - 1U), limits),
                 ErrorCode::Truncated);
}

CCAP_TEST(a_wrong_format_version_is_refused) {
  std::string bytes = valid_artifact();
  const Limits limits = Limits::defaults();
  CCAP_CHECK_OK(manifest, cooling_capacity::read_artifact_manifest(bytes));
  CCAP_CHECK_EQ(manifest.format_version, cooling_capacity::kArtifactFormatVersion);

  std::string newer = bytes;
  put_u32_at(newer, kVersionOffset, cooling_capacity::kArtifactFormatVersion + 1U);
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(newer), ErrorCode::IncompatibleVersion);
  CCAP_CHECK_ERR(cooling_capacity::decode_snapshot(newer, limits), ErrorCode::IncompatibleVersion);

  std::string zero = bytes;
  put_u32_at(zero, kVersionOffset, 0U);
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(zero), ErrorCode::IncompatibleVersion);
  CCAP_CHECK_ERR(cooling_capacity::decode_snapshot(zero, limits), ErrorCode::IncompatibleVersion);

  // A wrong version is reported as a version problem even though it also
  // invalidates the digest, because the reader must not claim to understand the
  // layout it is looking at.
  std::string huge = bytes;
  put_u32_at(huge, kVersionOffset, 0xFFFFFFFFU);
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(huge), ErrorCode::IncompatibleVersion);
}

CCAP_TEST(a_swapped_byte_order_marker_is_refused_explicitly) {
  std::string bytes = valid_artifact();
  const Limits limits = Limits::defaults();

  std::string swapped = bytes;
  put_u32_at(swapped, kEndianOffset, kOppositeEndianMarker);
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(swapped), ErrorCode::WrongEndianness);
  CCAP_CHECK_ERR(cooling_capacity::decode_snapshot(swapped, limits), ErrorCode::WrongEndianness);

  // A marker that is neither the expected value nor the opposite byte order is
  // corrupt rather than an endianness statement.
  std::string nonsense = bytes;
  put_u32_at(nonsense, kEndianOffset, 0x11223344U);
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(nonsense), ErrorCode::Corruption);
}

CCAP_TEST(a_bogus_magic_is_refused_as_an_unsupported_format) {
  std::string bytes = valid_artifact();
  const Limits limits = Limits::defaults();

  std::string renamed = bytes;
  renamed[0] = 'X';
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(renamed),
                 ErrorCode::UnsupportedFormat);
  CCAP_CHECK_ERR(cooling_capacity::decode_snapshot(renamed, limits),
                 ErrorCode::UnsupportedFormat);

  std::string emptied = bytes;
  for (std::size_t index = 0; index < 8U; ++index) {
    emptied[index] = '\0';
  }
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(emptied),
                 ErrorCode::UnsupportedFormat);
}

CCAP_TEST(a_corrupted_digest_is_refused) {
  std::string bytes = valid_artifact();
  const Limits limits = Limits::defaults();

  std::string flipped = bytes;
  flipped[flipped.size() - 1U] =
      static_cast<char>(static_cast<unsigned char>(flipped[flipped.size() - 1U]) ^ 0x80U);
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(flipped), ErrorCode::DigestMismatch);
  CCAP_CHECK_ERR(cooling_capacity::decode_snapshot(flipped, limits), ErrorCode::DigestMismatch);

  std::string zeroed = bytes;
  for (std::size_t index = zeroed.size() - Digest::kSize; index < zeroed.size(); ++index) {
    zeroed[index] = '\0';
  }
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(zeroed), ErrorCode::DigestMismatch);

  CCAP_CHECK_OK(valid, cooling_capacity::read_artifact_manifest(bytes));
  CCAP_CHECK(valid.verified);
}

CCAP_TEST(a_declared_payload_length_that_does_not_fit_is_refused) {
  std::string bytes = valid_artifact();
  const Limits limits = Limits::defaults();
  CCAP_CHECK_OK(declared, declared_payload_bytes(bytes));
  const std::uint64_t payload = declared;
  CCAP_CHECK(payload > 8U);

  std::string longer = bytes;
  put_u64_at(longer, kPayloadBytesOffset, payload + 4096U);
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(longer), ErrorCode::Truncated);
  CCAP_CHECK_ERR(cooling_capacity::decode_snapshot(longer, limits), ErrorCode::Truncated);

  std::string shorter = bytes;
  put_u64_at(shorter, kPayloadBytesOffset, payload - 8U);
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(shorter), ErrorCode::Truncated);

  // An implausible declared length is refused before any allocation is attempted.
  std::string implausible = bytes;
  put_u64_at(implausible, kPayloadBytesOffset, 1ULL << 35U);
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(implausible), ErrorCode::Oversized);

  std::string enormous = bytes;
  put_u64_at(enormous, kPayloadBytesOffset, 0xFFFFFFFFFFFFFFFFULL);
  CCAP_CHECK_ERR(cooling_capacity::read_artifact_manifest(enormous), ErrorCode::Oversized);
}

CCAP_TEST(an_artifact_with_a_truncated_final_record_does_not_decode) {
  const std::string bytes = valid_artifact();
  const Limits limits = Limits::defaults();
  CCAP_CHECK_OK(declared, declared_payload_bytes(bytes));
  const std::size_t payload_bytes = static_cast<std::size_t>(declared);
  CCAP_CHECK(payload_bytes > 8U);

  // Keep the header and digest self-consistent and only shorten the payload, so
  // the failure has to come from the record decoder rather than from the
  // manifest check.
  std::string body(bytes.data(), kHeaderSize + payload_bytes - 8U);
  put_u64_at(body, kPayloadBytesOffset, payload_bytes - 8U);
  const std::string damaged = with_trailer(body);

  CCAP_CHECK_OK(manifest, cooling_capacity::read_artifact_manifest(damaged));
  CCAP_CHECK(manifest.verified);
  CCAP_CHECK_EQ(manifest.payload_bytes, static_cast<std::uint64_t>(payload_bytes - 8U));

  const Result<std::shared_ptr<const CoolingSnapshot>> decoded =
      cooling_capacity::decode_snapshot(damaged, limits);
  CCAP_CHECK_FALSE(decoded.ok());
  if (!decoded.ok()) {
    const ErrorCode code = decoded.error().code();
    CCAP_CHECK(code == ErrorCode::Truncated || code == ErrorCode::Corruption);
  }
}

CCAP_TEST(an_edited_payload_is_bound_to_its_digest) {
  const std::string bytes = valid_artifact();
  const Limits limits = Limits::defaults();
  CCAP_CHECK_OK(original, cooling_capacity::read_artifact_manifest(bytes));

  // The site identifier is the first text in the payload and its label follows
  // it immediately, so a valid label rewrite leaves a structurally readable
  // artifact. The digest is an integrity check over the bytes, not an
  // authenticity proof: an editor that recomputes it produces a different, but
  // self-consistent, generation.
  const std::size_t identifier = bytes.find("site-a", kHeaderSize);
  CCAP_CHECK_NE(identifier, std::string::npos);
  const std::size_t label = bytes.find("site-a", identifier + 1U);
  CCAP_CHECK_NE(label, std::string::npos);
  CCAP_CHECK(label + 6U > kHeaderSize);

  std::string body(bytes.data(), bytes.size() - Digest::kSize);
  body[label + 5U] = 'b';
  const std::string edited = with_trailer(body);

  CCAP_CHECK_OK(manifest, cooling_capacity::read_artifact_manifest(edited));
  CCAP_CHECK(manifest.verified);
  CCAP_CHECK_NE(manifest.digest, original.digest);

  CCAP_CHECK_OK(decoded, cooling_capacity::decode_snapshot(edited, limits));
  CCAP_CHECK_EQ(decoded->digest(), manifest.digest);
  CCAP_CHECK_EQ(decoded->sites().size(), 1U);
  if (!decoded->sites().empty()) {
    CCAP_CHECK_EQ(decoded->sites().front().label.str(), std::string("site-b"));
  }
  // The edit cannot masquerade as the original generation.
  CCAP_CHECK_NE(decoded->digest(), original.digest);
  CCAP_CHECK_EQ(decoded->generation(), original.generation);
}

CCAP_TEST(an_edit_that_breaks_a_structural_rule_is_refused) {
  const std::string bytes = valid_artifact();
  const Limits limits = Limits::defaults();
  const std::size_t identifier = bytes.find("site-a", kHeaderSize);
  CCAP_CHECK_NE(identifier, std::string::npos);

  // A space is outside the identifier grammar, so the record cannot be read even
  // though the trailer digest has been recomputed over the edited bytes: the
  // failure provably comes from the structure, not from the integrity check.
  std::string body(bytes.data(), bytes.size() - Digest::kSize);
  body[identifier + 5U] = ' ';
  const std::string edited = with_trailer(body);

  CCAP_CHECK_OK(manifest, cooling_capacity::read_artifact_manifest(edited));
  CCAP_CHECK(manifest.verified);
  CCAP_CHECK_ERR(cooling_capacity::decode_snapshot(edited, limits),
                 ErrorCode::InvalidIdentifier);

  // The untouched artifact still decodes, so the refusal above is the edit.
  CCAP_CHECK_OK(still_valid, cooling_capacity::decode_snapshot(bytes, limits));
  CCAP_CHECK_EQ(still_valid->sites().size(), 1U);
  if (!still_valid->sites().empty()) {
    CCAP_CHECK_EQ(still_valid->sites().front().id.str(), std::string("site-a"));
  }
}

CCAP_TEST(a_non_canonical_record_order_is_refused) {
  // Two sites whose encoded records are the same size. Swapping them keeps every
  // length and the digest self-consistent, but the payload is no longer in
  // canonical order: a decoder that quietly accepted it would produce a
  // generation whose own encoding differs from the bytes it came from.
  const SimpleFacility facility = simple_facility();
  SnapshotBuilder builder;
  builder.set_generation(CapacityGeneration::first());
  builder.set_constructed_at(fixture_now());
  CCAP_CHECK_VOID(builder.add(make_site("site-b")));
  CCAP_CHECK_VOID(add_simple_facility(builder, facility));
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  CCAP_CHECK_EQ(snapshot->sites().size(), 2U);

  const Limits limits = Limits::defaults();
  CCAP_CHECK_OK(bytes, cooling_capacity::encode_snapshot(*snapshot, limits));

  const std::size_t identifier = bytes.find("site-a", kHeaderSize);
  CCAP_CHECK_NE(identifier, std::string::npos);
  const std::size_t first = identifier - 4U;
  // id text, label text and the revision of one site record.
  const std::size_t record = 4U + 6U + 4U + 6U + 8U;
  CCAP_CHECK_EQ(bytes.compare(first + 4U, 6U, "site-a"), 0);
  CCAP_CHECK_EQ(bytes.compare(first + record + 4U, 6U, "site-b"), 0);

  std::string body(bytes.data(), bytes.size() - Digest::kSize);
  const std::string first_record = body.substr(first, record);
  const std::string second_record = body.substr(first + record, record);
  body.replace(first, record, second_record);
  body.replace(first + record, record, first_record);
  const std::string reordered = with_trailer(body);

  CCAP_CHECK_OK(manifest, cooling_capacity::read_artifact_manifest(reordered));
  CCAP_CHECK(manifest.verified);
  CCAP_CHECK_ERR(cooling_capacity::decode_snapshot(reordered, limits), ErrorCode::Corruption);
}