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

#ifndef COOLING_CAPACITY_DIGEST_HPP
#define COOLING_CAPACITY_DIGEST_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "cooling_capacity/errors.hpp"
#include "cooling_capacity/export.hpp"

namespace cooling_capacity {

// An integrity digest. The default and only algorithm is SHA-256 (FIPS 180-4).
// The digest is an integrity check over a byte range, not an authenticity
// proof: it detects accidental damage and mismatched stores. Callers that need
// authenticity must protect the store directory itself.
class CCAP_EXPORT Digest {
 public:
  static constexpr std::size_t kSize = 32;

  Digest() = default;

  [[nodiscard]] static Digest of(std::string_view bytes);

  [[nodiscard]] static Result<Digest> parse_hex(std::string_view text);
  [[nodiscard]] std::string to_hex() const;

  [[nodiscard]] const std::array<std::uint8_t, kSize>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] bool is_zero() const noexcept;

  friend bool operator==(const Digest& lhs, const Digest& rhs) { return lhs.bytes_ == rhs.bytes_; }
  friend bool operator!=(const Digest& lhs, const Digest& rhs) { return !(lhs == rhs); }
  friend bool operator<(const Digest& lhs, const Digest& rhs) { return lhs.bytes_ < rhs.bytes_; }

 private:
  friend class Sha256;

  static Digest from_raw(const std::array<std::uint8_t, kSize>& bytes) {
    Digest digest;
    digest.bytes_ = bytes;
    return digest;
  }

  std::array<std::uint8_t, kSize> bytes_{};
};

// Streaming SHA-256. Used where the caller must not hold the whole input in
// memory at once.
class CCAP_EXPORT Sha256 {
 public:
  Sha256() { reset(); }

  void reset() noexcept;
  void update(std::string_view bytes) noexcept;
  [[nodiscard]] Digest finish() const noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
};

}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_DIGEST_HPP
