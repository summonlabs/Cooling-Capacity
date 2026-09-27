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

#include "cooling_capacity/commitment.hpp"

#include "enum_table.hpp"

namespace cooling_capacity {

namespace {

constexpr detail::EnumName<CommitmentState> kCommitmentStateNames[] = {
    {CommitmentState::Planned, "planned"},     {CommitmentState::Held, "held"},
    {CommitmentState::Committed, "committed"}, {CommitmentState::Released, "released"},
    {CommitmentState::Expired, "expired"},     {CommitmentState::Superseded, "superseded"},
};

constexpr detail::EnumName<CommitmentOperation> kOperationNames[] = {
    {CommitmentOperation::Plan, "plan"},
    {CommitmentOperation::Hold, "hold"},
    {CommitmentOperation::Commit, "commit"},
    {CommitmentOperation::Release, "release"},
    {CommitmentOperation::Expire, "expire"},
};

}  // namespace

std::string_view to_string(CommitmentState state) noexcept {
  return detail::name_from_table(kCommitmentStateNames, state);
}

Result<CommitmentState> parse_commitment_state(std::string_view text) {
  return detail::parse_from_table("commitment state", kCommitmentStateNames, text);
}

bool consumes_capacity(CommitmentState state) noexcept {
  return state == CommitmentState::Held || state == CommitmentState::Committed;
}

std::string_view to_string(CommitmentOperation operation) noexcept {
  return detail::name_from_table(kOperationNames, operation);
}

Result<CommitmentOperation> parse_commitment_operation(std::string_view text) {
  return detail::parse_from_table("commitment operation", kOperationNames, text);
}

CommitmentState resulting_state(CommitmentOperation operation) noexcept {
  switch (operation) {
    case CommitmentOperation::Plan:
      return CommitmentState::Planned;
    case CommitmentOperation::Hold:
      return CommitmentState::Held;
    case CommitmentOperation::Commit:
      return CommitmentState::Committed;
    case CommitmentOperation::Release:
      return CommitmentState::Released;
    case CommitmentOperation::Expire:
      return CommitmentState::Expired;
  }
  return CommitmentState::Planned;
}

bool CommitmentRecord::same_request_as(const CommitmentRecord& other) const {
  return attempt == other.attempt && zone == other.zone && pinned_loop == other.pinned_loop &&
         medium == other.medium && compatibility == other.compatibility &&
         thermal == other.thermal && expires_at == other.expires_at && note == other.note;
}

bool operator==(const CommitmentRecord& lhs, const CommitmentRecord& rhs) {
  return lhs.id == rhs.id && lhs.attempt == rhs.attempt && lhs.zone == rhs.zone &&
         lhs.pinned_loop == rhs.pinned_loop && lhs.medium == rhs.medium &&
         lhs.compatibility == rhs.compatibility && lhs.thermal == rhs.thermal &&
         lhs.state == rhs.state && lhs.actor == rhs.actor && lhs.created_at == rhs.created_at &&
         lhs.expires_at == rhs.expires_at &&
         lhs.created_generation == rhs.created_generation && lhs.supersedes == rhs.supersedes &&
         lhs.note == rhs.note && lhs.revision == rhs.revision;
}

Result<void> CommitmentRequest::validate() const {
  if (id.empty()) {
    return Error(ErrorCode::InvalidArgument, "commitment request has no commitment identifier");
  }
  if (attempt.empty()) {
    return Error(ErrorCode::InvalidArgument, "commitment request has no attempt identifier");
  }
  if (actor.empty()) {
    return Error(ErrorCode::InvalidArgument, "commitment request has no actor");
  }
  if (zone.empty()) {
    return Error(ErrorCode::InvalidArgument, "commitment request has no zone");
  }
  if (expected_generation.value() == 0) {
    return Error(ErrorCode::InvalidArgument,
                 "commitment request must name the generation it was formed against");
  }
  if (medium_of(compatibility) != medium) {
    return Error(ErrorCode::IncompatibleMedium,
                 "commitment compatibility class does not belong to the requested medium")
        .with("medium", std::string(to_string(medium)))
        .with("class", std::string(to_string(compatibility)));
  }
  switch (operation) {
    case CommitmentOperation::Plan:
    case CommitmentOperation::Hold:
    case CommitmentOperation::Commit:
      if (thermal.is_negative() || thermal.is_zero()) {
        return Error(ErrorCode::InvalidArgument,
                     "a commitment that consumes capacity must ask for a positive load")
            .with("thermal_milliwatts", std::to_string(thermal.milliwatts()));
      }
      break;
    case CommitmentOperation::Release:
    case CommitmentOperation::Expire:
      if (!target.has_value()) {
        return Error(ErrorCode::InvalidArgument,
                     "releasing or expiring a commitment requires a target commitment");
      }
      break;
  }
  if (expires_at.has_value() && *expires_at < requested_at) {
    return Error(ErrorCode::InvalidArgument,
                 "commitment expiry is before the instant the request was formed")
        .with("requested_at", requested_at.to_string())
        .with("expires_at", expires_at->to_string());
  }
  return Result<void>();
}

bool operator==(const CommitmentOutcome& lhs, const CommitmentOutcome& rhs) {
  return lhs.id == rhs.id && lhs.attempt == rhs.attempt && lhs.state == rhs.state &&
         lhs.replayed == rhs.replayed && lhs.generation == rhs.generation &&
         lhs.committed_after == rhs.committed_after && lhs.usable_after == rhs.usable_after &&
         lhs.reserve_after == rhs.reserve_after;
}

}  // namespace cooling_capacity
