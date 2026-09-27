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

#include "cooling_capacity/ids.hpp"

namespace cooling_capacity {

namespace {

struct EntityKindName {
  EntityKind kind;
  std::string_view name;
};

constexpr EntityKindName kEntityKindNames[] = {
    {EntityKind::Site, "site"},           {EntityKind::Facility, "facility"},
    {EntityKind::Zone, "zone"},           {EntityKind::Loop, "loop"},
    {EntityKind::Plant, "plant"},         {EntityKind::Equipment, "equipment"},
    {EntityKind::Manifold, "manifold"},   {EntityKind::Domain, "domain"},
    {EntityKind::Policy, "policy"},       {EntityKind::Evidence, "evidence"},
    {EntityKind::Commitment, "commitment"},
};

}  // namespace

std::string_view to_string(EntityKind kind) noexcept {
  for (const EntityKindName& entry : kEntityKindNames) {
    if (entry.kind == kind) {
      return entry.name;
    }
  }
  return "unrecognised_entity_kind";
}

Result<EntityKind> parse_entity_kind(std::string_view text) {
  for (const EntityKindName& entry : kEntityKindNames) {
    if (entry.name == text) {
      return entry.kind;
    }
  }
  return Error(ErrorCode::InvalidEnumValue, "unknown entity kind").with("text", std::string(text));
}

}  // namespace cooling_capacity
