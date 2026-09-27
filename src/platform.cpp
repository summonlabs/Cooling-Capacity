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

#include "platform.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <climits>
#include <cstdlib>
#endif

#include "cooling_capacity/text.hpp"

namespace cooling_capacity {
namespace detail {

namespace {

bool has_invalid_component_characters(const std::string& path) {
  for (const char raw : path) {
    const auto value = static_cast<unsigned char>(raw);
    if (value < 0x20U || value == 0x7FU) {
      return true;
    }
  }
  return false;
}

bool is_separator(char value) { return value == '\\' || value == '/'; }

// Windows silently strips trailing spaces and dots from a path component, which
// makes two different requests address the same file. Rejecting them keeps the
// mapping between a requested path and the file it names one-to-one. The two
// traversal components are exempt: `..` ends in a dot but is resolved by the
// platform rather than rewritten.
//
// Both separators are treated as separators in one pass. Splitting by only one
// of them would make a path written with the other one look like a single
// component, which is how `a\inner\..` slipped past an earlier revision.
bool has_rewritable_component(const std::string& path) {
  std::size_t start = 0;
  while (start <= path.size()) {
    std::size_t stop = start;
    while (stop < path.size() && !is_separator(path[stop])) {
      ++stop;
    }
    if (stop > start) {
      const std::string component = path.substr(start, stop - start);
      if (component != "." && component != "..") {
        const char last = component.back();
        if (last == ' ' || last == '.') {
          return true;
        }
      }
    }
    if (stop >= path.size()) {
      break;
    }
    start = stop + 1;
  }
  return false;
}

bool is_reserved_device_name(const std::string& component) {
  std::string base = component;
  const std::size_t dot = base.find('.');
  if (dot != std::string::npos) {
    base = base.substr(0, dot);
  }
  for (char& raw : base) {
    raw = static_cast<char>(std::tolower(static_cast<unsigned char>(raw)));
  }
  if (base == "con" || base == "prn" || base == "aux" || base == "nul") {
    return true;
  }
  if (base.size() == 4) {
    if (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0) {
      return base[3] >= '1' && base[3] <= '9';
    }
  }
  return false;
}

bool has_reserved_component(const std::string& path) {
  std::size_t start = 0;
  while (start <= path.size()) {
    std::size_t stop = start;
    while (stop < path.size() && !is_separator(path[stop])) {
      ++stop;
    }
    if (stop > start && is_reserved_device_name(path.substr(start, stop - start))) {
      return true;
    }
    if (stop >= path.size()) {
      break;
    }
    start = stop + 1;
  }
  return false;
}

}  // namespace

std::string join(const std::string& directory, const std::string& name) {
  if (directory.empty()) {
    return name;
  }
  const char last = directory.back();
  if (last == '/' || last == '\\') {
    return directory + name;
  }
#if defined(_WIN32)
  return directory + "\\" + name;
#else
  return directory + "/" + name;
#endif
}

#if defined(_WIN32)

namespace {

Result<std::wstring> widen(const std::string& text) {
  if (text.empty()) {
    return Error(ErrorCode::PathInvalid, "path is empty");
  }
  if (!is_valid_utf8(text)) {
    return Error(ErrorCode::InvalidUtf8, "path is not valid UTF-8");
  }
  if (text.find('\0') != std::string::npos) {
    return Error(ErrorCode::PathInvalid, "path contains an embedded NUL");
  }
  const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(),
                                         static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) {
    return Error(ErrorCode::InvalidUtf8, "path could not be converted to UTF-16")
        .with("system_error", last_system_error_text());
  }
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(),
                                          static_cast<int>(text.size()), wide.data(), needed);
  if (written != needed) {
    return Error(ErrorCode::InvalidUtf8, "path conversion to UTF-16 was incomplete");
  }
  return wide;
}

Result<std::string> narrow(const std::wstring& text) {
  if (text.empty()) {
    return std::string();
  }
  const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return Error(ErrorCode::InvalidUtf8, "path could not be converted to UTF-8");
  }
  std::string narrow_text(static_cast<std::size_t>(needed), '\0');
  const int written = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                          narrow_text.data(), needed, nullptr, nullptr);
  if (written != needed) {
    return Error(ErrorCode::InvalidUtf8, "path conversion to UTF-8 was incomplete");
  }
  return narrow_text;
}

Error win32_failure(const char* what, const std::string& path) {
  return Error(ErrorCode::IoFailure, std::string(what) + " failed")
      .with("path", path)
      .with("system_error", last_system_error_text());
}

}  // namespace

// Prefixing with \\?\ turns off the Win32 path rewriting rules, which is what
// makes long paths and names that contain dots and spaces behave predictably.
// Declared in platform.hpp so that the writer lock can open the same file the
// same way.
Result<std::wstring> native_path(const std::string& normalized_utf8) {
  CCAP_TRY_DECLARE(wide, widen(normalized_utf8));
  if (wide.size() >= 4 && wide.compare(0, 4, L"\\\\?\\") == 0) {
    return wide;
  }
  if (wide.size() >= 2 && wide[0] == L'\\' && wide[1] == L'\\') {
    std::wstring converted = L"\\\\?\\UNC\\";
    converted.append(wide, 2, std::wstring::npos);
    return converted;
  }
  std::wstring converted = L"\\\\?\\";
  converted.append(wide);
  return converted;
}

std::string last_system_error_text() {
  const DWORD code = GetLastError();
  if (code == 0) {
    return "no error";
  }
  LPWSTR buffer = nullptr;
  const DWORD written = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buffer), 0,
      nullptr);
  std::string text = "code " + std::to_string(code);
  if (written != 0 && buffer != nullptr) {
    std::wstring wide(buffer, written);
    while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' || wide.back() == L' ')) {
      wide.pop_back();
    }
    Result<std::string> narrowed = narrow(wide);
    if (narrowed.ok()) {
      text.append(": ");
      text.append(narrowed.value());
    }
  }
  if (buffer != nullptr) {
    LocalFree(buffer);
  }
  return text;
}

Result<std::string> absolute_path(const std::string& path) {
  if (path.empty()) {
    return Error(ErrorCode::PathInvalid, "path is empty");
  }
  if (has_invalid_component_characters(path)) {
    return Error(ErrorCode::PathInvalid, "path contains a control character");
  }
  // The check runs on what the caller asked for, before Win32 is given a chance
  // to canonicalise it: GetFullPathNameW is free to tidy trailing dots and
  // spaces away, which would hide the very rewriting this check exists to
  // refuse.
  if (has_rewritable_component(path)) {
    return Error(ErrorCode::PathInvalid,
                 "path has a component ending in a space or dot, which Windows would rewrite")
        .with("path", path);
  }
  if (has_reserved_component(path)) {
    return Error(ErrorCode::PathInvalid, "path has a component that names a reserved device")
        .with("path", path);
  }
  CCAP_TRY_DECLARE(wide, widen(path));
  const DWORD needed = GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
  if (needed == 0) {
    return Error(ErrorCode::PathInvalid, "path could not be resolved")
        .with("path", path)
        .with("system_error", last_system_error_text());
  }
  std::wstring full(static_cast<std::size_t>(needed), L'\0');
  const DWORD written = GetFullPathNameW(wide.c_str(), needed, full.data(), nullptr);
  if (written == 0 || written >= needed) {
    return Error(ErrorCode::PathInvalid, "path could not be resolved")
        .with("path", path)
        .with("system_error", last_system_error_text());
  }
  full.resize(static_cast<std::size_t>(written));
  while (full.size() > 3 && (full.back() == L'\\' || full.back() == L'/')) {
    full.pop_back();
  }
  CCAP_TRY_DECLARE(normalized, narrow(full));
  if (has_rewritable_component(normalized)) {
    return Error(ErrorCode::PathInvalid,
                 "path has a component ending in a space or dot, which Windows would rewrite")
        .with("path", normalized);
  }
  if (has_reserved_component(normalized)) {
    return Error(ErrorCode::PathInvalid, "path has a component that names a reserved device")
        .with("path", normalized);
  }
  if (normalized.size() >= MAX_PATH) {
    // The extended-length prefix is added at the point of use, so a long path is
    // supported; nothing further is required here.
  }
  return normalized;
}

Result<bool> path_exists(const std::string& path) {
  CCAP_TRY_DECLARE(extended, native_path(path));
  const DWORD attributes = GetFileAttributesW(extended.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ||
        code == ERROR_INVALID_NAME) {
      return false;
    }
    return Error(ErrorCode::IoFailure, "could not query the path")
        .with("path", path)
        .with("system_error", last_system_error_text());
  }
  return true;
}

Result<bool> is_directory(const std::string& path) {
  CCAP_TRY_DECLARE(extended, native_path(path));
  const DWORD attributes = GetFileAttributesW(extended.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ||
        code == ERROR_INVALID_NAME) {
      return false;
    }
    return Error(ErrorCode::IoFailure, "could not query the path")
        .with("path", path)
        .with("system_error", last_system_error_text());
  }
  return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

Result<bool> is_link_like(const std::string& path) {
  CCAP_TRY_DECLARE(extended, native_path(path));
  const DWORD attributes = GetFileAttributesW(extended.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ||
        code == ERROR_INVALID_NAME) {
      return false;
    }
    return Error(ErrorCode::IoFailure, "could not query the path")
        .with("path", path)
        .with("system_error", last_system_error_text());
  }
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

Result<void> ensure_directory(const std::string& path) {
  CCAP_TRY_DECLARE(extended, native_path(path));
  if (CreateDirectoryW(extended.c_str(), nullptr) != 0) {
    return Result<void>();
  }
  const DWORD code = GetLastError();
  if (code == ERROR_ALREADY_EXISTS) {
    const DWORD attributes = GetFileAttributesW(extended.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      return Result<void>();
    }
    return Error(ErrorCode::PathInvalid, "path exists and is not a directory").with("path", path);
  }
  return win32_failure("create directory", path);
}

Result<std::uint64_t> file_size(const std::string& path) {
  CCAP_TRY_DECLARE(extended, native_path(path));
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (GetFileAttributesExW(extended.c_str(), GetFileExInfoStandard, &data) == 0) {
    return win32_failure("query file size", path);
  }
  const auto high = static_cast<std::uint64_t>(data.nFileSizeHigh);
  const auto low = static_cast<std::uint64_t>(data.nFileSizeLow);
  return (high << 32U) | low;
}

Result<std::string> read_file(const std::string& path, std::uint64_t max_bytes) {
  CCAP_TRY_DECLARE(size, file_size(path));
  if (size > max_bytes) {
    return Error(ErrorCode::Oversized, "file is larger than the configured maximum")
        .with("path", path)
        .with("bytes", std::to_string(size))
        .with("maximum", std::to_string(max_bytes));
  }
  CCAP_TRY_DECLARE(extended, native_path(path));
  HANDLE handle = CreateFileW(extended.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return win32_failure("open file", path);
  }
  std::string contents;
  contents.resize(static_cast<std::size_t>(size));
  std::size_t read_total = 0;
  while (read_total < contents.size()) {
    const std::size_t remaining = contents.size() - read_total;
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 1U << 20U));
    DWORD read_now = 0;
    if (ReadFile(handle, contents.data() + read_total, chunk, &read_now, nullptr) == 0) {
      const std::string text = last_system_error_text();
      CloseHandle(handle);
      return Error(ErrorCode::IoFailure, "reading the file failed")
          .with("path", path)
          .with("system_error", text);
    }
    if (read_now == 0) {
      CloseHandle(handle);
      return Error(ErrorCode::Truncated, "file is shorter than its recorded size")
          .with("path", path);
    }
    read_total += read_now;
  }
  CloseHandle(handle);
  return contents;
}

Result<void> write_file_create_new(const std::string& path, std::string_view bytes, bool flush) {
  CCAP_TRY_DECLARE(extended, native_path(path));
  HANDLE handle = CreateFileW(extended.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
      return Error(ErrorCode::AlreadyExists, "file already exists").with("path", path);
    }
    return win32_failure("create file", path);
  }
  std::size_t written_total = 0;
  while (written_total < bytes.size()) {
    const std::size_t remaining = bytes.size() - written_total;
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 1U << 20U));
    DWORD written_now = 0;
    if (WriteFile(handle, bytes.data() + written_total, chunk, &written_now, nullptr) == 0) {
      const std::string text = last_system_error_text();
      CloseHandle(handle);
      DeleteFileW(extended.c_str());
      return Error(ErrorCode::IoFailure, "writing the file failed")
          .with("path", path)
          .with("system_error", text);
    }
    written_total += written_now;
  }
  if (flush && FlushFileBuffers(handle) == 0) {
    const std::string text = last_system_error_text();
    CloseHandle(handle);
    DeleteFileW(extended.c_str());
    return Error(ErrorCode::IoFailure, "flushing the file to the device failed")
        .with("path", path)
        .with("system_error", text);
  }
  if (CloseHandle(handle) == 0) {
    return win32_failure("close file", path);
  }
  return Result<void>();
}

Result<void> publish_new_file(const std::string& staging, const std::string& target, bool flush) {
  CCAP_TRY_DECLARE(from, native_path(staging));
  CCAP_TRY_DECLARE(to, native_path(target));
  DWORD flags = MOVEFILE_WRITE_THROUGH;
  if (!flush) {
    flags = 0;
  }
  if (MoveFileExW(from.c_str(), to.c_str(), flags) == 0) {
    const DWORD code = GetLastError();
    if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
      return Error(ErrorCode::AlreadyExists, "target already exists").with("path", target);
    }
    return Error(ErrorCode::IoFailure, "publishing the file failed")
        .with("path", target)
        .with("system_error", last_system_error_text());
  }
  return Result<void>();
}

Result<void> replace_file(const std::string& staging, const std::string& target, bool flush) {
  CCAP_TRY_DECLARE(from, native_path(staging));
  CCAP_TRY_DECLARE(to, native_path(target));
  DWORD flags = MOVEFILE_REPLACE_EXISTING;
  if (flush) {
    flags |= MOVEFILE_WRITE_THROUGH;
  }
  if (MoveFileExW(from.c_str(), to.c_str(), flags) == 0) {
    return Error(ErrorCode::IoFailure, "replacing the file failed")
        .with("path", target)
        .with("system_error", last_system_error_text());
  }
  return Result<void>();
}

Result<void> remove_file(const std::string& path) {
  CCAP_TRY_DECLARE(extended, native_path(path));
  if (DeleteFileW(extended.c_str()) != 0) {
    return Result<void>();
  }
  const DWORD code = GetLastError();
  if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
    return Result<void>();
  }
  return win32_failure("remove file", path);
}

Result<void> sync_directory(const std::string& path) {
  // Windows offers no directory flush. The durability of a replacement comes
  // from MOVEFILE_WRITE_THROUGH on the move itself, so this is a deliberate
  // no-op rather than an unimplemented function.
  (void)path;
  return Result<void>();
}

Result<std::vector<std::string>> list_directory(const std::string& path) {
  CCAP_TRY_DECLARE(extended, native_path(path));
  std::wstring pattern = extended;
  if (!pattern.empty() && pattern.back() != L'\\') {
    pattern.push_back(L'\\');
  }
  pattern.push_back(L'*');
  WIN32_FIND_DATAW data{};
  HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
  std::vector<std::string> names;
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND) {
      return names;
    }
    return win32_failure("list directory", path);
  }
  do {
    const std::wstring name(data.cFileName);
    if (name == L"." || name == L"..") {
      continue;
    }
    CCAP_TRY_DECLARE(narrowed, narrow(name));
    names.push_back(narrowed);
  } while (FindNextFileW(handle, &data) != 0);
  const DWORD last = GetLastError();
  FindClose(handle);
  if (last != ERROR_NO_MORE_FILES) {
    return Error(ErrorCode::IoFailure, "listing the directory failed")
        .with("path", path)
        .with("system_error", last_system_error_text());
  }
  std::sort(names.begin(), names.end());
  return names;
}

Result<std::string> file_name_of(const std::string& path) {
  const std::size_t slash = path.find_last_of("\\/");
  if (slash == std::string::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

#else  // POSIX

std::string last_system_error_text() {
  const int code = errno;
  if (code == 0) {
    return "no error";
  }
  return std::string(std::strerror(code)) + " (errno " + std::to_string(code) + ")";
}

namespace {

Error posix_failure(const char* what, const std::string& path) {
  return Error(ErrorCode::IoFailure, std::string(what) + " failed")
      .with("path", path)
      .with("system_error", last_system_error_text());
}

bool is_absolute(const std::string& path) { return !path.empty() && path.front() == '/'; }

Result<std::string> working_directory() {
  std::vector<char> buffer(4096);
  while (true) {
    if (getcwd(buffer.data(), buffer.size()) != nullptr) {
      return std::string(buffer.data());
    }
    if (errno != ERANGE || buffer.size() > (1U << 20U)) {
      return Error(ErrorCode::IoFailure, "could not determine the current directory")
          .with("system_error", last_system_error_text());
    }
    buffer.resize(buffer.size() * 2U);
  }
}

}  // namespace

Result<std::string> absolute_path(const std::string& path) {
  if (path.empty()) {
    return Error(ErrorCode::PathInvalid, "path is empty");
  }
  if (path.find('\0') != std::string::npos) {
    return Error(ErrorCode::PathInvalid, "path contains an embedded NUL");
  }
  if (!is_valid_utf8(path)) {
    return Error(ErrorCode::InvalidUtf8, "path is not valid UTF-8");
  }
  if (has_invalid_component_characters(path)) {
    return Error(ErrorCode::PathInvalid, "path contains a control character");
  }
  std::string combined = path;
  if (!is_absolute(combined)) {
    CCAP_TRY_DECLARE(cwd, working_directory());
    combined = cwd + "/" + path;
  }
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= combined.size()) {
    const std::size_t end = combined.find('/', start);
    const std::size_t stop = end == std::string::npos ? combined.size() : end;
    const std::string component = combined.substr(start, stop - start);
    if (component.empty() || component == ".") {
      // skip
    } else if (component == "..") {
      if (!parts.empty()) {
        parts.pop_back();
      }
    } else {
      parts.push_back(component);
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  std::string normalized = "/";
  for (std::size_t index = 0; index < parts.size(); ++index) {
    if (index != 0) {
      normalized.push_back('/');
    }
    normalized.append(parts[index]);
  }
  return normalized;
}

Result<bool> path_exists(const std::string& path) {
  struct stat info {};
  if (::lstat(path.c_str(), &info) == 0) {
    return true;
  }
  if (errno == ENOENT || errno == ENOTDIR) {
    return false;
  }
  return posix_failure("query the path", path);
}

Result<bool> is_directory(const std::string& path) {
  struct stat info {};
  if (::stat(path.c_str(), &info) == 0) {
    return S_ISDIR(info.st_mode);
  }
  if (errno == ENOENT || errno == ENOTDIR) {
    return false;
  }
  return posix_failure("query the path", path);
}

Result<bool> is_link_like(const std::string& path) {
  struct stat info {};
  if (::lstat(path.c_str(), &info) == 0) {
    return S_ISLNK(info.st_mode);
  }
  if (errno == ENOENT || errno == ENOTDIR) {
    return false;
  }
  return posix_failure("query the path", path);
}

Result<void> ensure_directory(const std::string& path) {
  if (::mkdir(path.c_str(), 0777) == 0) {
    return Result<void>();
  }
  if (errno == EEXIST) {
    CCAP_TRY_DECLARE(directory, is_directory(path));
    if (directory) {
      return Result<void>();
    }
    return Error(ErrorCode::PathInvalid, "path exists and is not a directory").with("path", path);
  }
  return posix_failure("create directory", path);
}

Result<std::uint64_t> file_size(const std::string& path) {
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0) {
    return posix_failure("query file size", path);
  }
  if (!S_ISREG(info.st_mode)) {
    return Error(ErrorCode::PathInvalid, "path is not a regular file").with("path", path);
  }
  return static_cast<std::uint64_t>(info.st_size);
}

Result<std::string> read_file(const std::string& path, std::uint64_t max_bytes) {
  CCAP_TRY_DECLARE(size, file_size(path));
  if (size > max_bytes) {
    return Error(ErrorCode::Oversized, "file is larger than the configured maximum")
        .with("path", path)
        .with("bytes", std::to_string(size))
        .with("maximum", std::to_string(max_bytes));
  }
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    return posix_failure("open file", path);
  }
  std::string contents;
  contents.resize(static_cast<std::size_t>(size));
  std::size_t total = 0;
  while (total < contents.size()) {
    const ssize_t got = ::read(descriptor, contents.data() + total, contents.size() - total);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      const std::string text = last_system_error_text();
      ::close(descriptor);
      return Error(ErrorCode::IoFailure, "reading the file failed")
          .with("path", path)
          .with("system_error", text);
    }
    if (got == 0) {
      ::close(descriptor);
      return Error(ErrorCode::Truncated, "file is shorter than its recorded size")
          .with("path", path);
    }
    total += static_cast<std::size_t>(got);
  }
  ::close(descriptor);
  return contents;
}

Result<void> write_file_create_new(const std::string& path, std::string_view bytes, bool flush) {
  const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
  if (descriptor < 0) {
    if (errno == EEXIST) {
      return Error(ErrorCode::AlreadyExists, "file already exists").with("path", path);
    }
    return posix_failure("create file", path);
  }
  std::size_t total = 0;
  while (total < bytes.size()) {
    const ssize_t written = ::write(descriptor, bytes.data() + total, bytes.size() - total);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      const std::string text = last_system_error_text();
      ::close(descriptor);
      ::unlink(path.c_str());
      return Error(ErrorCode::IoFailure, "writing the file failed")
          .with("path", path)
          .with("system_error", text);
    }
    total += static_cast<std::size_t>(written);
  }
  if (flush && ::fsync(descriptor) != 0) {
    const std::string text = last_system_error_text();
    ::close(descriptor);
    ::unlink(path.c_str());
    return Error(ErrorCode::IoFailure, "flushing the file to the device failed")
        .with("path", path)
        .with("system_error", text);
  }
  if (::close(descriptor) != 0) {
    return posix_failure("close file", path);
  }
  return Result<void>();
}

Result<void> publish_new_file(const std::string& staging, const std::string& target, bool flush) {
  if (flush) {
    // The staging file was already flushed when it was written; a rename within
    // a directory is atomic and is made durable by flushing the directory.
  }
  if (::link(staging.c_str(), target.c_str()) != 0) {
    if (errno == EEXIST) {
      return Error(ErrorCode::AlreadyExists, "target already exists").with("path", target);
    }
    return posix_failure("publish file", target);
  }
  if (::unlink(staging.c_str()) != 0) {
    return posix_failure("remove staging file", staging);
  }
  return Result<void>();
}

Result<void> replace_file(const std::string& staging, const std::string& target, bool flush) {
  (void)flush;
  if (::rename(staging.c_str(), target.c_str()) != 0) {
    return posix_failure("replace file", target);
  }
  return Result<void>();
}

Result<void> remove_file(const std::string& path) {
  if (::unlink(path.c_str()) == 0) {
    return Result<void>();
  }
  if (errno == ENOENT) {
    return Result<void>();
  }
  return posix_failure("remove file", path);
}

Result<void> sync_directory(const std::string& path) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor < 0) {
    return posix_failure("open directory for flushing", path);
  }
  if (::fsync(descriptor) != 0) {
    const std::string text = last_system_error_text();
    ::close(descriptor);
    return Error(ErrorCode::IoFailure, "flushing the directory failed")
        .with("path", path)
        .with("system_error", text);
  }
  ::close(descriptor);
  return Result<void>();
}

Result<std::vector<std::string>> list_directory(const std::string& path) {
  DIR* directory = ::opendir(path.c_str());
  if (directory == nullptr) {
    return posix_failure("open directory", path);
  }
  std::vector<std::string> names;
  while (true) {
    errno = 0;
    struct dirent* entry = ::readdir(directory);
    if (entry == nullptr) {
      if (errno != 0) {
        const std::string text = last_system_error_text();
        ::closedir(directory);
        return Error(ErrorCode::IoFailure, "listing the directory failed")
            .with("path", path)
            .with("system_error", text);
      }
      break;
    }
    const std::string name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    names.push_back(name);
  }
  ::closedir(directory);
  std::sort(names.begin(), names.end());
  return names;
}

Result<std::string> file_name_of(const std::string& path) {
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

#endif

}  // namespace detail
}  // namespace cooling_capacity
