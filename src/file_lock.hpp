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

#ifndef COOLING_CAPACITY_SRC_FILE_LOCK_HPP
#define COOLING_CAPACITY_SRC_FILE_LOCK_HPP

#include <memory>
#include <string>

#include "cooling_capacity/errors.hpp"

namespace cooling_capacity {
namespace detail {

// A cross-process exclusive lock held on a file for as long as the object
// lives. The lock is granted by the operating system, so it is released when
// the holding process dies, whether it exited cleanly or was killed. A second
// attempt to acquire the same lock, from another process or from another
// object in this process, fails with `LockConflict`.
class WriterLock {
 public:
  [[nodiscard]] static Result<std::unique_ptr<WriterLock>> acquire(const std::string& path);
  ~WriterLock();

  WriterLock(const WriterLock&) = delete;
  WriterLock& operator=(const WriterLock&) = delete;

 private:
  explicit WriterLock(void* handle) noexcept;
  void* handle_ = nullptr;
#if !defined(_WIN32)
  int descriptor_ = -1;
#endif
};

}  // namespace detail
}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_SRC_FILE_LOCK_HPP
