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

#include "cooling_capacity/version.hpp"

#include "cooling_capacity/canonical.hpp"

namespace cooling_capacity {

std::string_view version_string() noexcept { return "1.0.0"; }

std::uint32_t artifact_format_version() noexcept { return kArtifactFormatVersion; }

Result<Version> Version::parse(std::string_view text) {
  const std::size_t first = text.find('.');
  if (first == std::string_view::npos) {
    return Error(ErrorCode::InvalidArgument, "version must have the form MAJOR.MINOR.PATCH")
        .with("text", std::string(text));
  }
  const std::size_t second = text.find('.', first + 1);
  if (second == std::string_view::npos) {
    return Error(ErrorCode::InvalidArgument, "version must have the form MAJOR.MINOR.PATCH")
        .with("text", std::string(text));
  }
  if (text.find('.', second + 1) != std::string_view::npos) {
    return Error(ErrorCode::InvalidArgument, "version has more than three components")
        .with("text", std::string(text));
  }
  CCAP_TRY_DECLARE(major, parse_uint32(text.substr(0, first)));
  CCAP_TRY_DECLARE(minor, parse_uint32(text.substr(first + 1, second - first - 1)));
  CCAP_TRY_DECLARE(patch, parse_uint32(text.substr(second + 1)));
  Version version;
  version.major = static_cast<int>(major);
  version.minor = static_cast<int>(minor);
  version.patch = static_cast<int>(patch);
  return version;
}

std::string Version::to_string() const {
  return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

Version library_version() noexcept { return Version{kVersionMajor, kVersionMinor, kVersionPatch}; }

}  // namespace cooling_capacity
