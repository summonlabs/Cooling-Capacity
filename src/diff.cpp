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

#include "cooling_capacity/diff.hpp"

#include <algorithm>
#include <map>

namespace cooling_capacity {

namespace {

std::string yes_no(bool value) { return value ? "true" : "false"; }

template <class Id>
std::string join_typed(const std::vector<Id>& ids) {
  std::string out;
  for (std::size_t index = 0; index < ids.size(); ++index) {
    if (index != 0) {
      out.push_back(',');
    }
    out.append(ids[index].str());
  }
  return out;
}

template <class Id>
std::string optional_typed(const std::optional<Id>& id) {
  return id.has_value() ? id->str() : std::string("-");
}

std::string optional_power(const std::optional<ThermalPower>& power) {
  return power.has_value() ? power->to_string() : std::string("-");
}

std::string optional_time(const std::optional<Timestamp>& instant) {
  return instant.has_value() ? instant->to_string() : std::string("-");
}

using Fields = std::vector<std::pair<std::string, std::string>>;

void add_field(Fields& fields, const char* name, std::string value) {
  fields.emplace_back(name, std::move(value));
}

}  // namespace

std::string_view to_string(ChangeKind kind) noexcept {
  switch (kind) {
    case ChangeKind::Added:
      return "added";
    case ChangeKind::Removed:
      return "removed";
    case ChangeKind::Modified:
      return "modified";
  }
  return "unrecognised";
}

bool operator==(const RecordChange& lhs, const RecordChange& rhs) {
  return lhs.kind == rhs.kind && lhs.id == rhs.id && lhs.change == rhs.change &&
         lhs.fields == rhs.fields;
}

std::string RecordChange::to_string() const {
  std::string out(cooling_capacity::to_string(kind));
  out.push_back(' ');
  out.append(id.str());
  out.append(": ");
  out.append(cooling_capacity::to_string(change));
  for (const FieldDelta& delta : fields) {
    out.push_back(' ');
    out.append(delta.field.str());
    out.push_back('=');
    out.append(delta.before);
    out.append("->");
    out.append(delta.after);
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> record_fields(const CoolingSnapshot& snapshot,
                                                               EntityKind kind,
                                                               const Identifier& id) {
  Fields fields;
  // The identifier arrives as an untyped token because a diff walks a
  // heterogeneous set of records. Parsing it back into the right family here
  // means a mismatched kind and identifier simply yields no fields, rather than
  // being reinterpreted as another family's record.
  switch (kind) {
    case EntityKind::Site: {
      Result<SiteId> typed = SiteId::parse(id.view());
      if (!typed.ok()) {
        return fields;
      }
      const SiteRecord* record = snapshot.find_site(typed.value());
      if (record == nullptr) {
        return fields;
      }
      add_field(fields, "label", record->label.str());
      add_field(fields, "revision", record->revision.to_string());
      break;
    }
    case EntityKind::Facility: {
      Result<FacilityId> typed = FacilityId::parse(id.view());
      if (!typed.ok()) {
        return fields;
      }
      const FacilityRecord* record = snapshot.find_facility(typed.value());
      if (record == nullptr) {
        return fields;
      }
      add_field(fields, "label", record->label.str());
      add_field(fields, "revision", record->revision.to_string());
      add_field(fields, "site", record->site.str());
      break;
    }
    case EntityKind::Domain: {
      Result<DomainId> typed = DomainId::parse(id.view());
      if (!typed.ok()) {
        return fields;
      }
      const DomainRecord* record = snapshot.find_domain(typed.value());
      if (record == nullptr) {
        return fields;
      }
      add_field(fields, "facility", record->facility.str());
      add_field(fields, "label", record->label.str());
      add_field(fields, "medium", std::string(to_string(record->medium)));
      add_field(fields, "revision", record->revision.to_string());
      break;
    }
    case EntityKind::Manifold: {
      Result<ManifoldId> typed = ManifoldId::parse(id.view());
      if (!typed.ok()) {
        return fields;
      }
      const ManifoldRecord* record = snapshot.find_manifold(typed.value());
      if (record == nullptr) {
        return fields;
      }
      add_field(fields, "facility", record->facility.str());
      add_field(fields, "label", record->label.str());
      add_field(fields, "medium", std::string(to_string(record->medium)));
      add_field(fields, "revision", record->revision.to_string());
      add_field(fields, "transport_evidence", optional_typed(record->transport_evidence));
      add_field(fields, "transport_limit", record->transport_limit.to_string());
      break;
    }
    case EntityKind::Equipment: {
      Result<EquipmentId> typed = EquipmentId::parse(id.view());
      if (!typed.ok()) {
        return fields;
      }
      const EquipmentRecord* record = snapshot.find_equipment(typed.value());
      if (record == nullptr) {
        return fields;
      }
      add_field(fields, "degradation_assessed_at", optional_time(record->degradation.assessed_at));
      add_field(fields, "degradation_basis",
                std::string(to_string(record->degradation.basis)));
      add_field(fields, "degradation_evidence",
                optional_typed(record->degradation.evidence));
      add_field(fields, "degradation_factor", record->degradation.factor.to_string());
      std::string derates;
      for (std::size_t index = 0; index < record->derates.size(); ++index) {
        if (index != 0) {
          derates.push_back(',');
        }
        derates.append(record->derates[index].name.str());
        derates.push_back('=');
        derates.append(record->derates[index].factor.to_string());
        derates.push_back('/');
        derates.append(to_string(record->derates[index].reason));
      }
      add_field(fields, "derates", derates);
      add_field(fields, "facility", record->facility.str());
      add_field(fields, "kind", std::string(to_string(record->kind)));
      add_field(fields, "label", record->label.str());
      add_field(fields, "nominal", record->nominal.to_string());
      add_field(fields, "revision", record->revision.to_string());
      add_field(fields, "state", std::string(to_string(record->state)));
      add_field(fields, "state_evidence", optional_typed(record->state_evidence));
      add_field(fields, "state_since", optional_time(record->state_since));
      add_field(fields, "validated", optional_power(record->validated));
      add_field(fields, "validated_evidence", optional_typed(record->validated_evidence));
      break;
    }
    case EntityKind::Loop: {
      Result<LoopId> typed = LoopId::parse(id.view());
      if (!typed.ok()) {
        return fields;
      }
      const LoopRecord* record = snapshot.find_loop(typed.value());
      if (record == nullptr) {
        return fields;
      }
      add_field(fields, "domain", optional_typed(record->domain));
      add_field(fields, "equipment", join_typed(record->equipment));
      add_field(fields, "facility", record->facility.str());
      add_field(fields, "kind", std::string(to_string(record->kind)));
      add_field(fields, "label", record->label.str());
      add_field(fields, "manifolds", join_typed(record->manifolds));
      add_field(fields, "primary_plant", optional_typed(record->primary_plant));
      add_field(fields, "redundancy", std::string(to_string(record->redundancy)));
      add_field(fields, "revision", record->revision.to_string());
      add_field(fields, "secondary_plant", optional_typed(record->secondary_plant));
      add_field(fields, "supply_mode", std::string(to_string(record->supply_mode)));
      add_field(fields, "transport_evidence", optional_typed(record->transport_evidence));
      add_field(fields, "transport_limit", record->transport_limit.to_string());
      add_field(fields, "transport_limit_declared", yes_no(record->transport_limit_declared));
      break;
    }
    case EntityKind::Plant: {
      Result<PlantId> typed = PlantId::parse(id.view());
      if (!typed.ok()) {
        return fields;
      }
      const PlantRecord* record = snapshot.find_plant(typed.value());
      if (record == nullptr) {
        return fields;
      }
      add_field(fields, "equipment", join_typed(record->equipment));
      add_field(fields, "facility", record->facility.str());
      add_field(fields, "label", record->label.str());
      add_field(fields, "medium", std::string(to_string(record->medium)));
      add_field(fields, "redundancy", std::string(to_string(record->redundancy)));
      add_field(fields, "revision", record->revision.to_string());
      break;
    }
    case EntityKind::Zone: {
      Result<ZoneId> typed = ZoneId::parse(id.view());
      if (!typed.ok()) {
        return fields;
      }
      const ZoneRecord* record = snapshot.find_zone(typed.value());
      if (record == nullptr) {
        return fields;
      }
      std::string classes;
      for (std::size_t index = 0; index < record->compatibility.size(); ++index) {
        if (index != 0) {
          classes.push_back(',');
        }
        classes.append(to_string(record->compatibility[index]));
      }
      add_field(fields, "classes", classes);
      add_field(fields, "domains", join_typed(record->domains));
      add_field(fields, "facility", record->facility.str());
      add_field(fields, "label", record->label.str());
      add_field(fields, "loops", join_typed(record->loops));
      add_field(fields, "media", record->media.to_string());
      add_field(fields, "reserve_floor", record->reserve_floor.to_string());
      add_field(fields, "revision", record->revision.to_string());
      break;
    }
    case EntityKind::Evidence: {
      Result<EvidenceId> typed = EvidenceId::parse(id.view());
      if (!typed.ok()) {
        return fields;
      }
      const EvidenceRecord* record = snapshot.find_evidence(typed.value());
      if (record == nullptr) {
        return fields;
      }
      add_field(fields, "actor", record->provenance.actor.str());
      add_field(fields, "kind", std::string(to_string(record->kind)));
      add_field(fields, "medium", record->medium.has_value()
                                      ? std::string(to_string(*record->medium))
                                      : std::string("-"));
      add_field(fields, "observed_at", record->observed_at.to_string());
      add_field(fields, "reference", record->provenance.reference.str());
      add_field(fields, "revision", record->revision.to_string());
      add_field(fields, "source", std::string(to_string(record->source)));
      add_field(fields, "source_generation",
                record->source_generation.has_value() ? record->source_generation->to_string()
                                                      : std::string("-"));
      add_field(fields, "state", std::string(to_string(record->state)));
      add_field(fields, "subject", std::string(subject_kind_name(record->subject)) + ":" +
                                       subject_text(record->subject));
      add_field(fields, "supersedes", optional_typed(record->provenance.supersedes));
      add_field(fields, "valid_until", optional_time(record->valid_until));
      add_field(fields, "value", value_text(record->value));
      add_field(fields, "value_kind", std::string(value_kind_name(record->value)));
      break;
    }
    case EntityKind::Commitment: {
      Result<CommitmentId> typed = CommitmentId::parse(id.view());
      if (!typed.ok()) {
        return fields;
      }
      const CommitmentRecord* record = snapshot.find_commitment(typed.value());
      if (record == nullptr) {
        return fields;
      }
      add_field(fields, "actor", record->actor.str());
      add_field(fields, "attempt", record->attempt.str());
      add_field(fields, "class", std::string(to_string(record->compatibility)));
      add_field(fields, "created_at", record->created_at.to_string());
      add_field(fields, "created_generation", record->created_generation.to_string());
      add_field(fields, "expires_at", optional_time(record->expires_at));
      add_field(fields, "medium", std::string(to_string(record->medium)));
      add_field(fields, "note", record->note.str());
      add_field(fields, "pinned_loop", optional_typed(record->pinned_loop));
      add_field(fields, "revision", record->revision.to_string());
      add_field(fields, "state", std::string(to_string(record->state)));
      add_field(fields, "supersedes", optional_typed(record->supersedes));
      add_field(fields, "thermal", record->thermal.to_string());
      add_field(fields, "zone", record->zone.str());
      break;
    }
    case EntityKind::Policy: {
      if (snapshot.policy().id.value() != id) {
        return fields;
      }
      add_field(fields, "revision", snapshot.policy().revision.to_string());
      break;
    }
  }
  std::sort(fields.begin(), fields.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
  return fields;
}

namespace {

struct RecordRef {
  EntityKind kind;
  Identifier id;
};

std::vector<RecordRef> all_records(const CoolingSnapshot& snapshot) {
  std::vector<RecordRef> refs;
  for (const SiteRecord& record : snapshot.sites()) {
    refs.push_back({EntityKind::Site, record.id.value()});
  }
  for (const FacilityRecord& record : snapshot.facilities()) {
    refs.push_back({EntityKind::Facility, record.id.value()});
  }
  for (const DomainRecord& record : snapshot.domains()) {
    refs.push_back({EntityKind::Domain, record.id.value()});
  }
  for (const ManifoldRecord& record : snapshot.manifolds()) {
    refs.push_back({EntityKind::Manifold, record.id.value()});
  }
  for (const EquipmentRecord& record : snapshot.equipment()) {
    refs.push_back({EntityKind::Equipment, record.id.value()});
  }
  for (const LoopRecord& record : snapshot.loops()) {
    refs.push_back({EntityKind::Loop, record.id.value()});
  }
  for (const PlantRecord& record : snapshot.plants()) {
    refs.push_back({EntityKind::Plant, record.id.value()});
  }
  for (const ZoneRecord& record : snapshot.zones()) {
    refs.push_back({EntityKind::Zone, record.id.value()});
  }
  for (const EvidenceRecord& record : snapshot.evidence()) {
    refs.push_back({EntityKind::Evidence, record.id.value()});
  }
  for (const CommitmentRecord& record : snapshot.commitments()) {
    refs.push_back({EntityKind::Commitment, record.id.value()});
  }
  return refs;
}

}  // namespace

GenerationDiff diff(const CoolingSnapshot& from, const CoolingSnapshot& to) {
  GenerationDiff result;
  result.from = from.generation();
  result.to = to.generation();
  result.from_digest = from.digest();
  result.to_digest = to.digest();
  result.identical = from.digest() == to.digest();

  std::map<std::pair<unsigned, std::string>, std::pair<const CoolingSnapshot*, const CoolingSnapshot*>>
      pairs;
  for (const RecordRef& ref : all_records(from)) {
    pairs[{static_cast<unsigned>(ref.kind), ref.id.str()}].first = &from;
  }
  for (const RecordRef& ref : all_records(to)) {
    pairs[{static_cast<unsigned>(ref.kind), ref.id.str()}].second = &to;
  }

  for (const auto& entry : pairs) {
    const EntityKind kind = static_cast<EntityKind>(entry.first.first);
    Result<Identifier> id = Identifier::parse(entry.first.second);
    if (!id.ok()) {
      continue;
    }
    const bool in_from = entry.second.first != nullptr;
    const bool in_to = entry.second.second != nullptr;
    RecordChange change;
    change.kind = kind;
    change.id = id.value();
    if (!in_from) {
      change.change = ChangeKind::Added;
    } else if (!in_to) {
      change.change = ChangeKind::Removed;
    } else {
      const Fields before = record_fields(from, kind, change.id);
      const Fields after = record_fields(to, kind, change.id);
      std::map<std::string, std::string> before_map(before.begin(), before.end());
      std::map<std::string, std::string> after_map(after.begin(), after.end());
      for (const auto& field : before_map) {
        const auto other = after_map.find(field.first);
        const std::string other_value = other == after_map.end() ? std::string("-") : other->second;
        if (other_value != field.second) {
          FieldDelta delta;
          delta.field = Identifier::literal(field.first);
          delta.before = field.second;
          delta.after = other_value;
          change.fields.push_back(std::move(delta));
        }
      }
      for (const auto& field : after_map) {
        if (before_map.find(field.first) == before_map.end()) {
          FieldDelta delta;
          delta.field = Identifier::literal(field.first);
          delta.before = "-";
          delta.after = field.second;
          change.fields.push_back(std::move(delta));
        }
      }
      std::sort(change.fields.begin(), change.fields.end(),
                [](const FieldDelta& lhs, const FieldDelta& rhs) {
                  return lhs.field < rhs.field;
                });
      if (change.fields.empty()) {
        continue;
      }
      change.change = ChangeKind::Modified;
    }
    switch (change.change) {
      case ChangeKind::Added:
        ++result.added;
        break;
      case ChangeKind::Removed:
        ++result.removed;
        break;
      case ChangeKind::Modified:
        ++result.modified;
        break;
    }
    result.changes.push_back(std::move(change));
  }

  std::sort(result.changes.begin(), result.changes.end(),
            [](const RecordChange& lhs, const RecordChange& rhs) {
              if (lhs.kind != rhs.kind) {
                return static_cast<unsigned>(lhs.kind) < static_cast<unsigned>(rhs.kind);
              }
              return lhs.id < rhs.id;
            });

  result.explanation.emplace_back(
      ExplanationCode::DiffSummary,
      std::vector<ExplanationParam>{
          param("from", from.generation().value()), param("to", to.generation().value()),
          param("changes", static_cast<std::uint64_t>(result.change_count())),
          param("added", static_cast<std::uint64_t>(result.added)),
          param("removed", static_cast<std::uint64_t>(result.removed)),
          param("modified", static_cast<std::uint64_t>(result.modified))});
  return result;
}

}  // namespace cooling_capacity
