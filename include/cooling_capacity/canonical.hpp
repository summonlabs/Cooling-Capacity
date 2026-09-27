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

#ifndef COOLING_CAPACITY_CANONICAL_HPP
#define COOLING_CAPACITY_CANONICAL_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "cooling_capacity/digest.hpp"
#include "cooling_capacity/errors.hpp"
#include "cooling_capacity/export.hpp"
#include "cooling_capacity/limits.hpp"
#include "cooling_capacity/snapshot.hpp"

namespace cooling_capacity {

// The canonical byte encoding of one snapshot generation.
//
// Layout: a fixed header, then a length-prefixed payload, then the digest of the
// header and payload. Every integer is little-endian and is written byte by
// byte, never by copying a struct. The header carries a byte-order marker whose
// decoded value must be the expected constant, so an artifact written on a
// different-endian host is refused explicitly rather than misread.
//
// Decoding is strict: wrong version, wrong byte order, wrong digest, truncated,
// oversized, unknown tag, duplicate identifier and dangling reference are all
// refused with a distinct error code.

// Size of the fixed header, before the payload.
inline constexpr std::size_t kArtifactHeaderSize = 64;

// Encodes one snapshot. The snapshot must have been produced by a builder, so
// its ordering is already canonical.
[[nodiscard]] CCAP_EXPORT Result<std::string> encode_snapshot(const CoolingSnapshot& snapshot,
                                                              const Limits& limits);

// Decodes one snapshot. `limits` bounds every declared length before it is
// used. The result is a freshly constructed snapshot whose origin is
// `Constructed`: a decoded artifact is data, and the caller decides how much
// authority to give it.
[[nodiscard]] CCAP_EXPORT Result<std::shared_ptr<const CoolingSnapshot>> decode_snapshot(
    std::string_view bytes, const Limits& limits);

// The digest a snapshot would carry when encoded. This is the integrity anchor
// the generation file and the manifest both refer to.
[[nodiscard]] CCAP_EXPORT Digest canonical_digest(const CoolingSnapshot& snapshot);

// A one-line, human-readable manifest of an artifact: format version, the
// generation, the digest and the record counts. Produced without decoding the
// whole payload, so inspection tooling can describe a damaged artifact.
struct ArtifactManifest {
  std::uint32_t format_version = 0;
  CapacityGeneration generation;
  Digest digest;
  std::uint64_t payload_bytes = 0;
  bool verified = false;
};

[[nodiscard]] CCAP_EXPORT Result<ArtifactManifest> read_artifact_manifest(std::string_view bytes);

// Renders the snapshot in a canonical, line-oriented text form. Used by the
// command line tool's export command. The text form is not the persistence
// format; it is a deterministic projection for operators and diffs.
[[nodiscard]] CCAP_EXPORT std::string render_snapshot_text(const CoolingSnapshot& snapshot);

}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_CANONICAL_HPP
