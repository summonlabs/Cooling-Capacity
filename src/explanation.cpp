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

#include "cooling_capacity/explanation.hpp"

#include <array>

#include "enum_table.hpp"

namespace cooling_capacity {

namespace {

struct CodeTemplate {
  ExplanationCode code;
  std::string_view name;
  std::string_view text;
};

// One entry per code. The templates are the human-readable projection of the
// machine-readable code and its parameters; they are part of the public
// contract and are covered by tests.
constexpr CodeTemplate kTemplates[] = {
    {ExplanationCode::SnapshotBasis, "snapshot-basis",
     "snapshot generation {generation} on basis {basis} evaluated at {evaluated_at}"},
    {ExplanationCode::ZoneResolved, "zone-resolved",
     "zone {zone} in facility {facility} declares media {media} and classes {classes}"},
    {ExplanationCode::ZoneNotFound, "zone-not-found", "zone {zone} is not present in the snapshot"},
    {ExplanationCode::MediumUnsupported, "medium-unsupported",
     "zone {zone} does not declare medium {medium}"},
    {ExplanationCode::ClassUnsupported, "class-unsupported",
     "zone {zone} does not support compatibility class {class} on medium {medium}"},
    {ExplanationCode::LoopConsidered, "loop-considered",
     "loop {loop} of kind {kind} considered for medium {medium}"},
    {ExplanationCode::LoopClassMismatch, "loop-class-mismatch",
     "loop {loop} cannot serve compatibility class {class}"},
    {ExplanationCode::LoopTransportDeclared, "loop-transport-declared",
     "loop {loop} declares a transport limit of {limit}"},
    {ExplanationCode::LoopTransportUndeclared, "loop-transport-undeclared",
     "loop {loop} has no declared transport limit, so its capacity is unknown"},
    {ExplanationCode::LoopCapacity, "loop-capacity",
     "loop {loop} usable capacity is {usable}, limited by {limited_by}"},
    {ExplanationCode::EquipmentConsidered, "equipment-considered",
     "equipment {equipment} of kind {kind} in state {state} on loop {loop}"},
    {ExplanationCode::EquipmentOutOfService, "equipment-out-of-service",
     "equipment {equipment} contributes nothing because it is {state} ({reason})"},
    {ExplanationCode::EquipmentStateUnknown, "equipment-state-unknown",
     "equipment {equipment} has an unknown operating state"},
    {ExplanationCode::EquipmentCapacitySource, "equipment-capacity-source",
     "equipment {equipment} uses a {basis} capacity of {value}"},
    {ExplanationCode::EquipmentValidationMissing, "equipment-validation-missing",
     "equipment {equipment} has no validating evidence and the policy requires one"},
    {ExplanationCode::EquipmentDerated, "equipment-derated",
     "equipment {equipment} derated by {factor} for {reason}, cumulative {cumulative}"},
    {ExplanationCode::EquipmentDerateBelowFloor, "equipment-derate-below-floor",
     "equipment {equipment} has cumulative derate {cumulative} below the policy floor {floor}"},
    {ExplanationCode::EquipmentDegraded, "equipment-degraded",
     "equipment {equipment} degraded by {factor} on basis {basis}, cumulative {cumulative}"},
    {ExplanationCode::EquipmentSharedBetweenLoops, "equipment-shared-between-loops",
     "equipment {equipment} is shared by {loops} loops and contributes {share} here"},
    {ExplanationCode::EquipmentTransportOnly, "equipment-transport-only",
     "equipment {equipment} of kind {kind} carries no thermal capacity"},
    {ExplanationCode::EquipmentCapacityMissing, "equipment-capacity-missing",
     "equipment {equipment} has no usable capacity declaration"},
    {ExplanationCode::RedundancyApplied, "redundancy-applied",
     "group {group} declares {declared}, effective {effective}: {counted} of {members} units "
     "counted, {excluded} excluded"},
    {ExplanationCode::RedundancyRequirementMet, "redundancy-requirement-met",
     "group {group} declared {declared} meets the required {required}"},
    {ExplanationCode::RedundancyRequirementUnmet, "redundancy-requirement-unmet",
     "group {group} effective {effective} does not meet the required {required}"},
    {ExplanationCode::CommittedLoadCharged, "committed-load-charged",
     "{scope} carries {committed} of committed load in state {states}"},
    {ExplanationCode::ZoneLocalHeadroom, "zone-local-headroom",
     "zone {zone} local headroom is {local} from {loops} serving loops"},
    {ExplanationCode::PlantShared, "plant-shared",
     "plant {plant} is shared by {loops} loops: usable {usable}, committed {committed}, "
     "headroom {headroom}"},
    {ExplanationCode::PlantSupplyAlternates, "plant-supply-alternates",
     "loop {loop} alternates between plants {primary} and {secondary}"},
    {ExplanationCode::SharedHeadroomIsShared, "shared-headroom-is-shared",
     "shared headroom of {plants} plants is not additive across zones"},
    {ExplanationCode::ReserveHeld, "reserve-held",
     "{scope} holds back {reserve} of {before} as a reserve, leaving {after}"},
    {ExplanationCode::Bottleneck, "bottleneck",
     "binding constraint is {kind} {subject}: limit {limit}, consumed {consumed}, "
     "remaining {remaining}"},
    {ExplanationCode::CandidateRequest, "candidate-request",
     "candidate load of {thermal} on medium {medium} with class {class} for zone {zone}"},
    {ExplanationCode::CandidateDisposition, "candidate-disposition",
     "candidate is {disposition}: {reason}"},
    {ExplanationCode::EvidenceAssessed, "evidence-assessed",
     "evidence {evidence} of kind {kind} is {freshness} at age {age}"},
    {ExplanationCode::EvidenceRejected, "evidence-rejected",
     "evidence {evidence} was rejected: {reason}"},
    {ExplanationCode::PolicyApplied, "policy-applied",
     "policy {policy} revision {revision}: stale {stale_action}, unknown {unknown_action}"},
    {ExplanationCode::SnapshotRecovered, "snapshot-recovered",
     "snapshot generation {generation} was recovered from a store at {loaded_at} and has not "
     "been revalidated"},
    {ExplanationCode::SnapshotRevalidated, "snapshot-revalidated",
     "snapshot generation {generation} was revalidated against evidence current at {at}"},
    {ExplanationCode::CommitmentApplied, "commitment-applied",
     "commitment {commitment} {operation} as {state} for {thermal}"},
    {ExplanationCode::CommitmentReplayed, "commitment-replayed",
     "attempt {attempt} was already applied to commitment {commitment}"},
    {ExplanationCode::NoObservation, "no-observation",
     "no fresh observation exists for {scope} on medium {medium}"},
    {ExplanationCode::ObservationCharged, "observation-charged",
     "{scope} reports an observed load of {observed} from evidence {evidence}"},
    {ExplanationCode::DiffSummary, "diff-summary",
     "generation {from} to {to} changed {changes} records: {added} added, {removed} removed, "
     "{modified} modified"},
    {ExplanationCode::OverCommitted, "over-committed",
     "{scope} is over-committed by {deficit}"},
    {ExplanationCode::TransportOnlyGroupMember, "transport-only-group-member",
     "equipment {equipment} helps move heat on loop {loop} but adds no removal capacity"},
};

}  // namespace

std::string_view to_string(ExplanationCode code) noexcept {
  for (const CodeTemplate& entry : kTemplates) {
    if (entry.code == code) {
      return entry.name;
    }
  }
  return "unrecognised";
}

ExplanationCode explanation_code_at(std::size_t index) {
  return index < kExplanationCodeCount ? static_cast<ExplanationCode>(index)
                                       : ExplanationCode::SnapshotBasis;
}

std::string_view explanation_template(ExplanationCode code) noexcept {
  for (const CodeTemplate& entry : kTemplates) {
    if (entry.code == code) {
      return entry.text;
    }
  }
  return "unrecognised explanation code";
}

std::vector<std::string> explanation_required_parameters(ExplanationCode code) {
  const std::string_view text = explanation_template(code);
  std::vector<std::string> keys;
  std::size_t index = 0;
  while (index < text.size()) {
    const std::size_t open = text.find('{', index);
    if (open == std::string_view::npos) {
      break;
    }
    const std::size_t close = text.find('}', open + 1);
    if (close == std::string_view::npos) {
      break;
    }
    std::string key(text.substr(open + 1, close - open - 1));
    bool seen = false;
    for (const std::string& existing : keys) {
      if (existing == key) {
        seen = true;
        break;
      }
    }
    if (!seen) {
      keys.push_back(std::move(key));
    }
    index = close + 1;
  }
  return keys;
}

ExplanationStep::ExplanationStep(ExplanationCode code, std::vector<ExplanationParam> params)
    : code_(code), params_(std::move(params)) {}

bool ExplanationStep::has(const std::string& key) const {
  for (const ExplanationParam& entry : params_) {
    if (entry.key == key) {
      return true;
    }
  }
  return false;
}

std::string ExplanationStep::render() const {
  const std::string_view text = explanation_template(code_);
  std::string out;
  out.reserve(text.size() + 32U);
  std::size_t index = 0;
  while (index < text.size()) {
    const std::size_t open = text.find('{', index);
    if (open == std::string_view::npos) {
      out.append(text.substr(index));
      break;
    }
    out.append(text.substr(index, open - index));
    const std::size_t close = text.find('}', open + 1);
    if (close == std::string_view::npos) {
      out.append(text.substr(open));
      break;
    }
    const std::string key(text.substr(open + 1, close - open - 1));
    bool substituted = false;
    for (const ExplanationParam& entry : params_) {
      if (entry.key == key) {
        out.append(entry.value);
        substituted = true;
        break;
      }
    }
    if (!substituted) {
      out.append("<missing:");
      out.append(key);
      out.push_back('>');
    }
    index = close + 1;
  }
  return out;
}

std::string ExplanationStep::to_string() const {
  std::string out(cooling_capacity::to_string(code_));
  out.append(": ");
  out.append(render());
  return out;
}

ExplanationBuilder& ExplanationBuilder::add(ExplanationCode code,
                                            std::vector<ExplanationParam> params) {
  steps_.emplace_back(code, std::move(params));
  return *this;
}

std::string ExplanationBuilder::render() const {
  return render_steps(steps_);
}

std::string render_steps(const std::vector<ExplanationStep>& steps) {
  std::string out;
  for (const ExplanationStep& step : steps) {
    out.append(step.to_string());
    out.push_back('\n');
  }
  return out;
}

ExplanationParam param(std::string key, std::string value) {
  return ExplanationParam{std::move(key), std::move(value)};
}

ExplanationParam param(std::string key, std::string_view value) {
  return ExplanationParam{std::move(key), std::string(value)};
}

ExplanationParam param(std::string key, const char* value) {
  return ExplanationParam{std::move(key), std::string(value)};
}

ExplanationParam param(std::string key, std::int64_t value) {
  return ExplanationParam{std::move(key), std::to_string(value)};
}

ExplanationParam param(std::string key, std::uint64_t value) {
  return ExplanationParam{std::move(key), std::to_string(value)};
}

ExplanationParam param(std::string key, bool value) {
  return ExplanationParam{std::move(key), value ? "true" : "false"};
}

}  // namespace cooling_capacity
