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

#ifndef COOLING_CAPACITY_EVIDENCE_HPP
#define COOLING_CAPACITY_EVIDENCE_HPP

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include "cooling_capacity/ids.hpp"
#include "cooling_capacity/medium.hpp"
#include "cooling_capacity/text.hpp"
#include "cooling_capacity/units.hpp"

namespace cooling_capacity {

// Evidence is what makes a capacity answer authoritative rather than merely
// declared. Every piece of evidence names what it is about, who said it, when
// it was observed and which source generation it came from.
enum class EvidenceKind : std::uint8_t {
  Nameplate = 0,
  CapacityValidation = 1,
  LoadMeasurement = 2,
  Telemetry = 3,
  MaintenanceOrder = 4,
  EnvironmentalConstraint = 5,
  TransportSurvey = 6,
  DegradationAssessment = 7,
  RedundancyDeclaration = 8,
  OperatorAssertion = 9,
  VendorBulletin = 10,
};

inline constexpr std::size_t kEvidenceKindCount = 11;

[[nodiscard]] CCAP_EXPORT std::string_view to_string(EvidenceKind kind) noexcept;
[[nodiscard]] CCAP_EXPORT Result<EvidenceKind> parse_evidence_kind(std::string_view text);
[[nodiscard]] CCAP_EXPORT EvidenceKind evidence_kind_at(std::size_t index);

// Who or what produced the evidence. The source is recorded separately from the
// actor so that a vendor document and an operator assertion carrying the same
// value remain distinguishable.
enum class EvidenceSource : std::uint8_t {
  VendorDocument = 0,
  CommissioningTool = 1,
  BmsTelemetry = 2,
  OperatorEntry = 3,
  FacilityRegistry = 4,
  Derived = 5,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(EvidenceSource source) noexcept;
[[nodiscard]] CCAP_EXPORT Result<EvidenceSource> parse_evidence_source(std::string_view text);

// What the evidence is about. A variant, so an evidence record about a plant
// cannot be read as if it were about a zone.
using EvidenceSubject =
    std::variant<SiteId, FacilityId, ZoneId, LoopId, PlantId, EquipmentId, ManifoldId, DomainId>;

[[nodiscard]] CCAP_EXPORT std::string_view subject_kind_name(const EvidenceSubject& subject) noexcept;
[[nodiscard]] CCAP_EXPORT std::string subject_text(const EvidenceSubject& subject);

// What the evidence says. `monostate` is a declaration without a scalar value,
// which is how a maintenance order or a redundancy declaration is recorded.
using EvidenceValue =
    std::variant<std::monostate, ThermalPower, Fraction, Temperature, VolumetricFlow,
                 OperatingState, RedundancyClass, MediumMask>;

[[nodiscard]] CCAP_EXPORT std::string_view value_kind_name(const EvidenceValue& value) noexcept;
[[nodiscard]] CCAP_EXPORT std::string value_text(const EvidenceValue& value);

// Whether a piece of evidence is still in force. Withdrawal and supersession are
// explicit states, not deletions: the record stays visible to inspection.
enum class EvidenceState : std::uint8_t {
  Active = 0,
  Superseded = 1,
  Withdrawn = 2,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(EvidenceState state) noexcept;
[[nodiscard]] CCAP_EXPORT Result<EvidenceState> parse_evidence_state(std::string_view text);

// Where a record came from and under whose authority it was written.
struct Provenance {
  ActorId actor;
  DocumentRef reference;
  std::optional<EvidenceId> supersedes;

  friend bool operator==(const Provenance& lhs, const Provenance& rhs) {
    return lhs.actor == rhs.actor && lhs.reference == rhs.reference &&
           lhs.supersedes == rhs.supersedes;
  }
};

struct EvidenceRecord {
  EvidenceId id;
  EvidenceKind kind = EvidenceKind::OperatorAssertion;
  EvidenceSource source = EvidenceSource::OperatorEntry;
  EvidenceSubject subject;
  std::optional<CoolingMedium> medium;
  EvidenceValue value;
  Timestamp observed_at;
  std::optional<Timestamp> valid_until;
  std::optional<SourceGeneration> source_generation;
  Provenance provenance;
  EvidenceState state = EvidenceState::Active;
  RecordRevision revision;

  friend bool operator==(const EvidenceRecord& lhs, const EvidenceRecord& rhs) {
    return lhs.id == rhs.id && lhs.kind == rhs.kind && lhs.source == rhs.source &&
           lhs.subject == rhs.subject && lhs.medium == rhs.medium && lhs.value == rhs.value &&
           lhs.observed_at == rhs.observed_at && lhs.valid_until == rhs.valid_until &&
           lhs.source_generation == rhs.source_generation && lhs.provenance == rhs.provenance &&
           lhs.state == rhs.state && lhs.revision == rhs.revision;
  }
};

// The outcome of testing one evidence record against a policy and an instant.
// A record that fails freshness is reported as stale; it is never quietly
// treated as current, and a store reopened after a restart is expected to
// produce Stale/Expired dispositions for everything whose window has passed.
enum class EvidenceFreshness : std::uint8_t {
  Fresh = 0,
  Stale = 1,
  ExpiredByValidity = 2,
  Withdrawn = 3,
  Superseded = 4,
  FutureDated = 5,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(EvidenceFreshness value) noexcept;

// True when the disposition allows the record to be used as authoritative
// input under an accepting policy.
[[nodiscard]] CCAP_EXPORT bool is_usable(EvidenceFreshness value) noexcept;

struct EvidenceAssessment {
  EvidenceId id;
  EvidenceFreshness freshness = EvidenceFreshness::Stale;
  DurationMs age;
  Timestamp observed_at;
  Timestamp assessed_at;

  friend bool operator==(const EvidenceAssessment& lhs, const EvidenceAssessment& rhs) {
    return lhs.id == rhs.id && lhs.freshness == rhs.freshness && lhs.age == rhs.age &&
           lhs.observed_at == rhs.observed_at && lhs.assessed_at == rhs.assessed_at;
  }
};

}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_EVIDENCE_HPP
