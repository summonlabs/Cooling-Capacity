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

#ifndef COOLING_CAPACITY_STORE_HPP
#define COOLING_CAPACITY_STORE_HPP

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cooling_capacity/canonical.hpp"
#include "cooling_capacity/ids.hpp"
#include "cooling_capacity/limits.hpp"
#include "cooling_capacity/snapshot.hpp"
#include "cooling_capacity/units.hpp"

namespace cooling_capacity {

namespace detail {
class WriterLock;
}

// Writer authority is a lock an operating-system process holds, not a flag in
// memory. A second process, or a second store object in this process, is
// refused while the lock is held, and the lock is released by the operating
// system when the holder dies.
enum class StoreAccess : std::uint8_t {
  ReadOnly = 0,
  ReadWrite = 1,
};

// What to do when the head pointer is missing or fails its integrity check.
enum class HeadRecovery : std::uint8_t {
  // Refuse to open. This is the default: a store whose authority is damaged is
  // not silently repaired.
  Strict = 0,
  // Explicit operator recovery: reconstruct the head from the highest generation
  // that verifies end to end, and record that it happened.
  ReconstructFromGenerations = 1,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(HeadRecovery recovery) noexcept;

struct StoreOpenOptions {
  StoreAccess access = StoreAccess::ReadWrite;
  bool create_if_missing = false;
  HeadRecovery head_recovery = HeadRecovery::Strict;
  // When false, a store root that is a symbolic link, junction or other reparse
  // point is refused. The default is false.
  bool allow_reparse_root = false;
  Limits limits;
};

enum class StoreState : std::uint8_t {
  Closed = 0,
  OpenReadOnly = 1,
  OpenWritable = 2,
  WriterHeld = 3,
};

[[nodiscard]] CCAP_EXPORT std::string_view to_string(StoreState state) noexcept;

struct GenerationInfo {
  CapacityGeneration generation;
  Digest digest;
  std::uint64_t bytes = 0;
  bool readable = false;
  bool is_head = false;

  friend bool operator==(const GenerationInfo& lhs, const GenerationInfo& rhs) {
    return lhs.generation == rhs.generation && lhs.digest == rhs.digest &&
           lhs.bytes == rhs.bytes && lhs.readable == rhs.readable && lhs.is_head == rhs.is_head;
  }
};

// What the store found, and did, when it opened. Everything the store repaired
// or ignored is listed here; nothing is repaired silently.
struct StoreRecovery {
  bool manifest_present = false;
  bool manifest_valid = false;
  bool head_present = false;
  bool head_reconstructed = false;
  bool head_replaced_from_earlier_generation = false;
  CapacityGeneration head_generation;
  StoreIncarnation incarnation;
  WriterEpoch writer_epoch;
  std::vector<GenerationInfo> generations;
  std::vector<std::string> removed_staging_files;
  std::vector<std::string> warnings;
};

struct CommitOptions {
  // The generation the caller believes is current. A mismatch is refused with
  // `StaleGeneration` before anything is written.
  CapacityGeneration expected_generation;
  Timestamp committed_at;
};

// The evidence that a commit completed. `head_replaced` is the point at which
// the new generation became authoritative.
struct CommitReceipt {
  CapacityGeneration generation;
  Digest digest;
  WriterEpoch epoch;
  Timestamp committed_at;
  std::uint64_t artifact_bytes = 0;
  std::uint64_t manifest_bytes = 0;
  bool head_replaced = false;

  friend bool operator==(const CommitReceipt& lhs, const CommitReceipt& rhs) {
    return lhs.generation == rhs.generation && lhs.digest == rhs.digest &&
           lhs.epoch == rhs.epoch && lhs.artifact_bytes == rhs.artifact_bytes &&
           lhs.manifest_bytes == rhs.manifest_bytes && lhs.head_replaced == rhs.head_replaced;
  }
};

struct StoreVerification {
  bool ok = false;
  CapacityGeneration head_generation;
  Digest head_digest;
  bool head_readable = false;
  std::vector<GenerationInfo> generations;
  std::size_t unreadable_generations = 0;
  std::size_t stray_files = 0;
  ValidationReport validation;
  std::vector<std::string> problems;

  [[nodiscard]] std::string render() const;
};

// A durable, integrity-checked store of snapshot generations.
//
// Persistence protocol, in order, for every commit:
//   plan -> validate -> reserve the generation -> write staging -> flush ->
//   verify the staged bytes -> atomic publish -> flush the directory ->
//   replace the manifest atomically and write through -> retire old generations.
// The manifest is the commit point. A generation file that was published but
// never referenced by the manifest is left alone and remains readable by
// explicit generation number.
class CCAP_EXPORT CoolingCapacityStore {
 public:
  [[nodiscard]] static Result<std::unique_ptr<CoolingCapacityStore>> open(
      const std::string& directory, const StoreOpenOptions& options);

  ~CoolingCapacityStore();
  CoolingCapacityStore(const CoolingCapacityStore&) = delete;
  CoolingCapacityStore& operator=(const CoolingCapacityStore&) = delete;

  [[nodiscard]] const std::string& directory() const noexcept { return directory_; }
  [[nodiscard]] StoreState state() const noexcept { return state_; }
  [[nodiscard]] const StoreRecovery& recovery() const noexcept { return recovery_; }
  [[nodiscard]] const Limits& limits() const noexcept { return options_.limits; }

  // Takes the cross-process writer lock and advances the writer epoch. Fails
  // with `LockConflict` when another process or store object holds it.
  [[nodiscard]] Result<WriterEpoch> acquire_writer(const ActorId& actor);
  [[nodiscard]] Result<void> release_writer();
  [[nodiscard]] bool holds_writer() const noexcept { return state_ == StoreState::WriterHeld; }
  [[nodiscard]] Result<WriterEpoch> writer_epoch() const;

  [[nodiscard]] Result<void> close();

  [[nodiscard]] Result<CapacityGeneration> head_generation() const;
  [[nodiscard]] Result<std::shared_ptr<const CoolingSnapshot>> load_head() const;
  [[nodiscard]] Result<std::shared_ptr<const CoolingSnapshot>> load_generation(
      CapacityGeneration generation) const;
  [[nodiscard]] Result<std::vector<GenerationInfo>> generations() const;

  [[nodiscard]] Result<CommitReceipt> commit(const CoolingSnapshot& candidate,
                                             const CommitOptions& options);

  // Removes one non-head generation and its file. The head can never be retired
  // this way; retiring it would orphan the manifest.
  [[nodiscard]] Result<void> retire(CapacityGeneration generation);

  // At least as strict as the open path: reads the manifest, every generation
  // file, decodes each one and validates it.
  [[nodiscard]] Result<StoreVerification> verify() const;

 private:
  CoolingCapacityStore(std::string directory, StoreOpenOptions options);

  Result<void> initialize();
  Result<void> load_and_check_manifest();
  Result<void> reconstruct_head();
  Result<void> write_manifest(const CapacityGeneration& head, const Digest& head_digest,
                              Timestamp written_at);
  Result<void> clean_staging();
  Result<std::string> generation_file_name(CapacityGeneration generation) const;
  Result<std::string> read_generation_bytes(CapacityGeneration generation,
                                            GenerationInfo* info) const;

  std::string directory_;
  StoreOpenOptions options_;
  StoreState state_ = StoreState::Closed;
  StoreRecovery recovery_;
  WriterEpoch epoch_;
  CapacityGeneration head_;
  Digest head_digest_;
  std::uint64_t head_bytes_ = 0;
  std::array<std::uint8_t, 16> store_id_{};
  std::uint64_t manifest_sequence_ = 0;
  std::unique_ptr<detail::WriterLock> lock_;
};

}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_STORE_HPP
