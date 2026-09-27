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

#include "cooling_capacity/limits.hpp"

namespace cooling_capacity {

Result<void> Limits::validate() const {
  const auto reject_if_zero = [](std::size_t value, const char* field) -> Result<void> {
    if (value == 0) {
      return Error(ErrorCode::InvalidArgument, "limit must be greater than zero")
          .with("field", field);
    }
    return Result<void>();
  };

  CCAP_TRY(reject_if_zero(max_sites, "max_sites"));
  CCAP_TRY(reject_if_zero(max_facilities, "max_facilities"));
  CCAP_TRY(reject_if_zero(max_zones, "max_zones"));
  CCAP_TRY(reject_if_zero(max_loops, "max_loops"));
  CCAP_TRY(reject_if_zero(max_plants, "max_plants"));
  CCAP_TRY(reject_if_zero(max_manifolds, "max_manifolds"));
  CCAP_TRY(reject_if_zero(max_domains, "max_domains"));
  CCAP_TRY(reject_if_zero(max_equipment, "max_equipment"));
  CCAP_TRY(reject_if_zero(max_evidence, "max_evidence"));
  CCAP_TRY(reject_if_zero(max_commitments, "max_commitments"));
  CCAP_TRY(reject_if_zero(max_points_of_connection, "max_points_of_connection"));
  CCAP_TRY(reject_if_zero(max_loops_per_zone, "max_loops_per_zone"));
  CCAP_TRY(reject_if_zero(max_equipment_per_loop, "max_equipment_per_loop"));
  CCAP_TRY(reject_if_zero(max_derate_factors, "max_derate_factors"));
  CCAP_TRY(reject_if_zero(max_freshness_rules, "max_freshness_rules"));
  CCAP_TRY(reject_if_zero(max_generations_retained, "max_generations_retained"));

  if (max_artifact_bytes < 4096ULL) {
    return Error(ErrorCode::InvalidArgument,
                 "max_artifact_bytes is too small to hold even an empty artifact")
        .with("max_artifact_bytes", std::to_string(max_artifact_bytes));
  }
  if (max_artifact_bytes > (1ULL << 34)) {
    return Error(ErrorCode::InvalidArgument,
                 "max_artifact_bytes exceeds the hard ceiling of 16 GiB")
        .with("max_artifact_bytes", std::to_string(max_artifact_bytes));
  }
  if (max_writer_epoch == 0) {
    return Error(ErrorCode::InvalidArgument, "max_writer_epoch must be greater than zero");
  }
  if (max_generations_retained > 4096) {
    return Error(ErrorCode::InvalidArgument,
                 "max_generations_retained exceeds the hard ceiling of 4096")
        .with("max_generations_retained", std::to_string(max_generations_retained));
  }
  return Result<void>();
}

}  // namespace cooling_capacity
