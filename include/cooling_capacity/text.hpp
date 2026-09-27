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

#ifndef COOLING_CAPACITY_TEXT_HPP
#define COOLING_CAPACITY_TEXT_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "cooling_capacity/errors.hpp"
#include "cooling_capacity/export.hpp"

namespace cooling_capacity {

// Bounds that apply to every externally supplied text field. They are checked
// before any allocation that depends on the input length.
inline constexpr std::size_t kMaxIdentifierLength = 63;
inline constexpr std::size_t kMaxTextLength = 512;
inline constexpr std::size_t kMaxDocumentReferenceLength = 127;

// Strict UTF-8 validation as defined by RFC 3629: rejects overlong encodings,
// UTF-16 surrogate code points and code points above U+10FFFF.
[[nodiscard]] CCAP_EXPORT bool is_valid_utf8(std::string_view text) noexcept;

// True when the text contains no C0 control characters, no DEL and no C1
// control characters. Combining marks and non-ASCII printable text are allowed.
[[nodiscard]] CCAP_EXPORT bool is_printable_text(std::string_view text) noexcept;

// Canonical unsigned decimal. Rejects sign characters, whitespace, underscores,
// leading zeros (except for the single digit "0") and anything that does not
// fit in 64 bits.
[[nodiscard]] CCAP_EXPORT Result<std::uint64_t> parse_uint64(std::string_view text);
[[nodiscard]] CCAP_EXPORT Result<std::uint32_t> parse_uint32(std::string_view text);

// Canonical signed decimal for the small set of places signed values are
// legal. Same strictness as parse_uint64, plus an optional leading '-'.
[[nodiscard]] CCAP_EXPORT Result<std::int64_t> parse_int64(std::string_view text);

[[nodiscard]] CCAP_EXPORT std::string to_hex(std::string_view bytes);
[[nodiscard]] CCAP_EXPORT Result<std::string> parse_hex(std::string_view text);

// An identifier is a non-empty token of at most kMaxIdentifierLength bytes
// drawn from [A-Za-z0-9._:-] and starting with an alphanumeric character. The
// grammar deliberately excludes path separators, whitespace, quoting
// characters and a leading dot, so an identifier can never be a path fragment.
class CCAP_EXPORT Identifier {
 public:
  Identifier() = default;

  [[nodiscard]] static Result<Identifier> parse(std::string_view text);

  // For identifiers that are already correct because they appear as literals in
  // source. Violations are programming errors and terminate the process. Never
  // call this with external input.
  [[nodiscard]] static Identifier literal(std::string_view text);

  [[nodiscard]] const std::string& str() const noexcept { return value_; }
  [[nodiscard]] std::string_view view() const noexcept { return value_; }
  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

  friend bool operator==(const Identifier& lhs, const Identifier& rhs) {
    return lhs.value_ == rhs.value_;
  }
  friend bool operator!=(const Identifier& lhs, const Identifier& rhs) { return !(lhs == rhs); }
  friend bool operator<(const Identifier& lhs, const Identifier& rhs) {
    return lhs.value_ < rhs.value_;
  }
  friend bool operator>(const Identifier& lhs, const Identifier& rhs) { return rhs < lhs; }
  friend bool operator<=(const Identifier& lhs, const Identifier& rhs) { return !(rhs < lhs); }
  friend bool operator>=(const Identifier& lhs, const Identifier& rhs) { return !(lhs < rhs); }

  [[nodiscard]] std::string to_string() const { return value_; }

 private:
  std::string value_;
};

// Free-form annotation text. Validated as printable UTF-8 and bounded in bytes.
class CCAP_EXPORT BoundedText {
 public:
  BoundedText() = default;

  [[nodiscard]] static Result<BoundedText> parse(std::string_view text);
  [[nodiscard]] static BoundedText literal(std::string_view text);

  [[nodiscard]] const std::string& str() const noexcept { return value_; }
  [[nodiscard]] std::string_view view() const noexcept { return value_; }
  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

  friend bool operator==(const BoundedText& lhs, const BoundedText& rhs) {
    return lhs.value_ == rhs.value_;
  }
  friend bool operator!=(const BoundedText& lhs, const BoundedText& rhs) { return !(lhs == rhs); }
  friend bool operator<(const BoundedText& lhs, const BoundedText& rhs) {
    return lhs.value_ < rhs.value_;
  }

 private:
  std::string value_;
};

// A document, ticket or order reference emitted by an external system. Longer
// than an identifier, same restrictive character set plus '/', which appears in
// real change and incident references.
class CCAP_EXPORT DocumentRef {
 public:
  DocumentRef() = default;

  [[nodiscard]] static Result<DocumentRef> parse(std::string_view text);
  [[nodiscard]] static DocumentRef literal(std::string_view text);

  [[nodiscard]] const std::string& str() const noexcept { return value_; }
  [[nodiscard]] std::string_view view() const noexcept { return value_; }
  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

  friend bool operator==(const DocumentRef& lhs, const DocumentRef& rhs) {
    return lhs.value_ == rhs.value_;
  }
  friend bool operator!=(const DocumentRef& lhs, const DocumentRef& rhs) { return !(lhs == rhs); }
  friend bool operator<(const DocumentRef& lhs, const DocumentRef& rhs) {
    return lhs.value_ < rhs.value_;
  }

 private:
  std::string value_;
};

}  // namespace cooling_capacity

namespace std {

template <>
struct hash<cooling_capacity::Identifier> {
  std::size_t operator()(const cooling_capacity::Identifier& id) const noexcept {
    return std::hash<std::string_view>{}(id.view());
  }
};

template <>
struct hash<cooling_capacity::BoundedText> {
  std::size_t operator()(const cooling_capacity::BoundedText& text) const noexcept {
    return std::hash<std::string_view>{}(text.view());
  }
};

template <>
struct hash<cooling_capacity::DocumentRef> {
  std::size_t operator()(const cooling_capacity::DocumentRef& ref) const noexcept {
    return std::hash<std::string_view>{}(ref.view());
  }
};

}  // namespace std

#endif  // COOLING_CAPACITY_TEXT_HPP
