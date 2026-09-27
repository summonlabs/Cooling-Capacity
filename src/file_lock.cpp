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

#include "file_lock.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <cerrno>
#endif

#include "platform.hpp"

namespace cooling_capacity {
namespace detail {

WriterLock::WriterLock(void* handle) noexcept : handle_(handle) {}

WriterLock::~WriterLock() {
#if defined(_WIN32)
  if (handle_ != nullptr) {
    auto* handle = static_cast<HANDLE>(handle_);
    OVERLAPPED overlapped{};
    UnlockFileEx(handle, 0, 1, 0, &overlapped);
    CloseHandle(handle);
    handle_ = nullptr;
  }
#else
  if (descriptor_ >= 0) {
    ::flock(descriptor_, LOCK_UN);
    ::close(descriptor_);
    descriptor_ = -1;
  }
#endif
}

#if defined(_WIN32)

Result<std::unique_ptr<WriterLock>> WriterLock::acquire(const std::string& path) {
  CCAP_TRY_DECLARE(exists, path_exists(path));
  if (exists) {
    CCAP_TRY_DECLARE(link, is_link_like(path));
    if (link) {
      return Error(ErrorCode::PathInvalid,
                   "the lock file is a symbolic link, junction or reparse point")
          .with("path", path);
    }
  }
  CCAP_TRY_DECLARE(wide, native_path(path));
  // The handle must be shareable: every contender opens the same file and then
  // contends for the byte-range lock. That is what makes the exclusion work
  // across processes instead of at open time.
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Error(ErrorCode::IoFailure, "could not open the lock file")
        .with("path", path)
        .with("system_error", last_system_error_text());
  }
  BY_HANDLE_FILE_INFORMATION information{};
  if (GetFileInformationByHandle(handle, &information) == 0) {
    const std::string text = last_system_error_text();
    CloseHandle(handle);
    return Error(ErrorCode::IoFailure, "could not inspect the lock file")
        .with("path", path)
        .with("system_error", text);
  }
  if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    CloseHandle(handle);
    return Error(ErrorCode::PathInvalid,
                 "the lock file is a symbolic link, junction or reparse point")
        .with("path", path);
  }
  OVERLAPPED overlapped{};
  if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                 &overlapped) == 0) {
    const DWORD code = GetLastError();
    CloseHandle(handle);
    if (code == ERROR_LOCK_VIOLATION || code == ERROR_IO_PENDING) {
      return Error(ErrorCode::LockConflict, "another writer already holds the store lock")
          .with("path", path);
    }
    return Error(ErrorCode::IoFailure, "could not take the store lock")
        .with("path", path)
        .with("system_error", last_system_error_text());
  }
  std::unique_ptr<WriterLock> lock(new WriterLock(handle));
  return lock;
}

#else

Result<std::unique_ptr<WriterLock>> WriterLock::acquire(const std::string& path) {
  CCAP_TRY_DECLARE(exists, path_exists(path));
  if (exists) {
    CCAP_TRY_DECLARE(link, is_link_like(path));
    if (link) {
      return Error(ErrorCode::PathInvalid, "the lock file is a symbolic link").with("path", path);
    }
  }
  const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0666);
  if (descriptor < 0) {
    return Error(ErrorCode::IoFailure, "could not open the lock file")
        .with("path", path)
        .with("system_error", last_system_error_text());
  }
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    const int code = errno;
    ::close(descriptor);
    if (code == EWOULDBLOCK || code == EAGAIN || code == EACCES) {
      return Error(ErrorCode::LockConflict, "another writer already holds the store lock")
          .with("path", path);
    }
    return Error(ErrorCode::IoFailure, "could not take the store lock")
        .with("path", path)
        .with("system_error", last_system_error_text());
  }
  std::unique_ptr<WriterLock> lock(new WriterLock(nullptr));
  lock->descriptor_ = descriptor;
  return lock;
}

#endif

}  // namespace detail
}  // namespace cooling_capacity
