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

#ifndef COOLING_CAPACITY_VERSION_HPP
#define COOLING_CAPACITY_VERSION_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "cooling_capacity/errors.hpp"
#include "cooling_capacity/export.hpp"

namespace cooling_capacity {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;

// "1.0.0".
[[nodiscard]] CCAP_EXPORT std::string_view version_string() noexcept;

// The version of the canonical artifact format this build reads and writes.
[[nodiscard]] CCAP_EXPORT std::uint32_t artifact_format_version() noexcept;

struct Version {
  int major = 0;
  int minor = 0;
  int patch = 0;

  [[nodiscard]] static Result<Version> parse(std::string_view text);
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const Version& lhs, const Version& rhs) {
    return lhs.major == rhs.major && lhs.minor == rhs.minor && lhs.patch == rhs.patch;
  }
  friend bool operator!=(const Version& lhs, const Version& rhs) { return !(lhs == rhs); }
  friend bool operator<(const Version& lhs, const Version& rhs) {
    if (lhs.major != rhs.major) {
      return lhs.major < rhs.major;
    }
    if (lhs.minor != rhs.minor) {
      return lhs.minor < rhs.minor;
    }
    return lhs.patch < rhs.patch;
  }
};

[[nodiscard]] CCAP_EXPORT Version library_version() noexcept;

}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_VERSION_HPP
