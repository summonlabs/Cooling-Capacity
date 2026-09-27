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

#include "cooling_capacity/digest.hpp"

#include <cstring>

#include "cooling_capacity/text.hpp"

namespace cooling_capacity {

namespace {

// First 32 bits of the fractional parts of the cube roots of the first 64
// primes, as specified by FIPS 180-4.
constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

constexpr std::uint32_t rotr(std::uint32_t value, unsigned count) noexcept {
  return (value >> count) | (value << (32U - count));
}

}  // namespace

void Sha256::reset() noexcept {
  state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  buffer_.fill(0);
  buffered_ = 0;
  total_bytes_ = 0;
}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::uint32_t schedule[64];
  for (std::size_t index = 0; index < 16; ++index) {
    schedule[index] = (static_cast<std::uint32_t>(block[index * 4U]) << 24U) |
                      (static_cast<std::uint32_t>(block[index * 4U + 1U]) << 16U) |
                      (static_cast<std::uint32_t>(block[index * 4U + 2U]) << 8U) |
                      static_cast<std::uint32_t>(block[index * 4U + 3U]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t s0 = rotr(schedule[index - 15], 7) ^ rotr(schedule[index - 15], 18) ^
                             (schedule[index - 15] >> 3U);
    const std::uint32_t s1 = rotr(schedule[index - 2], 17) ^ rotr(schedule[index - 2], 19) ^
                             (schedule[index - 2] >> 10U);
    schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t choose = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + choose + kRoundConstants[index] + schedule[index];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::string_view bytes) noexcept {
  total_bytes_ += bytes.size();
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t room = 64U - buffered_;
    const std::size_t take = (bytes.size() - offset) < room ? (bytes.size() - offset) : room;
    std::memcpy(buffer_.data() + buffered_, bytes.data() + offset, take);
    buffered_ += take;
    offset += take;
    if (buffered_ == 64U) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
}

Digest Sha256::finish() const noexcept {
  Sha256 scratch = *this;
  const std::uint64_t bit_length = scratch.total_bytes_ * 8U;

  // The padding is 0x80, then zeros, then the 64-bit big-endian bit length. It
  // occupies one block when fewer than 56 bytes are buffered and two otherwise.
  std::uint8_t padding[72] = {};
  const std::size_t remaining = scratch.buffered_;
  const std::size_t pad_length = remaining < 56U ? (56U - remaining) : (120U - remaining);
  padding[0] = 0x80U;
  for (std::size_t index = 0; index < 8U; ++index) {
    padding[pad_length + index] =
        static_cast<std::uint8_t>((bit_length >> (8U * (7U - index))) & 0xFFU);
  }
  scratch.update(std::string_view(reinterpret_cast<const char*>(padding), pad_length + 8U));

  std::array<std::uint8_t, Digest::kSize> bytes{};
  for (std::size_t index = 0; index < 8U; ++index) {
    bytes[index * 4U] = static_cast<std::uint8_t>((scratch.state_[index] >> 24U) & 0xFFU);
    bytes[index * 4U + 1U] = static_cast<std::uint8_t>((scratch.state_[index] >> 16U) & 0xFFU);
    bytes[index * 4U + 2U] = static_cast<std::uint8_t>((scratch.state_[index] >> 8U) & 0xFFU);
    bytes[index * 4U + 3U] = static_cast<std::uint8_t>(scratch.state_[index] & 0xFFU);
  }
  return Digest::from_raw(bytes);
}

Digest Digest::of(std::string_view bytes) {
  Sha256 hasher;
  hasher.update(bytes);
  return hasher.finish();
}

Result<Digest> Digest::parse_hex(std::string_view text) {
  if (text.size() != kSize * 2U) {
    return Error(ErrorCode::InvalidArgument, "digest text must be exactly 64 hex digits")
        .with("length", std::to_string(text.size()));
  }
  CCAP_TRY_DECLARE(raw, cooling_capacity::parse_hex(text));
  Digest digest;
  for (std::size_t index = 0; index < kSize; ++index) {
    digest.bytes_[index] = static_cast<std::uint8_t>(raw[index]);
  }
  return digest;
}

std::string Digest::to_hex() const {
  return cooling_capacity::to_hex(std::string_view(reinterpret_cast<const char*>(bytes_.data()),
                                                   bytes_.size()));
}

bool Digest::is_zero() const noexcept {
  for (const std::uint8_t byte : bytes_) {
    if (byte != 0U) {
      return false;
    }
  }
  return true;
}

}  // namespace cooling_capacity
