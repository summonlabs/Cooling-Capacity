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

#include "cooling_capacity/accounting.hpp"

#include "enum_table.hpp"

namespace cooling_capacity {

namespace {

constexpr detail::EnumName<CapacityStatus> kStatusNames[] = {
    {CapacityStatus::Known, "known"},
    {CapacityStatus::Unknown, "unknown"},
    {CapacityStatus::Unsupported, "unsupported"},
    {CapacityStatus::Unavailable, "unavailable"},
};

constexpr detail::EnumName<CapacityReason> kReasonNames[] = {
    {CapacityReason::None, "none"},
    {CapacityReason::NotServedByAnyLoop, "not-served-by-any-loop"},
    {CapacityReason::MediumNotSupported, "medium-not-supported"},
    {CapacityReason::ClassNotSupported, "class-not-supported"},
    {CapacityReason::NoCapacityDeclared, "no-capacity-declared"},
    {CapacityReason::TransportNotDeclared, "transport-not-declared"},
    {CapacityReason::EvidenceMissing, "evidence-missing"},
    {CapacityReason::EvidenceStale, "evidence-stale"},
    {CapacityReason::EvidenceWithdrawn, "evidence-withdrawn"},
    {CapacityReason::EvidenceFutureDated, "evidence-future-dated"},
    {CapacityReason::OperatingStateUnknown, "operating-state-unknown"},
    {CapacityReason::ValidationMissing, "validation-missing"},
    {CapacityReason::DerateBelowFloor, "derate-below-floor"},
    {CapacityReason::OutOfService, "out-of-service"},
    {CapacityReason::Faulted, "faulted"},
    {CapacityReason::Maintenance, "maintenance"},
    {CapacityReason::Decommissioned, "decommissioned"},
    {CapacityReason::EmptyGroup, "empty-group"},
    {CapacityReason::UnknownContributor, "unknown-contributor"},
    {CapacityReason::RecoveredPendingRevalidation, "recovered-pending-revalidation"},
    {CapacityReason::NoObservation, "no-observation"},
    {CapacityReason::OverflowRefused, "overflow-refused"},
    {CapacityReason::Standby, "standby"},
};

constexpr detail::EnumName<CapacityBasis> kBasisNames[] = {
    {CapacityBasis::None, "none"},
    {CapacityBasis::Nominal, "nominal"},
    {CapacityBasis::Validated, "validated"},
};

}  // namespace

std::string_view to_string(CapacityStatus status) noexcept {
  return detail::name_from_table(kStatusNames, status);
}

Result<CapacityStatus> parse_capacity_status(std::string_view text) {
  return detail::parse_from_table("capacity status", kStatusNames, text);
}

std::string_view to_string(CapacityReason reason) noexcept {
  return detail::name_from_table(kReasonNames, reason);
}

Result<CapacityReason> parse_capacity_reason(std::string_view text) {
  return detail::parse_from_table("capacity reason", kReasonNames, text);
}

std::string_view to_string(CapacityBasis basis) noexcept {
  return detail::name_from_table(kBasisNames, basis);
}

CapacityValue CapacityValue::known(ThermalPower amount) {
  CapacityValue value;
  value.status_ = CapacityStatus::Known;
  value.reason_ = CapacityReason::None;
  value.amount_ = amount;
  return value;
}

CapacityValue CapacityValue::unknown(CapacityReason reason) {
  CapacityValue value;
  value.status_ = CapacityStatus::Unknown;
  value.reason_ = reason;
  value.amount_ = ThermalPower::from_milliwatts(0);
  return value;
}

CapacityValue CapacityValue::unsupported(CapacityReason reason) {
  CapacityValue value;
  value.status_ = CapacityStatus::Unsupported;
  value.reason_ = reason;
  value.amount_ = ThermalPower::from_milliwatts(0);
  return value;
}

CapacityValue CapacityValue::unavailable(CapacityReason reason) {
  CapacityValue value;
  value.status_ = CapacityStatus::Unavailable;
  value.reason_ = reason;
  value.amount_ = ThermalPower::from_milliwatts(0);
  return value;
}

Result<ThermalPower> CapacityValue::exact_amount() const {
  if (status_ != CapacityStatus::Known) {
    return Error(ErrorCode::Unknown, "capacity value is not known")
        .with("status", std::string(cooling_capacity::to_string(status_)))
        .with("reason", std::string(cooling_capacity::to_string(reason_)));
  }
  return amount_;
}

std::string CapacityValue::to_string() const {
  switch (status_) {
    case CapacityStatus::Known:
      return amount_.to_string();
    case CapacityStatus::Unknown:
      return std::string("unknown(") + std::string(cooling_capacity::to_string(reason_)) + ")";
    case CapacityStatus::Unsupported:
      return std::string("unsupported(") + std::string(cooling_capacity::to_string(reason_)) + ")";
    case CapacityStatus::Unavailable:
      return std::string("unavailable(") + std::string(cooling_capacity::to_string(reason_)) + ")";
  }
  return "unknown";
}

Result<CapacityValue> sum_values(const std::vector<CapacityValue>& parts) {
  bool any_unknown = false;
  bool any_unsupported = false;
  bool any_available = false;
  CapacityReason unknown_reason = CapacityReason::EvidenceMissing;
  CapacityReason unsupported_reason = CapacityReason::NotServedByAnyLoop;
  std::int64_t total = 0;
  for (const CapacityValue& part : parts) {
    switch (part.status()) {
      case CapacityStatus::Known: {
        const std::int64_t addend = part.amount_or_zero().milliwatts();
        if (addend > 0 && total > std::numeric_limits<std::int64_t>::max() - addend) {
          return Error(ErrorCode::ArithmeticOverflow, "capacity sum overflowed");
        }
        if (addend < 0 && total < std::numeric_limits<std::int64_t>::min() - addend) {
          return Error(ErrorCode::ArithmeticOverflow, "capacity sum underflowed");
        }
        total += addend;
        any_available = true;
        break;
      }
      case CapacityStatus::Unavailable:
        // A definitely-zero contributor is a real, known zero: it adds nothing
        // and does not poison the sum.
        any_available = true;
        break;
      case CapacityStatus::Unsupported:
        any_unsupported = true;
        if (!any_unknown) {
          unsupported_reason = part.reason();
        }
        break;
      case CapacityStatus::Unknown:
        if (!any_unknown) {
          unknown_reason = part.reason();
        }
        any_unknown = true;
        break;
    }
  }
  if (any_unknown) {
    return CapacityValue::unknown(unknown_reason);
  }
  if (!any_available && any_unsupported) {
    return CapacityValue::unsupported(unsupported_reason);
  }
  return CapacityValue::known(ThermalPower::from_milliwatts(total));
}

Result<CapacityValue> min_values(const std::vector<CapacityValue>& parts) {
  if (parts.empty()) {
    return CapacityValue::unknown(CapacityReason::EmptyGroup);
  }
  bool any_unknown = false;
  CapacityReason unknown_reason = CapacityReason::EvidenceMissing;
  bool have_known = false;
  bool any_unsupported = false;
  std::int64_t best = 0;
  for (const CapacityValue& part : parts) {
    switch (part.status()) {
      case CapacityStatus::Unknown:
        if (!any_unknown) {
          unknown_reason = part.reason();
        }
        any_unknown = true;
        break;
      case CapacityStatus::Unavailable:
        // A constituent that is definitely zero makes the smallest of the
        // limits definitely zero. Returning the transport limit of a fully
        // faulted loop would be exactly the kind of silent overstatement this
        // type exists to prevent.
        return part;
      case CapacityStatus::Unsupported:
        any_unsupported = true;
        break;
      case CapacityStatus::Known: {
        const std::int64_t amount = part.amount_or_zero().milliwatts();
        if (!have_known || amount < best) {
          best = amount;
          have_known = true;
        }
        break;
      }
    }
  }
  if (any_unknown) {
    return CapacityValue::unknown(unknown_reason);
  }
  if (!have_known) {
    // Every part was Unsupported: the ceiling exists but is not a number.
    return CapacityValue::unsupported(any_unsupported ? parts.front().reason()
                                                      : CapacityReason::NotServedByAnyLoop);
  }
  return CapacityValue::known(ThermalPower::from_milliwatts(best));
}

Result<CapacityValue> apply_ceiling(CapacityValue value, ThermalPower ceiling) {
  CCAP_TRY_DECLARE(limited, min_values({value, CapacityValue::known(ceiling)}));
  return limited;
}

Result<void> finalise_breakdown(CapacityBreakdown& breakdown) {
  const CapacityValue& usable = breakdown.usable;
  const CapacityValue& committed = breakdown.committed;

  if (usable.is_known() && committed.is_known()) {
    const std::int64_t usable_raw = usable.amount_or_zero().milliwatts();
    const std::int64_t committed_raw = committed.amount_or_zero().milliwatts();
    if (committed_raw > usable_raw) {
      breakdown.over_committed = true;
      breakdown.reserve = CapacityValue::known(ThermalPower::from_milliwatts(0));
      breakdown.deficit =
          CapacityValue::known(ThermalPower::from_milliwatts(committed_raw - usable_raw));
    } else {
      breakdown.over_committed = false;
      breakdown.reserve =
          CapacityValue::known(ThermalPower::from_milliwatts(usable_raw - committed_raw));
      breakdown.deficit = CapacityValue::known(ThermalPower::from_milliwatts(0));
    }
  } else if (usable.is_known() && committed.status() == CapacityStatus::Unavailable) {
    breakdown.over_committed = false;
    breakdown.reserve = usable;
    breakdown.deficit = CapacityValue::known(ThermalPower::from_milliwatts(0));
  } else {
    // An unknown usable or committed figure cannot establish a reserve, and it
    // certainly cannot establish that the point is over-committed.
    breakdown.over_committed = false;
    breakdown.reserve = usable.is_known() ? CapacityValue::unknown(committed.reason())
                                          : CapacityValue::unknown(usable.reason());
    breakdown.deficit = CapacityValue::known(ThermalPower::from_milliwatts(0));
  }

  if (usable.is_known() && breakdown.observed.is_known()) {
    const std::int64_t usable_raw = usable.amount_or_zero().milliwatts();
    const std::int64_t observed_raw = breakdown.observed.amount_or_zero().milliwatts();
    const std::int64_t remaining = observed_raw >= usable_raw ? 0 : usable_raw - observed_raw;
    breakdown.observed_headroom =
        CapacityValue::known(ThermalPower::from_milliwatts(remaining));
  } else {
    breakdown.observed_headroom = CapacityValue::unknown(
        usable.is_known() ? breakdown.observed.reason() : usable.reason());
  }
  return Result<void>();
}

}  // namespace cooling_capacity
