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

#include "cooling_capacity/store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#include "cooling_capacity/clock.hpp"
#include "cooling_capacity/digest.hpp"
#include "cooling_capacity/snapshot.hpp"
#include "cooling_capacity/text.hpp"
#include "cooling_capacity/version.hpp"
#include "enum_table.hpp"
#include "file_lock.hpp"
#include "platform.hpp"

namespace cooling_capacity {

namespace {

constexpr char kManifestMagic[8] = {'C', 'C', 'A', 'P', 'M', 'A', 'N', '1'};
constexpr std::uint32_t kEndianMarker = 0x01020304U;
constexpr std::size_t kManifestSize = 256;
constexpr std::size_t kManifestDigestOffset = 128;
constexpr char kManifestName[] = "MANIFEST";
constexpr char kManifestStagingName[] = "MANIFEST.tmp";
constexpr char kLockName[] = "LOCK";
constexpr char kGenerationPrefix[] = "gen-";
constexpr char kGenerationSuffix[] = ".ccap";
constexpr char kStagingPrefix[] = "staging-";
constexpr char kStagingSuffix[] = ".tmp";
constexpr std::size_t kGenerationDigits = 20;

struct Manifest {
  StoreIncarnation incarnation;
  WriterEpoch epoch;
  CapacityGeneration head;
  Digest head_digest;
  std::uint64_t head_bytes = 0;
  std::int64_t written_at = 0;
  std::uint64_t sequence = 0;
  std::array<std::uint8_t, 16> store_id{};
};

void put_u32(std::string& out, std::uint32_t value) {
  for (unsigned index = 0; index < 4U; ++index) {
    out.push_back(static_cast<char>((value >> (8U * index)) & 0xFFU));
  }
}

void put_u64(std::string& out, std::uint64_t value) {
  for (unsigned index = 0; index < 8U; ++index) {
    out.push_back(static_cast<char>((value >> (8U * index)) & 0xFFU));
  }
}

std::uint32_t read_u32(const std::string& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4U; ++index) {
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + index]))
             << (8U * index);
  }
  return value;
}

std::uint64_t read_u64(const std::string& bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8U; ++index) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + index]))
             << (8U * index);
  }
  return value;
}

std::string encode_manifest(const Manifest& manifest) {
  std::string out;
  out.reserve(kManifestSize);
  out.append(kManifestMagic, sizeof(kManifestMagic));
  put_u32(out, kStoreManifestVersion);
  put_u32(out, kEndianMarker);
  out.append(reinterpret_cast<const char*>(manifest.store_id.data()), manifest.store_id.size());
  put_u64(out, manifest.incarnation.value());
  put_u64(out, manifest.epoch.value());
  put_u64(out, manifest.head.value());
  const std::array<std::uint8_t, Digest::kSize>& digest = manifest.head_digest.bytes();
  out.append(reinterpret_cast<const char*>(digest.data()), digest.size());
  put_u64(out, manifest.head_bytes);
  put_u64(out, static_cast<std::uint64_t>(manifest.written_at));
  put_u64(out, manifest.sequence);
  put_u64(out, 0U);
  put_u64(out, 0U);
  if (out.size() != kManifestDigestOffset) {
    // The layout above must reach the digest offset exactly; a mismatch is a
    // programming error and is treated as one.
    return std::string();
  }
  const Digest check = Digest::of(out);
  out.append(reinterpret_cast<const char*>(check.bytes().data()), check.bytes().size());
  out.resize(kManifestSize, '\0');
  return out;
}

Result<Manifest> decode_manifest(const std::string& bytes) {
  if (bytes.size() != kManifestSize) {
    return Error(ErrorCode::Corruption, "the manifest is not the expected size")
        .with("bytes", std::to_string(bytes.size()))
        .with("expected", std::to_string(kManifestSize));
  }
  if (std::memcmp(bytes.data(), kManifestMagic, sizeof(kManifestMagic)) != 0) {
    return Error(ErrorCode::UnsupportedFormat, "the manifest magic does not match");
  }
  const std::uint32_t endian = read_u32(bytes, 12U);
  if (endian == 0x04030201U) {
    return Error(ErrorCode::WrongEndianness,
                 "the manifest was written on a host with the opposite byte order");
  }
  if (endian != kEndianMarker) {
    return Error(ErrorCode::Corruption, "the manifest byte-order marker is not recognised");
  }
  const std::uint32_t version = read_u32(bytes, 8U);
  if (version != kStoreManifestVersion) {
    return Error(ErrorCode::IncompatibleVersion, "the manifest version is not supported")
        .with("manifest_version", std::to_string(version))
        .with("reader_version", std::to_string(kStoreManifestVersion));
  }
  std::array<std::uint8_t, Digest::kSize> recorded{};
  std::memcpy(recorded.data(), bytes.data() + kManifestDigestOffset, Digest::kSize);
  const Digest computed = Digest::of(std::string_view(bytes.data(), kManifestDigestOffset));
  if (computed.bytes() != recorded) {
    return Error(ErrorCode::DigestMismatch, "the manifest digest does not match its contents");
  }
  Manifest manifest;
  std::memcpy(manifest.store_id.data(), bytes.data() + 16U, manifest.store_id.size());
  manifest.incarnation = StoreIncarnation::from_value(read_u64(bytes, 32U));
  manifest.epoch = WriterEpoch::from_value(read_u64(bytes, 40U));
  manifest.head = CapacityGeneration::from_value(read_u64(bytes, 48U));
  std::array<std::uint8_t, Digest::kSize> head_digest{};
  std::memcpy(head_digest.data(), bytes.data() + 56U, head_digest.size());
  Result<Digest> parsed = [&head_digest]() -> Result<Digest> {
    const std::string hex = cooling_capacity::to_hex(
        std::string_view(reinterpret_cast<const char*>(head_digest.data()), head_digest.size()));
    return Digest::parse_hex(hex);
  }();
  if (!parsed.ok()) {
    return parsed.error();
  }
  manifest.head_digest = parsed.value();
  manifest.head_bytes = read_u64(bytes, 88U);
  manifest.written_at = static_cast<std::int64_t>(read_u64(bytes, 96U));
  manifest.sequence = read_u64(bytes, 104U);
  return manifest;
}

std::string format_generation_number(std::uint64_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%0*llu", static_cast<int>(kGenerationDigits),
                static_cast<unsigned long long>(value));
  return std::string(buffer);
}

bool parse_generation_number(std::string_view name, CapacityGeneration& out) {
  if (name.size() != kGenerationDigits) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char raw : name) {
    if (raw < '0' || raw > '9') {
      return false;
    }
    value = value * 10U + static_cast<std::uint64_t>(raw - '0');
  }
  if (value == 0) {
    return false;
  }
  out = CapacityGeneration::from_value(value);
  return true;
}

bool starts_with(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::array<std::uint8_t, 16> make_store_id() {
  std::array<std::uint8_t, 16> id{};
  std::random_device device;
  for (std::size_t index = 0; index < id.size(); index += 4U) {
    const std::uint32_t value = device();
    id[index] = static_cast<std::uint8_t>(value & 0xFFU);
    id[index + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    id[index + 2U] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    id[index + 3U] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
  }
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto millis = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
  for (unsigned index = 0; index < 8U; ++index) {
    id[index] ^= static_cast<std::uint8_t>((millis >> (8U * index)) & 0xFFU);
  }
  return id;
}

constexpr detail::EnumName<HeadRecovery> kHeadRecoveryNames[] = {
    {HeadRecovery::Strict, "strict"},
    {HeadRecovery::ReconstructFromGenerations, "reconstruct-from-generations"},
};

constexpr detail::EnumName<StoreState> kStoreStateNames[] = {
    {StoreState::Closed, "closed"},
    {StoreState::OpenReadOnly, "open-read-only"},
    {StoreState::OpenWritable, "open-writable"},
    {StoreState::WriterHeld, "writer-held"},
};

}  // namespace

std::string_view to_string(HeadRecovery recovery) noexcept {
  return detail::name_from_table(kHeadRecoveryNames, recovery);
}

std::string_view to_string(StoreState state) noexcept {
  return detail::name_from_table(kStoreStateNames, state);
}

std::string StoreVerification::render() const {
  std::string out;
  out.append("head generation ");
  out.append(head_generation.to_string());
  out.push_back(' ');
  out.append(head_readable ? "readable" : "unreadable");
  out.push_back('\n');
  out.append("generations ");
  out.append(std::to_string(generations.size()));
  out.push_back('\n');
  out.append("unreadable ");
  out.append(std::to_string(unreadable_generations));
  out.push_back('\n');
  out.append("stray files ");
  out.append(std::to_string(stray_files));
  out.push_back('\n');
  for (const std::string& problem : problems) {
    out.append("problem: ");
    out.append(problem);
    out.push_back('\n');
  }
  out.append(validation.render());
  return out;
}

CoolingCapacityStore::CoolingCapacityStore(std::string directory, StoreOpenOptions options)
    : directory_(std::move(directory)), options_(std::move(options)) {}

CoolingCapacityStore::~CoolingCapacityStore() = default;

Result<std::string> CoolingCapacityStore::generation_file_name(
    CapacityGeneration generation) const {
  std::string name(kGenerationPrefix);
  name.append(format_generation_number(generation.value()));
  name.append(kGenerationSuffix);
  return detail::join(directory_, name);
}

Result<void> CoolingCapacityStore::clean_staging() {
  CCAP_TRY_DECLARE(names, detail::list_directory(directory_));
  for (const std::string& name : names) {
    if (!starts_with(name, kStagingPrefix) || !ends_with(name, kStagingSuffix)) {
      continue;
    }
    CCAP_TRY(detail::remove_file(detail::join(directory_, name)));
    recovery_.removed_staging_files.push_back(name);
  }
  if (!recovery_.removed_staging_files.empty()) {
    recovery_.warnings.push_back("removed " +
                                 std::to_string(recovery_.removed_staging_files.size()) +
                                 " staging file(s) left by an interrupted commit");
  }
  return Result<void>();
}

Result<void> CoolingCapacityStore::write_manifest(const CapacityGeneration& head,
                                                  const Digest& head_digest,
                                                  Timestamp written_at) {
  Manifest manifest;
  manifest.store_id = store_id_;
  manifest.incarnation = recovery_.incarnation;
  manifest.epoch = epoch_;
  manifest.head = head;
  manifest.head_digest = head_digest;
  manifest.head_bytes = head_bytes_;
  manifest.written_at = written_at.unix_milliseconds();
  manifest.sequence = manifest_sequence_ + 1U;
  const std::string bytes = encode_manifest(manifest);
  if (bytes.empty()) {
    return Error(ErrorCode::InvariantViolation, "the manifest could not be encoded");
  }
  const std::string staging = detail::join(directory_, kManifestStagingName);
  CCAP_TRY(detail::remove_file(staging));
  CCAP_TRY(detail::write_file_create_new(staging, bytes, true));
  CCAP_TRY_DECLARE(verify_bytes, detail::read_file(staging, kManifestSize * 4U));
  if (verify_bytes != bytes) {
    CCAP_TRY(detail::remove_file(staging));
    return Error(ErrorCode::Corruption, "the staged manifest did not read back identically");
  }
  CCAP_TRY(detail::replace_file(staging, detail::join(directory_, kManifestName), true));
  CCAP_TRY(detail::sync_directory(directory_));
  manifest_sequence_ = manifest.sequence;
  recovery_.manifest_present = true;
  recovery_.manifest_valid = true;
  recovery_.incarnation = manifest.incarnation;
  recovery_.writer_epoch = manifest.epoch;
  recovery_.head_generation = manifest.head;
  recovery_.head_present = manifest.head.value() != 0;
  return Result<void>();
}

Result<void> CoolingCapacityStore::load_and_check_manifest() {
  const std::string path = detail::join(directory_, kManifestName);
  CCAP_TRY_DECLARE(exists, detail::path_exists(path));
  if (!exists) {
    recovery_.manifest_present = false;
    return Error(ErrorCode::MissingManifest, "the store has no manifest")
        .with("path", path);
  }
  CCAP_TRY_DECLARE(link, detail::is_link_like(path));
  if (link) {
    return Error(ErrorCode::PathInvalid,
                 "the manifest is a symbolic link, junction or reparse point")
        .with("path", path);
  }
  CCAP_TRY_DECLARE(bytes, detail::read_file(path, kManifestSize * 4U));
  CCAP_TRY_DECLARE(manifest, decode_manifest(bytes));
  recovery_.manifest_present = true;
  recovery_.manifest_valid = true;
  recovery_.incarnation = manifest.incarnation;
  recovery_.writer_epoch = manifest.epoch;
  recovery_.head_generation = manifest.head;
  recovery_.head_present = manifest.head.value() != 0;
  epoch_ = manifest.epoch;
  head_ = manifest.head;
  head_digest_ = manifest.head_digest;
  head_bytes_ = manifest.head_bytes;
  store_id_ = manifest.store_id;
  manifest_sequence_ = manifest.sequence;
  return Result<void>();
}

Result<void> CoolingCapacityStore::reconstruct_head() {
  recovery_.head_reconstructed = true;
  recovery_.warnings.push_back(
      "the manifest was unreadable and the head was reconstructed from generation files");
  CCAP_TRY_DECLARE(names, detail::list_directory(directory_));
  std::vector<CapacityGeneration> candidates;
  for (const std::string& name : names) {
    if (!starts_with(name, kGenerationPrefix) || !ends_with(name, kGenerationSuffix)) {
      continue;
    }
    const std::string_view middle(name.data() + (sizeof(kGenerationPrefix) - 1U),
                                  name.size() - (sizeof(kGenerationPrefix) - 1U) -
                                      (sizeof(kGenerationSuffix) - 1U));
    CapacityGeneration generation;
    if (parse_generation_number(middle, generation)) {
      candidates.push_back(generation);
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](CapacityGeneration lhs, CapacityGeneration rhs) { return lhs > rhs; });
  for (const CapacityGeneration& generation : candidates) {
    CCAP_TRY_DECLARE(path, generation_file_name(generation));
    Result<std::string> bytes = detail::read_file(path, options_.limits.max_artifact_bytes);
    if (!bytes.ok()) {
      recovery_.warnings.push_back("generation " + generation.to_string() +
                                   " could not be read during reconstruction: " +
                                   bytes.error().message());
      continue;
    }
    Result<std::shared_ptr<const CoolingSnapshot>> decoded =
        decode_snapshot(bytes.value(), options_.limits);
    if (!decoded.ok()) {
      recovery_.warnings.push_back("generation " + generation.to_string() +
                                   " failed verification during reconstruction: " +
                                   decoded.error().message());
      continue;
    }
    head_ = generation;
    head_digest_ = decoded.value()->digest();
    recovery_.head_present = true;
    recovery_.head_generation = generation;
    recovery_.head_replaced_from_earlier_generation = true;
    CCAP_TRY(write_manifest(head_, head_digest_, Timestamp::epoch()));
    return Result<void>();
  }
  return Error(ErrorCode::Corruption,
               "the manifest is unreadable and no generation could be verified")
      .with("directory", directory_);
}

Result<void> CoolingCapacityStore::initialize() {
  const Result<void> loaded = load_and_check_manifest();
  if (loaded.ok()) {
    return Result<void>();
  }
  const ErrorCode cause = loaded.error().code();
  const bool missing = cause == ErrorCode::MissingManifest;
  const bool damaged = cause == ErrorCode::Corruption || cause == ErrorCode::DigestMismatch ||
                       cause == ErrorCode::UnsupportedFormat || cause == ErrorCode::Truncated ||
                       cause == ErrorCode::IncompatibleVersion ||
                       cause == ErrorCode::WrongEndianness;
  if (!missing && !damaged) {
    return loaded.error();
  }

  if (missing) {
    if (!options_.create_if_missing) {
      return loaded.error();
    }
    if (options_.access == StoreAccess::ReadOnly) {
      return Error(ErrorCode::ReadOnly, "a read-only handle cannot create a store")
          .with("path", directory_);
    }
    store_id_ = make_store_id();
    manifest_sequence_ = 0;
    recovery_.incarnation = StoreIncarnation::first();
    epoch_ = WriterEpoch::from_value(0);
    head_ = CapacityGeneration::from_value(0);
    head_digest_ = Digest();
    head_bytes_ = 0;
    CCAP_TRY(write_manifest(head_, head_digest_, Timestamp::epoch()));
    recovery_.warnings.push_back("created a new store");
    return Result<void>();
  }

  // The manifest exists but cannot be trusted.
  if (options_.head_recovery == HeadRecovery::Strict) {
    return loaded.error();
  }
  if (options_.access == StoreAccess::ReadOnly) {
    return Error(ErrorCode::ReadOnly,
                 "a damaged store cannot be reconstructed through a read-only handle")
        .with("path", directory_)
        .with("cause", loaded.error().message());
  }
  recovery_.warnings.push_back("manifest problem: " + loaded.error().message());
  recovery_.manifest_valid = false;
  store_id_ = make_store_id();
  manifest_sequence_ = 0;
  recovery_.incarnation = StoreIncarnation::first();
  epoch_ = WriterEpoch::from_value(0);
  return reconstruct_head();
}

Result<std::unique_ptr<CoolingCapacityStore>> CoolingCapacityStore::open(
    const std::string& directory, const StoreOpenOptions& options) {
  CCAP_TRY(options.limits.validate());
  CCAP_TRY_DECLARE(absolute, detail::absolute_path(directory));
  CCAP_TRY_DECLARE(exists, detail::path_exists(absolute));
  if (exists) {
    CCAP_TRY_DECLARE(is_dir, detail::is_directory(absolute));
    if (!is_dir) {
      return Error(ErrorCode::PathInvalid, "the store path exists and is not a directory")
          .with("path", absolute);
    }
    CCAP_TRY_DECLARE(link, detail::is_link_like(absolute));
    if (link && !options.allow_reparse_root) {
      return Error(ErrorCode::PathInvalid,
                   "the store root is a symbolic link, junction or reparse point; pass "
                   "allow_reparse_root to accept it")
          .with("path", absolute);
    }
  } else {
    if (!options.create_if_missing) {
      return Error(ErrorCode::NotFound, "the store directory does not exist")
          .with("path", absolute);
    }
    if (options.access == StoreAccess::ReadOnly) {
      return Error(ErrorCode::ReadOnly, "a read-only handle cannot create a store")
          .with("path", absolute);
    }
    CCAP_TRY(detail::ensure_directory(absolute));
  }

  std::unique_ptr<CoolingCapacityStore> store(
      new CoolingCapacityStore(absolute, options));
  // The handle is marked open before initialisation, because initialisation
  // itself reads the generations back and those reads go through the same
  // open-handle checks as every other read.
  store->state_ = options.access == StoreAccess::ReadOnly ? StoreState::OpenReadOnly
                                                          : StoreState::OpenWritable;
  CCAP_TRY(store->initialize());
  CCAP_TRY(store->clean_staging());
  CCAP_TRY_DECLARE(generations, store->generations());
  store->recovery_.generations = generations;
  return store;
}

Result<void> CoolingCapacityStore::close() {
  if (state_ == StoreState::Closed) {
    return Result<void>();
  }
  lock_.reset();
  state_ = StoreState::Closed;
  return Result<void>();
}

Result<WriterEpoch> CoolingCapacityStore::acquire_writer(const ActorId& actor) {
  (void)actor;
  if (state_ == StoreState::Closed) {
    return Error(ErrorCode::Closed, "the store is closed").with("path", directory_);
  }
  if (options_.access == StoreAccess::ReadOnly) {
    return Error(ErrorCode::ReadOnly, "the store was opened read-only")
        .with("path", directory_);
  }
  if (state_ == StoreState::WriterHeld) {
    return epoch_;
  }
  CCAP_TRY_DECLARE(lock, detail::WriterLock::acquire(detail::join(directory_, kLockName)));
  lock_ = std::move(lock);

  // Re-read the manifest under the lock so that the epoch is advanced against
  // the state the previous writer left behind, not against a stale copy.
  const Result<void> reloaded = load_and_check_manifest();
  if (!reloaded.ok() && reloaded.error().code() != ErrorCode::MissingManifest) {
    lock_.reset();
    return reloaded.error();
  }
  if (epoch_.value() >= options_.limits.max_writer_epoch) {
    lock_.reset();
    return Error(ErrorCode::LimitExceeded, "the writer epoch is exhausted")
        .with("epoch", epoch_.to_string());
  }
  CCAP_TRY_DECLARE(next_epoch, epoch_.next());
  epoch_ = next_epoch;
  const Result<void> written = write_manifest(head_, head_digest_, Timestamp::epoch());
  if (!written.ok()) {
    lock_.reset();
    return written.error();
  }
  state_ = StoreState::WriterHeld;
  return epoch_;
}

Result<void> CoolingCapacityStore::release_writer() {
  if (state_ != StoreState::WriterHeld) {
    return Result<void>();
  }
  lock_.reset();
  state_ = StoreState::OpenWritable;
  return Result<void>();
}

Result<WriterEpoch> CoolingCapacityStore::writer_epoch() const {
  if (state_ == StoreState::Closed) {
    return Error(ErrorCode::Closed, "the store is closed").with("path", directory_);
  }
  return epoch_;
}

Result<CapacityGeneration> CoolingCapacityStore::head_generation() const {
  if (state_ == StoreState::Closed) {
    return Error(ErrorCode::Closed, "the store is closed").with("path", directory_);
  }
  if (head_.value() == 0) {
    return Error(ErrorCode::NotFound, "the store holds no generation yet")
        .with("path", directory_);
  }
  return head_;
}

Result<std::string> CoolingCapacityStore::read_generation_bytes(CapacityGeneration generation,
                                                               GenerationInfo* info) const {
  CCAP_TRY_DECLARE(path, generation_file_name(generation));
  CCAP_TRY_DECLARE(exists, detail::path_exists(path));
  if (!exists) {
    return Error(ErrorCode::NotFound, "the generation file is not present")
        .with("generation", generation.to_string())
        .with("path", path);
  }
  CCAP_TRY_DECLARE(link, detail::is_link_like(path));
  if (link) {
    return Error(ErrorCode::PathInvalid,
                 "the generation file is a symbolic link, junction or reparse point")
        .with("path", path);
  }
  CCAP_TRY_DECLARE(size, detail::file_size(path));
  if (size > options_.limits.max_artifact_bytes) {
    return Error(ErrorCode::Oversized, "the generation file exceeds the configured maximum")
        .with("generation", generation.to_string())
        .with("bytes", std::to_string(size))
        .with("maximum", std::to_string(options_.limits.max_artifact_bytes));
  }
  CCAP_TRY_DECLARE(bytes, detail::read_file(path, options_.limits.max_artifact_bytes));
  if (info != nullptr) {
    info->generation = generation;
    info->bytes = size;
    info->readable = true;
    info->is_head = generation == head_;
    Result<ArtifactManifest> manifest = read_artifact_manifest(bytes);
    if (manifest.ok()) {
      info->digest = manifest.value().digest;
    }
  }
  return bytes;
}

Result<std::shared_ptr<const CoolingSnapshot>> CoolingCapacityStore::load_generation(
    CapacityGeneration generation) const {
  if (state_ == StoreState::Closed) {
    return Error(ErrorCode::Closed, "the store is closed").with("path", directory_);
  }
  CCAP_TRY_DECLARE(bytes, read_generation_bytes(generation, nullptr));
  CCAP_TRY_DECLARE(snapshot, decode_snapshot(bytes, options_.limits));
  if (snapshot->generation() != generation) {
    return Error(ErrorCode::Corruption,
                 "the generation file declares a different generation than its name")
        .with("file_generation", generation.to_string())
        .with("declared_generation", snapshot->generation().to_string());
  }
  return snapshot;
}

Result<std::shared_ptr<const CoolingSnapshot>> CoolingCapacityStore::load_head() const {
  // The manifest is re-read so that a handle sees the head another writer has
  // published, rather than the head it happened to observe when it opened.
  CCAP_TRY_DECLARE(generation, head_generation());
  CCAP_TRY_DECLARE(snapshot, load_generation(generation));
  std::shared_ptr<const CoolingSnapshot> result = snapshot;
  if (result->digest() != head_digest_) {
    return Error(ErrorCode::DigestMismatch,
                 "the head generation does not match the digest the manifest records")
        .with("generation", generation.to_string())
        .with("manifest_digest", head_digest_.to_hex())
        .with("file_digest", result->digest().to_hex());
  }
  return result->with_provenance(SnapshotOrigin::RecoveredFromStore, std::nullopt);
}

Result<std::vector<GenerationInfo>> CoolingCapacityStore::generations() const {
  if (state_ == StoreState::Closed) {
    return Error(ErrorCode::Closed, "the store is closed").with("path", directory_);
  }
  CCAP_TRY_DECLARE(names, detail::list_directory(directory_));
  std::vector<GenerationInfo> found;
  for (const std::string& name : names) {
    if (!starts_with(name, kGenerationPrefix) || !ends_with(name, kGenerationSuffix)) {
      continue;
    }
    const std::string_view middle(name.data() + (sizeof(kGenerationPrefix) - 1U),
                                  name.size() - (sizeof(kGenerationPrefix) - 1U) -
                                      (sizeof(kGenerationSuffix) - 1U));
    CapacityGeneration generation;
    if (!parse_generation_number(middle, generation)) {
      continue;
    }
    GenerationInfo info;
    info.generation = generation;
    info.is_head = generation == head_;
    Result<std::string> bytes = read_generation_bytes(generation, &info);
    if (!bytes.ok()) {
      info.readable = false;
    } else {
      Result<ArtifactManifest> manifest = read_artifact_manifest(bytes.value());
      info.readable = manifest.ok();
      if (manifest.ok()) {
        info.digest = manifest.value().digest;
        // A file whose contents declare a different generation than its name is
        // not a readable generation of this store, even though its bytes are
        // intact. Reporting it as readable would let a renamed file masquerade
        // as history it does not hold.
        if (manifest.value().generation != generation) {
          info.readable = false;
        }
      }
    }
    found.push_back(info);
  }
  std::sort(found.begin(), found.end(), [](const GenerationInfo& lhs, const GenerationInfo& rhs) {
    return lhs.generation < rhs.generation;
  });
  return found;
}

Result<CommitReceipt> CoolingCapacityStore::commit(const CoolingSnapshot& candidate,
                                                   const CommitOptions& options) {
  if (state_ == StoreState::Closed) {
    return Error(ErrorCode::Closed, "the store is closed").with("path", directory_);
  }
  if (state_ != StoreState::WriterHeld) {
    return Error(ErrorCode::WriterBusy, "the store does not hold writer authority")
        .with("path", directory_);
  }

  // Plan and validate: the caller's precondition is checked against the
  // manifest on disk, not against an in-memory copy.
  CCAP_TRY(load_and_check_manifest());
  if (head_ != options.expected_generation) {
    return Error::stale_generation("the store head moved since the request was formed",
                                   options.expected_generation.value(), head_.value());
  }
  CCAP_TRY_DECLARE(next_generation, head_.next());
  if (candidate.generation() != next_generation) {
    return Error(ErrorCode::Conflict,
                 "the candidate generation is not the successor of the current head")
        .with("candidate", candidate.generation().to_string())
        .with("expected", next_generation.to_string());
  }

  // Reserve the generation and write staging.
  CCAP_TRY_DECLARE(artifact, encode_snapshot(candidate, options_.limits));
  std::string staging_name(kStagingPrefix);
  staging_name.append(epoch_.to_string());
  staging_name.push_back('-');
  staging_name.append(candidate.generation().to_string());
  staging_name.append(kStagingSuffix);
  const std::string staging_path = detail::join(directory_, staging_name);
  CCAP_TRY(detail::remove_file(staging_path));
  CCAP_TRY(detail::write_file_create_new(staging_path, artifact, true));

  // Verify the staged bytes before anything becomes visible.
  const Result<std::string> staged = detail::read_file(staging_path, options_.limits.max_artifact_bytes);
  if (!staged.ok()) {
    CCAP_TRY(detail::remove_file(staging_path));
    return staged.error();
  }
  if (staged.value() != artifact) {
    CCAP_TRY(detail::remove_file(staging_path));
    return Error(ErrorCode::Corruption, "the staged generation did not read back identically")
        .with("path", staging_path);
  }

  // Atomic publish, then a directory flush.
  CCAP_TRY_DECLARE(target, generation_file_name(candidate.generation()));
  const Result<void> published = detail::publish_new_file(staging_path, target, true);
  if (!published.ok()) {
    if (published.error().code() != ErrorCode::AlreadyExists) {
      CCAP_TRY(detail::remove_file(staging_path));
      return published.error();
    }
    // The generation file is already there. That is what a commit that died
    // between publishing the generation and replacing the manifest leaves
    // behind, and the correct recovery is to finish the commit rather than to
    // refuse it. The artifact is a pure function of the candidate, so the
    // bytes already on disk must be exactly the bytes that were about to be
    // written; anything else is a genuine conflict.
    CCAP_TRY_DECLARE(existing, detail::read_file(target, options_.limits.max_artifact_bytes));
    if (existing != artifact) {
      CCAP_TRY(detail::remove_file(staging_path));
      return Error(ErrorCode::Corruption,
                   "a different artifact is already published under this generation number")
          .with("generation", candidate.generation().to_string())
          .with("path", target);
    }
    CCAP_TRY(detail::remove_file(staging_path));
  }
  CCAP_TRY(detail::sync_directory(directory_));

  // Re-read what was published.
  const Result<std::string> published_bytes =
      detail::read_file(target, options_.limits.max_artifact_bytes);
  if (!published_bytes.ok()) {
    return published_bytes.error();
  }
  if (published_bytes.value() != artifact) {
    return Error(ErrorCode::Corruption, "the published generation does not match what was staged")
        .with("path", target);
  }

  // The manifest replacement is the commit point.
  const Digest digest = candidate.digest();
  CCAP_TRY(write_manifest(candidate.generation(), digest, options.committed_at));

  head_ = candidate.generation();
  head_digest_ = digest;

  // Retire everything beyond the retention window, oldest first, never the head.
  CCAP_TRY_DECLARE(all, generations());
  if (all.size() > options_.limits.max_generations_retained) {
    const std::size_t excess = all.size() - options_.limits.max_generations_retained;
    for (std::size_t index = 0; index < excess; ++index) {
      if (all[index].generation == head_) {
        continue;
      }
      CCAP_TRY_DECLARE(path, generation_file_name(all[index].generation));
      CCAP_TRY(detail::remove_file(path));
    }
  }

  CommitReceipt receipt;
  receipt.generation = candidate.generation();
  receipt.digest = digest;
  receipt.epoch = epoch_;
  receipt.committed_at = options.committed_at;
  receipt.artifact_bytes = static_cast<std::uint64_t>(artifact.size());
  receipt.manifest_bytes = kManifestSize;
  receipt.head_replaced = true;
  recovery_.head_generation = head_;
  recovery_.head_present = true;
  return receipt;
}

Result<void> CoolingCapacityStore::retire(CapacityGeneration generation) {
  if (state_ == StoreState::Closed) {
    return Error(ErrorCode::Closed, "the store is closed").with("path", directory_);
  }
  if (state_ != StoreState::WriterHeld) {
    return Error(ErrorCode::WriterBusy, "retiring a generation requires writer authority")
        .with("path", directory_);
  }
  if (generation == head_) {
    return Error(ErrorCode::PreconditionFailed,
                 "the head generation cannot be retired; commit a successor first")
        .with("generation", generation.to_string());
  }
  CCAP_TRY_DECLARE(path, generation_file_name(generation));
  CCAP_TRY_DECLARE(exists, detail::path_exists(path));
  if (!exists) {
    return Error(ErrorCode::NotFound, "the generation to retire is not present")
        .with("generation", generation.to_string())
        .with("path", path);
  }
  CCAP_TRY(detail::remove_file(path));
  CCAP_TRY(detail::sync_directory(directory_));
  return Result<void>();
}

Result<StoreVerification> CoolingCapacityStore::verify() const {
  StoreVerification verification;
  if (state_ == StoreState::Closed) {
    return Error(ErrorCode::Closed, "the store is closed").with("path", directory_);
  }
  const Result<std::string> manifest_bytes =
      detail::read_file(detail::join(directory_, kManifestName), kManifestSize * 4U);
  if (!manifest_bytes.ok()) {
    verification.problems.push_back(manifest_bytes.error().to_string());
    return verification;
  }
  const Result<Manifest> manifest = decode_manifest(manifest_bytes.value());
  if (!manifest.ok()) {
    verification.problems.push_back(manifest.error().to_string());
    return verification;
  }
  verification.head_generation = manifest.value().head;
  verification.head_digest = manifest.value().head_digest;

  CCAP_TRY_DECLARE(names, detail::list_directory(directory_));
  for (const std::string& name : names) {
    if (name == kManifestName || name == kManifestStagingName || name == kLockName) {
      continue;
    }
    if (!starts_with(name, kGenerationPrefix) || !ends_with(name, kGenerationSuffix)) {
      ++verification.stray_files;
      verification.problems.push_back("stray file in the store: " + name);
      continue;
    }
    const std::string_view middle(name.data() + (sizeof(kGenerationPrefix) - 1U),
                                  name.size() - (sizeof(kGenerationPrefix) - 1U) -
                                      (sizeof(kGenerationSuffix) - 1U));
    CapacityGeneration generation;
    if (!parse_generation_number(middle, generation)) {
      ++verification.stray_files;
      verification.problems.push_back("generation file with an unparsable number: " + name);
      continue;
    }
    GenerationInfo info;
    info.generation = generation;
    info.is_head = generation == manifest.value().head;
    const Result<std::string> bytes = read_generation_bytes(generation, &info);
    if (!bytes.ok()) {
      info.readable = false;
      ++verification.unreadable_generations;
      verification.problems.push_back("generation " + generation.to_string() +
                                      " could not be read: " + bytes.error().to_string());
      verification.generations.push_back(info);
      continue;
    }
    const Result<std::shared_ptr<const CoolingSnapshot>> decoded =
        decode_snapshot(bytes.value(), options_.limits);
    if (!decoded.ok()) {
      info.readable = false;
      ++verification.unreadable_generations;
      verification.problems.push_back("generation " + generation.to_string() +
                                      " failed to decode: " + decoded.error().to_string());
      verification.generations.push_back(info);
      continue;
    }
    info.readable = true;
    info.digest = decoded.value()->digest();
    if (decoded.value()->generation() != generation) {
      info.readable = false;
      verification.problems.push_back("generation " + generation.to_string() +
                                      " declares generation " +
                                      decoded.value()->generation().to_string());
      verification.generations.push_back(info);
      continue;
    }
    if (generation == manifest.value().head) {
      verification.head_readable = true;
      if (info.digest != manifest.value().head_digest) {
        verification.problems.push_back(
            "the head generation digest does not match the manifest");
      }
    }
    const ValidationReport report = validate_snapshot(*decoded.value());
    for (const ValidationFinding& finding : report.findings()) {
      verification.validation.add(finding);
    }
    verification.generations.push_back(info);
  }
  std::sort(verification.generations.begin(), verification.generations.end(),
            [](const GenerationInfo& lhs, const GenerationInfo& rhs) {
              return lhs.generation < rhs.generation;
            });
  if (!verification.head_readable) {
    verification.problems.push_back("the head generation is not readable");
  }
  if (verification.generations.empty()) {
    verification.problems.push_back("the store contains no generation files");
  }
  verification.ok = verification.problems.empty() && verification.validation.ok() &&
                    verification.head_readable && !verification.generations.empty();
  return verification;
}

}  // namespace cooling_capacity
