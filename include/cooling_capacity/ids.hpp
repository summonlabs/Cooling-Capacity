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

#ifndef COOLING_CAPACITY_IDS_HPP
#define COOLING_CAPACITY_IDS_HPP

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>

#include "cooling_capacity/errors.hpp"
#include "cooling_capacity/export.hpp"
#include "cooling_capacity/text.hpp"

namespace cooling_capacity {

// Identity tags. Each tag is a distinct empty type, so two identifiers from
// different families are different types with no conversion between them.
struct SiteIdTag {};
struct FacilityIdTag {};
struct ZoneIdTag {};
struct LoopIdTag {};
struct PlantIdTag {};
struct EquipmentIdTag {};
struct ManifoldIdTag {};
struct DomainIdTag {};
struct EvidenceIdTag {};
struct PolicyIdTag {};
struct CommitmentIdTag {};
struct AttemptIdTag {};
struct ActorIdTag {};

// A typed identifier. The representation is the validated identifier grammar
// from text.hpp, so an identifier can never carry a path separator, whitespace
// or a quoting character.
template <class Tag>
class BasicId {
 public:
  BasicId() = default;

  [[nodiscard]] static Result<BasicId> parse(std::string_view text) {
    CCAP_TRY_DECLARE(identifier, Identifier::parse(text));
    BasicId id;
    id.value_ = std::move(identifier);
    return id;
  }

  // For identifiers that appear as literals in source. Violations terminate.
  [[nodiscard]] static BasicId literal(std::string_view text) {
    BasicId id;
    id.value_ = Identifier::literal(text);
    return id;
  }

  // Wraps an already validated identifier. Used by the canonical decoder, which
  // has parsed the token once and must not parse it a second time.
  [[nodiscard]] static BasicId from_identifier(const Identifier& identifier) {
    BasicId id;
    id.value_ = identifier;
    return id;
  }

  [[nodiscard]] const Identifier& value() const noexcept { return value_; }
  [[nodiscard]] const std::string& str() const noexcept { return value_.str(); }
  [[nodiscard]] std::string_view view() const noexcept { return value_.view(); }
  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }
  [[nodiscard]] std::string to_string() const { return value_.to_string(); }

  friend bool operator==(const BasicId& lhs, const BasicId& rhs) { return lhs.value_ == rhs.value_; }
  friend bool operator!=(const BasicId& lhs, const BasicId& rhs) { return !(lhs == rhs); }
  friend bool operator<(const BasicId& lhs, const BasicId& rhs) { return lhs.value_ < rhs.value_; }
  friend bool operator>(const BasicId& lhs, const BasicId& rhs) { return rhs < lhs; }
  friend bool operator<=(const BasicId& lhs, const BasicId& rhs) { return !(rhs < lhs); }
  friend bool operator>=(const BasicId& lhs, const BasicId& rhs) { return !(lhs < rhs); }

 private:
  Identifier value_;
};

using SiteId = BasicId<SiteIdTag>;
using FacilityId = BasicId<FacilityIdTag>;
using ZoneId = BasicId<ZoneIdTag>;
using LoopId = BasicId<LoopIdTag>;
using PlantId = BasicId<PlantIdTag>;
using EquipmentId = BasicId<EquipmentIdTag>;
using ManifoldId = BasicId<ManifoldIdTag>;
using DomainId = BasicId<DomainIdTag>;
using EvidenceId = BasicId<EvidenceIdTag>;
using PolicyId = BasicId<PolicyIdTag>;
using CommitmentId = BasicId<CommitmentIdTag>;
using AttemptId = BasicId<AttemptIdTag>;
using ActorId = BasicId<ActorIdTag>;

// Monotone counters. Each family is a distinct type; adding a revision to a
// generation does not compile, and a stale comparison has to be written out
// explicitly by the caller.
struct CapacityGenerationTag {};
struct WriterEpochTag {};
struct RecordRevisionTag {};
struct SourceGenerationTag {};
struct PolicyRevisionTag {};
struct StoreIncarnationTag {};

template <class Tag>
class Counter {
 public:
  using rep = std::uint64_t;

  constexpr Counter() = default;
  [[nodiscard]] static constexpr Counter from_value(rep value) noexcept { return Counter(value); }
  [[nodiscard]] static constexpr Counter first() noexcept { return Counter(1); }

  [[nodiscard]] constexpr rep value() const noexcept { return value_; }

  // Checked successor. Overflow is reported rather than wrapped.
  [[nodiscard]] Result<Counter> next() const noexcept {
    if (value_ == std::numeric_limits<rep>::max()) {
      return Error(ErrorCode::ArithmeticOverflow, "counter is exhausted");
    }
    return Counter(value_ + 1U);
  }

  [[nodiscard]] std::string to_string() const { return std::to_string(value_); }

  friend constexpr bool operator==(Counter lhs, Counter rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend constexpr bool operator!=(Counter lhs, Counter rhs) noexcept { return !(lhs == rhs); }
  friend constexpr bool operator<(Counter lhs, Counter rhs) noexcept {
    return lhs.value_ < rhs.value_;
  }
  friend constexpr bool operator>(Counter lhs, Counter rhs) noexcept { return rhs < lhs; }
  friend constexpr bool operator<=(Counter lhs, Counter rhs) noexcept { return !(rhs < lhs); }
  friend constexpr bool operator>=(Counter lhs, Counter rhs) noexcept { return !(lhs < rhs); }

 private:
  constexpr explicit Counter(rep value) noexcept : value_(value) {}
  rep value_ = 0;
};

using CapacityGeneration = Counter<CapacityGenerationTag>;
using WriterEpoch = Counter<WriterEpochTag>;
using RecordRevision = Counter<RecordRevisionTag>;
using SourceGeneration = Counter<SourceGenerationTag>;
using PolicyRevision = Counter<PolicyRevisionTag>;
using StoreIncarnation = Counter<StoreIncarnationTag>;

// The kinds of record a snapshot can hold. Used by diffs, validation findings
// and the command line tool. Numeric values are part of the serialized format.
enum class EntityKind : std::uint8_t {
  Site = 0,
  Facility = 1,
  Zone = 2,
  Loop = 3,
  Plant = 4,
  Equipment = 5,
  Manifold = 6,
  Domain = 7,
  Policy = 8,
  Evidence = 9,
  Commitment = 10,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(EntityKind kind) noexcept;
[[nodiscard]] CCAP_EXPORT Result<EntityKind> parse_entity_kind(std::string_view text);

}  // namespace cooling_capacity

namespace std {

template <class Tag>
struct hash<cooling_capacity::BasicId<Tag>> {
  std::size_t operator()(const cooling_capacity::BasicId<Tag>& id) const noexcept {
    return std::hash<std::string_view>{}(id.view());
  }
};

template <class Tag>
struct hash<cooling_capacity::Counter<Tag>> {
  std::size_t operator()(const cooling_capacity::Counter<Tag>& counter) const noexcept {
    return std::hash<std::uint64_t>{}(counter.value());
  }
};

}  // namespace std

#endif  // COOLING_CAPACITY_IDS_HPP
