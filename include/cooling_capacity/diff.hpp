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

#ifndef COOLING_CAPACITY_DIFF_HPP
#define COOLING_CAPACITY_DIFF_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cooling_capacity/digest.hpp"
#include "cooling_capacity/explanation.hpp"
#include "cooling_capacity/ids.hpp"
#include "cooling_capacity/snapshot.hpp"

namespace cooling_capacity {

enum class ChangeKind : std::uint8_t {
  Added = 0,
  Removed = 1,
  Modified = 2,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(ChangeKind kind) noexcept;

// One field that differs between two versions of a record. Both sides are
// rendered canonically so the diff is stable and readable.
struct FieldDelta {
  Identifier field;
  std::string before;
  std::string after;

  friend bool operator==(const FieldDelta& lhs, const FieldDelta& rhs) {
    return lhs.field == rhs.field && lhs.before == rhs.before && lhs.after == rhs.after;
  }
};

struct RecordChange {
  EntityKind kind = EntityKind::Zone;
  Identifier id;
  ChangeKind change = ChangeKind::Modified;
  std::vector<FieldDelta> fields;

  friend bool operator==(const RecordChange& lhs, const RecordChange& rhs);
  [[nodiscard]] std::string to_string() const;
};

struct GenerationDiff {
  CapacityGeneration from;
  CapacityGeneration to;
  Digest from_digest;
  Digest to_digest;
  std::size_t added = 0;
  std::size_t removed = 0;
  std::size_t modified = 0;
  bool identical = false;
  std::vector<RecordChange> changes;
  std::vector<ExplanationStep> explanation;

  [[nodiscard]] std::size_t change_count() const noexcept {
    return added + removed + modified;
  }
};

// Deterministic difference between two snapshots. Changes are ordered by record
// kind and then by identifier; field deltas within a record are ordered by
// field name. Nothing about the ordering depends on insertion order or on hash
// container iteration.
[[nodiscard]] CCAP_EXPORT GenerationDiff diff(const CoolingSnapshot& from,
                                              const CoolingSnapshot& to);

// Canonical field rendering used by diffs and by inspection tooling.
[[nodiscard]] CCAP_EXPORT std::vector<std::pair<std::string, std::string>> record_fields(
    const CoolingSnapshot& snapshot, EntityKind kind, const Identifier& id);

}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_DIFF_HPP
