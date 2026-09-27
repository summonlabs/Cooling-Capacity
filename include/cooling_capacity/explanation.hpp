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

#ifndef COOLING_CAPACITY_EXPLANATION_HPP
#define COOLING_CAPACITY_EXPLANATION_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cooling_capacity/errors.hpp"
#include "cooling_capacity/export.hpp"

namespace cooling_capacity {

// Stable explanation codes. Every capacity answer, admission decision and diff
// is accompanied by an ordered list of these steps, each with named parameters.
// The code and the parameter names are the machine-readable contract; the
// rendered sentence is the human-readable projection of the same data.
enum class ExplanationCode : std::uint8_t {
  SnapshotBasis = 0,
  ZoneResolved = 1,
  ZoneNotFound = 2,
  MediumUnsupported = 3,
  ClassUnsupported = 4,
  LoopConsidered = 5,
  LoopClassMismatch = 6,
  LoopTransportDeclared = 7,
  LoopTransportUndeclared = 8,
  LoopCapacity = 9,
  EquipmentConsidered = 10,
  EquipmentOutOfService = 11,
  EquipmentStateUnknown = 12,
  EquipmentCapacitySource = 13,
  EquipmentValidationMissing = 14,
  EquipmentDerated = 15,
  EquipmentDerateBelowFloor = 16,
  EquipmentDegraded = 17,
  EquipmentSharedBetweenLoops = 18,
  EquipmentTransportOnly = 19,
  EquipmentCapacityMissing = 20,
  RedundancyApplied = 21,
  RedundancyRequirementMet = 22,
  RedundancyRequirementUnmet = 23,
  CommittedLoadCharged = 24,
  ZoneLocalHeadroom = 25,
  PlantShared = 26,
  PlantSupplyAlternates = 27,
  SharedHeadroomIsShared = 28,
  ReserveHeld = 29,
  Bottleneck = 30,
  CandidateRequest = 31,
  CandidateDisposition = 32,
  EvidenceAssessed = 33,
  EvidenceRejected = 34,
  PolicyApplied = 35,
  SnapshotRecovered = 36,
  SnapshotRevalidated = 37,
  CommitmentApplied = 38,
  CommitmentReplayed = 39,
  NoObservation = 40,
  ObservationCharged = 41,
  DiffSummary = 42,
  OverCommitted = 43,
  TransportOnlyGroupMember = 44,
};

inline constexpr std::size_t kExplanationCodeCount = 45;

[[nodiscard]] CCAP_EXPORT std::string_view to_string(ExplanationCode code) noexcept;
[[nodiscard]] CCAP_EXPORT ExplanationCode explanation_code_at(std::size_t index);

// The parameterised sentence for one code. Placeholders are written {name}.
[[nodiscard]] CCAP_EXPORT std::string_view explanation_template(ExplanationCode code) noexcept;

// The placeholder names a code's template requires, in order of first
// appearance. A test asserts that every step the engine emits supplies them.
[[nodiscard]] CCAP_EXPORT std::vector<std::string> explanation_required_parameters(
    ExplanationCode code);

struct ExplanationParam {
  std::string key;
  std::string value;

  friend bool operator==(const ExplanationParam& lhs, const ExplanationParam& rhs) {
    return lhs.key == rhs.key && lhs.value == rhs.value;
  }
};

class CCAP_EXPORT ExplanationStep {
 public:
  ExplanationStep() = default;
  ExplanationStep(ExplanationCode code, std::vector<ExplanationParam> params);

  [[nodiscard]] ExplanationCode code() const noexcept { return code_; }
  [[nodiscard]] const std::vector<ExplanationParam>& params() const noexcept { return params_; }
  [[nodiscard]] bool has(const std::string& key) const;

  // The template with every placeholder substituted. A placeholder with no
  // parameter renders as `<missing:name>`, which is visible rather than silent.
  [[nodiscard]] std::string render() const;
  // "code: rendered sentence".
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const ExplanationStep& lhs, const ExplanationStep& rhs) {
    return lhs.code_ == rhs.code_ && lhs.params_ == rhs.params_;
  }

 private:
  ExplanationCode code_ = ExplanationCode::SnapshotBasis;
  std::vector<ExplanationParam> params_;
};

// Convenience builder that keeps call sites readable.
class CCAP_EXPORT ExplanationBuilder {
 public:
  ExplanationBuilder& add(ExplanationCode code, std::vector<ExplanationParam> params = {});
  [[nodiscard]] const std::vector<ExplanationStep>& steps() const noexcept { return steps_; }
  [[nodiscard]] std::vector<ExplanationStep> take() { return std::move(steps_); }
  [[nodiscard]] std::string render() const;

 private:
  std::vector<ExplanationStep> steps_;
};

[[nodiscard]] CCAP_EXPORT std::string render_steps(const std::vector<ExplanationStep>& steps);

[[nodiscard]] CCAP_EXPORT ExplanationParam param(std::string key, std::string value);
[[nodiscard]] CCAP_EXPORT ExplanationParam param(std::string key, std::string_view value);
[[nodiscard]] CCAP_EXPORT ExplanationParam param(std::string key, const char* value);
[[nodiscard]] CCAP_EXPORT ExplanationParam param(std::string key, std::int64_t value);
[[nodiscard]] CCAP_EXPORT ExplanationParam param(std::string key, std::uint64_t value);
[[nodiscard]] CCAP_EXPORT ExplanationParam param(std::string key, bool value);

}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_EXPLANATION_HPP
