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

#include "cooling_capacity/canonical.hpp"

#include <cstring>
#include <limits>
#include <string>

namespace cooling_capacity {

namespace {

// Magic string and byte-order marker. The marker is written as a little-endian
// 32-bit constant; a reader that decodes it as 0x04030201 knows the writer used
// the other byte order and refuses the artifact explicitly.
constexpr char kArtifactMagic[8] = {'C', 'C', 'A', 'P', 'G', 'E', 'N', '1'};
constexpr std::uint32_t kEndianMarker = 0x01020304U;
constexpr std::uint8_t kMetadataVersion = 1;
constexpr std::uint8_t kPresent = 1;
constexpr std::uint8_t kAbsent = 0;
constexpr std::size_t kMinimumRecordBytes = 4;

enum class SubjectTag : std::uint8_t {
  Site = 1,
  Facility = 2,
  Zone = 3,
  Loop = 4,
  Plant = 5,
  Equipment = 6,
  Manifold = 7,
  Domain = 8,
};

enum class ValueTag : std::uint8_t {
  Declaration = 0,
  ThermalPower = 1,
  Fraction = 2,
  Temperature = 3,
  VolumetricFlow = 4,
  OperatingState = 5,
  RedundancyClass = 6,
  MediumMask = 7,
};

// ---------------------------------------------------------------------------
// Writer helpers
// ---------------------------------------------------------------------------

void put_u8(std::string& out, std::uint8_t value) { out.push_back(static_cast<char>(value)); }

void put_u32(std::string& out, std::uint32_t value) {
  for (unsigned index = 0; index < 4U; ++index) {
    out.push_back(static_cast<char>((value >> (8U * index)) & 0xFFU));
  }
}

void put_u64(std::string& out, std::uint64_t value) {
  for (unsigned index = 0; index < 8U; ++index) {
    out.push_back(static_cast<char>((value >> (8U * index)) & 0xFFU));
  }
}

void put_i64(std::string& out, std::int64_t value) {
  put_u64(out, static_cast<std::uint64_t>(value));
}

void put_bool(std::string& out, bool value) { put_u8(out, value ? 1U : 0U); }

void put_text(std::string& out, std::string_view text) {
  put_u32(out, static_cast<std::uint32_t>(text.size()));
  out.append(text);
}

template <class Id>
void put_typed_id(std::string& out, const Id& id) {
  put_text(out, id.view());
}

template <class Id>
void put_optional_id(std::string& out, const std::optional<Id>& id) {
  if (id.has_value()) {
    put_u8(out, kPresent);
    put_typed_id(out, *id);
  } else {
    put_u8(out, kAbsent);
  }
}

void put_optional_time(std::string& out, const std::optional<Timestamp>& instant) {
  if (instant.has_value()) {
    put_u8(out, kPresent);
    put_i64(out, instant->unix_milliseconds());
  } else {
    put_u8(out, kAbsent);
  }
}

void put_optional_power(std::string& out, const std::optional<ThermalPower>& power) {
  if (power.has_value()) {
    put_u8(out, kPresent);
    put_i64(out, power->milliwatts());
  } else {
    put_u8(out, kAbsent);
  }
}

void put_optional_generation(std::string& out, const std::optional<SourceGeneration>& generation) {
  if (generation.has_value()) {
    put_u8(out, kPresent);
    put_u64(out, generation->value());
  } else {
    put_u8(out, kAbsent);
  }
}

template <class Enum>
void put_enum(std::string& out, Enum value) {
  put_u8(out, static_cast<std::uint8_t>(value));
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------

class Reader {
 public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {}

  [[nodiscard]] std::size_t remaining() const { return bytes_.size() - offset_; }
  [[nodiscard]] std::size_t offset() const { return offset_; }

  Result<std::uint8_t> u8() {
    if (remaining() < 1U) {
      return truncated();
    }
    return static_cast<std::uint8_t>(bytes_[offset_++]);
  }

  Result<std::uint32_t> u32() {
    if (remaining() < 4U) {
      return truncated();
    }
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4U; ++index) {
      value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_[offset_ + index]))
               << (8U * index);
    }
    offset_ += 4U;
    return value;
  }

  Result<std::uint64_t> u64() {
    if (remaining() < 8U) {
      return truncated();
    }
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8U; ++index) {
      value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes_[offset_ + index]))
               << (8U * index);
    }
    offset_ += 8U;
    return value;
  }

  Result<std::int64_t> i64() {
    Result<std::uint64_t> raw = u64();
    if (!raw.ok()) {
      return raw.error();
    }
    return static_cast<std::int64_t>(raw.value());
  }

  Result<bool> flag() {
    Result<std::uint8_t> raw = u8();
    if (!raw.ok()) {
      return raw.error();
    }
    if (raw.value() > 1U) {
      return Error(ErrorCode::Corruption, "a boolean field holds a value other than zero or one")
          .with("value", std::to_string(raw.value()));
    }
    return raw.value() == 1U;
  }

  Result<std::string> text(std::size_t max_length) {
    Result<std::uint32_t> length = u32();
    if (!length.ok()) {
      return length.error();
    }
    const auto declared = static_cast<std::size_t>(length.value());
    if (declared > max_length) {
      return Error(ErrorCode::Oversized, "a declared text length exceeds the permitted maximum")
          .with("length", std::to_string(declared))
          .with("maximum", std::to_string(max_length));
    }
    if (remaining() < declared) {
      return truncated();
    }
    std::string value(bytes_.substr(offset_, declared));
    offset_ += declared;
    return value;
  }

  Result<Identifier> identifier() {
    Result<std::string> token = text(kMaxIdentifierLength);
    if (!token.ok()) {
      return token.error();
    }
    return Identifier::parse(token.value());
  }

  template <class Id>
  Result<Id> typed_id() {
    Result<Identifier> token = identifier();
    if (!token.ok()) {
      return token.error();
    }
    return Id::from_identifier(token.value());
  }

  template <class Id>
  Result<std::optional<Id>> optional_id() {
    Result<bool> present = flag();
    if (!present.ok()) {
      return present.error();
    }
    if (!present.value()) {
      return std::optional<Id>();
    }
    Result<Id> id = typed_id<Id>();
    if (!id.ok()) {
      return id.error();
    }
    return std::optional<Id>(id.value());
  }

  Result<std::optional<Timestamp>> optional_time() {
    Result<bool> present = flag();
    if (!present.ok()) {
      return present.error();
    }
    if (!present.value()) {
      return std::optional<Timestamp>();
    }
    Result<std::int64_t> raw = i64();
    if (!raw.ok()) {
      return raw.error();
    }
    return std::optional<Timestamp>(Timestamp::from_unix_milliseconds(raw.value()));
  }

  Result<std::optional<ThermalPower>> optional_power() {
    Result<bool> present = flag();
    if (!present.ok()) {
      return present.error();
    }
    if (!present.value()) {
      return std::optional<ThermalPower>();
    }
    Result<std::int64_t> raw = i64();
    if (!raw.ok()) {
      return raw.error();
    }
    return std::optional<ThermalPower>(ThermalPower::from_milliwatts(raw.value()));
  }

  Result<std::optional<SourceGeneration>> optional_generation() {
    Result<bool> present = flag();
    if (!present.ok()) {
      return present.error();
    }
    if (!present.value()) {
      return std::optional<SourceGeneration>();
    }
    Result<std::uint64_t> raw = u64();
    if (!raw.ok()) {
      return raw.error();
    }
    return std::optional<SourceGeneration>(SourceGeneration::from_value(raw.value()));
  }

  // Reads a count and checks it against both a configured maximum and the bytes
  // that remain, so a hostile count cannot drive an allocation.
  Result<std::size_t> count(std::size_t maximum, std::size_t minimum_bytes_per_item) {
    Result<std::uint32_t> raw = u32();
    if (!raw.ok()) {
      return raw.error();
    }
    const auto value = static_cast<std::size_t>(raw.value());
    if (value > maximum) {
      return Error(ErrorCode::LimitExceeded, "a declared collection size exceeds the limit")
          .with("count", std::to_string(value))
          .with("maximum", std::to_string(maximum));
    }
    if (minimum_bytes_per_item != 0U && value > remaining() / minimum_bytes_per_item) {
      return Error(ErrorCode::Truncated,
                   "a declared collection size does not fit in the remaining bytes")
          .with("count", std::to_string(value))
          .with("remaining", std::to_string(remaining()));
    }
    return value;
  }

  [[nodiscard]] Result<void> expect_end() const {
    if (remaining() != 0U) {
      return Error(ErrorCode::Corruption, "the artifact has trailing bytes after the payload")
          .with("trailing", std::to_string(remaining()));
    }
    return Result<void>();
  }

 private:
  [[nodiscard]] Error truncated() const {
    return Error(ErrorCode::Truncated, "the artifact ended before the declared structure completed")
        .with("offset", std::to_string(offset_))
        .with("size", std::to_string(bytes_.size()));
  }

  std::string_view bytes_;
  std::size_t offset_ = 0;
};

Result<void> require_tag(Result<std::uint8_t> raw, std::uint8_t maximum, const char* what) {
  if (!raw.ok()) {
    return raw.error();
  }
  if (raw.value() > maximum) {
    return Error(ErrorCode::InvalidEnumValue, std::string("unknown ") + what + " tag")
        .with("tag", std::to_string(raw.value()));
  }
  return Result<void>();
}

template <class Enum>
Result<Enum> read_enum(Reader& reader, std::uint8_t maximum, const char* what) {
  Result<std::uint8_t> raw = reader.u8();
  if (!raw.ok()) {
    return raw.error();
  }
  if (raw.value() > maximum) {
    return Error(ErrorCode::InvalidEnumValue, std::string("unknown ") + what + " value")
        .with("value", std::to_string(raw.value()));
  }
  return static_cast<Enum>(raw.value());
}

Result<BoundedText> read_label(Reader& reader) {
  Result<std::string> text = reader.text(kMaxTextLength);
  if (!text.ok()) {
    return text.error();
  }
  return BoundedText::parse(text.value());
}

// ---------------------------------------------------------------------------
// Evidence variants
// ---------------------------------------------------------------------------

void encode_evidence_subject(std::string& out, const EvidenceSubject& subject) {
  std::visit(
      [&out](const auto& id) {
        using IdType = std::decay_t<decltype(id)>;
        if constexpr (std::is_same_v<IdType, SiteId>) {
          put_u8(out, static_cast<std::uint8_t>(SubjectTag::Site));
        } else if constexpr (std::is_same_v<IdType, FacilityId>) {
          put_u8(out, static_cast<std::uint8_t>(SubjectTag::Facility));
        } else if constexpr (std::is_same_v<IdType, ZoneId>) {
          put_u8(out, static_cast<std::uint8_t>(SubjectTag::Zone));
        } else if constexpr (std::is_same_v<IdType, LoopId>) {
          put_u8(out, static_cast<std::uint8_t>(SubjectTag::Loop));
        } else if constexpr (std::is_same_v<IdType, PlantId>) {
          put_u8(out, static_cast<std::uint8_t>(SubjectTag::Plant));
        } else if constexpr (std::is_same_v<IdType, EquipmentId>) {
          put_u8(out, static_cast<std::uint8_t>(SubjectTag::Equipment));
        } else if constexpr (std::is_same_v<IdType, ManifoldId>) {
          put_u8(out, static_cast<std::uint8_t>(SubjectTag::Manifold));
        } else {
          put_u8(out, static_cast<std::uint8_t>(SubjectTag::Domain));
        }
        put_text(out, id.view());
      },
      subject);
}

Result<EvidenceSubject> decode_evidence_subject(Reader& reader) {
  Result<std::uint8_t> tag = reader.u8();
  CCAP_TRY(require_tag(tag, static_cast<std::uint8_t>(SubjectTag::Domain), "evidence subject"));
  Result<Identifier> token = reader.identifier();
  if (!token.ok()) {
    return token.error();
  }
  switch (static_cast<SubjectTag>(tag.value())) {
    case SubjectTag::Site:
      return EvidenceSubject{SiteId::from_identifier(token.value())};
    case SubjectTag::Facility:
      return EvidenceSubject{FacilityId::from_identifier(token.value())};
    case SubjectTag::Zone:
      return EvidenceSubject{ZoneId::from_identifier(token.value())};
    case SubjectTag::Loop:
      return EvidenceSubject{LoopId::from_identifier(token.value())};
    case SubjectTag::Plant:
      return EvidenceSubject{PlantId::from_identifier(token.value())};
    case SubjectTag::Equipment:
      return EvidenceSubject{EquipmentId::from_identifier(token.value())};
    case SubjectTag::Manifold:
      return EvidenceSubject{ManifoldId::from_identifier(token.value())};
    case SubjectTag::Domain:
      return EvidenceSubject{DomainId::from_identifier(token.value())};
  }
  return Error(ErrorCode::InvariantViolation, "unreachable evidence subject tag");
}

void encode_evidence_value(std::string& out, const EvidenceValue& value) {
  std::visit(
      [&out](const auto& item) {
        using ItemType = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<ItemType, std::monostate>) {
          put_u8(out, static_cast<std::uint8_t>(ValueTag::Declaration));
        } else if constexpr (std::is_same_v<ItemType, ThermalPower>) {
          put_u8(out, static_cast<std::uint8_t>(ValueTag::ThermalPower));
          put_i64(out, item.milliwatts());
        } else if constexpr (std::is_same_v<ItemType, Fraction>) {
          put_u8(out, static_cast<std::uint8_t>(ValueTag::Fraction));
          put_i64(out, item.parts_per_million());
        } else if constexpr (std::is_same_v<ItemType, Temperature>) {
          put_u8(out, static_cast<std::uint8_t>(ValueTag::Temperature));
          put_i64(out, item.millidegrees_celsius());
        } else if constexpr (std::is_same_v<ItemType, VolumetricFlow>) {
          put_u8(out, static_cast<std::uint8_t>(ValueTag::VolumetricFlow));
          put_i64(out, item.millilitres_per_second());
        } else if constexpr (std::is_same_v<ItemType, OperatingState>) {
          put_u8(out, static_cast<std::uint8_t>(ValueTag::OperatingState));
          put_u8(out, static_cast<std::uint8_t>(item));
        } else if constexpr (std::is_same_v<ItemType, RedundancyClass>) {
          put_u8(out, static_cast<std::uint8_t>(ValueTag::RedundancyClass));
          put_u8(out, static_cast<std::uint8_t>(item));
        } else {
          put_u8(out, static_cast<std::uint8_t>(ValueTag::MediumMask));
          put_u8(out, item.bits());
        }
      },
      value);
}

Result<EvidenceValue> decode_evidence_value(Reader& reader) {
  Result<std::uint8_t> tag = reader.u8();
  CCAP_TRY(require_tag(tag, static_cast<std::uint8_t>(ValueTag::MediumMask), "evidence value"));
  switch (static_cast<ValueTag>(tag.value())) {
    case ValueTag::Declaration:
      return EvidenceValue{std::monostate{}};
    case ValueTag::ThermalPower: {
      Result<std::int64_t> raw = reader.i64();
      if (!raw.ok()) {
        return raw.error();
      }
      return EvidenceValue{ThermalPower::from_milliwatts(raw.value())};
    }
    case ValueTag::Fraction: {
      Result<std::int64_t> raw = reader.i64();
      if (!raw.ok()) {
        return raw.error();
      }
      if (raw.value() < 0 || raw.value() > Fraction::kScale) {
        return Error(ErrorCode::Corruption, "a fraction in evidence is outside [0, 1]")
            .with("parts_per_million", std::to_string(raw.value()));
      }
      return EvidenceValue{Fraction::from_parts_per_million(raw.value())};
    }
    case ValueTag::Temperature: {
      Result<std::int64_t> raw = reader.i64();
      if (!raw.ok()) {
        return raw.error();
      }
      return EvidenceValue{Temperature::from_millidegrees_celsius(raw.value())};
    }
    case ValueTag::VolumetricFlow: {
      Result<std::int64_t> raw = reader.i64();
      if (!raw.ok()) {
        return raw.error();
      }
      return EvidenceValue{VolumetricFlow::from_millilitres_per_second(raw.value())};
    }
    case ValueTag::OperatingState: {
      Result<OperatingState> state = read_enum<OperatingState>(
          reader, static_cast<std::uint8_t>(OperatingState::Unknown), "operating state");
      if (!state.ok()) {
        return state.error();
      }
      return EvidenceValue{state.value()};
    }
    case ValueTag::RedundancyClass: {
      Result<RedundancyClass> value = read_enum<RedundancyClass>(
          reader, static_cast<std::uint8_t>(RedundancyClass::TwoNPlusTwo), "redundancy class");
      if (!value.ok()) {
        return value.error();
      }
      return EvidenceValue{value.value()};
    }
    case ValueTag::MediumMask: {
      Result<std::uint8_t> raw = reader.u8();
      if (!raw.ok()) {
        return raw.error();
      }
      if (raw.value() > 3U) {
        return Error(ErrorCode::Corruption, "a medium mask has bits outside the defined media")
            .with("bits", std::to_string(raw.value()));
      }
      MediumMask mask;
      if ((raw.value() & 1U) != 0U) {
        mask.insert(CoolingMedium::Air);
      }
      if ((raw.value() & 2U) != 0U) {
        mask.insert(CoolingMedium::Liquid);
      }
      return EvidenceValue{mask};
    }
  }
  return Error(ErrorCode::InvariantViolation, "unreachable evidence value tag");
}

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

void encode_policy(std::string& out, const CoolingPolicy& policy) {
  put_text(out, policy.id.view());
  put_u64(out, policy.revision.value());
  put_u32(out, static_cast<std::uint32_t>(policy.freshness.size()));
  for (const FreshnessRule& rule : policy.freshness) {
    put_enum(out, rule.kind);
    put_i64(out, rule.max_age.milliseconds());
  }
  put_enum(out, policy.stale_action);
  put_enum(out, policy.unknown_action);
  put_enum(out, policy.basis_requirement);
  put_i64(out, policy.minimum_reserve.parts_per_million());
  put_i64(out, policy.minimum_cumulative_derate.parts_per_million());
  put_i64(out, policy.shared_pool_reserve.parts_per_million());
  put_bool(out, policy.require_transport_declaration);
  put_bool(out, policy.require_revalidation_after_recovery);
  put_i64(out, policy.reconciliation_tolerance.parts_per_million());
}

Result<CoolingPolicy> decode_policy(Reader& reader, const Limits& limits) {
  CoolingPolicy policy;
  CCAP_TRY_DECLARE(id, reader.identifier());
  policy.id = PolicyId::from_identifier(id);
  CCAP_TRY_DECLARE(revision, reader.u64());
  policy.revision = PolicyRevision::from_value(revision);
  CCAP_TRY_DECLARE(rule_count, reader.count(limits.max_freshness_rules, 9U));
  policy.freshness.reserve(rule_count);
  for (std::size_t index = 0; index < rule_count; ++index) {
    FreshnessRule rule;
    CCAP_TRY_DECLARE(kind,
                     read_enum<EvidenceKind>(
                         reader, static_cast<std::uint8_t>(kEvidenceKindCount - 1U), "evidence kind"));
    rule.kind = kind;
    CCAP_TRY_DECLARE(age, reader.i64());
    rule.max_age = DurationMs::from_milliseconds(age);
    policy.freshness.push_back(rule);
  }
  CCAP_TRY_DECLARE(stale,
                   read_enum<StaleEvidenceAction>(
                       reader, static_cast<std::uint8_t>(StaleEvidenceAction::Use),
                       "stale evidence action"));
  policy.stale_action = stale;
  CCAP_TRY_DECLARE(unknown,
                   read_enum<UnknownCapacityAction>(
                       reader, static_cast<std::uint8_t>(UnknownCapacityAction::TreatAsZero),
                       "unknown capacity action"));
  policy.unknown_action = unknown;
  CCAP_TRY_DECLARE(basis,
                   read_enum<CapacityBasisRequirement>(
                       reader, static_cast<std::uint8_t>(CapacityBasisRequirement::ValidatedRequired),
                       "capacity basis requirement"));
  policy.basis_requirement = basis;
  CCAP_TRY_DECLARE(minimum_reserve, reader.i64());
  policy.minimum_reserve = Fraction::from_parts_per_million(minimum_reserve);
  CCAP_TRY_DECLARE(minimum_derate, reader.i64());
  policy.minimum_cumulative_derate = Fraction::from_parts_per_million(minimum_derate);
  CCAP_TRY_DECLARE(shared_reserve, reader.i64());
  policy.shared_pool_reserve = Fraction::from_parts_per_million(shared_reserve);
  CCAP_TRY_DECLARE(require_transport, reader.flag());
  policy.require_transport_declaration = require_transport;
  CCAP_TRY_DECLARE(require_revalidation, reader.flag());
  policy.require_revalidation_after_recovery = require_revalidation;
  CCAP_TRY_DECLARE(tolerance, reader.i64());
  policy.reconciliation_tolerance = Fraction::from_parts_per_million(tolerance);
  return policy;
}

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

void encode_site(std::string& out, const SiteRecord& record) {
  put_text(out, record.id.view());
  put_text(out, record.label.view());
  put_u64(out, record.revision.value());
}

Result<SiteRecord> decode_site(Reader& reader) {
  SiteRecord record;
  CCAP_TRY_DECLARE(id, reader.typed_id<SiteId>());
  record.id = id;
  CCAP_TRY_DECLARE(label, read_label(reader));
  record.label = label;
  CCAP_TRY_DECLARE(revision, reader.u64());
  record.revision = RecordRevision::from_value(revision);
  return record;
}

void encode_facility(std::string& out, const FacilityRecord& record) {
  put_text(out, record.id.view());
  put_text(out, record.site.view());
  put_text(out, record.label.view());
  put_u64(out, record.revision.value());
}

Result<FacilityRecord> decode_facility(Reader& reader) {
  FacilityRecord record;
  CCAP_TRY_DECLARE(id, reader.typed_id<FacilityId>());
  record.id = id;
  CCAP_TRY_DECLARE(site, reader.typed_id<SiteId>());
  record.site = site;
  CCAP_TRY_DECLARE(label, read_label(reader));
  record.label = label;
  CCAP_TRY_DECLARE(revision, reader.u64());
  record.revision = RecordRevision::from_value(revision);
  return record;
}

void encode_domain(std::string& out, const DomainRecord& record) {
  put_text(out, record.id.view());
  put_text(out, record.facility.view());
  put_enum(out, record.medium);
  put_text(out, record.label.view());
  put_u64(out, record.revision.value());
}

Result<DomainRecord> decode_domain(Reader& reader) {
  DomainRecord record;
  CCAP_TRY_DECLARE(id, reader.typed_id<DomainId>());
  record.id = id;
  CCAP_TRY_DECLARE(facility, reader.typed_id<FacilityId>());
  record.facility = facility;
  CCAP_TRY_DECLARE(medium, read_enum<CoolingMedium>(reader, 1U, "cooling medium"));
  record.medium = medium;
  CCAP_TRY_DECLARE(label, read_label(reader));
  record.label = label;
  CCAP_TRY_DECLARE(revision, reader.u64());
  record.revision = RecordRevision::from_value(revision);
  return record;
}

void encode_manifold(std::string& out, const ManifoldRecord& record) {
  put_text(out, record.id.view());
  put_text(out, record.facility.view());
  put_enum(out, record.medium);
  put_text(out, record.label.view());
  put_i64(out, record.transport_limit.milliwatts());
  put_optional_id(out, record.transport_evidence);
  put_u64(out, record.revision.value());
}

Result<ManifoldRecord> decode_manifold(Reader& reader) {
  ManifoldRecord record;
  CCAP_TRY_DECLARE(id, reader.typed_id<ManifoldId>());
  record.id = id;
  CCAP_TRY_DECLARE(facility, reader.typed_id<FacilityId>());
  record.facility = facility;
  CCAP_TRY_DECLARE(medium, read_enum<CoolingMedium>(reader, 1U, "cooling medium"));
  record.medium = medium;
  CCAP_TRY_DECLARE(label, read_label(reader));
  record.label = label;
  CCAP_TRY_DECLARE(limit, reader.i64());
  record.transport_limit = ThermalPower::from_milliwatts(limit);
  CCAP_TRY_DECLARE(evidence, reader.optional_id<EvidenceId>());
  record.transport_evidence = evidence;
  CCAP_TRY_DECLARE(revision, reader.u64());
  record.revision = RecordRevision::from_value(revision);
  return record;
}

void encode_equipment(std::string& out, const EquipmentRecord& record) {
  put_text(out, record.id.view());
  put_text(out, record.facility.view());
  put_text(out, record.label.view());
  put_enum(out, record.kind);
  put_enum(out, record.state);
  put_optional_id(out, record.state_evidence);
  put_optional_time(out, record.state_since);
  put_i64(out, record.nominal.milliwatts());
  put_optional_power(out, record.validated);
  put_optional_id(out, record.validated_evidence);
  put_u32(out, static_cast<std::uint32_t>(record.derates.size()));
  for (const DerateFactor& derate : record.derates) {
    put_text(out, derate.name.view());
    put_i64(out, derate.factor.parts_per_million());
    put_enum(out, derate.reason);
    put_optional_id(out, derate.evidence);
  }
  put_i64(out, record.degradation.factor.parts_per_million());
  put_enum(out, record.degradation.basis);
  put_optional_id(out, record.degradation.evidence);
  put_optional_time(out, record.degradation.assessed_at);
  put_u64(out, record.revision.value());
}

Result<EquipmentRecord> decode_equipment(Reader& reader, const Limits& limits) {
  EquipmentRecord record;
  CCAP_TRY_DECLARE(id, reader.typed_id<EquipmentId>());
  record.id = id;
  CCAP_TRY_DECLARE(facility, reader.typed_id<FacilityId>());
  record.facility = facility;
  CCAP_TRY_DECLARE(label, read_label(reader));
  record.label = label;
  CCAP_TRY_DECLARE(kind,
                   read_enum<EquipmentKind>(
                       reader, static_cast<std::uint8_t>(EquipmentKind::Economizer),
                       "equipment kind"));
  record.kind = kind;
  CCAP_TRY_DECLARE(state, read_enum<OperatingState>(
                              reader, static_cast<std::uint8_t>(OperatingState::Unknown),
                              "operating state"));
  record.state = state;
  CCAP_TRY_DECLARE(state_evidence, reader.optional_id<EvidenceId>());
  record.state_evidence = state_evidence;
  CCAP_TRY_DECLARE(state_since, reader.optional_time());
  record.state_since = state_since;
  CCAP_TRY_DECLARE(nominal, reader.i64());
  record.nominal = ThermalPower::from_milliwatts(nominal);
  CCAP_TRY_DECLARE(validated, reader.optional_power());
  record.validated = validated;
  CCAP_TRY_DECLARE(validated_evidence, reader.optional_id<EvidenceId>());
  record.validated_evidence = validated_evidence;
  CCAP_TRY_DECLARE(derate_count, reader.count(limits.max_derate_factors, 14U));
  record.derates.reserve(derate_count);
  for (std::size_t index = 0; index < derate_count; ++index) {
    DerateFactor derate;
    CCAP_TRY_DECLARE(name, reader.identifier());
    derate.name = name;
    CCAP_TRY_DECLARE(factor, reader.i64());
    if (factor < 0 || factor > Fraction::kScale) {
      return Error(ErrorCode::Corruption, "a derate factor is outside [0, 1]")
          .with("parts_per_million", std::to_string(factor));
    }
    derate.factor = Fraction::from_parts_per_million(factor);
    CCAP_TRY_DECLARE(reason,
                     read_enum<DerateReason>(reader, static_cast<std::uint8_t>(DerateReason::Custom),
                                             "derate reason"));
    derate.reason = reason;
    CCAP_TRY_DECLARE(evidence, reader.optional_id<EvidenceId>());
    derate.evidence = evidence;
    record.derates.push_back(std::move(derate));
  }
  CCAP_TRY_DECLARE(degradation_factor, reader.i64());
  if (degradation_factor < 0 || degradation_factor > Fraction::kScale) {
    return Error(ErrorCode::Corruption, "a degradation factor is outside [0, 1]")
        .with("parts_per_million", std::to_string(degradation_factor));
  }
  record.degradation.factor = Fraction::from_parts_per_million(degradation_factor);
  CCAP_TRY_DECLARE(degradation_basis,
                   read_enum<DegradationBasis>(
                       reader, static_cast<std::uint8_t>(DegradationBasis::ElapsedService),
                       "degradation basis"));
  record.degradation.basis = degradation_basis;
  CCAP_TRY_DECLARE(degradation_evidence, reader.optional_id<EvidenceId>());
  record.degradation.evidence = degradation_evidence;
  CCAP_TRY_DECLARE(assessed_at, reader.optional_time());
  record.degradation.assessed_at = assessed_at;
  CCAP_TRY_DECLARE(revision, reader.u64());
  record.revision = RecordRevision::from_value(revision);
  return record;
}

void encode_loop(std::string& out, const LoopRecord& record) {
  put_text(out, record.id.view());
  put_text(out, record.facility.view());
  put_text(out, record.label.view());
  put_enum(out, record.kind);
  put_i64(out, record.transport_limit.milliwatts());
  put_bool(out, record.transport_limit_declared);
  put_optional_id(out, record.transport_evidence);
  put_u32(out, static_cast<std::uint32_t>(record.equipment.size()));
  for (const EquipmentId& unit : record.equipment) {
    put_typed_id(out, unit);
  }
  put_u32(out, static_cast<std::uint32_t>(record.manifolds.size()));
  for (const ManifoldId& manifold : record.manifolds) {
    put_typed_id(out, manifold);
  }
  put_optional_id(out, record.domain);
  put_optional_id(out, record.primary_plant);
  put_optional_id(out, record.secondary_plant);
  put_enum(out, record.supply_mode);
  put_enum(out, record.redundancy);
  put_u64(out, record.revision.value());
}

Result<LoopRecord> decode_loop(Reader& reader, const Limits& limits) {
  LoopRecord record;
  CCAP_TRY_DECLARE(id, reader.typed_id<LoopId>());
  record.id = id;
  CCAP_TRY_DECLARE(facility, reader.typed_id<FacilityId>());
  record.facility = facility;
  CCAP_TRY_DECLARE(label, read_label(reader));
  record.label = label;
  CCAP_TRY_DECLARE(kind,
                   read_enum<LoopKind>(reader, static_cast<std::uint8_t>(LoopKind::Refrigerant),
                                       "loop kind"));
  record.kind = kind;
  CCAP_TRY_DECLARE(transport, reader.i64());
  record.transport_limit = ThermalPower::from_milliwatts(transport);
  CCAP_TRY_DECLARE(declared, reader.flag());
  record.transport_limit_declared = declared;
  CCAP_TRY_DECLARE(transport_evidence, reader.optional_id<EvidenceId>());
  record.transport_evidence = transport_evidence;
  CCAP_TRY_DECLARE(equipment_count, reader.count(limits.max_equipment_per_loop, 4U));
  record.equipment.reserve(equipment_count);
  for (std::size_t index = 0; index < equipment_count; ++index) {
    CCAP_TRY_DECLARE(unit, reader.typed_id<EquipmentId>());
    record.equipment.push_back(unit);
  }
  CCAP_TRY_DECLARE(manifold_count, reader.count(limits.max_manifolds, 4U));
  record.manifolds.reserve(manifold_count);
  for (std::size_t index = 0; index < manifold_count; ++index) {
    CCAP_TRY_DECLARE(manifold, reader.typed_id<ManifoldId>());
    record.manifolds.push_back(manifold);
  }
  CCAP_TRY_DECLARE(domain, reader.optional_id<DomainId>());
  record.domain = domain;
  CCAP_TRY_DECLARE(primary, reader.optional_id<PlantId>());
  record.primary_plant = primary;
  CCAP_TRY_DECLARE(secondary, reader.optional_id<PlantId>());
  record.secondary_plant = secondary;
  CCAP_TRY_DECLARE(mode,
                   read_enum<PlantSupplyMode>(
                       reader, static_cast<std::uint8_t>(PlantSupplyMode::Alternates),
                       "plant supply mode"));
  record.supply_mode = mode;
  CCAP_TRY_DECLARE(redundancy,
                   read_enum<RedundancyClass>(
                       reader, static_cast<std::uint8_t>(RedundancyClass::TwoNPlusTwo),
                       "redundancy class"));
  record.redundancy = redundancy;
  CCAP_TRY_DECLARE(revision, reader.u64());
  record.revision = RecordRevision::from_value(revision);
  return record;
}

void encode_plant(std::string& out, const PlantRecord& record) {
  put_text(out, record.id.view());
  put_text(out, record.facility.view());
  put_text(out, record.label.view());
  put_enum(out, record.medium);
  put_u32(out, static_cast<std::uint32_t>(record.equipment.size()));
  for (const EquipmentId& unit : record.equipment) {
    put_typed_id(out, unit);
  }
  put_enum(out, record.redundancy);
  put_u64(out, record.revision.value());
}

Result<PlantRecord> decode_plant(Reader& reader, const Limits& limits) {
  PlantRecord record;
  CCAP_TRY_DECLARE(id, reader.typed_id<PlantId>());
  record.id = id;
  CCAP_TRY_DECLARE(facility, reader.typed_id<FacilityId>());
  record.facility = facility;
  CCAP_TRY_DECLARE(label, read_label(reader));
  record.label = label;
  CCAP_TRY_DECLARE(medium, read_enum<CoolingMedium>(reader, 1U, "cooling medium"));
  record.medium = medium;
  CCAP_TRY_DECLARE(equipment_count, reader.count(limits.max_equipment_per_loop, 4U));
  record.equipment.reserve(equipment_count);
  for (std::size_t index = 0; index < equipment_count; ++index) {
    CCAP_TRY_DECLARE(unit, reader.typed_id<EquipmentId>());
    record.equipment.push_back(unit);
  }
  CCAP_TRY_DECLARE(redundancy,
                   read_enum<RedundancyClass>(
                       reader, static_cast<std::uint8_t>(RedundancyClass::TwoNPlusTwo),
                       "redundancy class"));
  record.redundancy = redundancy;
  CCAP_TRY_DECLARE(revision, reader.u64());
  record.revision = RecordRevision::from_value(revision);
  return record;
}

void encode_zone(std::string& out, const ZoneRecord& record) {
  put_text(out, record.id.view());
  put_text(out, record.facility.view());
  put_text(out, record.label.view());
  put_u8(out, record.media.bits());
  put_u32(out, static_cast<std::uint32_t>(record.compatibility.size()));
  for (const CompatibilityClass value : record.compatibility) {
    put_enum(out, value);
  }
  put_u32(out, static_cast<std::uint32_t>(record.loops.size()));
  for (const LoopId& loop : record.loops) {
    put_typed_id(out, loop);
  }
  put_u32(out, static_cast<std::uint32_t>(record.domains.size()));
  for (const DomainId& domain : record.domains) {
    put_typed_id(out, domain);
  }
  put_i64(out, record.reserve_floor.parts_per_million());
  put_u64(out, record.revision.value());
}

Result<ZoneRecord> decode_zone(Reader& reader, const Limits& limits) {
  ZoneRecord record;
  CCAP_TRY_DECLARE(id, reader.typed_id<ZoneId>());
  record.id = id;
  CCAP_TRY_DECLARE(facility, reader.typed_id<FacilityId>());
  record.facility = facility;
  CCAP_TRY_DECLARE(label, read_label(reader));
  record.label = label;
  CCAP_TRY_DECLARE(mask_bits, reader.u8());
  if (mask_bits > 3U) {
    return Error(ErrorCode::Corruption, "a zone medium mask has bits outside the defined media")
        .with("bits", std::to_string(mask_bits));
  }
  if ((mask_bits & 1U) != 0U) {
    record.media.insert(CoolingMedium::Air);
  }
  if ((mask_bits & 2U) != 0U) {
    record.media.insert(CoolingMedium::Liquid);
  }
  CCAP_TRY_DECLARE(class_count, reader.count(kCoolingMediumCount * 8U, 1U));
  record.compatibility.reserve(class_count);
  for (std::size_t index = 0; index < class_count; ++index) {
    CCAP_TRY_DECLARE(value,
                     read_enum<CompatibilityClass>(
                         reader, static_cast<std::uint8_t>(CompatibilityClass::FacilityWater),
                         "compatibility class"));
    record.compatibility.push_back(value);
  }
  CCAP_TRY_DECLARE(loop_count, reader.count(limits.max_loops_per_zone, 4U));
  record.loops.reserve(loop_count);
  for (std::size_t index = 0; index < loop_count; ++index) {
    CCAP_TRY_DECLARE(loop, reader.typed_id<LoopId>());
    record.loops.push_back(loop);
  }
  CCAP_TRY_DECLARE(domain_count, reader.count(limits.max_domains, 4U));
  record.domains.reserve(domain_count);
  for (std::size_t index = 0; index < domain_count; ++index) {
    CCAP_TRY_DECLARE(domain, reader.typed_id<DomainId>());
    record.domains.push_back(domain);
  }
  CCAP_TRY_DECLARE(reserve, reader.i64());
  if (reserve < 0 || reserve > Fraction::kScale) {
    return Error(ErrorCode::Corruption, "a zone reserve floor is outside [0, 1]")
        .with("parts_per_million", std::to_string(reserve));
  }
  record.reserve_floor = Fraction::from_parts_per_million(reserve);
  CCAP_TRY_DECLARE(revision, reader.u64());
  record.revision = RecordRevision::from_value(revision);
  return record;
}

void encode_evidence(std::string& out, const EvidenceRecord& record) {
  put_text(out, record.id.view());
  put_enum(out, record.kind);
  put_enum(out, record.source);
  encode_evidence_subject(out, record.subject);
  put_u8(out, record.medium.has_value() ? kPresent : kAbsent);
  if (record.medium.has_value()) {
    put_enum(out, *record.medium);
  }
  encode_evidence_value(out, record.value);
  put_i64(out, record.observed_at.unix_milliseconds());
  put_optional_time(out, record.valid_until);
  put_optional_generation(out, record.source_generation);
  put_text(out, record.provenance.actor.view());
  put_text(out, record.provenance.reference.view());
  put_optional_id(out, record.provenance.supersedes);
  put_enum(out, record.state);
  put_u64(out, record.revision.value());
}

Result<EvidenceRecord> decode_evidence(Reader& reader) {
  EvidenceRecord record;
  CCAP_TRY_DECLARE(id, reader.typed_id<EvidenceId>());
  record.id = id;
  CCAP_TRY_DECLARE(kind,
                   read_enum<EvidenceKind>(
                       reader, static_cast<std::uint8_t>(kEvidenceKindCount - 1U), "evidence kind"));
  record.kind = kind;
  CCAP_TRY_DECLARE(source,
                   read_enum<EvidenceSource>(
                       reader, static_cast<std::uint8_t>(EvidenceSource::Derived),
                       "evidence source"));
  record.source = source;
  CCAP_TRY_DECLARE(subject, decode_evidence_subject(reader));
  record.subject = subject;
  CCAP_TRY_DECLARE(has_medium, reader.flag());
  if (has_medium) {
    CCAP_TRY_DECLARE(medium, read_enum<CoolingMedium>(reader, 1U, "cooling medium"));
    record.medium = medium;
  }
  CCAP_TRY_DECLARE(value, decode_evidence_value(reader));
  record.value = value;
  CCAP_TRY_DECLARE(observed_at, reader.i64());
  record.observed_at = Timestamp::from_unix_milliseconds(observed_at);
  CCAP_TRY_DECLARE(valid_until, reader.optional_time());
  record.valid_until = valid_until;
  CCAP_TRY_DECLARE(source_generation, reader.optional_generation());
  record.source_generation = source_generation;
  CCAP_TRY_DECLARE(actor, reader.typed_id<ActorId>());
  record.provenance.actor = actor;
  CCAP_TRY_DECLARE(reference, reader.text(kMaxDocumentReferenceLength));
  CCAP_TRY_DECLARE(parsed_reference, DocumentRef::parse(reference));
  record.provenance.reference = parsed_reference;
  CCAP_TRY_DECLARE(supersedes, reader.optional_id<EvidenceId>());
  record.provenance.supersedes = supersedes;
  CCAP_TRY_DECLARE(state,
                   read_enum<EvidenceState>(reader, static_cast<std::uint8_t>(EvidenceState::Withdrawn),
                                            "evidence state"));
  record.state = state;
  CCAP_TRY_DECLARE(revision, reader.u64());
  record.revision = RecordRevision::from_value(revision);
  return record;
}

void encode_commitment(std::string& out, const CommitmentRecord& record) {
  put_text(out, record.id.view());
  put_text(out, record.attempt.view());
  put_text(out, record.zone.view());
  put_optional_id(out, record.pinned_loop);
  put_enum(out, record.medium);
  put_enum(out, record.compatibility);
  put_i64(out, record.thermal.milliwatts());
  put_enum(out, record.state);
  put_text(out, record.actor.view());
  put_i64(out, record.created_at.unix_milliseconds());
  put_optional_time(out, record.expires_at);
  put_u64(out, record.created_generation.value());
  put_optional_id(out, record.supersedes);
  put_text(out, record.note.view());
  put_u64(out, record.revision.value());
}

Result<CommitmentRecord> decode_commitment(Reader& reader) {
  CommitmentRecord record;
  CCAP_TRY_DECLARE(id, reader.typed_id<CommitmentId>());
  record.id = id;
  CCAP_TRY_DECLARE(attempt, reader.typed_id<AttemptId>());
  record.attempt = attempt;
  CCAP_TRY_DECLARE(zone, reader.typed_id<ZoneId>());
  record.zone = zone;
  CCAP_TRY_DECLARE(pinned, reader.optional_id<LoopId>());
  record.pinned_loop = pinned;
  CCAP_TRY_DECLARE(medium, read_enum<CoolingMedium>(reader, 1U, "cooling medium"));
  record.medium = medium;
  CCAP_TRY_DECLARE(compatibility,
                   read_enum<CompatibilityClass>(
                       reader, static_cast<std::uint8_t>(CompatibilityClass::FacilityWater),
                       "compatibility class"));
  record.compatibility = compatibility;
  CCAP_TRY_DECLARE(thermal, reader.i64());
  record.thermal = ThermalPower::from_milliwatts(thermal);
  CCAP_TRY_DECLARE(state,
                   read_enum<CommitmentState>(
                       reader, static_cast<std::uint8_t>(CommitmentState::Superseded),
                       "commitment state"));
  record.state = state;
  CCAP_TRY_DECLARE(actor, reader.typed_id<ActorId>());
  record.actor = actor;
  CCAP_TRY_DECLARE(created_at, reader.i64());
  record.created_at = Timestamp::from_unix_milliseconds(created_at);
  CCAP_TRY_DECLARE(expires_at, reader.optional_time());
  record.expires_at = expires_at;
  CCAP_TRY_DECLARE(created_generation, reader.u64());
  record.created_generation = CapacityGeneration::from_value(created_generation);
  CCAP_TRY_DECLARE(supersedes, reader.optional_id<CommitmentId>());
  record.supersedes = supersedes;
  CCAP_TRY_DECLARE(note, read_label(reader));
  record.note = note;
  CCAP_TRY_DECLARE(revision, reader.u64());
  record.revision = RecordRevision::from_value(revision);
  return record;
}

// ---------------------------------------------------------------------------
// Payload
// ---------------------------------------------------------------------------

Result<std::string> encode_payload(const CoolingSnapshot& snapshot, const Limits& limits) {
  std::string payload;
  payload.reserve(4096U);
  put_u8(payload, kMetadataVersion);
  put_i64(payload, snapshot.constructed_at().unix_milliseconds());
  if (snapshot.revalidated_at().has_value()) {
    put_u8(payload, kPresent);
    put_i64(payload, snapshot.revalidated_at()->unix_milliseconds());
  } else {
    put_u8(payload, kAbsent);
  }
  encode_policy(payload, snapshot.policy());

  const auto check = [&limits](std::size_t count, std::size_t maximum,
                               const char* what) -> Result<void> {
    if (count > maximum) {
      return Error(ErrorCode::LimitExceeded, std::string("too many ") + what + " to encode")
          .with("count", std::to_string(count))
          .with("maximum", std::to_string(maximum));
    }
    return Result<void>();
  };

  CCAP_TRY(check(snapshot.sites().size(), limits.max_sites, "sites"));
  put_u32(payload, static_cast<std::uint32_t>(snapshot.sites().size()));
  for (const SiteRecord& record : snapshot.sites()) {
    encode_site(payload, record);
  }
  CCAP_TRY(check(snapshot.facilities().size(), limits.max_facilities, "facilities"));
  put_u32(payload, static_cast<std::uint32_t>(snapshot.facilities().size()));
  for (const FacilityRecord& record : snapshot.facilities()) {
    encode_facility(payload, record);
  }
  CCAP_TRY(check(snapshot.domains().size(), limits.max_domains, "domains"));
  put_u32(payload, static_cast<std::uint32_t>(snapshot.domains().size()));
  for (const DomainRecord& record : snapshot.domains()) {
    encode_domain(payload, record);
  }
  CCAP_TRY(check(snapshot.manifolds().size(), limits.max_manifolds, "manifolds"));
  put_u32(payload, static_cast<std::uint32_t>(snapshot.manifolds().size()));
  for (const ManifoldRecord& record : snapshot.manifolds()) {
    encode_manifold(payload, record);
  }
  CCAP_TRY(check(snapshot.equipment().size(), limits.max_equipment, "equipment"));
  put_u32(payload, static_cast<std::uint32_t>(snapshot.equipment().size()));
  for (const EquipmentRecord& record : snapshot.equipment()) {
    encode_equipment(payload, record);
  }
  CCAP_TRY(check(snapshot.loops().size(), limits.max_loops, "loops"));
  put_u32(payload, static_cast<std::uint32_t>(snapshot.loops().size()));
  for (const LoopRecord& record : snapshot.loops()) {
    encode_loop(payload, record);
  }
  CCAP_TRY(check(snapshot.plants().size(), limits.max_plants, "plants"));
  put_u32(payload, static_cast<std::uint32_t>(snapshot.plants().size()));
  for (const PlantRecord& record : snapshot.plants()) {
    encode_plant(payload, record);
  }
  CCAP_TRY(check(snapshot.zones().size(), limits.max_zones, "zones"));
  put_u32(payload, static_cast<std::uint32_t>(snapshot.zones().size()));
  for (const ZoneRecord& record : snapshot.zones()) {
    encode_zone(payload, record);
  }
  CCAP_TRY(check(snapshot.evidence().size(), limits.max_evidence, "evidence records"));
  put_u32(payload, static_cast<std::uint32_t>(snapshot.evidence().size()));
  for (const EvidenceRecord& record : snapshot.evidence()) {
    encode_evidence(payload, record);
  }
  CCAP_TRY(check(snapshot.commitments().size(), limits.max_commitments, "commitments"));
  put_u32(payload, static_cast<std::uint32_t>(snapshot.commitments().size()));
  for (const CommitmentRecord& record : snapshot.commitments()) {
    encode_commitment(payload, record);
  }
  return payload;
}

void write_header(char* header, const CapacityGeneration& generation, std::uint64_t payload_bytes,
                  std::int64_t constructed_at) {
  std::string view;
  view.reserve(kArtifactHeaderSize);
  view.append(kArtifactMagic, sizeof(kArtifactMagic));
  put_u32(view, kArtifactFormatVersion);
  put_u32(view, kEndianMarker);
  put_u64(view, generation.value());
  put_u64(view, payload_bytes);
  put_i64(view, constructed_at);
  put_u64(view, 0U);
  put_u64(view, 0U);
  put_u64(view, 0U);
  // The reserved fields above exist so that this buffer is exactly the size of
  // the header. The copy below must never read past the end of it: an earlier
  // revision copied kArtifactHeaderSize bytes out of a shorter buffer, which
  // put uninitialised memory into every artifact and made the canonical digest
  // unstable.
  if (view.size() != kArtifactHeaderSize) {
    std::abort();
  }
  std::memcpy(header, view.data(), kArtifactHeaderSize);
}

}  // namespace

namespace {

// The digest covers the header and the payload but not the trailing digest
// itself, which is what the trailer records. `encode_body` and
// `encode_snapshot` share this so that a snapshot's digest and the digest an
// artifact carries are always the same value.
Result<std::string> encode_body(const CoolingSnapshot& snapshot, const Limits& limits) {
  CCAP_TRY(limits.validate());
  CCAP_TRY_DECLARE(payload, encode_payload(snapshot, limits));
  if (payload.size() + kArtifactHeaderSize + Digest::kSize > limits.max_artifact_bytes) {
    return Error(ErrorCode::Oversized, "the encoded artifact exceeds the configured maximum")
        .with("bytes", std::to_string(payload.size() + kArtifactHeaderSize + Digest::kSize))
        .with("maximum", std::to_string(limits.max_artifact_bytes));
  }
  std::string body(kArtifactHeaderSize, '\0');
  write_header(body.data(), snapshot.generation(), payload.size(),
               snapshot.constructed_at().unix_milliseconds());
  body.append(payload);
  return body;
}

}  // namespace

Result<std::string> encode_snapshot(const CoolingSnapshot& snapshot, const Limits& limits) {
  CCAP_TRY_DECLARE(body, encode_body(snapshot, limits));
  const Digest digest = Digest::of(body);
  const std::array<std::uint8_t, Digest::kSize>& raw = digest.bytes();
  body.append(reinterpret_cast<const char*>(raw.data()), raw.size());
  return body;
}

Digest canonical_digest(const CoolingSnapshot& snapshot) {
  Limits limits;
  Result<std::string> body = encode_body(snapshot, limits);
  if (!body.ok()) {
    // The digest of a snapshot that cannot be encoded is the digest of a marker
    // naming it, so that a digest always exists and two different snapshots
    // never share one by accident.
    const std::string fallback =
        "cooling-capacity/unencodable/" + snapshot.generation().to_string();
    return Digest::of(fallback);
  }
  return Digest::of(body.value());
}

Result<ArtifactManifest> read_artifact_manifest(std::string_view bytes) {
  if (bytes.size() < kArtifactHeaderSize) {
    return Error(ErrorCode::Truncated, "the artifact is shorter than its header")
        .with("bytes", std::to_string(bytes.size()));
  }
  if (std::memcmp(bytes.data(), kArtifactMagic, sizeof(kArtifactMagic)) != 0) {
    return Error(ErrorCode::UnsupportedFormat, "the artifact magic does not match")
        .with("expected", std::string(kArtifactMagic, sizeof(kArtifactMagic)));
  }
  const auto read_u32_at = [&bytes](std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4U; ++index) {
      value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + index]))
               << (8U * index);
    }
    return value;
  };
  const auto read_u64_at = [&bytes](std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8U; ++index) {
      value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + index]))
               << (8U * index);
    }
    return value;
  };
  const std::uint32_t endian = read_u32_at(12U);
  if (endian == 0x04030201U) {
    return Error(ErrorCode::WrongEndianness,
                 "the artifact was written on a host with the opposite byte order");
  }
  if (endian != kEndianMarker) {
    return Error(ErrorCode::Corruption, "the artifact byte-order marker is not recognised")
        .with("marker", std::to_string(endian));
  }
  const std::uint32_t version = read_u32_at(8U);
  if (version != kArtifactFormatVersion) {
    return Error(ErrorCode::IncompatibleVersion, "the artifact format version is not supported")
        .with("artifact_version", std::to_string(version))
        .with("reader_version", std::to_string(kArtifactFormatVersion));
  }
  ArtifactManifest manifest;
  manifest.format_version = version;
  manifest.generation = CapacityGeneration::from_value(read_u64_at(16U));
  manifest.payload_bytes = read_u64_at(24U);
  if (manifest.payload_bytes > (1ULL << 34)) {
    return Error(ErrorCode::Oversized, "the declared payload length is implausible")
        .with("payload_bytes", std::to_string(manifest.payload_bytes));
  }
  const std::uint64_t expected_total =
      static_cast<std::uint64_t>(kArtifactHeaderSize) + manifest.payload_bytes + Digest::kSize;
  if (static_cast<std::uint64_t>(bytes.size()) != expected_total) {
    return Error(ErrorCode::Truncated,
                 "the artifact length does not match the declared payload length")
        .with("actual_bytes", std::to_string(bytes.size()))
        .with("expected_bytes", std::to_string(expected_total));
  }
  const std::string_view body = bytes.substr(0, bytes.size() - Digest::kSize);
  const std::string_view trailer = bytes.substr(bytes.size() - Digest::kSize);
  const Digest computed = Digest::of(body);
  std::array<std::uint8_t, Digest::kSize> recorded{};
  std::memcpy(recorded.data(), trailer.data(), Digest::kSize);
  if (computed.bytes() != recorded) {
    return Error(ErrorCode::DigestMismatch, "the artifact digest does not match its contents");
  }
  manifest.digest = computed;
  manifest.verified = true;
  return manifest;
}

Result<std::shared_ptr<const CoolingSnapshot>> decode_snapshot(std::string_view bytes,
                                                               const Limits& limits) {
  CCAP_TRY(limits.validate());
  if (bytes.size() > limits.max_artifact_bytes) {
    return Error(ErrorCode::Oversized, "the artifact exceeds the configured maximum")
        .with("bytes", std::to_string(bytes.size()))
        .with("maximum", std::to_string(limits.max_artifact_bytes));
  }
  CCAP_TRY_DECLARE(manifest, read_artifact_manifest(bytes));
  const std::string_view payload =
      bytes.substr(kArtifactHeaderSize, static_cast<std::size_t>(manifest.payload_bytes));

  Reader reader(payload);
  CCAP_TRY_DECLARE(metadata_version, reader.u8());
  if (metadata_version != kMetadataVersion) {
    return Error(ErrorCode::IncompatibleVersion, "the artifact metadata version is not supported")
        .with("metadata_version", std::to_string(metadata_version))
        .with("reader_version", std::to_string(kMetadataVersion));
  }
  SnapshotBuilder builder;
  builder.set_generation(manifest.generation);
  CCAP_TRY_DECLARE(constructed_at, reader.i64());
  builder.set_constructed_at(Timestamp::from_unix_milliseconds(constructed_at));
  CCAP_TRY_DECLARE(has_revalidated, reader.flag());
  std::optional<Timestamp> revalidated_at;
  if (has_revalidated) {
    CCAP_TRY_DECLARE(instant, reader.i64());
    revalidated_at = Timestamp::from_unix_milliseconds(instant);
  }
  builder.set_limits(limits);
  CCAP_TRY_DECLARE(policy, decode_policy(reader, limits));
  builder.set_policy(policy);

  CCAP_TRY_DECLARE(site_count, reader.count(limits.max_sites, kMinimumRecordBytes));
  for (std::size_t index = 0; index < site_count; ++index) {
    CCAP_TRY_DECLARE(record, decode_site(reader));
    CCAP_TRY(builder.add(record));
  }
  CCAP_TRY_DECLARE(facility_count, reader.count(limits.max_facilities, kMinimumRecordBytes));
  for (std::size_t index = 0; index < facility_count; ++index) {
    CCAP_TRY_DECLARE(record, decode_facility(reader));
    CCAP_TRY(builder.add(record));
  }
  CCAP_TRY_DECLARE(domain_count, reader.count(limits.max_domains, kMinimumRecordBytes));
  for (std::size_t index = 0; index < domain_count; ++index) {
    CCAP_TRY_DECLARE(record, decode_domain(reader));
    CCAP_TRY(builder.add(record));
  }
  CCAP_TRY_DECLARE(manifold_count, reader.count(limits.max_manifolds, kMinimumRecordBytes));
  for (std::size_t index = 0; index < manifold_count; ++index) {
    CCAP_TRY_DECLARE(record, decode_manifold(reader));
    CCAP_TRY(builder.add(record));
  }
  CCAP_TRY_DECLARE(equipment_count, reader.count(limits.max_equipment, kMinimumRecordBytes));
  for (std::size_t index = 0; index < equipment_count; ++index) {
    CCAP_TRY_DECLARE(record, decode_equipment(reader, limits));
    CCAP_TRY(builder.add(record));
  }
  CCAP_TRY_DECLARE(loop_count, reader.count(limits.max_loops, kMinimumRecordBytes));
  for (std::size_t index = 0; index < loop_count; ++index) {
    CCAP_TRY_DECLARE(record, decode_loop(reader, limits));
    CCAP_TRY(builder.add(record));
  }
  CCAP_TRY_DECLARE(plant_count, reader.count(limits.max_plants, kMinimumRecordBytes));
  for (std::size_t index = 0; index < plant_count; ++index) {
    CCAP_TRY_DECLARE(record, decode_plant(reader, limits));
    CCAP_TRY(builder.add(record));
  }
  CCAP_TRY_DECLARE(zone_count, reader.count(limits.max_zones, kMinimumRecordBytes));
  for (std::size_t index = 0; index < zone_count; ++index) {
    CCAP_TRY_DECLARE(record, decode_zone(reader, limits));
    CCAP_TRY(builder.add(record));
  }
  CCAP_TRY_DECLARE(evidence_count, reader.count(limits.max_evidence, kMinimumRecordBytes));
  for (std::size_t index = 0; index < evidence_count; ++index) {
    CCAP_TRY_DECLARE(record, decode_evidence(reader));
    CCAP_TRY(builder.add(record));
  }
  CCAP_TRY_DECLARE(commitment_count, reader.count(limits.max_commitments, kMinimumRecordBytes));
  for (std::size_t index = 0; index < commitment_count; ++index) {
    CCAP_TRY_DECLARE(record, decode_commitment(reader));
    CCAP_TRY(builder.add(record));
  }
  CCAP_TRY(reader.expect_end());

  CCAP_TRY_DECLARE(snapshot, builder.build_unvalidated());
  std::shared_ptr<const CoolingSnapshot> result = snapshot;
  if (revalidated_at.has_value()) {
    result = result->with_provenance(SnapshotOrigin::Constructed, revalidated_at);
  }
  if (result->digest() != manifest.digest) {
    return Error(ErrorCode::Corruption,
                 "the decoded artifact is not in canonical form: re-encoding it produces "
                 "different bytes");
  }
  return result;
}

std::string render_snapshot_text(const CoolingSnapshot& snapshot) {
  std::string out;
  out.reserve(4096U);
  out.append("cooling-capacity snapshot generation ");
  out.append(snapshot.generation().to_string());
  out.push_back('\n');
  out.append("constructed_at ");
  out.append(snapshot.constructed_at().to_string());
  out.push_back('\n');
  if (snapshot.revalidated_at().has_value()) {
    out.append("revalidated_at ");
    out.append(snapshot.revalidated_at()->to_string());
    out.push_back('\n');
  }
  out.append("digest ");
  out.append(snapshot.digest().to_hex());
  out.push_back('\n');
  out.append("policy ");
  out.append(snapshot.policy().id.str());
  out.push_back(' ');
  out.append(snapshot.policy().revision.to_string());
  out.push_back('\n');
  const auto line = [&out](std::string_view kind, const Identifier& id) {
    out.append(kind);
    out.push_back(' ');
    out.append(id.str());
    out.push_back('\n');
  };
  for (const SiteRecord& record : snapshot.sites()) {
    line("site", record.id.value());
  }
  for (const FacilityRecord& record : snapshot.facilities()) {
    line("facility", record.id.value());
  }
  for (const DomainRecord& record : snapshot.domains()) {
    line("domain", record.id.value());
  }
  for (const ManifoldRecord& record : snapshot.manifolds()) {
    line("manifold", record.id.value());
  }
  for (const EquipmentRecord& record : snapshot.equipment()) {
    line("equipment", record.id.value());
  }
  for (const LoopRecord& record : snapshot.loops()) {
    line("loop", record.id.value());
  }
  for (const PlantRecord& record : snapshot.plants()) {
    line("plant", record.id.value());
  }
  for (const ZoneRecord& record : snapshot.zones()) {
    line("zone", record.id.value());
  }
  for (const EvidenceRecord& record : snapshot.evidence()) {
    line("evidence", record.id.value());
  }
  for (const CommitmentRecord& record : snapshot.commitments()) {
    line("commitment", record.id.value());
  }
  out.append("records ");
  out.append(std::to_string(snapshot.record_count()));
  out.push_back('\n');
  return out;
}

}  // namespace cooling_capacity
