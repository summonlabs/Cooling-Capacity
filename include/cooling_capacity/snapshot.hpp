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

#ifndef COOLING_CAPACITY_SNAPSHOT_HPP
#define COOLING_CAPACITY_SNAPSHOT_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cooling_capacity/commitment.hpp"
#include "cooling_capacity/digest.hpp"
#include "cooling_capacity/evidence.hpp"
#include "cooling_capacity/ids.hpp"
#include "cooling_capacity/limits.hpp"
#include "cooling_capacity/model.hpp"
#include "cooling_capacity/policy.hpp"
#include "cooling_capacity/text.hpp"

namespace cooling_capacity {

// Where a snapshot came from. A snapshot recovered from a durable store is not
// the same authority as one that was just built or revalidated, and the
// distinction survives into every answer derived from it.
enum class SnapshotOrigin : std::uint8_t {
  Constructed = 0,
  RecoveredFromStore = 1,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(SnapshotOrigin origin) noexcept;

enum class ValidationSeverity : std::uint8_t {
  Warning = 0,
  Error = 1,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(ValidationSeverity severity) noexcept;

// Validation findings. The codes are stable; the order in which validation is
// performed is fixed and tested, so a model with several defects always reports
// the same first finding.
enum class ValidationCode : std::uint8_t {
  DuplicateIdentity = 0,
  RecordLimitExceeded = 1,
  EmptyIdentifier = 2,
  PolicyInvalid = 3,
  PolicyMissing = 4,
  FacilityMissing = 5,
  SiteMissing = 6,
  ZoneLoopMissing = 7,
  LoopEquipmentMissing = 8,
  PlantEquipmentMissing = 9,
  PlantMissing = 10,
  ManifoldMissing = 11,
  DomainMissing = 12,
  LoopMediumMismatch = 13,
  PlantMediumMismatch = 14,
  EquipmentMediumMismatch = 15,
  EvidenceMediumMismatch = 16,
  TransportLimitNegative = 17,
  NominalCapacityNegative = 18,
  ValidatedCapacityNegative = 19,
  ValidatedCapacityAboveNominal = 20,
  DerateOutOfRange = 21,
  DerateDuplicateName = 22,
  DerateCountExceeded = 23,
  DerateEvidenceMissing = 24,
  DegradationOutOfRange = 25,
  DegradationEvidenceMissing = 26,
  DegradationBasisMismatch = 27,
  TransientFactorMissing = 28,
  ZoneWithoutLoops = 29,
  ZoneMediumWithoutClass = 30,
  ZoneClassNotServedByLoop = 31,
  LoopWithoutEquipment = 32,
  LoopWithoutTransportDeclaration = 33,
  LoopSharesNoPlant = 34,
  SupplyModeWithoutSecondaryPlant = 35,
  SupplyModeWithSecondaryPlantButSingle = 36,
  LoopTransportAboveEquipment = 37,
  LoopEquipmentSharedWithoutDeclaration = 38,
  EvidenceSubjectMissing = 39,
  EvidenceValueKindMismatch = 40,
  EvidenceValidityInverted = 41,
  EvidenceSupersedesMissing = 42,
  EvidenceSupersedesSelf = 43,
  EvidenceDuplicateActiveKind = 44,
  CommitmentZoneMissing = 45,
  CommitmentLoopMissing = 46,
  CommitmentLoopNotServingZone = 47,
  CommitmentNegativeLoad = 48,
  CommitmentMediumMismatch = 49,
  CommitmentClassUnsupportedByZone = 50,
  CommitmentExpiryBeforeCreation = 51,
  CommitmentDuplicateAttempt = 52,
  CommitmentSupersedesMissing = 53,
  CommitmentOverCommitted = 54,
  ReserveFloorOutOfRange = 55,
  ZoneReserveBelowPolicy = 56,
  EquipmentTemperatureInvalid = 57,
  TransportOnlyCapacityDeclared = 58,
  RedundancyDeclarationWithoutEvidence = 59,
  PlantWithoutEquipment = 60,
  ZoneDomainMissing = 61,
  LoopDomainMissing = 62,
  LoopDomainMediumMismatch = 63,
  EvidenceDuplicateIdentity = 64,
  EvidenceReferenceMissing = 65,
  CommitmentMediumMismatchWithZone = 66,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(ValidationCode code) noexcept;

struct ValidationFinding {
  ValidationSeverity severity = ValidationSeverity::Error;
  ValidationCode code = ValidationCode::DuplicateIdentity;
  EntityKind kind = EntityKind::Zone;
  Identifier subject;
  std::string detail;

  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const ValidationFinding& lhs, const ValidationFinding& rhs) {
    return lhs.severity == rhs.severity && lhs.code == rhs.code && lhs.kind == rhs.kind &&
           lhs.subject == rhs.subject && lhs.detail == rhs.detail;
  }
};

class CCAP_EXPORT ValidationReport {
 public:
  void add(ValidationFinding finding);
  [[nodiscard]] bool ok() const noexcept;
  [[nodiscard]] bool has(ValidationCode code) const;
  [[nodiscard]] std::size_t error_count() const noexcept;
  [[nodiscard]] std::size_t warning_count() const noexcept;
  [[nodiscard]] const std::vector<ValidationFinding>& findings() const noexcept {
    return findings_;
  }
  [[nodiscard]] std::string render() const;

 private:
  std::vector<ValidationFinding> findings_;
};

// An immutable, generation-bound catalog of cooling records. Instances are
// created by SnapshotBuilder and are never mutated afterwards, so an analysis
// can hold one without locking anything.
class CCAP_EXPORT CoolingSnapshot {
 public:
  [[nodiscard]] CapacityGeneration generation() const noexcept { return generation_; }
  [[nodiscard]] const CoolingPolicy& policy() const noexcept { return policy_; }
  [[nodiscard]] Timestamp constructed_at() const noexcept { return constructed_at_; }
  [[nodiscard]] SnapshotOrigin origin() const noexcept { return origin_; }
  [[nodiscard]] const std::optional<Timestamp>& revalidated_at() const noexcept {
    return revalidated_at_;
  }
  [[nodiscard]] const Digest& digest() const noexcept { return digest_; }
  [[nodiscard]] const Limits& limits() const noexcept { return limits_; }

  [[nodiscard]] const std::vector<SiteRecord>& sites() const noexcept { return sites_; }
  [[nodiscard]] const std::vector<FacilityRecord>& facilities() const noexcept {
    return facilities_;
  }
  [[nodiscard]] const std::vector<DomainRecord>& domains() const noexcept { return domains_; }
  [[nodiscard]] const std::vector<ManifoldRecord>& manifolds() const noexcept { return manifolds_; }
  [[nodiscard]] const std::vector<EquipmentRecord>& equipment() const noexcept {
    return equipment_;
  }
  [[nodiscard]] const std::vector<LoopRecord>& loops() const noexcept { return loops_; }
  [[nodiscard]] const std::vector<PlantRecord>& plants() const noexcept { return plants_; }
  [[nodiscard]] const std::vector<ZoneRecord>& zones() const noexcept { return zones_; }
  [[nodiscard]] const std::vector<EvidenceRecord>& evidence() const noexcept { return evidence_; }
  [[nodiscard]] const std::vector<CommitmentRecord>& commitments() const noexcept {
    return commitments_;
  }

  // Lookups are exact: an identifier that is not present returns nullptr.
  [[nodiscard]] const SiteRecord* find_site(const SiteId& id) const noexcept;
  [[nodiscard]] const FacilityRecord* find_facility(const FacilityId& id) const noexcept;
  [[nodiscard]] const DomainRecord* find_domain(const DomainId& id) const noexcept;
  [[nodiscard]] const ManifoldRecord* find_manifold(const ManifoldId& id) const noexcept;
  [[nodiscard]] const EquipmentRecord* find_equipment(const EquipmentId& id) const noexcept;
  [[nodiscard]] const LoopRecord* find_loop(const LoopId& id) const noexcept;
  [[nodiscard]] const PlantRecord* find_plant(const PlantId& id) const noexcept;
  [[nodiscard]] const ZoneRecord* find_zone(const ZoneId& id) const noexcept;
  [[nodiscard]] const EvidenceRecord* find_evidence(const EvidenceId& id) const noexcept;
  [[nodiscard]] const CommitmentRecord* find_commitment(const CommitmentId& id) const noexcept;

  [[nodiscard]] std::size_t record_count() const noexcept;

  // True when a lookup by kind and identifier resolves to a record. Used by the
  // validator and by the canonical decoder.
  [[nodiscard]] bool contains(EntityKind kind, const Identifier& id) const noexcept;

  // Returns a copy of this snapshot with a different provenance stamp. Used by
  // the engine when it republishes recovered state after revalidation, and by
  // the canonical decoder when it materialises a stored generation.
  [[nodiscard]] std::shared_ptr<const CoolingSnapshot> with_provenance(
      SnapshotOrigin origin, std::optional<Timestamp> revalidated_at) const;

 private:
  friend class SnapshotBuilder;

  CoolingSnapshot() = default;

  CapacityGeneration generation_;
  CoolingPolicy policy_;
  Timestamp constructed_at_;
  SnapshotOrigin origin_ = SnapshotOrigin::Constructed;
  std::optional<Timestamp> revalidated_at_;
  Digest digest_;
  Limits limits_;
  std::vector<SiteRecord> sites_;
  std::vector<FacilityRecord> facilities_;
  std::vector<DomainRecord> domains_;
  std::vector<ManifoldRecord> manifolds_;
  std::vector<EquipmentRecord> equipment_;
  std::vector<LoopRecord> loops_;
  std::vector<PlantRecord> plants_;
  std::vector<ZoneRecord> zones_;
  std::vector<EvidenceRecord> evidence_;
  std::vector<CommitmentRecord> commitments_;
};

// Builder for one candidate generation. The builder is mutable and is not
// thread safe; the snapshot it produces is immutable and is.
class CCAP_EXPORT SnapshotBuilder {
 public:
  SnapshotBuilder();

  void set_generation(CapacityGeneration generation) noexcept { generation_ = generation; }
  [[nodiscard]] CapacityGeneration generation() const noexcept { return generation_; }
  void set_policy(CoolingPolicy policy) { policy_ = std::move(policy); }
  [[nodiscard]] const CoolingPolicy& policy() const noexcept { return policy_; }
  void set_constructed_at(Timestamp instant) noexcept { constructed_at_ = instant; }
  void set_limits(Limits limits) { limits_ = limits; }
  [[nodiscard]] const Limits& limits() const noexcept { return limits_; }

  [[nodiscard]] Result<void> add(SiteRecord record);
  [[nodiscard]] Result<void> add(FacilityRecord record);
  [[nodiscard]] Result<void> add(DomainRecord record);
  [[nodiscard]] Result<void> add(ManifoldRecord record);
  [[nodiscard]] Result<void> add(EquipmentRecord record);
  [[nodiscard]] Result<void> add(LoopRecord record);
  [[nodiscard]] Result<void> add(PlantRecord record);
  [[nodiscard]] Result<void> add(ZoneRecord record);
  [[nodiscard]] Result<void> add(EvidenceRecord record);
  [[nodiscard]] Result<void> add(CommitmentRecord record);

  [[nodiscard]] Result<void> replace(EquipmentRecord record);
  [[nodiscard]] Result<void> replace(LoopRecord record);
  [[nodiscard]] Result<void> replace(PlantRecord record);
  [[nodiscard]] Result<void> replace(ZoneRecord record);
  [[nodiscard]] Result<void> replace(EvidenceRecord record);
  [[nodiscard]] Result<void> replace(CommitmentRecord record);
  [[nodiscard]] Result<void> remove(EntityKind kind, const Identifier& id);

  [[nodiscard]] bool contains(EntityKind kind, const Identifier& id) const;

  // Canonicalises the ordering, validates, and freezes. When `report` is not
  // null it receives every finding, including warnings, whether or not the
  // build succeeds.
  [[nodiscard]] Result<std::shared_ptr<const CoolingSnapshot>> build(ValidationReport* report);

  // Canonicalises the ordering and freezes without running the validator. Used
  // by the canonical decoder, which must be able to materialise a damaged but
  // structurally readable artifact so that verification tooling can report what
  // is wrong with it instead of only that it is unreadable.
  [[nodiscard]] Result<std::shared_ptr<const CoolingSnapshot>> build_unvalidated();

  // Validates the current contents without freezing.
  [[nodiscard]] ValidationReport validate() const;

 private:
  [[nodiscard]] Result<std::shared_ptr<const CoolingSnapshot>> canonicalise_and_freeze();

  CapacityGeneration generation_;
  CoolingPolicy policy_;
  Timestamp constructed_at_;
  Limits limits_;
  std::vector<SiteRecord> sites_;
  std::vector<FacilityRecord> facilities_;
  std::vector<DomainRecord> domains_;
  std::vector<ManifoldRecord> manifolds_;
  std::vector<EquipmentRecord> equipment_;
  std::vector<LoopRecord> loops_;
  std::vector<PlantRecord> plants_;
  std::vector<ZoneRecord> zones_;
  std::vector<EvidenceRecord> evidence_;
  std::vector<CommitmentRecord> commitments_;
};

// Validates an already built snapshot's contents. Used by the store's
// verification path, which must be at least as strict as the normal open path.
[[nodiscard]] CCAP_EXPORT ValidationReport validate_snapshot(const CoolingSnapshot& snapshot);

}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_SNAPSHOT_HPP
