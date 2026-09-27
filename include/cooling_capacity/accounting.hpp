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

#ifndef COOLING_CAPACITY_ACCOUNTING_HPP
#define COOLING_CAPACITY_ACCOUNTING_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cooling_capacity/errors.hpp"
#include "cooling_capacity/export.hpp"
#include "cooling_capacity/medium.hpp"
#include "cooling_capacity/units.hpp"

namespace cooling_capacity {

// The four answers a capacity question can have. They are not interchangeable:
//
//   Known        the amount is established by usable evidence;
//   Unknown      the amount cannot be established, so nothing may be concluded;
//   Unsupported  this medium or compatibility class does not exist here at all,
//                which is a definite negative;
//   Unavailable  the amount is definitely zero right now, for a named reason.
//
// Zero is `Known(zero)`. Collapsing Unknown into zero is the single most
// dangerous shortcut in capacity accounting and this type refuses to allow it.
enum class CapacityStatus : std::uint8_t {
  Known = 0,
  Unknown = 1,
  Unsupported = 2,
  Unavailable = 3,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(CapacityStatus status) noexcept;
[[nodiscard]] CCAP_EXPORT Result<CapacityStatus> parse_capacity_status(std::string_view text);

// Why a capacity value is not simply Known. Every non-Known value carries a
// reason; `None` is reserved for Known values.
enum class CapacityReason : std::uint8_t {
  None = 0,
  NotServedByAnyLoop = 1,
  MediumNotSupported = 2,
  ClassNotSupported = 3,
  NoCapacityDeclared = 4,
  TransportNotDeclared = 5,
  EvidenceMissing = 6,
  EvidenceStale = 7,
  EvidenceWithdrawn = 8,
  EvidenceFutureDated = 9,
  OperatingStateUnknown = 10,
  ValidationMissing = 11,
  DerateBelowFloor = 12,
  OutOfService = 13,
  Faulted = 14,
  Maintenance = 15,
  Decommissioned = 16,
  EmptyGroup = 17,
  UnknownContributor = 18,
  RecoveredPendingRevalidation = 19,
  NoObservation = 20,
  OverflowRefused = 21,
  Standby = 22,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(CapacityReason reason) noexcept;
[[nodiscard]] CCAP_EXPORT Result<CapacityReason> parse_capacity_reason(std::string_view text);

// The strongest evidence basis behind a capacity figure. `Nominal` means the
// nameplate was used because no validation exists; `Validated` means a
// commissioned or otherwise validated figure was used.
enum class CapacityBasis : std::uint8_t {
  None = 0,
  Nominal = 1,
  Validated = 2,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(CapacityBasis basis) noexcept;

class CCAP_EXPORT CapacityValue {
 public:
  CapacityValue() = default;

  [[nodiscard]] static CapacityValue known(ThermalPower amount);
  [[nodiscard]] static CapacityValue unknown(CapacityReason reason);
  [[nodiscard]] static CapacityValue unsupported(CapacityReason reason);
  [[nodiscard]] static CapacityValue unavailable(CapacityReason reason);

  [[nodiscard]] CapacityStatus status() const noexcept { return status_; }
  [[nodiscard]] CapacityReason reason() const noexcept { return reason_; }
  [[nodiscard]] bool is_known() const noexcept { return status_ == CapacityStatus::Known; }
  [[nodiscard]] bool is_unsupported() const noexcept {
    return status_ == CapacityStatus::Unsupported;
  }
  [[nodiscard]] bool is_unknown() const noexcept { return status_ == CapacityStatus::Unknown; }

  // The amount, or zero for every non-Known status. The name is deliberately
  // blunt: callers that must not treat Unknown as zero use `exact_amount`.
  [[nodiscard]] ThermalPower amount_or_zero() const noexcept { return amount_; }
  [[nodiscard]] Result<ThermalPower> exact_amount() const;

  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const CapacityValue& lhs, const CapacityValue& rhs) {
    return lhs.status_ == rhs.status_ && lhs.amount_ == rhs.amount_ && lhs.reason_ == rhs.reason_;
  }
  friend bool operator!=(const CapacityValue& lhs, const CapacityValue& rhs) {
    return !(lhs == rhs);
  }

 private:
  CapacityStatus status_ = CapacityStatus::Unknown;
  CapacityReason reason_ = CapacityReason::EvidenceMissing;
  ThermalPower amount_;
};

// Exact sum. A single Unknown contributor makes the whole sum Unknown; an
// Unsupported contributor makes it Unsupported unless a stronger reason (an
// Unknown) is present. Overflow is refused rather than wrapped.
[[nodiscard]] CCAP_EXPORT Result<CapacityValue> sum_values(const std::vector<CapacityValue>& parts);

// Exact minimum over values that are comparable. Unknown wins over every known
// value, because a lower bound is not a bound if part of the set is unknown.
[[nodiscard]] CCAP_EXPORT Result<CapacityValue> min_values(const std::vector<CapacityValue>& parts);

// A floor applied on top of a value: the smaller of the two, with Unknown
// propagating.
[[nodiscard]] CCAP_EXPORT Result<CapacityValue> apply_ceiling(CapacityValue value,
                                                              ThermalPower ceiling);

// The full accounting chain for one medium at one aggregation point.
struct CapacityBreakdown {
  CoolingMedium medium = CoolingMedium::Air;
  CapacityValue nominal;
  CapacityValue validated;
  CapacityValue derated;
  CapacityValue degraded;
  CapacityValue usable;
  CapacityValue committed;
  CapacityValue observed;
  // Usable capacity less the load actually observed. This is a physical
  // statement about the plant now; `reserve` is an authority statement about
  // what has been handed out.
  CapacityValue observed_headroom;
  CapacityValue reserve;
  CapacityValue deficit;
  bool over_committed = false;
  CapacityBasis basis = CapacityBasis::None;

  friend bool operator==(const CapacityBreakdown& lhs, const CapacityBreakdown& rhs) {
    return lhs.medium == rhs.medium && lhs.nominal == rhs.nominal &&
           lhs.validated == rhs.validated && lhs.derated == rhs.derated &&
           lhs.degraded == rhs.degraded && lhs.usable == rhs.usable &&
           lhs.committed == rhs.committed && lhs.observed == rhs.observed &&
           lhs.observed_headroom == rhs.observed_headroom && lhs.reserve == rhs.reserve &&
           lhs.deficit == rhs.deficit && lhs.over_committed == rhs.over_committed &&
           lhs.basis == rhs.basis;
  }
};

// Computes reserve and deficit from a usable and a committed figure, keeping the
// over-commitment visible instead of clamping it away. An Unknown usable value
// yields an Unknown reserve and a zero deficit with `over_committed` false,
// because over-commitment cannot be established from an unknown capacity.
[[nodiscard]] CCAP_EXPORT Result<void> finalise_breakdown(CapacityBreakdown& breakdown);

}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_ACCOUNTING_HPP
