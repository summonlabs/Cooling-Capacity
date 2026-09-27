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

#include "cooling_capacity/errors.hpp"

#include <cstdio>
#include <cstdlib>

namespace cooling_capacity {

namespace {

struct CodeName {
  ErrorCode code;
  std::string_view name;
};

// The canonical names below are part of the public contract: they appear in
// machine-readable command line output and in tests.
constexpr CodeName kCodeNames[] = {
    {ErrorCode::Ok, "ok"},
    {ErrorCode::InvalidArgument, "invalid_argument"},
    {ErrorCode::InvalidIdentifier, "invalid_identifier"},
    {ErrorCode::InvalidEnumValue, "invalid_enum_value"},
    {ErrorCode::InvalidUtf8, "invalid_utf8"},
    {ErrorCode::InvalidText, "invalid_text"},
    {ErrorCode::DuplicateIdentity, "duplicate_identity"},
    {ErrorCode::MissingReference, "missing_reference"},
    {ErrorCode::DanglingReference, "dangling_reference"},
    {ErrorCode::SelfReference, "self_reference"},
    {ErrorCode::CyclicReference, "cyclic_reference"},
    {ErrorCode::NotFound, "not_found"},
    {ErrorCode::AlreadyExists, "already_exists"},
    {ErrorCode::Conflict, "conflict"},
    {ErrorCode::StaleGeneration, "stale_generation"},
    {ErrorCode::StaleAuthority, "stale_authority"},
    {ErrorCode::StaleEpoch, "stale_epoch"},
    {ErrorCode::StaleEvidence, "stale_evidence"},
    {ErrorCode::PreconditionFailed, "precondition_failed"},
    {ErrorCode::ReadOnly, "read_only"},
    {ErrorCode::NotOpen, "not_open"},
    {ErrorCode::Closed, "closed"},
    {ErrorCode::WriterBusy, "writer_busy"},
    {ErrorCode::IdempotencyConflict, "idempotency_conflict"},
    {ErrorCode::CapacityExceeded, "capacity_exceeded"},
    {ErrorCode::IncompatibleClass, "incompatible_class"},
    {ErrorCode::IncompatibleMedium, "incompatible_medium"},
    {ErrorCode::RedundancyUnsatisfied, "redundancy_unsatisfied"},
    {ErrorCode::ReserveViolation, "reserve_violation"},
    {ErrorCode::IncompatibleVersion, "incompatible_version"},
    {ErrorCode::Corruption, "corruption"},
    {ErrorCode::Truncated, "truncated"},
    {ErrorCode::Oversized, "oversized"},
    {ErrorCode::WrongEndianness, "wrong_endianness"},
    {ErrorCode::DigestMismatch, "digest_mismatch"},
    {ErrorCode::MissingManifest, "missing_manifest"},
    {ErrorCode::UnsupportedFormat, "unsupported_format"},
    {ErrorCode::LimitExceeded, "limit_exceeded"},
    {ErrorCode::ArithmeticOverflow, "arithmetic_overflow"},
    {ErrorCode::Unsupported, "unsupported"},
    {ErrorCode::Unavailable, "unavailable"},
    {ErrorCode::Unknown, "unknown"},
    {ErrorCode::Indeterminate, "indeterminate"},
    {ErrorCode::PermissionDenied, "permission_denied"},
    {ErrorCode::IoFailure, "io_failure"},
    {ErrorCode::LockConflict, "lock_conflict"},
    {ErrorCode::PathInvalid, "path_invalid"},
    {ErrorCode::ProcessFailure, "process_failure"},
    {ErrorCode::InvariantViolation, "invariant_violation"},
    {ErrorCode::InternalError, "internal_error"},
};

constexpr std::string_view kCategoryNames[] = {
    "ok", "invalid_argument", "precondition", "integrity", "resource", "environment", "internal",
};

}  // namespace

std::string_view to_string(ErrorCode code) noexcept {
  for (const CodeName& entry : kCodeNames) {
    if (entry.code == code) {
      return entry.name;
    }
  }
  return "unrecognised_error_code";
}

std::string_view to_string(ErrorCategory category) noexcept {
  const auto index = static_cast<std::size_t>(category);
  if (index < (sizeof(kCategoryNames) / sizeof(kCategoryNames[0]))) {
    return kCategoryNames[index];
  }
  return "unrecognised_error_category";
}

ErrorCategory category_of(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok:
      return ErrorCategory::Ok;
    case ErrorCode::InvalidArgument:
    case ErrorCode::InvalidIdentifier:
    case ErrorCode::InvalidEnumValue:
    case ErrorCode::InvalidUtf8:
    case ErrorCode::InvalidText:
    case ErrorCode::DuplicateIdentity:
    case ErrorCode::MissingReference:
    case ErrorCode::DanglingReference:
    case ErrorCode::SelfReference:
    case ErrorCode::CyclicReference:
      return ErrorCategory::InvalidArgument;
    case ErrorCode::NotFound:
    case ErrorCode::AlreadyExists:
    case ErrorCode::Conflict:
    case ErrorCode::StaleGeneration:
    case ErrorCode::StaleAuthority:
    case ErrorCode::StaleEpoch:
    case ErrorCode::StaleEvidence:
    case ErrorCode::PreconditionFailed:
    case ErrorCode::ReadOnly:
    case ErrorCode::NotOpen:
    case ErrorCode::Closed:
    case ErrorCode::WriterBusy:
    case ErrorCode::IdempotencyConflict:
    case ErrorCode::CapacityExceeded:
    case ErrorCode::IncompatibleClass:
    case ErrorCode::IncompatibleMedium:
    case ErrorCode::RedundancyUnsatisfied:
    case ErrorCode::ReserveViolation:
      return ErrorCategory::Precondition;
    case ErrorCode::IncompatibleVersion:
    case ErrorCode::Corruption:
    case ErrorCode::Truncated:
    case ErrorCode::Oversized:
    case ErrorCode::WrongEndianness:
    case ErrorCode::DigestMismatch:
    case ErrorCode::MissingManifest:
    case ErrorCode::UnsupportedFormat:
      return ErrorCategory::Integrity;
    case ErrorCode::LimitExceeded:
    case ErrorCode::ArithmeticOverflow:
    case ErrorCode::Unsupported:
    case ErrorCode::Unavailable:
    case ErrorCode::Unknown:
    case ErrorCode::Indeterminate:
      return ErrorCategory::Resource;
    case ErrorCode::PermissionDenied:
    case ErrorCode::IoFailure:
    case ErrorCode::LockConflict:
    case ErrorCode::PathInvalid:
    case ErrorCode::ProcessFailure:
      return ErrorCategory::Environment;
    case ErrorCode::InvariantViolation:
    case ErrorCode::InternalError:
      return ErrorCategory::Internal;
  }
  return ErrorCategory::Internal;
}

Error& Error::with(std::string key, std::string value) {
  context_.push_back(ErrorContext{std::move(key), std::move(value)});
  return *this;
}

Error& Error::with_generations(std::uint64_t expected, std::uint64_t actual) {
  expected_generation_ = expected;
  actual_generation_ = actual;
  return *this;
}

Error Error::copy_with(std::string key, std::string value) const {
  Error copy = *this;
  copy.with(std::move(key), std::move(value));
  return copy;
}

std::string Error::to_string() const {
  std::string out;
  out.reserve(message_.size() + 64);
  out.append(cooling_capacity::to_string(code_));
  out.append(": ");
  out.append(message_);
  bool first = true;
  for (const ErrorContext& entry : context_) {
    out.append(first ? " [" : ", ");
    first = false;
    out.append(entry.key);
    out.push_back('=');
    out.append(entry.value);
  }
  if (expected_generation_.has_value() || actual_generation_.has_value()) {
    out.append(first ? " [" : ", ");
    first = false;
    out.append("expected_generation=");
    out.append(expected_generation_.has_value() ? std::to_string(*expected_generation_) : "none");
    out.append(", actual_generation=");
    out.append(actual_generation_.has_value() ? std::to_string(*actual_generation_) : "none");
  }
  if (!first) {
    out.push_back(']');
  }
  return out;
}

Error Error::invalid_argument(std::string message) {
  return Error(ErrorCode::InvalidArgument, std::move(message));
}

Error Error::not_found(std::string message) {
  return Error(ErrorCode::NotFound, std::move(message));
}

Error Error::stale_generation(std::string message, std::uint64_t expected, std::uint64_t actual) {
  Error error(ErrorCode::StaleGeneration, std::move(message));
  error.with_generations(expected, actual);
  return error;
}

Error Error::corruption(std::string message) {
  return Error(ErrorCode::Corruption, std::move(message));
}

Error Error::limit_exceeded(std::string message) {
  return Error(ErrorCode::LimitExceeded, std::move(message));
}

Error Error::unsupported(std::string message) {
  return Error(ErrorCode::Unsupported, std::move(message));
}

Error Error::invariant(std::string message) {
  return Error(ErrorCode::InvariantViolation, std::move(message));
}

namespace detail {

void contract_violation(const char* expression, const char* file, int line) noexcept {
  std::fprintf(stderr, "cooling_capacity: contract violation: %s (%s:%d)\n", expression, file, line);
  std::fflush(stderr);
  std::abort();
}

}  // namespace detail

}  // namespace cooling_capacity
