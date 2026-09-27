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

#include "cooling_capacity/snapshot.hpp"

#include <algorithm>

#include "cooling_capacity/canonical.hpp"
#include "enum_table.hpp"

namespace cooling_capacity {

namespace {

constexpr detail::EnumName<SnapshotOrigin> kOriginNames[] = {
    {SnapshotOrigin::Constructed, "constructed"},
    {SnapshotOrigin::RecoveredFromStore, "recovered-from-store"},
};

constexpr detail::EnumName<ValidationSeverity> kSeverityNames[] = {
    {ValidationSeverity::Warning, "warning"},
    {ValidationSeverity::Error, "error"},
};

constexpr detail::EnumName<ValidationCode> kValidationCodeNames[] = {
    {ValidationCode::DuplicateIdentity, "duplicate-identity"},
    {ValidationCode::RecordLimitExceeded, "record-limit-exceeded"},
    {ValidationCode::EmptyIdentifier, "empty-identifier"},
    {ValidationCode::PolicyInvalid, "policy-invalid"},
    {ValidationCode::PolicyMissing, "policy-missing"},
    {ValidationCode::FacilityMissing, "facility-missing"},
    {ValidationCode::SiteMissing, "site-missing"},
    {ValidationCode::ZoneLoopMissing, "zone-loop-missing"},
    {ValidationCode::LoopEquipmentMissing, "loop-equipment-missing"},
    {ValidationCode::PlantEquipmentMissing, "plant-equipment-missing"},
    {ValidationCode::PlantMissing, "plant-missing"},
    {ValidationCode::ManifoldMissing, "manifold-missing"},
    {ValidationCode::DomainMissing, "domain-missing"},
    {ValidationCode::LoopMediumMismatch, "loop-medium-mismatch"},
    {ValidationCode::PlantMediumMismatch, "plant-medium-mismatch"},
    {ValidationCode::EquipmentMediumMismatch, "equipment-medium-mismatch"},
    {ValidationCode::EvidenceMediumMismatch, "evidence-medium-mismatch"},
    {ValidationCode::TransportLimitNegative, "transport-limit-negative"},
    {ValidationCode::NominalCapacityNegative, "nominal-capacity-negative"},
    {ValidationCode::ValidatedCapacityNegative, "validated-capacity-negative"},
    {ValidationCode::ValidatedCapacityAboveNominal, "validated-capacity-above-nominal"},
    {ValidationCode::DerateOutOfRange, "derate-out-of-range"},
    {ValidationCode::DerateDuplicateName, "derate-duplicate-name"},
    {ValidationCode::DerateCountExceeded, "derate-count-exceeded"},
    {ValidationCode::DerateEvidenceMissing, "derate-evidence-missing"},
    {ValidationCode::DegradationOutOfRange, "degradation-out-of-range"},
    {ValidationCode::DegradationEvidenceMissing, "degradation-evidence-missing"},
    {ValidationCode::DegradationBasisMismatch, "degradation-basis-mismatch"},
    {ValidationCode::TransientFactorMissing, "transient-factor-missing"},
    {ValidationCode::ZoneWithoutLoops, "zone-without-loops"},
    {ValidationCode::ZoneMediumWithoutClass, "zone-medium-without-class"},
    {ValidationCode::ZoneClassNotServedByLoop, "zone-class-not-served-by-loop"},
    {ValidationCode::LoopWithoutEquipment, "loop-without-equipment"},
    {ValidationCode::LoopWithoutTransportDeclaration, "loop-without-transport-declaration"},
    {ValidationCode::LoopSharesNoPlant, "loop-shares-no-plant"},
    {ValidationCode::SupplyModeWithoutSecondaryPlant, "supply-mode-without-secondary-plant"},
    {ValidationCode::SupplyModeWithSecondaryPlantButSingle,
     "supply-mode-with-secondary-plant-but-single"},
    {ValidationCode::LoopTransportAboveEquipment, "loop-transport-above-equipment"},
    {ValidationCode::LoopEquipmentSharedWithoutDeclaration,
     "loop-equipment-shared-without-declaration"},
    {ValidationCode::EvidenceSubjectMissing, "evidence-subject-missing"},
    {ValidationCode::EvidenceValueKindMismatch, "evidence-value-kind-mismatch"},
    {ValidationCode::EvidenceValidityInverted, "evidence-validity-inverted"},
    {ValidationCode::EvidenceSupersedesMissing, "evidence-supersedes-missing"},
    {ValidationCode::EvidenceSupersedesSelf, "evidence-supersedes-self"},
    {ValidationCode::EvidenceDuplicateActiveKind, "evidence-duplicate-active-kind"},
    {ValidationCode::CommitmentZoneMissing, "commitment-zone-missing"},
    {ValidationCode::CommitmentLoopMissing, "commitment-loop-missing"},
    {ValidationCode::CommitmentLoopNotServingZone, "commitment-loop-not-serving-zone"},
    {ValidationCode::CommitmentNegativeLoad, "commitment-negative-load"},
    {ValidationCode::CommitmentMediumMismatch, "commitment-medium-mismatch"},
    {ValidationCode::CommitmentClassUnsupportedByZone, "commitment-class-unsupported-by-zone"},
    {ValidationCode::CommitmentExpiryBeforeCreation, "commitment-expiry-before-creation"},
    {ValidationCode::CommitmentDuplicateAttempt, "commitment-duplicate-attempt"},
    {ValidationCode::CommitmentSupersedesMissing, "commitment-supersedes-missing"},
    {ValidationCode::CommitmentOverCommitted, "commitment-over-committed"},
    {ValidationCode::ReserveFloorOutOfRange, "reserve-floor-out-of-range"},
    {ValidationCode::ZoneReserveBelowPolicy, "zone-reserve-below-policy"},
    {ValidationCode::EquipmentTemperatureInvalid, "equipment-temperature-invalid"},
    {ValidationCode::TransportOnlyCapacityDeclared, "transport-only-capacity-declared"},
    {ValidationCode::RedundancyDeclarationWithoutEvidence,
     "redundancy-declaration-without-evidence"},
    {ValidationCode::PlantWithoutEquipment, "plant-without-equipment"},
    {ValidationCode::ZoneDomainMissing, "zone-domain-missing"},
    {ValidationCode::LoopDomainMissing, "loop-domain-missing"},
    {ValidationCode::LoopDomainMediumMismatch, "loop-domain-medium-mismatch"},
    {ValidationCode::EvidenceDuplicateIdentity, "evidence-duplicate-identity"},
    {ValidationCode::EvidenceReferenceMissing, "evidence-reference-missing"},
    {ValidationCode::CommitmentMediumMismatchWithZone, "commitment-medium-mismatch-with-zone"},
};

template <class Record>
bool sorted_unique_by_id(std::vector<Record>& records, Identifier* duplicate) {
  std::sort(records.begin(), records.end(),
            [](const Record& lhs, const Record& rhs) { return lhs.id < rhs.id; });
  for (std::size_t index = 1; index < records.size(); ++index) {
    if (records[index - 1].id == records[index].id) {
      *duplicate = records[index].id.value();
      return false;
    }
  }
  return true;
}

template <class T>
bool sort_unique_values(std::vector<T>& values) {
  std::sort(values.begin(), values.end());
  return std::adjacent_find(values.begin(), values.end()) == values.end();
}

}  // namespace

std::string_view to_string(SnapshotOrigin origin) noexcept {
  return detail::name_from_table(kOriginNames, origin);
}

std::string_view to_string(ValidationSeverity severity) noexcept {
  return detail::name_from_table(kSeverityNames, severity);
}

std::string_view to_string(ValidationCode code) noexcept {
  return detail::name_from_table(kValidationCodeNames, code);
}

std::string ValidationFinding::to_string() const {
  std::string out(cooling_capacity::to_string(severity));
  out.append(": ");
  out.append(cooling_capacity::to_string(kind));
  out.push_back(' ');
  out.append(subject.str());
  out.append(": ");
  out.append(cooling_capacity::to_string(code));
  if (!detail.empty()) {
    out.append(": ");
    out.append(detail);
  }
  return out;
}

void ValidationReport::add(ValidationFinding finding) { findings_.push_back(std::move(finding)); }

bool ValidationReport::ok() const noexcept { return error_count() == 0; }

bool ValidationReport::has(ValidationCode code) const {
  for (const ValidationFinding& finding : findings_) {
    if (finding.code == code) {
      return true;
    }
  }
  return false;
}

std::size_t ValidationReport::error_count() const noexcept {
  std::size_t count = 0;
  for (const ValidationFinding& finding : findings_) {
    if (finding.severity == ValidationSeverity::Error) {
      ++count;
    }
  }
  return count;
}

std::size_t ValidationReport::warning_count() const noexcept {
  return findings_.size() - error_count();
}

std::string ValidationReport::render() const {
  std::string out;
  for (const ValidationFinding& finding : findings_) {
    out.append(finding.to_string());
    out.push_back('\n');
  }
  return out;
}

std::size_t CoolingSnapshot::record_count() const noexcept {
  return sites_.size() + facilities_.size() + domains_.size() + manifolds_.size() +
         equipment_.size() + loops_.size() + plants_.size() + zones_.size() + evidence_.size() +
         commitments_.size();
}

namespace {

template <class Record, class Id>
const Record* find_by_id(const std::vector<Record>& records, const Id& id) {
  const auto iterator =
      std::lower_bound(records.begin(), records.end(), id,
                       [](const Record& record, const Id& key) { return record.id < key; });
  if (iterator == records.end() || iterator->id != id) {
    return nullptr;
  }
  return &*iterator;
}

}  // namespace

const SiteRecord* CoolingSnapshot::find_site(const SiteId& id) const noexcept {
  return find_by_id(sites_, id);
}
const FacilityRecord* CoolingSnapshot::find_facility(const FacilityId& id) const noexcept {
  return find_by_id(facilities_, id);
}
const DomainRecord* CoolingSnapshot::find_domain(const DomainId& id) const noexcept {
  return find_by_id(domains_, id);
}
const ManifoldRecord* CoolingSnapshot::find_manifold(const ManifoldId& id) const noexcept {
  return find_by_id(manifolds_, id);
}
const EquipmentRecord* CoolingSnapshot::find_equipment(const EquipmentId& id) const noexcept {
  return find_by_id(equipment_, id);
}
const LoopRecord* CoolingSnapshot::find_loop(const LoopId& id) const noexcept {
  return find_by_id(loops_, id);
}
const PlantRecord* CoolingSnapshot::find_plant(const PlantId& id) const noexcept {
  return find_by_id(plants_, id);
}
const ZoneRecord* CoolingSnapshot::find_zone(const ZoneId& id) const noexcept {
  return find_by_id(zones_, id);
}
const EvidenceRecord* CoolingSnapshot::find_evidence(const EvidenceId& id) const noexcept {
  return find_by_id(evidence_, id);
}
const CommitmentRecord* CoolingSnapshot::find_commitment(const CommitmentId& id) const noexcept {
  return find_by_id(commitments_, id);
}

bool CoolingSnapshot::contains(EntityKind kind, const Identifier& id) const noexcept {
  const auto matches = [&id](const auto& records) {
    const auto iterator = std::lower_bound(
        records.begin(), records.end(), id,
        [](const auto& record, const Identifier& key) { return record.id.value() < key; });
    return iterator != records.end() && iterator->id.value() == id;
  };
  switch (kind) {
    case EntityKind::Site:
      return matches(sites_);
    case EntityKind::Facility:
      return matches(facilities_);
    case EntityKind::Zone:
      return matches(zones_);
    case EntityKind::Loop:
      return matches(loops_);
    case EntityKind::Plant:
      return matches(plants_);
    case EntityKind::Equipment:
      return matches(equipment_);
    case EntityKind::Manifold:
      return matches(manifolds_);
    case EntityKind::Domain:
      return matches(domains_);
    case EntityKind::Evidence:
      return matches(evidence_);
    case EntityKind::Commitment:
      return matches(commitments_);
    case EntityKind::Policy:
      return policy_.id.value() == id;
  }
  return false;
}

std::shared_ptr<const CoolingSnapshot> CoolingSnapshot::with_provenance(
    SnapshotOrigin origin, std::optional<Timestamp> revalidated_at) const {
  auto copy = std::make_shared<CoolingSnapshot>(*this);
  copy->origin_ = origin;
  copy->revalidated_at_ = revalidated_at;
  return copy;
}

SnapshotBuilder::SnapshotBuilder() {
  policy_ = CoolingPolicy::defaults();
  constructed_at_ = Timestamp::epoch();
}

Result<void> SnapshotBuilder::add(SiteRecord record) {
  if (sites_.size() >= limits_.max_sites) {
    return Error(ErrorCode::LimitExceeded, "site limit reached")
        .with("limit", std::to_string(limits_.max_sites));
  }
  sites_.push_back(std::move(record));
  return Result<void>();
}

Result<void> SnapshotBuilder::add(FacilityRecord record) {
  if (facilities_.size() >= limits_.max_facilities) {
    return Error(ErrorCode::LimitExceeded, "facility limit reached")
        .with("limit", std::to_string(limits_.max_facilities));
  }
  facilities_.push_back(std::move(record));
  return Result<void>();
}

Result<void> SnapshotBuilder::add(DomainRecord record) {
  if (domains_.size() >= limits_.max_domains) {
    return Error(ErrorCode::LimitExceeded, "domain limit reached")
        .with("limit", std::to_string(limits_.max_domains));
  }
  domains_.push_back(std::move(record));
  return Result<void>();
}

Result<void> SnapshotBuilder::add(ManifoldRecord record) {
  if (manifolds_.size() >= limits_.max_manifolds) {
    return Error(ErrorCode::LimitExceeded, "manifold limit reached")
        .with("limit", std::to_string(limits_.max_manifolds));
  }
  manifolds_.push_back(std::move(record));
  return Result<void>();
}

Result<void> SnapshotBuilder::add(EquipmentRecord record) {
  if (equipment_.size() >= limits_.max_equipment) {
    return Error(ErrorCode::LimitExceeded, "equipment limit reached")
        .with("limit", std::to_string(limits_.max_equipment));
  }
  equipment_.push_back(std::move(record));
  return Result<void>();
}

Result<void> SnapshotBuilder::add(LoopRecord record) {
  if (loops_.size() >= limits_.max_loops) {
    return Error(ErrorCode::LimitExceeded, "loop limit reached")
        .with("limit", std::to_string(limits_.max_loops));
  }
  loops_.push_back(std::move(record));
  return Result<void>();
}

Result<void> SnapshotBuilder::add(PlantRecord record) {
  if (plants_.size() >= limits_.max_plants) {
    return Error(ErrorCode::LimitExceeded, "plant limit reached")
        .with("limit", std::to_string(limits_.max_plants));
  }
  plants_.push_back(std::move(record));
  return Result<void>();
}

Result<void> SnapshotBuilder::add(ZoneRecord record) {
  if (zones_.size() >= limits_.max_zones) {
    return Error(ErrorCode::LimitExceeded, "zone limit reached")
        .with("limit", std::to_string(limits_.max_zones));
  }
  zones_.push_back(std::move(record));
  return Result<void>();
}

Result<void> SnapshotBuilder::add(EvidenceRecord record) {
  if (evidence_.size() >= limits_.max_evidence) {
    return Error(ErrorCode::LimitExceeded, "evidence limit reached")
        .with("limit", std::to_string(limits_.max_evidence));
  }
  evidence_.push_back(std::move(record));
  return Result<void>();
}

Result<void> SnapshotBuilder::add(CommitmentRecord record) {
  if (commitments_.size() >= limits_.max_commitments) {
    return Error(ErrorCode::LimitExceeded, "commitment limit reached")
        .with("limit", std::to_string(limits_.max_commitments));
  }
  commitments_.push_back(std::move(record));
  return Result<void>();
}

namespace {

template <class Record, class Id>
Result<void> replace_in(std::vector<Record>& records, Record record, const Id& id) {
  const auto iterator =
      std::lower_bound(records.begin(), records.end(), id,
                       [](const Record& existing, const Id& key) { return existing.id < key; });
  if (iterator == records.end() || iterator->id != id) {
    return Error(ErrorCode::NotFound, "record to replace is not present").with("id", id.str());
  }
  *iterator = std::move(record);
  return Result<void>();
}

}  // namespace

Result<void> SnapshotBuilder::replace(EquipmentRecord record) {
  const EquipmentId id = record.id;
  return replace_in(equipment_, std::move(record), id);
}

Result<void> SnapshotBuilder::replace(LoopRecord record) {
  const LoopId id = record.id;
  return replace_in(loops_, std::move(record), id);
}

Result<void> SnapshotBuilder::replace(PlantRecord record) {
  const PlantId id = record.id;
  return replace_in(plants_, std::move(record), id);
}

Result<void> SnapshotBuilder::replace(ZoneRecord record) {
  const ZoneId id = record.id;
  return replace_in(zones_, std::move(record), id);
}

Result<void> SnapshotBuilder::replace(EvidenceRecord record) {
  const EvidenceId id = record.id;
  return replace_in(evidence_, std::move(record), id);
}

Result<void> SnapshotBuilder::replace(CommitmentRecord record) {
  const CommitmentId id = record.id;
  return replace_in(commitments_, std::move(record), id);
}

Result<void> SnapshotBuilder::remove(EntityKind kind, const Identifier& id) {
  const auto erase = [&id](auto& records) -> bool {
    const auto iterator = std::lower_bound(
        records.begin(), records.end(), id,
        [](const auto& record, const Identifier& key) { return record.id.value() < key; });
    if (iterator == records.end() || iterator->id.value() != id) {
      return false;
    }
    records.erase(iterator);
    return true;
  };
  bool removed = false;
  switch (kind) {
    case EntityKind::Site:
      removed = erase(sites_);
      break;
    case EntityKind::Facility:
      removed = erase(facilities_);
      break;
    case EntityKind::Domain:
      removed = erase(domains_);
      break;
    case EntityKind::Manifold:
      removed = erase(manifolds_);
      break;
    case EntityKind::Equipment:
      removed = erase(equipment_);
      break;
    case EntityKind::Loop:
      removed = erase(loops_);
      break;
    case EntityKind::Plant:
      removed = erase(plants_);
      break;
    case EntityKind::Zone:
      removed = erase(zones_);
      break;
    case EntityKind::Evidence:
      removed = erase(evidence_);
      break;
    case EntityKind::Commitment:
      removed = erase(commitments_);
      break;
    case EntityKind::Policy:
      return Error(ErrorCode::InvalidArgument, "a snapshot cannot exist without a policy");
  }
  if (!removed) {
    return Error(ErrorCode::NotFound, "record to remove is not present")
        .with("kind", std::string(cooling_capacity::to_string(kind)))
        .with("id", id.str());
  }
  return Result<void>();
}

bool SnapshotBuilder::contains(EntityKind kind, const Identifier& id) const {
  const auto has = [&id](const auto& records) {
    const auto iterator = std::lower_bound(
        records.begin(), records.end(), id,
        [](const auto& record, const Identifier& key) { return record.id.value() < key; });
    return iterator != records.end() && iterator->id.value() == id;
  };
  switch (kind) {
    case EntityKind::Site:
      return has(sites_);
    case EntityKind::Facility:
      return has(facilities_);
    case EntityKind::Zone:
      return has(zones_);
    case EntityKind::Loop:
      return has(loops_);
    case EntityKind::Plant:
      return has(plants_);
    case EntityKind::Equipment:
      return has(equipment_);
    case EntityKind::Manifold:
      return has(manifolds_);
    case EntityKind::Domain:
      return has(domains_);
    case EntityKind::Evidence:
      return has(evidence_);
    case EntityKind::Commitment:
      return has(commitments_);
    case EntityKind::Policy:
      return policy_.id.value() == id;
  }
  return false;
}

namespace {

Error duplicate_error(EntityKind kind, const Identifier& id) {
  return Error(ErrorCode::DuplicateIdentity, "a record with this identifier already exists")
      .with("kind", std::string(cooling_capacity::to_string(kind)))
      .with("id", id.str());
}

}  // namespace

Result<std::shared_ptr<const CoolingSnapshot>> SnapshotBuilder::canonicalise_and_freeze() {
  Identifier duplicate;
  if (!sorted_unique_by_id(sites_, &duplicate)) {
    return duplicate_error(EntityKind::Site, duplicate);
  }
  if (!sorted_unique_by_id(facilities_, &duplicate)) {
    return duplicate_error(EntityKind::Facility, duplicate);
  }
  if (!sorted_unique_by_id(domains_, &duplicate)) {
    return duplicate_error(EntityKind::Domain, duplicate);
  }
  if (!sorted_unique_by_id(manifolds_, &duplicate)) {
    return duplicate_error(EntityKind::Manifold, duplicate);
  }
  if (!sorted_unique_by_id(equipment_, &duplicate)) {
    return duplicate_error(EntityKind::Equipment, duplicate);
  }
  if (!sorted_unique_by_id(loops_, &duplicate)) {
    return duplicate_error(EntityKind::Loop, duplicate);
  }
  if (!sorted_unique_by_id(plants_, &duplicate)) {
    return duplicate_error(EntityKind::Plant, duplicate);
  }
  if (!sorted_unique_by_id(zones_, &duplicate)) {
    return duplicate_error(EntityKind::Zone, duplicate);
  }
  if (!sorted_unique_by_id(evidence_, &duplicate)) {
    return duplicate_error(EntityKind::Evidence, duplicate);
  }
  if (!sorted_unique_by_id(commitments_, &duplicate)) {
    return duplicate_error(EntityKind::Commitment, duplicate);
  }

  for (EquipmentRecord& record : equipment_) {
    std::sort(record.derates.begin(), record.derates.end(),
              [](const DerateFactor& lhs, const DerateFactor& rhs) { return lhs.name < rhs.name; });
    for (std::size_t index = 1; index < record.derates.size(); ++index) {
      if (record.derates[index - 1].name == record.derates[index].name) {
        return Error(ErrorCode::DuplicateIdentity, "equipment has two derate factors with one name")
            .with("equipment", record.id.str())
            .with("derate", record.derates[index].name.str());
      }
    }
  }
  for (LoopRecord& record : loops_) {
    if (!sort_unique_values(record.equipment)) {
      return Error(ErrorCode::DuplicateIdentity, "loop lists the same equipment twice")
          .with("loop", record.id.str());
    }
    if (!sort_unique_values(record.manifolds)) {
      return Error(ErrorCode::DuplicateIdentity, "loop lists the same manifold twice")
          .with("loop", record.id.str());
    }
  }
  for (PlantRecord& record : plants_) {
    if (!sort_unique_values(record.equipment)) {
      return Error(ErrorCode::DuplicateIdentity, "plant lists the same equipment twice")
          .with("plant", record.id.str());
    }
  }
  for (ZoneRecord& record : zones_) {
    if (!sort_unique_values(record.loops)) {
      return Error(ErrorCode::DuplicateIdentity, "zone lists the same loop twice")
          .with("zone", record.id.str());
    }
    if (!sort_unique_values(record.domains)) {
      return Error(ErrorCode::DuplicateIdentity, "zone lists the same domain twice")
          .with("zone", record.id.str());
    }
    if (!sort_unique_values(record.compatibility)) {
      return Error(ErrorCode::DuplicateIdentity, "zone lists the same compatibility class twice")
          .with("zone", record.id.str());
    }
  }
  std::sort(policy_.freshness.begin(), policy_.freshness.end(),
            [](const FreshnessRule& lhs, const FreshnessRule& rhs) { return lhs.kind < rhs.kind; });

  auto snapshot = std::shared_ptr<CoolingSnapshot>(new CoolingSnapshot());
  snapshot->generation_ = generation_;
  snapshot->policy_ = policy_;
  snapshot->constructed_at_ = constructed_at_;
  snapshot->origin_ = SnapshotOrigin::Constructed;
  snapshot->revalidated_at_ = std::nullopt;
  snapshot->limits_ = limits_;
  snapshot->sites_ = sites_;
  snapshot->facilities_ = facilities_;
  snapshot->domains_ = domains_;
  snapshot->manifolds_ = manifolds_;
  snapshot->equipment_ = equipment_;
  snapshot->loops_ = loops_;
  snapshot->plants_ = plants_;
  snapshot->zones_ = zones_;
  snapshot->evidence_ = evidence_;
  snapshot->commitments_ = commitments_;
  snapshot->digest_ = canonical_digest(*snapshot);
  return std::shared_ptr<const CoolingSnapshot>(snapshot);
}

Result<std::shared_ptr<const CoolingSnapshot>> SnapshotBuilder::build_unvalidated() {
  if (generation_.value() == 0) {
    return Error(ErrorCode::InvalidArgument, "snapshot generation must be at least one");
  }
  return canonicalise_and_freeze();
}

Result<std::shared_ptr<const CoolingSnapshot>> SnapshotBuilder::build(ValidationReport* report) {
  if (generation_.value() == 0) {
    return Error(ErrorCode::InvalidArgument, "snapshot generation must be at least one");
  }
  CCAP_TRY_DECLARE(frozen, canonicalise_and_freeze());
  const ValidationReport local = validate_snapshot(*frozen);
  if (report != nullptr) {
    *report = local;
  }
  if (!local.ok()) {
    const ValidationFinding* first = nullptr;
    for (const ValidationFinding& finding : local.findings()) {
      if (finding.severity == ValidationSeverity::Error) {
        first = &finding;
        break;
      }
    }
    Error error(ErrorCode::InvariantViolation, "the candidate generation failed validation");
    error.with("errors", std::to_string(local.error_count()));
    error.with("warnings", std::to_string(local.warning_count()));
    if (first != nullptr) {
      error.with("first_finding", first->to_string());
    }
    return error;
  }
  return frozen;
}

ValidationReport SnapshotBuilder::validate() const {
  SnapshotBuilder copy = *this;
  ValidationReport report;
  Result<std::shared_ptr<const CoolingSnapshot>> frozen = copy.canonicalise_and_freeze();
  if (!frozen.ok()) {
    ValidationFinding finding;
    finding.severity = ValidationSeverity::Error;
    finding.code = frozen.error().code() == ErrorCode::DuplicateIdentity
                       ? ValidationCode::DuplicateIdentity
                       : ValidationCode::RecordLimitExceeded;
    finding.kind = EntityKind::Zone;
    finding.subject = Identifier::literal("snapshot");
    finding.detail = frozen.error().message();
    report.add(std::move(finding));
    return report;
  }
  return validate_snapshot(*frozen.value());
}

}  // namespace cooling_capacity
