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

#ifndef COOLING_CAPACITY_COMMITMENT_HPP
#define COOLING_CAPACITY_COMMITMENT_HPP

#include <cstdint>
#include <optional>
#include <string_view>

#include "cooling_capacity/ids.hpp"
#include "cooling_capacity/medium.hpp"
#include "cooling_capacity/text.hpp"
#include "cooling_capacity/units.hpp"

namespace cooling_capacity {

// A commitment is the authority to occupy cooling capacity. Only `Held` and
// `Committed` consume capacity: a `Planned` record is a submitted intent that
// has been accepted into the catalog but does not yet reduce what is available
// to anyone else. `Released`, `Expired` and `Superseded` are terminal and
// consume nothing.
enum class CommitmentState : std::uint8_t {
  Planned = 0,
  Held = 1,
  Committed = 2,
  Released = 3,
  Expired = 4,
  Superseded = 5,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(CommitmentState state) noexcept;
[[nodiscard]] CCAP_EXPORT Result<CommitmentState> parse_commitment_state(std::string_view text);
[[nodiscard]] CCAP_EXPORT bool consumes_capacity(CommitmentState state) noexcept;

struct CommitmentRecord {
  CommitmentId id;
  // The attempt that created this record. Idempotency is keyed on the attempt,
  // not on the commitment, so a retried request is recognised even if it
  // chooses a different commitment identifier.
  AttemptId attempt;
  ZoneId zone;
  std::optional<LoopId> pinned_loop;
  CoolingMedium medium = CoolingMedium::Air;
  CompatibilityClass compatibility = CompatibilityClass::AirConvection;
  ThermalPower thermal;
  CommitmentState state = CommitmentState::Planned;
  ActorId actor;
  Timestamp created_at;
  std::optional<Timestamp> expires_at;
  CapacityGeneration created_generation;
  std::optional<CommitmentId> supersedes;
  BoundedText note;
  RecordRevision revision;

  // True when two requests would produce the same commitment content. Used for
  // replay detection, so it deliberately ignores the state and the revision.
  [[nodiscard]] bool same_request_as(const CommitmentRecord& other) const;

  friend bool operator==(const CommitmentRecord& lhs, const CommitmentRecord& rhs);
};

// What an operator is asking for. Every request carries the generation it was
// formed against; a request formed against an older generation is refused
// rather than merged into the current one.
enum class CommitmentOperation : std::uint8_t {
  Plan = 0,
  Hold = 1,
  Commit = 2,
  Release = 3,
  Expire = 4,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(CommitmentOperation operation) noexcept;
[[nodiscard]] CCAP_EXPORT Result<CommitmentOperation> parse_commitment_operation(
    std::string_view text);

// The state a successful operation leaves behind.
[[nodiscard]] CCAP_EXPORT CommitmentState resulting_state(CommitmentOperation operation) noexcept;

struct CommitmentRequest {
  CommitmentOperation operation = CommitmentOperation::Commit;
  CommitmentId id;
  AttemptId attempt;
  ZoneId zone;
  std::optional<LoopId> pinned_loop;
  CoolingMedium medium = CoolingMedium::Air;
  CompatibilityClass compatibility = CompatibilityClass::AirConvection;
  ThermalPower thermal;
  ActorId actor;
  Timestamp requested_at;
  std::optional<Timestamp> expires_at;
  CapacityGeneration expected_generation;
  std::optional<CommitmentId> target;
  BoundedText note;

  [[nodiscard]] Result<void> validate() const;
};

// The result of applying a request. `replayed` is true when the attempt had
// already been applied and nothing changed, which is a success, not a conflict.
struct CommitmentOutcome {
  CommitmentId id;
  AttemptId attempt;
  CommitmentState state = CommitmentState::Planned;
  bool replayed = false;
  CapacityGeneration generation;
  ThermalPower committed_after;
  ThermalPower usable_after;
  ThermalPower reserve_after;

  friend bool operator==(const CommitmentOutcome& lhs, const CommitmentOutcome& rhs);
};

}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_COMMITMENT_HPP
