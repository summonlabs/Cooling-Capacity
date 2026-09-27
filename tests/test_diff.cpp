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

#include "fixtures.hpp"
#include "test_harness.hpp"

using namespace ccap_test;

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {

using cooling_capacity::ChangeKind;
using cooling_capacity::CoolingSnapshot;
using cooling_capacity::EntityKind;
using cooling_capacity::ExplanationCode;
using cooling_capacity::ExplanationStep;
using cooling_capacity::FieldDelta;
using cooling_capacity::GenerationDiff;
using cooling_capacity::RecordChange;
using cooling_capacity::RedundancyClass;
using cooling_capacity::SnapshotBuilder;

SnapshotBuilder builder_for(std::uint64_t generation) {
  SnapshotBuilder builder;
  builder.set_generation(CapacityGeneration::from_value(generation));
  builder.set_constructed_at(fixture_now());
  return builder;
}

// Generation one: the simple facility.
std::shared_ptr<const CoolingSnapshot> first_generation() {
  SnapshotBuilder builder = builder_for(1);
  CCAP_CHECK_VOID(add_simple_facility(builder, simple_facility()));
  return build_snapshot(builder);
}

// Generation two: hall-2 is added, crah-b is removed and the air loop's
// transport limit and redundancy are both modified.
std::shared_ptr<const CoolingSnapshot> second_generation() {
  SimpleFacility facility = simple_facility();
  facility.loop.equipment = {facility.crah_a.id};
  facility.loop.transport_limit = watts(650000);
  facility.loop.redundancy = RedundancyClass::NPlusTwo;
  const ZoneRecord added = make_zone("hall-2", {facility.loop.id},
                                     {CompatibilityClass::AirConvection});

  SnapshotBuilder builder = builder_for(2);
  CCAP_CHECK_VOID(builder.add(facility.site));
  CCAP_CHECK_VOID(builder.add(facility.facility));
  CCAP_CHECK_VOID(builder.add(facility.crah_a));
  CCAP_CHECK_VOID(builder.add(facility.loop));
  CCAP_CHECK_VOID(builder.add(facility.zone));
  CCAP_CHECK_VOID(builder.add(added));
  return build_snapshot(builder);
}

const RecordChange* find_change(const GenerationDiff& diff, EntityKind kind, const char* id) {
  for (const RecordChange& change : diff.changes) {
    if (change.kind == kind && change.id.str() == id) {
      return &change;
    }
  }
  return nullptr;
}

bool is_ordered(const GenerationDiff& diff) {
  for (std::size_t index = 1; index < diff.changes.size(); ++index) {
    const RecordChange& previous = diff.changes[index - 1];
    const RecordChange& current = diff.changes[index];
    if (previous.kind == current.kind) {
      if (!(previous.id < current.id)) {
        return false;
      }
      continue;
    }
    if (!(static_cast<unsigned>(previous.kind) < static_cast<unsigned>(current.kind))) {
      return false;
    }
  }
  return true;
}

bool fields_are_ordered(const RecordChange& change) {
  for (std::size_t index = 1; index < change.fields.size(); ++index) {
    if (!(change.fields[index - 1].field < change.fields[index].field)) {
      return false;
    }
  }
  return true;
}

}  // namespace

CCAP_TEST(a_diff_counts_added_removed_and_modified_records) {
  const std::shared_ptr<const CoolingSnapshot> before = first_generation();
  const std::shared_ptr<const CoolingSnapshot> after = second_generation();
  const GenerationDiff diff = cooling_capacity::diff(*before, *after);

  CCAP_CHECK_EQ(diff.from, CapacityGeneration::from_value(1));
  CCAP_CHECK_EQ(diff.to, CapacityGeneration::from_value(2));
  CCAP_CHECK_EQ(diff.from_digest, before->digest());
  CCAP_CHECK_EQ(diff.to_digest, after->digest());
  CCAP_CHECK_FALSE(diff.identical);

  // Exactly one zone added, one equipment unit removed, one loop modified.
  CCAP_CHECK_EQ(diff.added, 1U);
  CCAP_CHECK_EQ(diff.removed, 1U);
  CCAP_CHECK_EQ(diff.modified, 1U);
  CCAP_CHECK_EQ(diff.change_count(), 3U);
  CCAP_CHECK_EQ(diff.changes.size(), 3U);

  const RecordChange* added = find_change(diff, EntityKind::Zone, "hall-2");
  CCAP_CHECK(added != nullptr);
  if (added != nullptr) {
    CCAP_CHECK_EQ(added->change, ChangeKind::Added);
    // An added record has no field deltas: there is nothing to compare against.
    CCAP_CHECK_EQ(added->fields.size(), 0U);
    // "<entity kind> <id>: <change kind>".
    CCAP_CHECK_EQ(added->to_string(), std::string("zone hall-2: added"));
  }

  const RecordChange* removed = find_change(diff, EntityKind::Equipment, "crah-b");
  CCAP_CHECK(removed != nullptr);
  if (removed != nullptr) {
    CCAP_CHECK_EQ(removed->change, ChangeKind::Removed);
    CCAP_CHECK_EQ(removed->fields.size(), 0U);
  }

  const RecordChange* modified = find_change(diff, EntityKind::Loop, "air-loop-1");
  CCAP_CHECK(modified != nullptr);
  if (modified != nullptr) {
    CCAP_CHECK_EQ(modified->change, ChangeKind::Modified);
    CCAP_CHECK_EQ(modified->fields.size(), 3U);
    CCAP_CHECK(fields_are_ordered(*modified));
    bool saw_transport = false;
    bool saw_equipment = false;
    bool saw_redundancy = false;
    for (const FieldDelta& delta : modified->fields) {
      if (delta.field.str() == "transport_limit") {
        saw_transport = true;
        CCAP_CHECK_EQ(delta.before, watts(700000).to_string());
        CCAP_CHECK_EQ(delta.after, watts(650000).to_string());
      } else if (delta.field.str() == "equipment") {
        saw_equipment = true;
        CCAP_CHECK_EQ(delta.before, std::string("crah-a,crah-b"));
        CCAP_CHECK_EQ(delta.after, std::string("crah-a"));
      } else if (delta.field.str() == "redundancy") {
        saw_redundancy = true;
        CCAP_CHECK_EQ(delta.before, std::string("n+1"));
        CCAP_CHECK_EQ(delta.after, std::string("n+2"));
      }
    }
    CCAP_CHECK(saw_transport);
    CCAP_CHECK(saw_equipment);
    CCAP_CHECK(saw_redundancy);
  }

  CCAP_CHECK_EQ(cooling_capacity::to_string(ChangeKind::Added), std::string_view("added"));
  CCAP_CHECK_EQ(cooling_capacity::to_string(ChangeKind::Removed), std::string_view("removed"));
  CCAP_CHECK_EQ(cooling_capacity::to_string(ChangeKind::Modified), std::string_view("modified"));
}

CCAP_TEST(a_diff_of_a_snapshot_with_itself_is_empty_and_identical) {
  const std::shared_ptr<const CoolingSnapshot> snapshot = first_generation();
  const GenerationDiff diff = cooling_capacity::diff(*snapshot, *snapshot);

  CCAP_CHECK(diff.identical);
  CCAP_CHECK_EQ(diff.added, 0U);
  CCAP_CHECK_EQ(diff.removed, 0U);
  CCAP_CHECK_EQ(diff.modified, 0U);
  CCAP_CHECK_EQ(diff.change_count(), 0U);
  CCAP_CHECK_EQ(diff.changes.size(), 0U);
  CCAP_CHECK_EQ(diff.from, diff.to);
  CCAP_CHECK_EQ(diff.from_digest, diff.to_digest);

  // The summary is still emitted, and says that nothing changed.
  CCAP_CHECK_EQ(diff.explanation.size(), 1U);
  CCAP_CHECK_EQ(diff.explanation.front().code(), ExplanationCode::DiffSummary);
  const std::string rendered = cooling_capacity::render_steps(diff.explanation);
  CCAP_CHECK(rendered.find("generation 1 to 1 changed 0 records") != std::string::npos);
  CCAP_CHECK(rendered.find("<missing:") == std::string::npos);
  CCAP_CHECK(rendered.find("0 added, 0 removed, 0 modified") != std::string::npos);
}

CCAP_TEST(diff_changes_are_ordered_by_kind_and_identifier) {
  const std::shared_ptr<const CoolingSnapshot> before = first_generation();
  const std::shared_ptr<const CoolingSnapshot> after = second_generation();
  const GenerationDiff diff = cooling_capacity::diff(*before, *after);

  CCAP_CHECK(is_ordered(diff));

  // EntityKind orders zone before loop before equipment, which is the order the
  // diff must present.
  CCAP_CHECK_EQ(diff.changes.size(), 3U);
  if (diff.changes.size() == 3U) {
    CCAP_CHECK_EQ(diff.changes[0].kind, EntityKind::Zone);
    CCAP_CHECK_EQ(diff.changes[0].id.str(), std::string("hall-2"));
    CCAP_CHECK_EQ(diff.changes[1].kind, EntityKind::Loop);
    CCAP_CHECK_EQ(diff.changes[1].id.str(), std::string("air-loop-1"));
    CCAP_CHECK_EQ(diff.changes[2].kind, EntityKind::Equipment);
    CCAP_CHECK_EQ(diff.changes[2].id.str(), std::string("crah-b"));
  }

  for (const RecordChange& change : diff.changes) {
    CCAP_CHECK(fields_are_ordered(change));
  }

  // The order does not depend on the direction the two snapshots were built in:
  // the reverse diff is the mirror image, not a different ordering.
  const GenerationDiff reversed = cooling_capacity::diff(*after, *before);
  CCAP_CHECK(is_ordered(reversed));
  CCAP_CHECK_EQ(reversed.added, 1U);
  CCAP_CHECK_EQ(reversed.removed, 1U);
  CCAP_CHECK_EQ(reversed.modified, 1U);
  CCAP_CHECK_EQ(reversed.changes.size(), diff.changes.size());
  for (std::size_t index = 0; index < diff.changes.size(); ++index) {
    CCAP_CHECK_EQ(reversed.changes[index].kind, diff.changes[index].kind);
    CCAP_CHECK_EQ(reversed.changes[index].id.str(), diff.changes[index].id.str());
  }
  if (reversed.changes.size() == 3U) {
    CCAP_CHECK_EQ(reversed.changes[0].change, ChangeKind::Removed);
    CCAP_CHECK_EQ(reversed.changes[2].change, ChangeKind::Added);
  }
}

CCAP_TEST(the_diff_explanation_is_the_machine_readable_summary) {
  const std::shared_ptr<const CoolingSnapshot> before = first_generation();
  const std::shared_ptr<const CoolingSnapshot> after = second_generation();
  const GenerationDiff diff = cooling_capacity::diff(*before, *after);

  CCAP_CHECK_EQ(diff.explanation.size(), 1U);
  const ExplanationStep& summary = diff.explanation.front();
  CCAP_CHECK_EQ(summary.code(), ExplanationCode::DiffSummary);
  CCAP_CHECK_EQ(cooling_capacity::to_string(summary.code()), std::string_view("diff-summary"));
  CCAP_CHECK(summary.has("from"));
  CCAP_CHECK(summary.has("to"));
  CCAP_CHECK(summary.has("changes"));
  CCAP_CHECK(summary.has("added"));
  CCAP_CHECK(summary.has("removed"));
  CCAP_CHECK(summary.has("modified"));

  const std::string rendered = cooling_capacity::render_steps(diff.explanation);
  CCAP_CHECK(rendered.find("diff-summary:") != std::string::npos);
  CCAP_CHECK(rendered.find("generation 1 to 2 changed 3 records") != std::string::npos);
  CCAP_CHECK(rendered.find("1 added, 1 removed, 1 modified") != std::string::npos);
  CCAP_CHECK(rendered.find("<missing:") == std::string::npos);
}

CCAP_TEST(record_fields_are_canonical_and_sorted) {
  const std::shared_ptr<const CoolingSnapshot> snapshot = first_generation();

  const std::vector<std::pair<std::string, std::string>> loop_fields =
      cooling_capacity::record_fields(*snapshot, EntityKind::Loop, identifier("air-loop-1"));
  CCAP_CHECK_FALSE(loop_fields.empty());
  CCAP_CHECK(std::is_sorted(loop_fields.begin(), loop_fields.end(),
                            [](const auto& lhs, const auto& rhs) {
                              return lhs.first < rhs.first;
                            }));
  bool saw_equipment = false;
  for (const auto& field : loop_fields) {
    if (field.first == "equipment") {
      saw_equipment = true;
      CCAP_CHECK_EQ(field.second, std::string("crah-a,crah-b"));
    }
  }
  CCAP_CHECK(saw_equipment);

  const std::vector<std::pair<std::string, std::string>> zone_fields =
      cooling_capacity::record_fields(*snapshot, EntityKind::Zone, identifier("hall-1"));
  bool saw_loops = false;
  bool saw_media = false;
  for (const auto& field : zone_fields) {
    if (field.first == "loops") {
      saw_loops = true;
      CCAP_CHECK_EQ(field.second, std::string("air-loop-1"));
    }
    if (field.first == "media") {
      saw_media = true;
      CCAP_CHECK_EQ(field.second, std::string("air"));
    }
  }
  CCAP_CHECK(saw_loops);
  CCAP_CHECK(saw_media);

  // The same identifier in the wrong family yields nothing rather than another
  // family's record.
  CCAP_CHECK_EQ(cooling_capacity::record_fields(*snapshot, EntityKind::Loop,
                                                identifier("hall-1")).size(),
                0U);
  CCAP_CHECK_EQ(cooling_capacity::record_fields(*snapshot, EntityKind::Zone,
                                                identifier("absent")).size(),
                0U);

  const std::vector<std::pair<std::string, std::string>> policy_fields =
      cooling_capacity::record_fields(*snapshot, EntityKind::Policy, identifier("default-policy"));
  CCAP_CHECK_EQ(policy_fields.size(), 1U);
  CCAP_CHECK_EQ(cooling_capacity::record_fields(*snapshot, EntityKind::Policy,
                                                identifier("other-policy")).size(),
                0U);
}