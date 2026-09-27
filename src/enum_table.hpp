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

#ifndef COOLING_CAPACITY_SRC_ENUM_TABLE_HPP
#define COOLING_CAPACITY_SRC_ENUM_TABLE_HPP

#include <cstddef>
#include <string>
#include <string_view>

#include "cooling_capacity/errors.hpp"

namespace cooling_capacity {
namespace detail {

// Internal helper: every enum in the public API has a total, stable name table
// and a strict parser. Unknown spellings are rejected; nothing is guessed and
// nothing is case-folded.
template <class Enum>
struct EnumName {
  Enum value;
  std::string_view name;
};

template <class Enum, std::size_t N>
Result<Enum> parse_from_table(const char* what, const EnumName<Enum> (&table)[N],
                              std::string_view text) {
  for (const EnumName<Enum>& entry : table) {
    if (entry.name == text) {
      return entry.value;
    }
  }
  return Error(ErrorCode::InvalidEnumValue, std::string("unknown ") + what)
      .with("text", std::string(text));
}

template <class Enum, std::size_t N>
std::string_view name_from_table(const EnumName<Enum> (&table)[N], Enum value) noexcept {
  for (const EnumName<Enum>& entry : table) {
    if (entry.value == value) {
      return entry.name;
    }
  }
  return "unrecognised";
}

}  // namespace detail
}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_SRC_ENUM_TABLE_HPP
