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

#ifndef COOLING_CAPACITY_CLOCK_HPP
#define COOLING_CAPACITY_CLOCK_HPP

#include "cooling_capacity/export.hpp"
#include "cooling_capacity/units.hpp"

namespace cooling_capacity {

// Time enters the library only through this interface. Nothing reads the
// system clock implicitly, so every freshness decision in a test is
// reproducible and every evaluation can name the instant it used.
class CCAP_EXPORT Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock();

  [[nodiscard]] virtual Timestamp now() const = 0;
};

// Wall-clock time from the host. Used by the command line tool and by examples.
class CCAP_EXPORT SystemClock final : public Clock {
 public:
  SystemClock() = default;
  [[nodiscard]] Timestamp now() const override;
};

// A clock the caller moves by hand. Used by tests, by the benchmark harness and
// by recovery scenarios that need to prove evidence does not silently become
// fresh.
class CCAP_EXPORT ManualClock final : public Clock {
 public:
  ManualClock() = default;
  explicit ManualClock(Timestamp start) : current_(start) {}

  [[nodiscard]] Timestamp now() const override { return current_; }

  void set(Timestamp instant) { current_ = instant; }
  void advance(DurationMs delta) {
    current_ = Timestamp::from_unix_milliseconds(current_.unix_milliseconds() +
                                                 delta.milliseconds());
  }

 private:
  Timestamp current_{};
};

// Parses and formats the canonical UTC timestamp form "YYYY-MM-DDTHH:MM:SS.mmmZ".
[[nodiscard]] CCAP_EXPORT Result<Timestamp> parse_timestamp(std::string_view text);

}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_CLOCK_HPP
