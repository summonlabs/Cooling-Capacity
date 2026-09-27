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

#include "cooling_capacity/text.hpp"

#include <array>
#include <cctype>
#include <limits>

namespace cooling_capacity {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

bool is_hex_digit(char value) noexcept {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
         (value >= 'A' && value <= 'F');
}

std::uint8_t hex_value(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return static_cast<std::uint8_t>(value - '0');
  }
  if (value >= 'a' && value <= 'f') {
    return static_cast<std::uint8_t>(value - 'a' + 10);
  }
  return static_cast<std::uint8_t>(value - 'A' + 10);
}

bool is_identifier_start(char value) noexcept {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
         (value >= '0' && value <= '9');
}

bool is_identifier_body(char value) noexcept {
  return is_identifier_start(value) || value == '.' || value == '_' || value == ':' || value == '-';
}

bool is_document_body(char value) noexcept {
  return is_identifier_body(value) || value == '/' || value == '#' || value == '@' || value == '+';
}

}  // namespace

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  const std::size_t size = text.size();
  while (index < size) {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t extra = 0;
    std::uint32_t code_point = 0;
    if (lead < 0x80U) {
      ++index;
      continue;
    } else if ((lead & 0xE0U) == 0xC0U) {
      extra = 1;
      code_point = lead & 0x1FU;
    } else if ((lead & 0xF0U) == 0xE0U) {
      extra = 2;
      code_point = lead & 0x0FU;
    } else if ((lead & 0xF8U) == 0xF0U) {
      extra = 3;
      code_point = lead & 0x07U;
    } else {
      return false;
    }
    if (index + extra >= size) {
      return false;
    }
    for (std::size_t offset = 1; offset <= extra; ++offset) {
      const auto continuation = static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xC0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (continuation & 0x3FU);
    }
    // Overlong encodings, surrogates and out-of-range code points.
    if (extra == 1 && code_point < 0x80U) {
      return false;
    }
    if (extra == 2 && code_point < 0x800U) {
      return false;
    }
    if (extra == 3 && code_point < 0x10000U) {
      return false;
    }
    if (code_point > 0x10FFFFU) {
      return false;
    }
    if (code_point >= 0xD800U && code_point <= 0xDFFFU) {
      return false;
    }
    index += extra + 1;
  }
  return true;
}

bool is_printable_text(std::string_view text) noexcept {
  if (!is_valid_utf8(text)) {
    return false;
  }
  // Decode again and reject control code points. Checking raw bytes here would
  // wrongly reject the continuation bytes 0x80..0x9F of ordinary characters
  // such as U+0100.
  std::size_t index = 0;
  const std::size_t size = text.size();
  while (index < size) {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t extra = 0;
    std::uint32_t code_point = 0;
    if (lead < 0x80U) {
      code_point = lead;
    } else if ((lead & 0xE0U) == 0xC0U) {
      extra = 1;
      code_point = lead & 0x1FU;
    } else if ((lead & 0xF0U) == 0xE0U) {
      extra = 2;
      code_point = lead & 0x0FU;
    } else {
      extra = 3;
      code_point = lead & 0x07U;
    }
    for (std::size_t offset = 1; offset <= extra; ++offset) {
      const auto continuation = static_cast<unsigned char>(text[index + offset]);
      code_point = (code_point << 6U) | (continuation & 0x3FU);
    }
    if (code_point < 0x20U || code_point == 0x7FU) {
      return false;
    }
    if (code_point >= 0x80U && code_point <= 0x9FU) {
      return false;
    }
    index += extra + 1;
  }
  return true;
}

Result<std::uint64_t> parse_uint64(std::string_view text) {
  if (text.empty()) {
    return Error(ErrorCode::InvalidArgument, "decimal text is empty");
  }
  if (text.size() > 20) {
    return Error(ErrorCode::InvalidArgument, "decimal text is longer than 20 digits")
        .with("length", std::to_string(text.size()));
  }
  if (text.size() > 1 && text.front() == '0') {
    return Error(ErrorCode::InvalidArgument, "decimal text has a leading zero")
        .with("text", std::string(text));
  }
  std::uint64_t value = 0;
  for (const char raw : text) {
    if (raw < '0' || raw > '9') {
      return Error(ErrorCode::InvalidArgument, "decimal text contains a non-digit")
          .with("text", std::string(text));
    }
    const auto digit = static_cast<std::uint64_t>(raw - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
      return Error(ErrorCode::ArithmeticOverflow, "decimal text does not fit in 64 bits")
          .with("text", std::string(text));
    }
    value = value * 10U + digit;
  }
  return value;
}

Result<std::uint32_t> parse_uint32(std::string_view text) {
  CCAP_TRY_DECLARE(value, parse_uint64(text));
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    return Error(ErrorCode::ArithmeticOverflow, "decimal text does not fit in 32 bits")
        .with("text", std::string(text));
  }
  return static_cast<std::uint32_t>(value);
}

Result<std::int64_t> parse_int64(std::string_view text) {
  if (text.empty()) {
    return Error(ErrorCode::InvalidArgument, "decimal text is empty");
  }
  bool negative = false;
  std::string_view digits = text;
  if (text.front() == '-') {
    negative = true;
    digits = text.substr(1);
    if (digits.empty()) {
      return Error(ErrorCode::InvalidArgument, "decimal text is a lone sign");
    }
  }
  CCAP_TRY_DECLARE(magnitude, parse_uint64(digits));
  if (negative) {
    constexpr auto min_magnitude =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;
    if (magnitude > min_magnitude) {
      return Error(ErrorCode::ArithmeticOverflow, "decimal text does not fit in 64 bits")
          .with("text", std::string(text));
    }
    if (magnitude == min_magnitude) {
      return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(magnitude);
  }
  if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return Error(ErrorCode::ArithmeticOverflow, "decimal text does not fit in 64 bits")
        .with("text", std::string(text));
  }
  return static_cast<std::int64_t>(magnitude);
}

std::string to_hex(std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size() * 2U);
  for (const char raw : bytes) {
    const auto value = static_cast<unsigned char>(raw);
    out.push_back(kHexDigits[value >> 4U]);
    out.push_back(kHexDigits[value & 0x0FU]);
  }
  return out;
}

Result<std::string> parse_hex(std::string_view text) {
  if (text.size() % 2U != 0U) {
    return Error(ErrorCode::InvalidArgument, "hex text has an odd number of digits")
        .with("length", std::to_string(text.size()));
  }
  std::string out;
  out.reserve(text.size() / 2U);
  for (std::size_t index = 0; index < text.size(); index += 2U) {
    if (!is_hex_digit(text[index]) || !is_hex_digit(text[index + 1U])) {
      return Error(ErrorCode::InvalidArgument, "hex text contains a non-hex digit");
    }
    const auto high = hex_value(text[index]);
    const auto low = hex_value(text[index + 1U]);
    out.push_back(static_cast<char>((high << 4U) | low));
  }
  return out;
}

Result<Identifier> Identifier::parse(std::string_view text) {
  if (text.empty()) {
    return Error(ErrorCode::InvalidIdentifier, "identifier is empty");
  }
  if (text.size() > kMaxIdentifierLength) {
    return Error(ErrorCode::InvalidIdentifier, "identifier is longer than the maximum length")
        .with("length", std::to_string(text.size()))
        .with("maximum", std::to_string(kMaxIdentifierLength));
  }
  if (!is_identifier_start(text.front())) {
    return Error(ErrorCode::InvalidIdentifier,
                 "identifier must start with an ASCII letter or digit")
        .with("text", std::string(text));
  }
  for (const char raw : text) {
    if (!is_identifier_body(raw)) {
      return Error(ErrorCode::InvalidIdentifier,
                   "identifier contains a character outside [A-Za-z0-9._:-]")
          .with("text", std::string(text));
    }
  }
  Identifier identifier;
  identifier.value_.assign(text);
  return identifier;
}

Identifier Identifier::literal(std::string_view text) {
  Result<Identifier> parsed = Identifier::parse(text);
  if (!parsed.ok()) {
    detail::contract_violation("Identifier::literal with an invalid literal", __FILE__, __LINE__);
  }
  return std::move(parsed).value();
}

Result<BoundedText> BoundedText::parse(std::string_view text) {
  if (text.size() > kMaxTextLength) {
    return Error(ErrorCode::InvalidText, "text is longer than the maximum length")
        .with("length", std::to_string(text.size()))
        .with("maximum", std::to_string(kMaxTextLength));
  }
  if (!is_valid_utf8(text)) {
    return Error(ErrorCode::InvalidUtf8, "text is not valid UTF-8");
  }
  if (!is_printable_text(text)) {
    return Error(ErrorCode::InvalidText,
                 "text contains a control character; annotations must be printable");
  }
  BoundedText bounded;
  bounded.value_.assign(text);
  return bounded;
}

BoundedText BoundedText::literal(std::string_view text) {
  Result<BoundedText> parsed = BoundedText::parse(text);
  if (!parsed.ok()) {
    detail::contract_violation("BoundedText::literal with an invalid literal", __FILE__, __LINE__);
  }
  return std::move(parsed).value();
}

Result<DocumentRef> DocumentRef::parse(std::string_view text) {
  if (text.empty()) {
    return Error(ErrorCode::InvalidArgument, "document reference is empty");
  }
  if (text.size() > kMaxDocumentReferenceLength) {
    return Error(ErrorCode::InvalidArgument,
                 "document reference is longer than the maximum length")
        .with("length", std::to_string(text.size()))
        .with("maximum", std::to_string(kMaxDocumentReferenceLength));
  }
  if (!is_identifier_start(text.front())) {
    return Error(ErrorCode::InvalidArgument,
                 "document reference must start with an ASCII letter or digit")
        .with("text", std::string(text));
  }
  for (const char raw : text) {
    if (!is_document_body(raw)) {
      return Error(ErrorCode::InvalidArgument,
                   "document reference contains an unsupported character")
          .with("text", std::string(text));
    }
  }
  DocumentRef reference;
  reference.value_.assign(text);
  return reference;
}

DocumentRef DocumentRef::literal(std::string_view text) {
  Result<DocumentRef> parsed = DocumentRef::parse(text);
  if (!parsed.ok()) {
    detail::contract_violation("DocumentRef::literal with an invalid literal", __FILE__, __LINE__);
  }
  return std::move(parsed).value();
}

}  // namespace cooling_capacity
