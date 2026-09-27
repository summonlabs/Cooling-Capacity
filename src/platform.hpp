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

#ifndef COOLING_CAPACITY_SRC_PLATFORM_HPP
#define COOLING_CAPACITY_SRC_PLATFORM_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cooling_capacity/errors.hpp"

namespace cooling_capacity {
namespace detail {

// Operating-system file primitives, isolated so that the durability and
// path-safety rules are in one auditable place. Every path handed to these
// functions is a UTF-8 string. Nothing here ever follows a link implicitly
// without the caller asking for it.

// Resolves a path against the current directory and removes redundant
// separators, `.` and `..` components lexically. Rejects empty paths, embedded
// NUL bytes, invalid UTF-8, components that Windows would silently rewrite
// (trailing space or dot) and reserved device names.
[[nodiscard]] Result<std::string> absolute_path(const std::string& path);

[[nodiscard]] Result<void> ensure_directory(const std::string& path);
[[nodiscard]] Result<bool> path_exists(const std::string& path);
[[nodiscard]] Result<bool> is_directory(const std::string& path);
// True for a symbolic link, a junction or any other reparse point.
[[nodiscard]] Result<bool> is_link_like(const std::string& path);
[[nodiscard]] Result<std::uint64_t> file_size(const std::string& path);

// Reads a whole file. The size is checked against `max_bytes` before the buffer
// is allocated, and the file is re-checked afterwards in case it grew.
[[nodiscard]] Result<std::string> read_file(const std::string& path, std::uint64_t max_bytes);

// Creates a new file and fails if it already exists. Every byte is flushed to
// the device before the handle is closed when `flush` is true.
[[nodiscard]] Result<void> write_file_create_new(const std::string& path, std::string_view bytes,
                                                 bool flush);

// Publishes `staging` as `target`. Fails if `target` already exists.
[[nodiscard]] Result<void> publish_new_file(const std::string& staging, const std::string& target,
                                            bool flush);

// Replaces `target` with `staging` atomically. On Windows the replacement is
// performed with MOVEFILE_WRITE_THROUGH so that the metadata update is durable
// before the call returns.
[[nodiscard]] Result<void> replace_file(const std::string& staging, const std::string& target,
                                        bool flush);

[[nodiscard]] Result<void> remove_file(const std::string& path);

// Flushes a directory entry. On POSIX this is `fsync` on the directory; on
// Windows there is no equivalent and the function does nothing, which is
// recorded in the documentation rather than hidden.
[[nodiscard]] Result<void> sync_directory(const std::string& path);

// Names (not paths) of the entries of a directory, excluding `.` and `..`.
[[nodiscard]] Result<std::vector<std::string>> list_directory(const std::string& path);

[[nodiscard]] std::string join(const std::string& directory, const std::string& name);
[[nodiscard]] Result<std::string> file_name_of(const std::string& path);

#if defined(_WIN32)
// Converts an already normalised UTF-8 path to the native UTF-16 form, applying
// the extended-length prefix so that long paths and literal dots and spaces are
// handled exactly as written.
[[nodiscard]] Result<std::wstring> native_path(const std::string& path);
#endif

// Human-readable text for the last operating-system error.
[[nodiscard]] std::string last_system_error_text();

}  // namespace detail
}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_SRC_PLATFORM_HPP
