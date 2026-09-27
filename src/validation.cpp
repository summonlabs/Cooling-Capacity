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

#include <algorithm>
#include <map>
#include <set>
#include <string>

#include "cooling_capacity/rollup.hpp"
#include "cooling_capacity/snapshot.hpp"

namespace cooling_capacity {

namespace {

class Validator {
 public:
  explicit Validator(const CoolingSnapshot& snapshot) : snapshot_(snapshot) {}

  ValidationReport run() {
    check_policy();
    check_facilities();
    check_domains();
    check_manifolds();
    check_equipment();
    check_loops();
    check_plants();
    check_zones();
    check_evidence();
    check_commitments();
    check_over_commitment();
    return std::move(report_);
  }

 private:
  void add(ValidationSeverity severity, ValidationCode code, EntityKind kind, const Identifier& id,
           std::string detail) {
    ValidationFinding finding;
    finding.severity = severity;
    finding.code = code;
    finding.kind = kind;
    finding.subject = id;
    finding.detail = std::move(detail);
    report_.add(std::move(finding));
  }

  void error(ValidationCode code, EntityKind kind, const Identifier& id, std::string detail) {
    add(ValidationSeverity::Error, code, kind, id, std::move(detail));
  }

  void warn(ValidationCode code, EntityKind kind, const Identifier& id, std::string detail) {
    add(ValidationSeverity::Warning, code, kind, id, std::move(detail));
  }

  [[nodiscard]] bool evidence_exists(const EvidenceId& id) const {
    return snapshot_.find_evidence(id) != nullptr;
  }

  void check_policy() {
    const Result<void> valid = snapshot_.policy().validate();
    if (!valid.ok()) {
      error(ValidationCode::PolicyInvalid, EntityKind::Policy, snapshot_.policy().id.value(),
            valid.error().message());
    }
  }

  void check_facilities() {
    for (const FacilityRecord& facility : snapshot_.facilities()) {
      if (snapshot_.find_site(facility.site) == nullptr) {
        error(ValidationCode::SiteMissing, EntityKind::Facility, facility.id.value(),
              "site " + facility.site.str() + " is not present");
      }
    }
  }

  void check_domains() {
    for (const DomainRecord& domain : snapshot_.domains()) {
      if (snapshot_.find_facility(domain.facility) == nullptr) {
        error(ValidationCode::FacilityMissing, EntityKind::Domain, domain.id.value(),
              "facility " + domain.facility.str() + " is not present");
      }
    }
  }

  void check_manifolds() {
    for (const ManifoldRecord& manifold : snapshot_.manifolds()) {
      if (snapshot_.find_facility(manifold.facility) == nullptr) {
        error(ValidationCode::FacilityMissing, EntityKind::Manifold, manifold.id.value(),
              "facility " + manifold.facility.str() + " is not present");
      }
      if (manifold.transport_limit.is_negative()) {
        error(ValidationCode::TransportLimitNegative, EntityKind::Manifold, manifold.id.value(),
              "transport limit " + manifold.transport_limit.to_string() + " is negative");
      }
      if (manifold.transport_evidence.has_value() &&
          !evidence_exists(*manifold.transport_evidence)) {
        error(ValidationCode::EvidenceReferenceMissing, EntityKind::Manifold, manifold.id.value(),
              "transport evidence " + manifold.transport_evidence->str() + " is not present");
      }
    }
  }

  void check_equipment() {
    for (const EquipmentRecord& unit : snapshot_.equipment()) {
      if (snapshot_.find_facility(unit.facility) == nullptr) {
        error(ValidationCode::FacilityMissing, EntityKind::Equipment, unit.id.value(),
              "facility " + unit.facility.str() + " is not present");
      }
      if (unit.nominal.is_negative()) {
        error(ValidationCode::NominalCapacityNegative, EntityKind::Equipment, unit.id.value(),
              "nominal capacity " + unit.nominal.to_string() + " is negative");
      }
      if (unit.validated.has_value()) {
        if (unit.validated->is_negative()) {
          error(ValidationCode::ValidatedCapacityNegative, EntityKind::Equipment, unit.id.value(),
                "validated capacity " + unit.validated->to_string() + " is negative");
        } else if (!unit.nominal.is_negative() &&
                   unit.validated->milliwatts() > unit.nominal.milliwatts()) {
          error(ValidationCode::ValidatedCapacityAboveNominal, EntityKind::Equipment, unit.id.value(),
                "validated capacity " + unit.validated->to_string() +
                    " is above the nominal capacity " + unit.nominal.to_string());
        }
      }
      if (unit.validated_evidence.has_value() && !evidence_exists(*unit.validated_evidence)) {
        error(ValidationCode::EvidenceReferenceMissing, EntityKind::Equipment, unit.id.value(),
              "validated-capacity evidence " + unit.validated_evidence->str() + " is not present");
      }
      if (unit.state_evidence.has_value() && !evidence_exists(*unit.state_evidence)) {
        error(ValidationCode::EvidenceReferenceMissing, EntityKind::Equipment, unit.id.value(),
              "state evidence " + unit.state_evidence->str() + " is not present");
      }
      if (unit.derates.size() > snapshot_.limits().max_derate_factors) {
        error(ValidationCode::DerateCountExceeded, EntityKind::Equipment, unit.id.value(),
              "derate factor count exceeds the configured maximum");
      }
      for (const DerateFactor& derate : unit.derates) {
        if (!derate.factor.in_unit_interval()) {
          error(ValidationCode::DerateOutOfRange, EntityKind::Equipment, unit.id.value(),
                "derate " + derate.name.str() + " is " + derate.factor.to_string());
        }
        if (derate.evidence.has_value() && !evidence_exists(*derate.evidence)) {
          error(ValidationCode::DerateEvidenceMissing, EntityKind::Equipment, unit.id.value(),
                "derate evidence " + derate.evidence->str() + " is not present");
        }
      }
      if (!unit.degradation.factor.in_unit_interval()) {
        error(ValidationCode::DegradationOutOfRange, EntityKind::Equipment, unit.id.value(),
              "degradation " + unit.degradation.factor.to_string() + " is outside [0, 1]");
      }
      if (unit.degradation.basis == DegradationBasis::None) {
        if (!unit.degradation.factor.is_one()) {
          error(ValidationCode::DegradationBasisMismatch, EntityKind::Equipment, unit.id.value(),
                "a degradation factor of " + unit.degradation.factor.to_string() +
                    " has no stated basis");
        }
        if (unit.degradation.assessed_at.has_value()) {
          error(ValidationCode::DegradationBasisMismatch, EntityKind::Equipment, unit.id.value(),
                "an assessment instant is recorded without a degradation basis");
        }
      } else {
        if (!unit.degradation.assessed_at.has_value()) {
          error(ValidationCode::TransientFactorMissing, EntityKind::Equipment, unit.id.value(),
                "a degradation with basis " +
                    std::string(to_string(unit.degradation.basis)) +
                    " has no assessment instant, so it would never expire");
        }
        if (unit.degradation.evidence.has_value() &&
            !evidence_exists(*unit.degradation.evidence)) {
          error(ValidationCode::DegradationEvidenceMissing, EntityKind::Equipment, unit.id.value(),
                "degradation evidence " + unit.degradation.evidence->str() + " is not present");
        }
      }
      if (is_transport_only(unit.kind) && !unit.nominal.is_zero()) {
        error(ValidationCode::TransportOnlyCapacityDeclared, EntityKind::Equipment, unit.id.value(),
              "transport-only equipment declares a nominal capacity of " +
                  unit.nominal.to_string());
      }
    }
  }

  void check_loops() {
    for (const LoopRecord& loop : snapshot_.loops()) {
      if (snapshot_.find_facility(loop.facility) == nullptr) {
        error(ValidationCode::FacilityMissing, EntityKind::Loop, loop.id.value(),
              "facility " + loop.facility.str() + " is not present");
      }
      if (loop.transport_limit.is_negative()) {
        error(ValidationCode::TransportLimitNegative, EntityKind::Loop, loop.id.value(),
              "transport limit " + loop.transport_limit.to_string() + " is negative");
      }
      if (loop.transport_evidence.has_value() && !evidence_exists(*loop.transport_evidence)) {
        error(ValidationCode::EvidenceReferenceMissing, EntityKind::Loop, loop.id.value(),
              "transport evidence " + loop.transport_evidence->str() + " is not present");
      }
      if (loop.equipment.empty()) {
        error(ValidationCode::LoopWithoutEquipment, EntityKind::Loop, loop.id.value(),
              "a loop with no equipment can never deliver capacity");
      }
      if (loop.equipment.size() > snapshot_.limits().max_equipment_per_loop) {
        error(ValidationCode::RecordLimitExceeded, EntityKind::Loop, loop.id.value(),
              "equipment count exceeds the configured maximum");
      }
      if (!loop.transport_limit_declared) {
        warn(ValidationCode::LoopWithoutTransportDeclaration, EntityKind::Loop, loop.id.value(),
             "no transport limit is declared, so the loop's capacity depends on policy");
      }
      const CoolingMedium loop_medium = medium_of(loop.kind);
      std::int64_t nominal_total = 0;
      for (const EquipmentId& unit_id : loop.equipment) {
        const EquipmentRecord* unit = snapshot_.find_equipment(unit_id);
        if (unit == nullptr) {
          error(ValidationCode::LoopEquipmentMissing, EntityKind::Loop, loop.id.value(),
                "equipment " + unit_id.str() + " is not present");
          continue;
        }
        if (unit->facility != loop.facility) {
          error(ValidationCode::FacilityMissing, EntityKind::Loop, loop.id.value(),
                "equipment " + unit_id.str() + " belongs to a different facility");
        }
        if (!is_transport_only(unit->kind) && medium_of(unit->kind) != loop_medium) {
          error(ValidationCode::EquipmentMediumMismatch, EntityKind::Loop, loop.id.value(),
                "equipment " + unit_id.str() + " removes heat from " +
                    std::string(to_string(medium_of(unit->kind))) + " but the loop carries " +
                    std::string(to_string(loop_medium)));
        }
        if (!unit->nominal.is_negative()) {
          if (nominal_total > std::numeric_limits<std::int64_t>::max() - unit->nominal.milliwatts()) {
            nominal_total = std::numeric_limits<std::int64_t>::max();
          } else {
            nominal_total += unit->nominal.milliwatts();
          }
        }
      }
      for (const ManifoldId& manifold_id : loop.manifolds) {
        const ManifoldRecord* manifold = snapshot_.find_manifold(manifold_id);
        if (manifold == nullptr) {
          error(ValidationCode::ManifoldMissing, EntityKind::Loop, loop.id.value(),
                "manifold " + manifold_id.str() + " is not present");
          continue;
        }
        if (manifold->medium != loop_medium) {
          error(ValidationCode::EquipmentMediumMismatch, EntityKind::Loop, loop.id.value(),
                "manifold " + manifold_id.str() + " carries " +
                    std::string(to_string(manifold->medium)) + " but the loop carries " +
                    std::string(to_string(loop_medium)));
        }
      }
      if (loop.domain.has_value()) {
        const DomainRecord* domain = snapshot_.find_domain(*loop.domain);
        if (domain == nullptr) {
          error(ValidationCode::LoopDomainMissing, EntityKind::Loop, loop.id.value(),
                "domain " + loop.domain->str() + " is not present");
        } else if (domain->medium != loop_medium) {
          error(ValidationCode::LoopDomainMediumMismatch, EntityKind::Loop, loop.id.value(),
                "domain " + loop.domain->str() + " is a " +
                    std::string(to_string(domain->medium)) + " domain but the loop carries " +
                    std::string(to_string(loop_medium)));
        }
      }
      if (loop.primary_plant.has_value() &&
          snapshot_.find_plant(*loop.primary_plant) == nullptr) {
        error(ValidationCode::PlantMissing, EntityKind::Loop, loop.id.value(),
              "plant " + loop.primary_plant->str() + " is not present");
      }
      if (loop.secondary_plant.has_value() &&
          snapshot_.find_plant(*loop.secondary_plant) == nullptr) {
        error(ValidationCode::PlantMissing, EntityKind::Loop, loop.id.value(),
              "plant " + loop.secondary_plant->str() + " is not present");
      }
      if (loop.supply_mode == PlantSupplyMode::Alternates && !loop.secondary_plant.has_value()) {
        error(ValidationCode::SupplyModeWithoutSecondaryPlant, EntityKind::Loop, loop.id.value(),
              "alternating supply requires a secondary plant");
      }
      if (loop.supply_mode == PlantSupplyMode::Single && loop.secondary_plant.has_value()) {
        error(ValidationCode::SupplyModeWithSecondaryPlantButSingle, EntityKind::Loop, loop.id.value(),
              "a secondary plant is declared but the supply mode is single");
      }
      if (loop.secondary_plant.has_value() && loop.primary_plant.has_value() &&
          *loop.secondary_plant == *loop.primary_plant) {
        error(ValidationCode::SupplyModeWithSecondaryPlantButSingle, EntityKind::Loop, loop.id.value(),
              "the primary and secondary plants are the same plant");
      }
      if (loop.transport_limit_declared && !loop.transport_limit.is_negative() &&
          nominal_total < loop.transport_limit.milliwatts()) {
        warn(ValidationCode::LoopTransportAboveEquipment, EntityKind::Loop, loop.id.value(),
             "the transport limit " + loop.transport_limit.to_string() +
                 " is above the nominal equipment total " +
                 ThermalPower::from_milliwatts(nominal_total).to_string());
      }
    }
  }

  void check_plants() {
    for (const PlantRecord& plant : snapshot_.plants()) {
      if (snapshot_.find_facility(plant.facility) == nullptr) {
        error(ValidationCode::FacilityMissing, EntityKind::Plant, plant.id.value(),
              "facility " + plant.facility.str() + " is not present");
      }
      if (plant.equipment.empty()) {
        warn(ValidationCode::PlantWithoutEquipment, EntityKind::Plant, plant.id.value(),
             "a plant with no equipment has no capacity of its own");
      }
      if (plant.equipment.size() > snapshot_.limits().max_equipment_per_loop) {
        error(ValidationCode::RecordLimitExceeded, EntityKind::Plant, plant.id.value(),
              "equipment count exceeds the configured maximum");
      }
      for (const EquipmentId& unit_id : plant.equipment) {
        const EquipmentRecord* unit = snapshot_.find_equipment(unit_id);
        if (unit == nullptr) {
          error(ValidationCode::PlantEquipmentMissing, EntityKind::Plant, plant.id.value(),
                "equipment " + unit_id.str() + " is not present");
          continue;
        }
        if (!is_transport_only(unit->kind) && medium_of(unit->kind) != plant.medium) {
          error(ValidationCode::PlantMediumMismatch, EntityKind::Plant, plant.id.value(),
                "equipment " + unit_id.str() + " removes heat from " +
                    std::string(to_string(medium_of(unit->kind))) + " but the plant is a " +
                    std::string(to_string(plant.medium)) + " plant");
        }
      }
    }
  }

  void check_zones() {
    for (const ZoneRecord& zone : snapshot_.zones()) {
      if (snapshot_.find_facility(zone.facility) == nullptr) {
        error(ValidationCode::FacilityMissing, EntityKind::Zone, zone.id.value(),
              "facility " + zone.facility.str() + " is not present");
      }
      if (zone.loops.empty()) {
        warn(ValidationCode::ZoneWithoutLoops, EntityKind::Zone, zone.id.value(),
             "a zone with no loops can serve no load");
      }
      if (zone.loops.size() > snapshot_.limits().max_loops_per_zone) {
        error(ValidationCode::RecordLimitExceeded, EntityKind::Zone, zone.id.value(),
              "loop count exceeds the configured maximum");
      }
      bool air_class = false;
      bool liquid_class = false;
      for (const CompatibilityClass value : zone.compatibility) {
        if (medium_of(value) == CoolingMedium::Air) {
          air_class = true;
        } else {
          liquid_class = true;
        }
      }
      if (zone.media.contains(CoolingMedium::Air) && !air_class) {
        error(ValidationCode::ZoneMediumWithoutClass, EntityKind::Zone, zone.id.value(),
              "the zone declares air but no air compatibility class");
      }
      if (zone.media.contains(CoolingMedium::Liquid) && !liquid_class) {
        error(ValidationCode::ZoneMediumWithoutClass, EntityKind::Zone, zone.id.value(),
              "the zone declares liquid but no liquid compatibility class");
      }
      if (!zone.reserve_floor.in_unit_interval()) {
        error(ValidationCode::ReserveFloorOutOfRange, EntityKind::Zone, zone.id.value(),
              "reserve floor " + zone.reserve_floor.to_string() + " is outside [0, 1]");
      } else if (zone.reserve_floor < snapshot_.policy().minimum_reserve) {
        // A zone floor weaker than the policy floor is not a defect: the
        // effective reserve is the larger of the two, so the policy still
        // applies. The finding is reported so that an operator can see that the
        // zone's own declaration has no effect.
        warn(ValidationCode::ZoneReserveBelowPolicy, EntityKind::Zone, zone.id.value(),
             "reserve floor " + zone.reserve_floor.to_string() +
                 " is below the policy minimum " +
                 snapshot_.policy().minimum_reserve.to_string() +
                 " and is therefore subsumed by it");
      }
      for (const DomainId& domain_id : zone.domains) {
        if (snapshot_.find_domain(domain_id) == nullptr) {
          error(ValidationCode::ZoneDomainMissing, EntityKind::Zone, zone.id.value(),
                "domain " + domain_id.str() + " is not present");
        }
      }
      for (const LoopId& loop_id : zone.loops) {
        const LoopRecord* loop = snapshot_.find_loop(loop_id);
        if (loop == nullptr) {
          error(ValidationCode::ZoneLoopMissing, EntityKind::Zone, zone.id.value(),
                "loop " + loop_id.str() + " is not present");
          continue;
        }
        if (loop->facility != zone.facility) {
          error(ValidationCode::FacilityMissing, EntityKind::Zone, zone.id.value(),
                "loop " + loop_id.str() + " belongs to a different facility");
        }
      }
      for (const CompatibilityClass value : zone.compatibility) {
        bool served = false;
        for (const LoopId& loop_id : zone.loops) {
          const LoopRecord* loop = snapshot_.find_loop(loop_id);
          if (loop != nullptr && serves(loop->kind, value)) {
            served = true;
            break;
          }
        }
        if (!served) {
          warn(ValidationCode::ZoneClassNotServedByLoop, EntityKind::Zone, zone.id.value(),
               "no serving loop can deliver compatibility class " +
                   std::string(to_string(value)));
        }
      }
    }
  }

  void check_evidence() {
    // Declarative evidence kinds may only have one active record for a given
    // subject and medium; measurement kinds may have many.
    std::set<std::string> declarative_keys;
    for (const EvidenceRecord& record : snapshot_.evidence()) {
      const bool subject_present = std::visit(
          [this](const auto& id) -> bool {
            using IdType = std::decay_t<decltype(id)>;
            if constexpr (std::is_same_v<IdType, SiteId>) {
              return snapshot_.find_site(id) != nullptr;
            } else if constexpr (std::is_same_v<IdType, FacilityId>) {
              return snapshot_.find_facility(id) != nullptr;
            } else if constexpr (std::is_same_v<IdType, ZoneId>) {
              return snapshot_.find_zone(id) != nullptr;
            } else if constexpr (std::is_same_v<IdType, LoopId>) {
              return snapshot_.find_loop(id) != nullptr;
            } else if constexpr (std::is_same_v<IdType, PlantId>) {
              return snapshot_.find_plant(id) != nullptr;
            } else if constexpr (std::is_same_v<IdType, EquipmentId>) {
              return snapshot_.find_equipment(id) != nullptr;
            } else if constexpr (std::is_same_v<IdType, ManifoldId>) {
              return snapshot_.find_manifold(id) != nullptr;
            } else {
              return snapshot_.find_domain(id) != nullptr;
            }
          },
          record.subject);
      if (!subject_present) {
        error(ValidationCode::EvidenceSubjectMissing, EntityKind::Evidence, record.id.value(),
              std::string("subject ") + std::string(subject_kind_name(record.subject)) + " " +
                  subject_text(record.subject) + " is not present");
      }
      if (!value_kind_allowed(record.kind, record.value)) {
        error(ValidationCode::EvidenceValueKindMismatch, EntityKind::Evidence, record.id.value(),
              std::string("evidence kind ") + std::string(to_string(record.kind)) +
                  " cannot carry a " + std::string(value_kind_name(record.value)) + " value");
      }
      if (std::holds_alternative<ThermalPower>(record.value) && !record.medium.has_value()) {
        error(ValidationCode::EvidenceMediumMismatch, EntityKind::Evidence, record.id.value(),
              "evidence carrying a thermal power must name the medium it is about");
      }
      if (record.medium.has_value() && !medium_matches_subject(record)) {
        error(ValidationCode::EvidenceMediumMismatch, EntityKind::Evidence, record.id.value(),
              "declared medium " + std::string(to_string(*record.medium)) +
                  " does not match the subject");
      }
      if (record.valid_until.has_value() && *record.valid_until < record.observed_at) {
        error(ValidationCode::EvidenceValidityInverted, EntityKind::Evidence, record.id.value(),
              "the validity window ends before the observation instant");
      }
      if (record.provenance.supersedes.has_value()) {
        if (*record.provenance.supersedes == record.id) {
          error(ValidationCode::EvidenceSupersedesSelf, EntityKind::Evidence, record.id.value(),
                "evidence supersedes itself");
        } else if (!evidence_exists(*record.provenance.supersedes)) {
          error(ValidationCode::EvidenceSupersedesMissing, EntityKind::Evidence, record.id.value(),
                "superseded evidence " + record.provenance.supersedes->str() + " is not present");
        }
      }
      if (record.state == EvidenceState::Active && is_declarative(record.kind)) {
        std::string key(cooling_capacity::to_string(record.kind));
        key.push_back('|');
        key.append(subject_kind_name(record.subject));
        key.push_back('|');
        key.append(subject_text(record.subject));
        key.push_back('|');
        key.append(record.medium.has_value() ? std::string(to_string(*record.medium)) : "-");
        if (!declarative_keys.insert(key).second) {
          error(ValidationCode::EvidenceDuplicateActiveKind, EntityKind::Evidence, record.id.value(),
                "another active " + std::string(to_string(record.kind)) +
                    " record already describes this subject and medium");
        }
      }
    }
  }

  [[nodiscard]] bool medium_matches_subject(const EvidenceRecord& record) const {
    return std::visit(
        [this, &record](const auto& id) -> bool {
          using IdType = std::decay_t<decltype(id)>;
          if constexpr (std::is_same_v<IdType, EquipmentId>) {
            const EquipmentRecord* unit = snapshot_.find_equipment(id);
            return unit == nullptr || is_transport_only(unit->kind) ||
                   medium_of(unit->kind) == *record.medium;
          } else if constexpr (std::is_same_v<IdType, LoopId>) {
            const LoopRecord* loop = snapshot_.find_loop(id);
            return loop == nullptr || medium_of(loop->kind) == *record.medium;
          } else if constexpr (std::is_same_v<IdType, PlantId>) {
            const PlantRecord* plant = snapshot_.find_plant(id);
            return plant == nullptr || plant->medium == *record.medium;
          } else if constexpr (std::is_same_v<IdType, DomainId>) {
            const DomainRecord* domain = snapshot_.find_domain(id);
            return domain == nullptr || domain->medium == *record.medium;
          } else if constexpr (std::is_same_v<IdType, ManifoldId>) {
            const ManifoldRecord* manifold = snapshot_.find_manifold(id);
            return manifold == nullptr || manifold->medium == *record.medium;
          } else {
            return true;
          }
        },
        record.subject);
  }

  [[nodiscard]] static bool is_declarative(EvidenceKind kind) {
    switch (kind) {
      case EvidenceKind::Nameplate:
      case EvidenceKind::CapacityValidation:
      case EvidenceKind::TransportSurvey:
      case EvidenceKind::RedundancyDeclaration:
      case EvidenceKind::DegradationAssessment:
        return true;
      default:
        return false;
    }
  }

  [[nodiscard]] static bool value_kind_allowed(EvidenceKind kind, const EvidenceValue& value) {
    const bool is_declaration = std::holds_alternative<std::monostate>(value);
    const bool is_power = std::holds_alternative<ThermalPower>(value);
    const bool is_fraction = std::holds_alternative<Fraction>(value);
    const bool is_temperature = std::holds_alternative<Temperature>(value);
    const bool is_flow = std::holds_alternative<VolumetricFlow>(value);
    const bool is_state = std::holds_alternative<OperatingState>(value);
    const bool is_redundancy = std::holds_alternative<RedundancyClass>(value);
    const bool is_mask = std::holds_alternative<MediumMask>(value);
    switch (kind) {
      case EvidenceKind::Nameplate:
      case EvidenceKind::CapacityValidation:
      case EvidenceKind::LoadMeasurement:
      case EvidenceKind::TransportSurvey:
        return is_power;
      case EvidenceKind::Telemetry:
        return is_power || is_temperature || is_flow || is_fraction;
      case EvidenceKind::MaintenanceOrder:
        return is_state || is_declaration;
      case EvidenceKind::EnvironmentalConstraint:
        return is_temperature || is_fraction || is_flow || is_declaration;
      case EvidenceKind::DegradationAssessment:
        return is_fraction;
      case EvidenceKind::RedundancyDeclaration:
        return is_redundancy || is_declaration;
      case EvidenceKind::OperatorAssertion:
        return true;
      case EvidenceKind::VendorBulletin:
        return is_declaration || is_power || is_temperature || is_fraction || is_mask;
    }
    return false;
  }

  void check_commitments() {
    std::set<std::string> attempts;
    for (const CommitmentRecord& commitment : snapshot_.commitments()) {
      if (commitment.zone.empty()) {
        error(ValidationCode::CommitmentZoneMissing, EntityKind::Commitment, commitment.id.value(),
              "commitment has no zone");
      } else if (snapshot_.find_zone(commitment.zone) == nullptr) {
        error(ValidationCode::CommitmentZoneMissing, EntityKind::Commitment, commitment.id.value(),
              "zone " + commitment.zone.str() + " is not present");
      }
      if (commitment.medium != medium_of(commitment.compatibility)) {
        error(ValidationCode::CommitmentMediumMismatch, EntityKind::Commitment, commitment.id.value(),
              "compatibility class " + std::string(to_string(commitment.compatibility)) +
                  " belongs to medium " +
                  std::string(to_string(medium_of(commitment.compatibility))) +
                  " but the commitment is for " +
                  std::string(to_string(commitment.medium)));
      }
      if (commitment.thermal.is_negative()) {
        error(ValidationCode::CommitmentNegativeLoad, EntityKind::Commitment, commitment.id.value(),
              "committed load is negative: " + commitment.thermal.to_string());
      }
      if (commitment.pinned_loop.has_value()) {
        const LoopRecord* loop = snapshot_.find_loop(*commitment.pinned_loop);
        if (loop == nullptr) {
          error(ValidationCode::CommitmentLoopMissing, EntityKind::Commitment, commitment.id.value(),
                "loop " + commitment.pinned_loop->str() + " is not present");
        } else {
          const ZoneRecord* zone = snapshot_.find_zone(commitment.zone);
          if (zone != nullptr &&
              std::find(zone->loops.begin(), zone->loops.end(), *commitment.pinned_loop) ==
                  zone->loops.end()) {
            error(ValidationCode::CommitmentLoopNotServingZone, EntityKind::Commitment,
                  commitment.id.value(),
                  "loop " + commitment.pinned_loop->str() + " does not serve zone " +
                      commitment.zone.str());
          }
        }
      } else if (consumes_capacity(commitment.state)) {
        // A commitment that consumes capacity has to say which loop carries it.
        // Otherwise its load would be visible at the zone and invisible at the
        // loop, which is an accounting hole rather than a modelling choice.
        error(ValidationCode::CommitmentLoopMissing, EntityKind::Commitment, commitment.id.value(),
              "a capacity-consuming commitment must name the loop that carries it");
      }
      const ZoneRecord* zone = snapshot_.find_zone(commitment.zone);
      if (zone != nullptr) {
        if (!zone->media.contains(commitment.medium)) {
          error(ValidationCode::CommitmentMediumMismatchWithZone, EntityKind::Commitment,
                commitment.id.value(),
                "zone " + zone->id.str() + " does not declare medium " +
                    std::string(to_string(commitment.medium)));
        } else if (std::find(zone->compatibility.begin(), zone->compatibility.end(),
                             commitment.compatibility) == zone->compatibility.end()) {
          error(ValidationCode::CommitmentClassUnsupportedByZone, EntityKind::Commitment,
                commitment.id.value(),
                "zone " + zone->id.str() + " does not support compatibility class " +
                    std::string(to_string(commitment.compatibility)));
        }
      }
      if (commitment.expires_at.has_value() && *commitment.expires_at < commitment.created_at) {
        error(ValidationCode::CommitmentExpiryBeforeCreation, EntityKind::Commitment,
              commitment.id.value(), "the expiry instant is before the creation instant");
      }
      if (commitment.supersedes.has_value() &&
          snapshot_.find_commitment(*commitment.supersedes) == nullptr) {
        error(ValidationCode::CommitmentSupersedesMissing, EntityKind::Commitment, commitment.id.value(),
              "superseded commitment " + commitment.supersedes->str() + " is not present");
      }
      if (consumes_capacity(commitment.state)) {
        const std::string key = commitment.attempt.str();
        if (!attempts.insert(key).second) {
          error(ValidationCode::CommitmentDuplicateAttempt, EntityKind::Commitment, commitment.id.value(),
                "attempt " + key + " is already applied to another capacity-consuming commitment");
        }
      }
    }
  }

  void check_over_commitment() {
    const CoolingAnalyzer analyzer(snapshot_, snapshot_.constructed_at(),
                                   AnalysisBasis::Fresh);
    for (const ZoneRecord& zone : snapshot_.zones()) {
      for (const CompatibilityClass value : zone.compatibility) {
        const Result<ZoneCapacity> capacity =
            analyzer.zone_capacity(zone.id, medium_of(value), value);
        if (!capacity.ok()) {
          continue;
        }
        if (capacity.value().breakdown.over_committed) {
          warn(ValidationCode::CommitmentOverCommitted, EntityKind::Zone, zone.id.value(),
               "committed load " + capacity.value().breakdown.committed.to_string() +
                   " exceeds the usable capacity " +
                   capacity.value().breakdown.usable.to_string() + " for class " +
                   std::string(to_string(value)));
        }
      }
    }
  }

  const CoolingSnapshot& snapshot_;
  ValidationReport report_;
};

}  // namespace

ValidationReport validate_snapshot(const CoolingSnapshot& snapshot) {
  Validator validator(snapshot);
  return validator.run();
}

}  // namespace cooling_capacity
