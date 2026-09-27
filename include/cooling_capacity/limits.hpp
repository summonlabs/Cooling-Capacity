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

#ifndef COOLING_CAPACITY_LIMITS_HPP
#define COOLING_CAPACITY_LIMITS_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include "cooling_capacity/errors.hpp"
#include "cooling_capacity/export.hpp"

namespace cooling_capacity {

// On-disk and on-the-wire format identifiers. Changing either value is a
// breaking format change; a store written by a different value is refused
// rather than guessed at.
inline constexpr std::uint32_t kArtifactFormatVersion = 1;
inline constexpr std::uint32_t kStoreManifestVersion = 1;

// Every bound that applies to an in-memory model or to a persisted artifact.
// All of them are checked before the corresponding allocation, so a hostile or
// damaged artifact cannot make the library allocate without limit.
//
// Defaults are deliberately far above any realistic facility and far below any
// value that would exhaust a normal host.
struct CCAP_EXPORT Limits {
  std::size_t max_sites = 64;
  std::size_t max_facilities = 256;
  std::size_t max_zones = 4096;
  std::size_t max_loops = 4096;
  std::size_t max_plants = 1024;
  std::size_t max_manifolds = 4096;
  std::size_t max_domains = 4096;
  std::size_t max_equipment = 16384;
  std::size_t max_evidence = 65536;
  std::size_t max_commitments = 65536;
  std::size_t max_points_of_connection = 4096;

  std::size_t max_loops_per_zone = 128;
  std::size_t max_equipment_per_loop = 512;
  std::size_t max_derate_factors = 32;
  std::size_t max_freshness_rules = 32;

  // Hard ceiling on a single artifact, checked against the file size before
  // the file is read and against every declared length inside it.
  std::uint64_t max_artifact_bytes = 64ULL * 1024ULL * 1024ULL;

  // Generations retained by the durable store before the oldest are retired.
  std::size_t max_generations_retained = 8;

  // Smallest and largest accepted writing epoch, so a damaged manifest cannot
  // hand out an implausible authority value.
  std::uint64_t max_writer_epoch = 0xF000000000000000ULL;

  [[nodiscard]] Result<void> validate() const;
  [[nodiscard]] static Limits defaults() noexcept { return Limits{}; }
};

}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_LIMITS_HPP
