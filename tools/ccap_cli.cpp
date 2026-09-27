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

// ccap - the Cooling Capacity inspection and administration tool.
//
// The tool is deliberately strict: unknown options, malformed values and
// missing required options are refused rather than defaulted, and every
// command exits with a stable code derived from the error model so that it can
// be used from a script.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "cooling_capacity/cooling_capacity.hpp"

namespace {

using cooling_capacity::CapacityGeneration;
using cooling_capacity::CapacityValue;
using cooling_capacity::CoolingCapacityEngine;
using cooling_capacity::CoolingMedium;
using cooling_capacity::CompatibilityClass;
using cooling_capacity::ErrorCode;
using cooling_capacity::EngineOptions;
using cooling_capacity::Identifier;
using cooling_capacity::Limits;
using cooling_capacity::LoopId;
using cooling_capacity::PlantId;
using cooling_capacity::RedundancyClass;
using cooling_capacity::Result;
using cooling_capacity::ThermalPower;
using cooling_capacity::ZoneId;

// Stable exit codes. They are part of the tool's contract.
constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitNotFound = 2;
constexpr int kExitRefused = 3;
constexpr int kExitNotAdmitted = 4;
constexpr int kExitIndeterminate = 5;
constexpr int kExitIntegrity = 6;

int exit_code_for(const cooling_capacity::Error& error) {
  switch (error.code()) {
    case ErrorCode::Ok:
      return kExitOk;
    case ErrorCode::InvalidArgument:
    case ErrorCode::InvalidIdentifier:
    case ErrorCode::InvalidEnumValue:
    case ErrorCode::InvalidUtf8:
    case ErrorCode::InvalidText:
      return kExitUsage;
    case ErrorCode::NotFound:
    case ErrorCode::MissingManifest:
      return kExitNotFound;
    case ErrorCode::StaleGeneration:
    case ErrorCode::StaleAuthority:
    case ErrorCode::StaleEpoch:
    case ErrorCode::StaleEvidence:
    case ErrorCode::PreconditionFailed:
    case ErrorCode::Conflict:
    case ErrorCode::IdempotencyConflict:
    case ErrorCode::LockConflict:
    case ErrorCode::WriterBusy:
    case ErrorCode::ReadOnly:
    case ErrorCode::AlreadyExists:
      return kExitRefused;
    case ErrorCode::CapacityExceeded:
    case ErrorCode::IncompatibleClass:
    case ErrorCode::IncompatibleMedium:
    case ErrorCode::RedundancyUnsatisfied:
    case ErrorCode::ReserveViolation:
    case ErrorCode::Unsupported:
      return kExitNotAdmitted;
    case ErrorCode::Unknown:
    case ErrorCode::Indeterminate:
    case ErrorCode::Unavailable:
      return kExitIndeterminate;
    case ErrorCode::IncompatibleVersion:
    case ErrorCode::Corruption:
    case ErrorCode::Truncated:
    case ErrorCode::Oversized:
    case ErrorCode::WrongEndianness:
    case ErrorCode::DigestMismatch:
    case ErrorCode::UnsupportedFormat:
    case ErrorCode::IoFailure:
    case ErrorCode::PermissionDenied:
    case ErrorCode::PathInvalid:
    case ErrorCode::InvariantViolation:
    case ErrorCode::InternalError:
    case ErrorCode::ArithmeticOverflow:
    case ErrorCode::LimitExceeded:
    case ErrorCode::ProcessFailure:
    case ErrorCode::DuplicateIdentity:
    case ErrorCode::MissingReference:
    case ErrorCode::DanglingReference:
    case ErrorCode::SelfReference:
    case ErrorCode::CyclicReference:
    case ErrorCode::Closed:
    case ErrorCode::NotOpen:
      return kExitIntegrity;
  }
  return kExitIntegrity;
}

int fail(const cooling_capacity::Error& error) {
  std::cerr << "error: " << error.to_string() << "\n";
  return exit_code_for(error);
}

struct Options {
  std::map<std::string, std::string> values;
  std::vector<std::string> positional;

  [[nodiscard]] bool has(const std::string& key) const { return values.count(key) != 0; }

  [[nodiscard]] Result<std::string> required(const std::string& key) const {
    const auto found = values.find(key);
    if (found == values.end()) {
      return cooling_capacity::Error(ErrorCode::InvalidArgument,
                                     "missing required option --" + key);
    }
    return found->second;
  }

  [[nodiscard]] std::string optional(const std::string& key,
                                     const std::string& fallback) const {
    const auto found = values.find(key);
    return found == values.end() ? fallback : found->second;
  }
};

void usage() {
  std::cout <<
      "ccap " << cooling_capacity::version_string()
            << " - Cooling Capacity inspection and administration\n"
               "\n"
               "usage: ccap <command> --store <directory> [options]\n"
               "\n"
               "commands:\n"
               "  version\n"
               "  verify       --store DIR                 verify the store end to end\n"
               "  generations  --store DIR                 list stored generations\n"
               "  show         --store DIR                 print every rollup\n"
               "  capacity     --store DIR --zone Z --medium air|liquid --class C\n"
               "  explain      --store DIR --zone Z --medium air|liquid --class C\n"
               "  evaluate     --store DIR --zone Z --medium M --class C --thermal-mw N\n"
               "               [--redundancy n|n+1|n+2|2n|2n+1|2(n+1)] [--loop L]\n"
               "  plan|hold|commit --store DIR --zone Z --medium M --class C --thermal-mw N\n"
               "               --commitment ID --attempt ID --actor ID [--expires-at TS]\n"
               "  release|expire --store DIR --commitment ID --attempt ID --actor ID\n"
               "               [--zone Z] [--medium M] [--class C]\n"
               "  revalidate   --store DIR                 revalidate recovered state\n"
               "  export       --store DIR [--generation N]  canonical text export\n"
               "  diff         --store DIR --from N --to M\n"
               "\n"
               "  --self-check                             run the built-in self check\n"
               "  --scenario <directory>                   run a full lifecycle scenario\n"
               "\n"
               "exit codes: 0 ok, 1 usage, 2 not found, 3 refused, 4 not admitted,\n"
               "            5 indeterminate, 6 integrity or I/O failure\n";
}

Result<CoolingMedium> parse_medium(const std::string& text) {
  return cooling_capacity::parse_cooling_medium(text);
}

Result<ThermalPower> parse_power(const std::string& text) {
  CCAP_TRY_DECLARE(milliwatts, cooling_capacity::parse_int64(text));
  return ThermalPower::from_milliwatts(milliwatts);
}

void print_value(const std::string& label, const CapacityValue& value) {
  std::cout << "  " << label << ": " << value.to_string() << "\n";
}

struct Query {
  ZoneId zone;
  CoolingMedium medium = CoolingMedium::Air;
  CompatibilityClass compatibility = CompatibilityClass::AirConvection;
};

// Read-only commands never take writer authority, so they can inspect a store
// that another process is writing.
EngineOptions read_engine() {
  EngineOptions options;
  options.actor = cooling_capacity::ActorId::literal("ccap-cli-reader");
  options.store.access = cooling_capacity::StoreAccess::ReadOnly;
  options.store.create_if_missing = false;
  return options;
}

EngineOptions write_engine() {
  EngineOptions options;
  options.actor = cooling_capacity::ActorId::literal("ccap-cli");
  options.store.access = cooling_capacity::StoreAccess::ReadWrite;
  options.store.create_if_missing = true;
  return options;
}

void print_breakdown(const cooling_capacity::CapacityBreakdown& breakdown) {
  print_value("nominal", breakdown.nominal);
  print_value("validated", breakdown.validated);
  print_value("derated", breakdown.derated);
  print_value("degraded", breakdown.degraded);
  print_value("usable", breakdown.usable);
  print_value("committed", breakdown.committed);
  print_value("observed", breakdown.observed);
  print_value("observed-headroom", breakdown.observed_headroom);
  print_value("reserve", breakdown.reserve);
  print_value("deficit", breakdown.deficit);
  std::cout << "  basis: " << cooling_capacity::to_string(breakdown.basis) << "\n";
  std::cout << "  over-committed: " << (breakdown.over_committed ? "yes" : "no") << "\n";
}

void print_zone(const cooling_capacity::ZoneCapacity& view) {
  std::cout << "zone " << view.zone.str() << " medium " << cooling_capacity::to_string(view.medium)
            << " class " << cooling_capacity::to_string(view.compatibility) << "\n";
  print_breakdown(view.breakdown);
  print_value("local-headroom", view.local_headroom);
  if (view.shared_headroom.has_value()) {
    print_value("shared-headroom", *view.shared_headroom);
    std::cout << "  shared-headroom-is-shared: yes\n";
  }
  print_value("offered", view.offered);
  for (const cooling_capacity::SharedPoolView& pool : view.shared_pools) {
    std::cout << "  pool " << pool.plant.str() << " usable " << pool.usable.to_string()
              << " committed " << pool.committed.to_string() << " headroom "
              << pool.headroom.to_string() << "\n";
  }
  if (view.bottleneck.has_value()) {
    std::cout << "  bottleneck: " << view.bottleneck->to_string() << "\n";
  }
}

Result<int> command_version(const Options& options) {
  (void)options;  // `version` intentionally takes no options.
  std::cout << "ccap " << cooling_capacity::version_string() << "\n";
  std::cout << "artifact format " << cooling_capacity::artifact_format_version() << "\n";
  return kExitOk;
}

Result<int> command_verify(const Options& options) {
  CCAP_TRY_DECLARE(directory, options.required("store"));
  cooling_capacity::StoreOpenOptions open;
  open.access = cooling_capacity::StoreAccess::ReadOnly;
  CCAP_TRY_DECLARE(store, cooling_capacity::CoolingCapacityStore::open(directory, open));
  CCAP_TRY_DECLARE(report, store->verify());
  std::cout << report.render();
  if (!store->recovery().warnings.empty()) {
    for (const std::string& warning : store->recovery().warnings) {
      std::cout << "recovery: " << warning << "\n";
    }
  }
  return report.ok ? kExitOk : kExitIntegrity;
}

Result<int> command_generations(const Options& options) {
  CCAP_TRY_DECLARE(directory, options.required("store"));
  CCAP_TRY_DECLARE(engine, CoolingCapacityEngine::open(directory, read_engine()));
  CCAP_TRY_DECLARE(generations, engine->generations());
  for (const cooling_capacity::GenerationInfo& info : generations) {
    std::cout << "generation " << info.generation.to_string() << " bytes " << info.bytes
              << " readable " << (info.readable ? "yes" : "no") << " head "
              << (info.is_head ? "yes" : "no") << " digest " << info.digest.to_hex() << "\n";
  }
  return kExitOk;
}

Result<int> command_show(const Options& options) {
  CCAP_TRY_DECLARE(directory, options.required("store"));
  CCAP_TRY_DECLARE(engine, CoolingCapacityEngine::open(directory, read_engine()));
  CCAP_TRY_DECLARE(report, engine->report());
  std::cout << "generation " << report.generation.to_string() << " basis "
            << cooling_capacity::to_string(report.basis) << " evaluated_at "
            << report.evaluated_at.to_string() << "\n";
  std::cout << "policy " << report.policy.str() << " revision "
            << report.policy_revision.to_string() << "\n";
  std::cout << "\nzones: " << report.zones.size() << "\n";
  for (const cooling_capacity::ZoneCapacity& view : report.zones) {
    print_zone(view);
    std::cout << "\n";
  }
  std::cout << "loops: " << report.loops.size() << "\n";
  for (const cooling_capacity::LoopCapacity& view : report.loops) {
    std::cout << "loop " << view.loop.str() << " medium "
              << cooling_capacity::to_string(view.medium) << "\n";
    print_breakdown(view.breakdown);
    std::cout << "  redundancy declared "
              << cooling_capacity::to_string(view.redundancy.declared) << " effective "
              << cooling_capacity::to_string(view.redundancy.effective) << "\n\n";
  }
  std::cout << "plants: " << report.plants.size() << "\n";
  for (const cooling_capacity::PlantCapacity& view : report.plants) {
    std::cout << "plant " << view.plant.str() << " medium "
              << cooling_capacity::to_string(view.medium) << "\n";
    print_breakdown(view.breakdown);
    std::cout << "\n";
  }
  return kExitOk;
}

Result<Query> read_query(const Options& options) {
  Query query;
  CCAP_TRY_DECLARE(zone, options.required("zone"));
  CCAP_TRY_DECLARE(zone_id, ZoneId::parse(zone));
  query.zone = zone_id;
  CCAP_TRY_DECLARE(medium, options.required("medium"));
  CCAP_TRY_DECLARE(medium_value, parse_medium(medium));
  query.medium = medium_value;
  CCAP_TRY_DECLARE(class_name, options.required("class"));
  CCAP_TRY_DECLARE(class_value, cooling_capacity::parse_compatibility_class(class_name));
  query.compatibility = class_value;
  if (cooling_capacity::medium_of(query.compatibility) != query.medium) {
    return cooling_capacity::Error(ErrorCode::IncompatibleMedium,
                                   "the compatibility class does not belong to the medium")
        .with("medium", medium)
        .with("class", class_name);
  }
  return query;
}

Result<int> command_capacity(const Options& options) {
  CCAP_TRY_DECLARE(directory, options.required("store"));
  CCAP_TRY_DECLARE(query, read_query(options));
  CCAP_TRY_DECLARE(engine, CoolingCapacityEngine::open(directory, read_engine()));
  CCAP_TRY_DECLARE(view, engine->zone_capacity(query.zone, query.medium, query.compatibility));
  print_zone(view);
  return kExitOk;
}

Result<int> command_explain(const Options& options) {
  CCAP_TRY_DECLARE(directory, options.required("store"));
  CCAP_TRY_DECLARE(query, read_query(options));
  CCAP_TRY_DECLARE(engine, CoolingCapacityEngine::open(directory, read_engine()));
  CCAP_TRY_DECLARE(view, engine->zone_capacity(query.zone, query.medium, query.compatibility));
  std::cout << cooling_capacity::render_steps(view.explanation);
  return kExitOk;
}

Result<int> command_evaluate(const Options& options) {
  CCAP_TRY_DECLARE(directory, options.required("store"));
  CCAP_TRY_DECLARE(query, read_query(options));
  CCAP_TRY_DECLARE(thermal_text, options.required("thermal-mw"));
  CCAP_TRY_DECLARE(thermal, parse_power(thermal_text));
  cooling_capacity::CandidateLoad load;
  load.zone = query.zone;
  load.medium = query.medium;
  load.compatibility = query.compatibility;
  load.thermal = thermal;
  load.actor = cooling_capacity::ActorId::literal("ccap-cli");
  if (options.has("redundancy")) {
    CCAP_TRY_DECLARE(redundancy, cooling_capacity::parse_redundancy_class(
                                     options.values.at("redundancy")));
    load.required_redundancy = redundancy;
  }
  if (options.has("loop")) {
    CCAP_TRY_DECLARE(loop, LoopId::parse(options.values.at("loop")));
    load.pinned_loop = loop;
  }
  CCAP_TRY_DECLARE(engine, CoolingCapacityEngine::open(directory, read_engine()));
  CCAP_TRY_DECLARE(evaluation, engine->evaluate(load));
  std::cout << evaluation.to_string() << "\n";
  std::cout << cooling_capacity::render_steps(evaluation.explanation);
  switch (evaluation.disposition) {
    case cooling_capacity::CandidateDisposition::Admitted:
      return kExitOk;
    case cooling_capacity::CandidateDisposition::Rejected:
      return kExitNotAdmitted;
    case cooling_capacity::CandidateDisposition::Indeterminate:
      return kExitIndeterminate;
  }
  return kExitIntegrity;
}

Result<int> command_commitment(const Options& options, cooling_capacity::CommitmentOperation op) {
  CCAP_TRY_DECLARE(directory, options.required("store"));
  CCAP_TRY_DECLARE(attempt_text, options.required("attempt"));
  CCAP_TRY_DECLARE(attempt, cooling_capacity::AttemptId::parse(attempt_text));
  CCAP_TRY_DECLARE(actor_text, options.required("actor"));
  CCAP_TRY_DECLARE(actor, cooling_capacity::ActorId::parse(actor_text));

  cooling_capacity::CommitmentRequest request;
  request.operation = op;
  request.attempt = attempt;
  request.actor = actor;
  request.requested_at = cooling_capacity::SystemClock().now();

  const bool releasing = op == cooling_capacity::CommitmentOperation::Release ||
                         op == cooling_capacity::CommitmentOperation::Expire;
  if (releasing) {
    CCAP_TRY_DECLARE(commitment_text, options.required("commitment"));
    CCAP_TRY_DECLARE(commitment, cooling_capacity::CommitmentId::parse(commitment_text));
    request.id = commitment;
    request.target = commitment;
    request.zone = ZoneId::literal("unused");
  } else {
    CCAP_TRY_DECLARE(query, read_query(options));
    CCAP_TRY_DECLARE(commitment_text, options.required("commitment"));
    CCAP_TRY_DECLARE(commitment, cooling_capacity::CommitmentId::parse(commitment_text));
    CCAP_TRY_DECLARE(thermal_text, options.required("thermal-mw"));
    CCAP_TRY_DECLARE(thermal, parse_power(thermal_text));
    request.id = commitment;
    request.zone = query.zone;
    request.medium = query.medium;
    request.compatibility = query.compatibility;
    request.thermal = thermal;
    if (options.has("loop")) {
      CCAP_TRY_DECLARE(loop, LoopId::parse(options.values.at("loop")));
      request.pinned_loop = loop;
    }
    if (options.has("expires-at")) {
      CCAP_TRY_DECLARE(expires, cooling_capacity::parse_timestamp(options.values.at("expires-at")));
      request.expires_at = expires;
    }
  }

  CCAP_TRY_DECLARE(engine, CoolingCapacityEngine::open(directory, write_engine()));
  request.expected_generation = engine->snapshot()->generation();
  CCAP_TRY_DECLARE(outcome, engine->apply(request));
  std::cout << "commitment " << outcome.id.str() << " attempt " << outcome.attempt.str()
            << " state " << cooling_capacity::to_string(outcome.state)
            << " replayed " << (outcome.replayed ? "yes" : "no") << " generation "
            << outcome.generation.to_string() << "\n";
  std::cout << "committed " << outcome.committed_after.to_string() << " usable "
            << outcome.usable_after.to_string() << " reserve "
            << outcome.reserve_after.to_string() << "\n";
  return kExitOk;
}

Result<int> command_revalidate(const Options& options) {
  CCAP_TRY_DECLARE(directory, options.required("store"));
  CCAP_TRY_DECLARE(engine, CoolingCapacityEngine::open(directory, write_engine()));
  const cooling_capacity::SystemClock clock;
  CCAP_TRY_DECLARE(generation, engine->revalidate(clock));
  std::cout << "revalidated generation " << generation.to_string() << " basis "
            << cooling_capacity::to_string(engine->basis()) << "\n";
  return kExitOk;
}

Result<int> command_export(const Options& options) {
  CCAP_TRY_DECLARE(directory, options.required("store"));
  CCAP_TRY_DECLARE(engine, CoolingCapacityEngine::open(directory, read_engine()));
  std::shared_ptr<const cooling_capacity::CoolingSnapshot> snapshot = engine->snapshot();
  if (options.has("generation")) {
    CCAP_TRY_DECLARE(number, cooling_capacity::parse_uint64(options.values.at("generation")));
    CCAP_TRY_DECLARE(loaded,
                     engine->load_generation(CapacityGeneration::from_value(number)));
    snapshot = loaded;
  }
  std::cout << cooling_capacity::render_snapshot_text(*snapshot);
  return kExitOk;
}

Result<int> command_diff(const Options& options) {
  CCAP_TRY_DECLARE(directory, options.required("store"));
  CCAP_TRY_DECLARE(from_text, options.required("from"));
  CCAP_TRY_DECLARE(to_text, options.required("to"));
  CCAP_TRY_DECLARE(from, cooling_capacity::parse_uint64(from_text));
  CCAP_TRY_DECLARE(to, cooling_capacity::parse_uint64(to_text));
  CCAP_TRY_DECLARE(engine, CoolingCapacityEngine::open(directory, read_engine()));
  CCAP_TRY_DECLARE(report, engine->diff(CapacityGeneration::from_value(from),
                                        CapacityGeneration::from_value(to)));
  std::cout << cooling_capacity::render_steps(report.explanation);
  for (const cooling_capacity::RecordChange& change : report.changes) {
    std::cout << change.to_string() << "\n";
  }
  return kExitOk;
}

// ---------------------------------------------------------------------------
// Self check and scenario
// ---------------------------------------------------------------------------

// Adds the scenario's records, replacing anything already present. The scenario
// is run more than once against the same directory by the test suite, so a
// second run has to describe the same facility rather than refuse it as a
// duplicate.
Result<void> add_air_site(cooling_capacity::SnapshotBuilder& builder) {
  const cooling_capacity::SiteId site_id_value = cooling_capacity::SiteId::literal("site-a");
  const cooling_capacity::FacilityId facility_id_value =
      cooling_capacity::FacilityId::literal("dc-1");
  if (!builder.contains(cooling_capacity::EntityKind::Site, site_id_value.value())) {
    cooling_capacity::SiteRecord site;
    site.id = site_id_value;
    site.label = cooling_capacity::BoundedText::literal("Site A");
    site.revision = cooling_capacity::RecordRevision::first();
    CCAP_TRY(builder.add(site));
  }

  if (!builder.contains(cooling_capacity::EntityKind::Facility, facility_id_value.value())) {
    cooling_capacity::FacilityRecord facility;
    facility.id = facility_id_value;
    facility.site = site_id_value;
    facility.label = cooling_capacity::BoundedText::literal("Data Center 1");
    facility.revision = cooling_capacity::RecordRevision::first();
    CCAP_TRY(builder.add(facility));
  }

  cooling_capacity::EquipmentRecord crah_a;
  crah_a.id = cooling_capacity::EquipmentId::literal("crah-a");
  crah_a.facility = facility_id_value;
  crah_a.label = cooling_capacity::BoundedText::literal("CRAH A");
  crah_a.kind = cooling_capacity::EquipmentKind::Crah;
  crah_a.state = cooling_capacity::OperatingState::InService;
  crah_a.nominal = ThermalPower::from_watts(400000).value();
  crah_a.revision = cooling_capacity::RecordRevision::first();
  CCAP_TRY(builder.contains(cooling_capacity::EntityKind::Equipment, crah_a.id.value())
               ? builder.replace(crah_a)
               : builder.add(crah_a));

  cooling_capacity::EquipmentRecord crah_b = crah_a;
  crah_b.id = cooling_capacity::EquipmentId::literal("crah-b");
  crah_b.label = cooling_capacity::BoundedText::literal("CRAH B");
  CCAP_TRY(builder.contains(cooling_capacity::EntityKind::Equipment, crah_b.id.value())
               ? builder.replace(crah_b)
               : builder.add(crah_b));

  cooling_capacity::LoopRecord loop;
  loop.id = LoopId::literal("air-loop-1");
  loop.facility = facility_id_value;
  loop.label = cooling_capacity::BoundedText::literal("Hall air loop");
  loop.kind = cooling_capacity::LoopKind::AirSupply;
  loop.transport_limit = ThermalPower::from_watts(700000).value();
  loop.transport_limit_declared = true;
  loop.equipment = {crah_a.id, crah_b.id};
  loop.redundancy = RedundancyClass::NPlusOne;
  loop.revision = cooling_capacity::RecordRevision::first();
  CCAP_TRY(builder.contains(cooling_capacity::EntityKind::Loop, loop.id.value())
               ? builder.replace(loop)
               : builder.add(loop));

  cooling_capacity::ZoneRecord zone;
  zone.id = ZoneId::literal("hall-1");
  zone.facility = facility_id_value;
  zone.label = cooling_capacity::BoundedText::literal("Hall 1");
  zone.media.insert(CoolingMedium::Air);
  zone.compatibility = {CompatibilityClass::AirConvection};
  zone.loops = {loop.id};
  zone.revision = cooling_capacity::RecordRevision::first();
  CCAP_TRY(builder.contains(cooling_capacity::EntityKind::Zone, zone.id.value())
               ? builder.replace(zone)
               : builder.add(zone));
  return Result<void>();
}

Result<void> self_check() {
  cooling_capacity::SnapshotBuilder builder;
  builder.set_generation(CapacityGeneration::first());
  builder.set_constructed_at(cooling_capacity::Timestamp::epoch());
  CCAP_TRY(add_air_site(builder));
  cooling_capacity::ValidationReport report;
  CCAP_TRY_DECLARE(snapshot, builder.build(&report));
  if (!report.ok()) {
    return cooling_capacity::Error(ErrorCode::InvariantViolation,
                                   "the self-check model failed validation")
        .with("findings", report.render());
  }

  const cooling_capacity::CoolingAnalyzer analyzer(*snapshot, cooling_capacity::Timestamp::epoch(),
                                                   cooling_capacity::AnalysisBasis::Fresh);
  CCAP_TRY_DECLARE(zone, analyzer.zone_capacity(ZoneId::literal("hall-1"), CoolingMedium::Air,
                                                CompatibilityClass::AirConvection));
  // Two 400 kW units in N+1 behind a 700 kW transport limit: the group gives
  // 400 kW, and the transport limit is not binding.
  const std::int64_t expected_usable = 400000000;
  if (!zone.breakdown.usable.is_known() ||
      zone.breakdown.usable.amount_or_zero().milliwatts() != expected_usable) {
    return cooling_capacity::Error(ErrorCode::InvariantViolation,
                                   "the self-check rollup produced an unexpected usable capacity")
        .with("actual", zone.breakdown.usable.to_string())
        .with("expected", ThermalPower::from_milliwatts(expected_usable).to_string());
  }

  cooling_capacity::CandidateLoad load;
  load.zone = ZoneId::literal("hall-1");
  load.medium = CoolingMedium::Air;
  load.compatibility = CompatibilityClass::AirConvection;
  load.thermal = ThermalPower::from_watts(300000).value();
  load.actor = cooling_capacity::ActorId::literal("ccap-cli");
  CCAP_TRY_DECLARE(admitted, cooling_capacity::evaluate_candidate(analyzer, load));
  if (!admitted.admitted()) {
    return cooling_capacity::Error(ErrorCode::InvariantViolation,
                                   "the self-check candidate was not admitted")
        .with("evaluation", admitted.to_string());
  }
  load.thermal = ThermalPower::from_watts(500000).value();
  CCAP_TRY_DECLARE(rejected, cooling_capacity::evaluate_candidate(analyzer, load));
  if (rejected.disposition != cooling_capacity::CandidateDisposition::Rejected) {
    return cooling_capacity::Error(ErrorCode::InvariantViolation,
                                   "the self-check oversized candidate was not rejected")
        .with("evaluation", rejected.to_string());
  }
  return Result<void>();
}

Result<void> scenario(const std::string& directory) {
  EngineOptions options;
  options.actor = cooling_capacity::ActorId::literal("ccap-scenario");
  options.store.access = cooling_capacity::StoreAccess::ReadWrite;
  options.store.create_if_missing = true;

  CCAP_TRY_DECLARE(engine, CoolingCapacityEngine::open(directory, options));
  CCAP_TRY_DECLARE(builder, engine->begin_mutation());
  CCAP_TRY(add_air_site(builder));
  CCAP_TRY_DECLARE(receipt, engine->publish(builder));
  std::cout << "published generation " << receipt.generation.to_string() << " digest "
            << receipt.digest.to_hex() << "\n";

  CCAP_TRY_DECLARE(view, engine->zone_capacity(ZoneId::literal("hall-1"), CoolingMedium::Air,
                                               CompatibilityClass::AirConvection));
  print_zone(view);

  cooling_capacity::CandidateLoad load;
  load.zone = ZoneId::literal("hall-1");
  load.medium = CoolingMedium::Air;
  load.compatibility = CompatibilityClass::AirConvection;
  load.thermal = ThermalPower::from_watts(200000).value();
  load.actor = options.actor;

  cooling_capacity::CommitmentRequest request;
  request.operation = cooling_capacity::CommitmentOperation::Commit;
  request.id = cooling_capacity::CommitmentId::literal("load-1");
  request.attempt = cooling_capacity::AttemptId::literal("attempt-1");
  request.zone = load.zone;
  request.medium = load.medium;
  request.compatibility = load.compatibility;
  request.thermal = load.thermal;
  request.actor = options.actor;
  request.requested_at = engine->snapshot()->constructed_at();
  request.expected_generation = engine->snapshot()->generation();
  CCAP_TRY_DECLARE(outcome, engine->apply(request));
  std::cout << "committed " << outcome.committed_after.to_string() << " reserve "
            << outcome.reserve_after.to_string() << " generation "
            << outcome.generation.to_string() << "\n";

  CCAP_TRY_DECLARE(after, engine->zone_capacity(load.zone, load.medium, load.compatibility));
  print_value("offered-after-commit", after.offered);

  CCAP_TRY_DECLARE(store, cooling_capacity::CoolingCapacityStore::open(directory, options.store));
  CCAP_TRY_DECLARE(verification, store->verify());
  std::cout << "verification ok " << (verification.ok ? "yes" : "no") << "\n";
  if (!verification.ok) {
    std::cout << verification.render();
    return cooling_capacity::Error(ErrorCode::InvariantViolation,
                                   "the scenario store did not verify");
  }
  return Result<void>();
}

Result<Options> parse_options(int argc, char** argv, std::string& command) {
  Options options;
  command.clear();
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--self-check") {
      command = "self-check";
      continue;
    }
    if (argument == "--scenario") {
      command = "scenario";
      // The scenario directory is the next positional argument.
      continue;
    }
    if (argument.size() > 2 && argument.compare(0, 2, "--") == 0) {
      const std::string key = argument.substr(2);
      if (key == "help") {
        command = "help";
        continue;
      }
      if (index + 1 >= argc) {
        return cooling_capacity::Error(ErrorCode::InvalidArgument,
                                       "option --" + key + " needs a value");
      }
      options.values[key] = argv[++index];
      continue;
    }
    if (!argument.empty() && argument.front() == '-') {
      return cooling_capacity::Error(ErrorCode::InvalidArgument, "unknown option")
          .with("option", argument);
    }
    options.positional.push_back(argument);
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  std::string command;
  Result<Options> parsed = parse_options(argc, argv, command);
  if (!parsed.ok()) {
    return fail(parsed.error());
  }
  const Options& options = parsed.value();
  if (command.empty()) {
    if (options.positional.empty()) {
      usage();
      return kExitUsage;
    }
    command = options.positional.front();
  }

  const auto dispatch = [&options, &command]() -> Result<int> {
    if (command == "self-check") {
      CCAP_TRY(self_check());
      std::cout << "self-check ok\n";
      return kExitOk;
    }
    if (command == "scenario") {
      if (options.positional.empty()) {
        return cooling_capacity::Error(ErrorCode::InvalidArgument,
                                       "the scenario command needs a store directory");
      }
      CCAP_TRY(scenario(options.positional.front()));
      return kExitOk;
    }
    if (command == "help" || command == "--help") {
      usage();
      return kExitOk;
    }
    if (command == "version") {
      return command_version(options);
    }
    if (command == "verify") {
      return command_verify(options);
    }
    if (command == "generations") {
      return command_generations(options);
    }
    if (command == "show") {
      return command_show(options);
    }
    if (command == "capacity") {
      return command_capacity(options);
    }
    if (command == "explain") {
      return command_explain(options);
    }
    if (command == "evaluate") {
      return command_evaluate(options);
    }
    if (command == "plan") {
      return command_commitment(options, cooling_capacity::CommitmentOperation::Plan);
    }
    if (command == "hold") {
      return command_commitment(options, cooling_capacity::CommitmentOperation::Hold);
    }
    if (command == "commit") {
      return command_commitment(options, cooling_capacity::CommitmentOperation::Commit);
    }
    if (command == "release") {
      return command_commitment(options, cooling_capacity::CommitmentOperation::Release);
    }
    if (command == "expire") {
      return command_commitment(options, cooling_capacity::CommitmentOperation::Expire);
    }
    if (command == "revalidate") {
      return command_revalidate(options);
    }
    if (command == "export") {
      return command_export(options);
    }
    if (command == "diff") {
      return command_diff(options);
    }
    return cooling_capacity::Error(ErrorCode::InvalidArgument, "unknown command")
        .with("command", command);
  };

  Result<int> outcome = dispatch();
  if (!outcome.ok()) {
    return fail(outcome.error());
  }
  return outcome.value();
}
