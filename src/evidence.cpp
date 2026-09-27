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

#include "cooling_capacity/evidence.hpp"

#include "enum_table.hpp"

namespace cooling_capacity {

namespace {

constexpr detail::EnumName<EvidenceKind> kEvidenceKindNames[] = {
    {EvidenceKind::Nameplate, "nameplate"},
    {EvidenceKind::CapacityValidation, "capacity-validation"},
    {EvidenceKind::LoadMeasurement, "load-measurement"},
    {EvidenceKind::Telemetry, "telemetry"},
    {EvidenceKind::MaintenanceOrder, "maintenance-order"},
    {EvidenceKind::EnvironmentalConstraint, "environmental-constraint"},
    {EvidenceKind::TransportSurvey, "transport-survey"},
    {EvidenceKind::DegradationAssessment, "degradation-assessment"},
    {EvidenceKind::RedundancyDeclaration, "redundancy-declaration"},
    {EvidenceKind::OperatorAssertion, "operator-assertion"},
    {EvidenceKind::VendorBulletin, "vendor-bulletin"},
};

constexpr detail::EnumName<EvidenceSource> kEvidenceSourceNames[] = {
    {EvidenceSource::VendorDocument, "vendor-document"},
    {EvidenceSource::CommissioningTool, "commissioning-tool"},
    {EvidenceSource::BmsTelemetry, "bms-telemetry"},
    {EvidenceSource::OperatorEntry, "operator-entry"},
    {EvidenceSource::FacilityRegistry, "facility-registry"},
    {EvidenceSource::Derived, "derived"},
};

constexpr detail::EnumName<EvidenceState> kEvidenceStateNames[] = {
    {EvidenceState::Active, "active"},
    {EvidenceState::Superseded, "superseded"},
    {EvidenceState::Withdrawn, "withdrawn"},
};

constexpr detail::EnumName<EvidenceFreshness> kFreshnessNames[] = {
    {EvidenceFreshness::Fresh, "fresh"},
    {EvidenceFreshness::Stale, "stale"},
    {EvidenceFreshness::ExpiredByValidity, "expired-by-validity"},
    {EvidenceFreshness::Withdrawn, "withdrawn"},
    {EvidenceFreshness::Superseded, "superseded"},
    {EvidenceFreshness::FutureDated, "future-dated"},
};

// The name and text of a subject or value are part of the canonical export and
// of the command line output, so they are stable and deterministic.
struct SubjectKindVisitor {
  std::string_view operator()(const SiteId&) const { return "site"; }
  std::string_view operator()(const FacilityId&) const { return "facility"; }
  std::string_view operator()(const ZoneId&) const { return "zone"; }
  std::string_view operator()(const LoopId&) const { return "loop"; }
  std::string_view operator()(const PlantId&) const { return "plant"; }
  std::string_view operator()(const EquipmentId&) const { return "equipment"; }
  std::string_view operator()(const ManifoldId&) const { return "manifold"; }
  std::string_view operator()(const DomainId&) const { return "domain"; }
};

struct ValueKindVisitor {
  std::string_view operator()(std::monostate) const { return "declaration"; }
  std::string_view operator()(ThermalPower) const { return "thermal-power"; }
  std::string_view operator()(Fraction) const { return "fraction"; }
  std::string_view operator()(Temperature) const { return "temperature"; }
  std::string_view operator()(VolumetricFlow) const { return "volumetric-flow"; }
  std::string_view operator()(OperatingState) const { return "operating-state"; }
  std::string_view operator()(RedundancyClass) const { return "redundancy-class"; }
  std::string_view operator()(MediumMask) const { return "medium-mask"; }
};

struct ValueTextVisitor {
  std::string operator()(std::monostate) const { return "declared"; }
  std::string operator()(ThermalPower value) const { return value.to_string(); }
  std::string operator()(Fraction value) const { return value.to_string(); }
  std::string operator()(Temperature value) const { return value.to_string(); }
  std::string operator()(VolumetricFlow value) const { return value.to_string(); }
  std::string operator()(OperatingState value) const {
    return std::string(cooling_capacity::to_string(value));
  }
  std::string operator()(RedundancyClass value) const {
    return std::string(cooling_capacity::to_string(value));
  }
  std::string operator()(MediumMask value) const { return value.to_string(); }
};

}  // namespace

std::string_view to_string(EvidenceKind kind) noexcept {
  return detail::name_from_table(kEvidenceKindNames, kind);
}

Result<EvidenceKind> parse_evidence_kind(std::string_view text) {
  return detail::parse_from_table("evidence kind", kEvidenceKindNames, text);
}

EvidenceKind evidence_kind_at(std::size_t index) {
  return index < kEvidenceKindCount ? static_cast<EvidenceKind>(index) : EvidenceKind::Nameplate;
}

std::string_view to_string(EvidenceSource source) noexcept {
  return detail::name_from_table(kEvidenceSourceNames, source);
}

Result<EvidenceSource> parse_evidence_source(std::string_view text) {
  return detail::parse_from_table("evidence source", kEvidenceSourceNames, text);
}

std::string_view to_string(EvidenceState state) noexcept {
  return detail::name_from_table(kEvidenceStateNames, state);
}

Result<EvidenceState> parse_evidence_state(std::string_view text) {
  return detail::parse_from_table("evidence state", kEvidenceStateNames, text);
}

std::string_view to_string(EvidenceFreshness value) noexcept {
  return detail::name_from_table(kFreshnessNames, value);
}

bool is_usable(EvidenceFreshness value) noexcept {
  return value == EvidenceFreshness::Fresh;
}

std::string_view subject_kind_name(const EvidenceSubject& subject) noexcept {
  return std::visit(SubjectKindVisitor{}, subject);
}

std::string subject_text(const EvidenceSubject& subject) {
  return std::visit(
      [](const auto& id) -> std::string { return std::string(id.view()); }, subject);
}

std::string_view value_kind_name(const EvidenceValue& value) noexcept {
  return std::visit(ValueKindVisitor{}, value);
}

std::string value_text(const EvidenceValue& value) {
  return std::visit(ValueTextVisitor{}, value);
}

}  // namespace cooling_capacity
