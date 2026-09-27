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

#include "test_process.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "cooling_capacity/cooling_capacity.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
extern char** environ;
#endif

namespace ccap_test {

namespace {

using cooling_capacity::Error;
using cooling_capacity::ErrorCode;
using cooling_capacity::Result;

void sleep_ms(unsigned millis) { std::this_thread::sleep_for(std::chrono::milliseconds(millis)); }

#if defined(_WIN32)

std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), needed);
  return wide;
}

std::string narrow(const std::wstring& text) {
  if (text.empty()) {
    return std::string();
  }
  const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string narrow_text(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), narrow_text.data(),
                      needed, nullptr, nullptr);
  return narrow_text;
}

std::wstring quote(const std::string& argument) {
  std::wstring out = L"\"";
  for (const char raw : argument) {
    if (raw == '"') {
      out.push_back(L'\\');
    }
    out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(raw)));
  }
  out.push_back(L'"');
  return out;
}

#endif

}  // namespace

std::string executable_path() {
#if defined(_WIN32)
  std::wstring buffer(4096, L'\0');
  const DWORD written = GetModuleFileNameW(nullptr, buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
  buffer.resize(written);
  return narrow(buffer);
#else
  char buffer[4096];
  const ssize_t written = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (written <= 0) {
    return std::string();
  }
  buffer[written] = '\0';
  return std::string(buffer);
#endif
}

Result<ChildProcess> spawn(const std::vector<std::string>& arguments) {
#if defined(_WIN32)
  const std::string exe = executable_path();
  std::wstring command_line = quote(exe);
  for (const std::string& argument : arguments) {
    command_line.push_back(L' ');
    command_line.append(quote(argument));
  }
  std::wstring mutable_command = command_line;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION information{};
  if (CreateProcessW(widen(exe).c_str(), mutable_command.data(), nullptr, nullptr, FALSE, 0,
                     nullptr, nullptr, &startup, &information) == 0) {
    return Error(ErrorCode::ProcessFailure, "could not start the child process")
        .with("path", exe)
        .with("system_error", std::to_string(GetLastError()));
  }
  CloseHandle(information.hThread);
  ChildProcess child;
  child.handle = information.hProcess;
  child.pid = information.dwProcessId;
  child.running = true;
  return child;
#else
  std::vector<std::string> storage;
  std::vector<char*> argv;
  storage.push_back(executable_path());
  for (const std::string& argument : arguments) {
    storage.push_back(argument);
  }
  for (std::string& item : storage) {
    argv.push_back(item.data());
  }
  argv.push_back(nullptr);
  pid_t pid = 0;
  const int status = ::posix_spawn(&pid, storage.front().c_str(), nullptr, nullptr, argv.data(),
                                   environ);
  if (status != 0) {
    return Error(ErrorCode::ProcessFailure, "could not start the child process")
        .with("system_error", std::strerror(status));
  }
  ChildProcess child;
  child.pid = static_cast<unsigned long>(pid);
  child.handle = reinterpret_cast<void*>(static_cast<std::intptr_t>(pid));
  child.running = true;
  return child;
#endif
}

Result<unsigned long> wait_for_exit(ChildProcess& child, unsigned budget_ms) {
  if (!child.running) {
    return Error(ErrorCode::ProcessFailure, "the child process is not running");
  }
#if defined(_WIN32)
  const DWORD outcome = WaitForSingleObject(static_cast<HANDLE>(child.handle), budget_ms);
  if (outcome == WAIT_TIMEOUT) {
    return Error(ErrorCode::ProcessFailure,
                 "the child process did not finish within the coordination budget")
        .with("pid", std::to_string(child.pid));
  }
  if (outcome != WAIT_OBJECT_0) {
    return Error(ErrorCode::ProcessFailure, "waiting for the child process failed")
        .with("system_error", std::to_string(GetLastError()));
  }
  DWORD code = 0;
  if (GetExitCodeProcess(static_cast<HANDLE>(child.handle), &code) == 0) {
    return Error(ErrorCode::ProcessFailure, "could not read the child exit code");
  }
  CloseHandle(static_cast<HANDLE>(child.handle));
  child.handle = nullptr;
  child.running = false;
  return static_cast<unsigned long>(code);
#else
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    int status = 0;
    const pid_t outcome = ::waitpid(static_cast<pid_t>(child.pid), &status, WNOHANG);
    if (outcome == static_cast<pid_t>(child.pid)) {
      child.running = false;
      if (WIFEXITED(status)) {
        return static_cast<unsigned long>(WEXITSTATUS(status));
      }
      return 1UL;
    }
    sleep_ms(5);
  }
  return Error(ErrorCode::ProcessFailure,
               "the child process did not finish within the coordination budget")
      .with("pid", std::to_string(child.pid));
#endif
}

Result<unsigned long> terminate(ChildProcess& child) {
  if (!child.running) {
    return Error(ErrorCode::ProcessFailure, "the child process is not running");
  }
#if defined(_WIN32)
  if (TerminateProcess(static_cast<HANDLE>(child.handle), 99U) == 0) {
    return Error(ErrorCode::ProcessFailure, "could not terminate the child process")
        .with("system_error", std::to_string(GetLastError()));
  }
  WaitForSingleObject(static_cast<HANDLE>(child.handle), INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(child.handle), &code);
  CloseHandle(static_cast<HANDLE>(child.handle));
  child.handle = nullptr;
  child.running = false;
  return static_cast<unsigned long>(code);
#else
  ::kill(static_cast<pid_t>(child.pid), SIGKILL);
  int status = 0;
  ::waitpid(static_cast<pid_t>(child.pid), &status, 0);
  child.running = false;
  return 99UL;
#endif
}

void release(ChildProcess& child) {
  if (child.handle == nullptr) {
    return;
  }
#if defined(_WIN32)
  CloseHandle(static_cast<HANDLE>(child.handle));
#endif
  child.handle = nullptr;
  child.running = false;
}

bool file_exists(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  return stream.good();
}

Result<void> write_text(const std::string& path, const std::string& text) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.good()) {
    return Error(ErrorCode::IoFailure, "could not create a file").with("path", path);
  }
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  stream.flush();
  if (!stream.good()) {
    return Error(ErrorCode::IoFailure, "could not write the whole file").with("path", path);
  }
  return Result<void>();
}

Result<std::string> read_text(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.good()) {
    return Error(ErrorCode::IoFailure, "could not open a file").with("path", path);
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

Result<void> remove_path(const std::string& path) {
  if (std::remove(path.c_str()) == 0) {
    return Result<void>();
  }
  return Result<void>();
}

Result<void> make_directory(const std::string& path) {
#if defined(_WIN32)
  if (CreateDirectoryW(widen(path).c_str(), nullptr) != 0) {
    return Result<void>();
  }
  const DWORD code = GetLastError();
  if (code == ERROR_ALREADY_EXISTS) {
    return Result<void>();
  }
  return Error(ErrorCode::IoFailure, "could not create the directory")
      .with("path", path)
      .with("system_error", std::to_string(code));
#else
  if (::mkdir(path.c_str(), 0777) == 0 || errno == EEXIST) {
    return Result<void>();
  }
  return Error(ErrorCode::IoFailure, "could not create the directory").with("path", path);
#endif
}

Result<void> remove_tree(const std::string& path) {
  // Only used on directories this test suite created, and only with fixed
  // names, so a plain recursive delete is safe here.
  const std::string command =
#if defined(_WIN32)
      "cmd /c rmdir /s /q \"" + path + "\" >nul 2>&1";
#else
      "rm -rf \"" + path + "\"";
#endif
  const int status = std::system(command.c_str());
  (void)status;
  return Result<void>();
}

Result<void> wait_for_file(const std::string& path, unsigned budget_ms) {
  const auto start = std::chrono::steady_clock::now();
  while (true) {
    if (file_exists(path)) {
      return Result<void>();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    if (elapsed > static_cast<long long>(budget_ms)) {
      return Error(ErrorCode::ProcessFailure,
                   "the expected file did not appear within the coordination budget")
          .with("path", path);
    }
    sleep_ms(5);
  }
}

namespace {

int wait_for_stop(const std::string& stop_file) {
  for (unsigned attempt = 0; attempt < 12000U; ++attempt) {
    if (file_exists(stop_file)) {
      return 0;
    }
    sleep_ms(5);
  }
  return 4;
}

}  // namespace

int child_hold_lock(const std::string& directory, const std::string& ready_file,
                    const std::string& stop_file) {
  cooling_capacity::EngineOptions options;
  options.actor = cooling_capacity::ActorId::literal("child-writer");
  options.store.access = cooling_capacity::StoreAccess::ReadWrite;
  options.store.create_if_missing = true;
  cooling_capacity::Result<std::unique_ptr<cooling_capacity::CoolingCapacityEngine>> engine =
      cooling_capacity::CoolingCapacityEngine::open(directory, options);
  if (!engine.ok()) {
    return 2;
  }
  if (!engine.value()->holds_writer()) {
    return 3;
  }
  const cooling_capacity::Result<void> ready = write_text(ready_file, "ready\n");
  if (!ready.ok()) {
    return 5;
  }
  const int outcome = wait_for_stop(stop_file);
  return outcome;
}

int child_commit_loop(const std::string& directory, const std::string& marker_file) {
  cooling_capacity::EngineOptions options;
  options.actor = cooling_capacity::ActorId::literal("child-committer");
  options.store.access = cooling_capacity::StoreAccess::ReadWrite;
  options.store.create_if_missing = true;
  cooling_capacity::Result<std::unique_ptr<cooling_capacity::CoolingCapacityEngine>> engine =
      cooling_capacity::CoolingCapacityEngine::open(directory, options);
  if (!engine.ok()) {
    return 2;
  }
  cooling_capacity::CoolingCapacityEngine& target = *engine.value();
  unsigned long published = 0;
  while (true) {
    cooling_capacity::Result<cooling_capacity::SnapshotBuilder> builder =
        target.begin_mutation();
    if (!builder.ok()) {
      return 6;
    }
    cooling_capacity::SnapshotBuilder& candidate = builder.value();

    const cooling_capacity::SiteId site = cooling_capacity::SiteId::literal("site-a");
    if (!candidate.contains(cooling_capacity::EntityKind::Site, site.value())) {
      cooling_capacity::SiteRecord record;
      record.id = site;
      record.label = cooling_capacity::BoundedText::literal("Site A");
      record.revision = cooling_capacity::RecordRevision::first();
      if (!candidate.add(record).ok()) {
        return 6;
      }
    }
    const cooling_capacity::FacilityId facility =
        cooling_capacity::FacilityId::literal("dc-1");
    if (!candidate.contains(cooling_capacity::EntityKind::Facility, facility.value())) {
      cooling_capacity::FacilityRecord record;
      record.id = facility;
      record.site = site;
      record.label = cooling_capacity::BoundedText::literal("Data Center 1");
      record.revision = cooling_capacity::RecordRevision::first();
      if (!candidate.add(record).ok()) {
        return 6;
      }
    }
    // Every generation is a pure function of its own number, so a re-attempt
    // after a crash produces byte-identical content and the commit protocol can
    // finish the interrupted publish instead of refusing it.
    const std::uint64_t generation = candidate.generation().value();
    cooling_capacity::EquipmentRecord unit;
    unit.id = cooling_capacity::EquipmentId::literal("crah-a");
    unit.facility = facility;
    unit.label = cooling_capacity::BoundedText::literal("CRAH A");
    unit.kind = cooling_capacity::EquipmentKind::Crah;
    unit.state = cooling_capacity::OperatingState::InService;
    unit.nominal = cooling_capacity::ThermalPower::from_milliwatts(
        400000000 + static_cast<std::int64_t>(generation % 1000U) * 1000);
    unit.revision = cooling_capacity::RecordRevision::first();
    const cooling_capacity::Result<void> applied =
        candidate.contains(cooling_capacity::EntityKind::Equipment, unit.id.value())
            ? candidate.replace(unit)
            : candidate.add(unit);
    if (!applied.ok()) {
      return 6;
    }
    const cooling_capacity::Result<cooling_capacity::CommitReceipt> receipt =
        target.publish(candidate);
    if (!receipt.ok()) {
      std::fprintf(stderr, "commit-loop child: publish failed: %s\n",
                   receipt.error().to_string().c_str());
      return 7;
    }
    ++published;
    const cooling_capacity::Result<void> marker =
        write_text(marker_file, std::to_string(published) + "\n");
    if (!marker.ok()) {
      return 8;
    }
  }
}

int child_read_head(const std::string& directory, const std::string& output_file) {
  cooling_capacity::EngineOptions options;
  options.actor = cooling_capacity::ActorId::literal("child-reader");
  options.store.access = cooling_capacity::StoreAccess::ReadOnly;
  cooling_capacity::Result<std::unique_ptr<cooling_capacity::CoolingCapacityEngine>> engine =
      cooling_capacity::CoolingCapacityEngine::open(directory, options);
  if (!engine.ok()) {
    return 2;
  }
  const std::string text = engine.value()->snapshot()->generation().to_string() + "\n" +
                           engine.value()->snapshot()->digest().to_hex() + "\n";
  const cooling_capacity::Result<void> written = write_text(output_file, text);
  if (!written.ok()) {
    return 3;
  }
  return 0;
}

int child_try_lock(const std::string& directory, const std::string& output_file) {
  cooling_capacity::StoreOpenOptions options;
  options.access = cooling_capacity::StoreAccess::ReadWrite;
  options.create_if_missing = true;
  cooling_capacity::Result<std::unique_ptr<cooling_capacity::CoolingCapacityStore>> store =
      cooling_capacity::CoolingCapacityStore::open(directory, options);
  if (!store.ok()) {
    return 2;
  }
  cooling_capacity::Result<cooling_capacity::WriterEpoch> epoch =
      store.value()->acquire_writer(cooling_capacity::ActorId::literal("child-prober"));
  std::string text;
  if (epoch.ok()) {
    text = "acquired " + epoch.value().to_string() + "\n";
  } else {
    text = std::string("refused ") +
           std::string(cooling_capacity::to_string(epoch.error().code())) + "\n";
  }
  const cooling_capacity::Result<void> written = write_text(output_file, text);
  if (!written.ok()) {
    return 3;
  }
  return 0;
}

int run_child_mode(int argc, char** argv) {
  if (argc < 3) {
    return -1;
  }
  const std::string mode = argv[2];
  if (mode == "hold-lock") {
    if (argc < 6) {
      return -1;
    }
    return child_hold_lock(argv[3], argv[4], argv[5]);
  }
  if (mode == "commit-loop") {
    if (argc < 5) {
      return -1;
    }
    return child_commit_loop(argv[3], argv[4]);
  }
  if (mode == "read-head") {
    if (argc < 5) {
      return -1;
    }
    return child_read_head(argv[3], argv[4]);
  }
  if (mode == "try-lock") {
    if (argc < 5) {
      return -1;
    }
    return child_try_lock(argv[3], argv[4]);
  }
  return -1;
}

}  // namespace ccap_test
