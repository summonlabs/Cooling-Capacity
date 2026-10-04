# Cooling Capacity

Cooling Capacity is the Data Center Control Plane (DCCP) runtime that owns
**cooling-capacity accounting**: how much thermal-removal capacity is safely
usable now, where it is available, under which cooling dependencies and
redundancy assumptions, and what evidence makes that answer authoritative.

This repository owns the accounting of cooling capacity; it does not own cooling
actuation, and it does not own placement.

* Portable C++20 library, CMake, no third-party dependencies.
* Exact integer accounting in canonical fixed units; no floating point takes
  part in any authoritative capacity figure.
* Immutable, generation-bound snapshots; publication is atomic and durable.
* Durable, integrity-checked, versioned persistence with cross-process writer
  fencing and conservative recovery.
* Installable and exported as `CoolingCapacity::cooling_capacity`.

---

## 1. Systems boundary

### What this repository owns

* **Cooling-capacity accounting** for one or more facilities: sites, facilities,
  zones, loops, plants, manifolds and thermal-removal equipment, with typed
  references between them.
* **Nominal, validated, derated, degraded and usable capacity**, committed
  thermal load, reserve, deficit, observed load and observed headroom, in exact
  integer milliwatts per medium.
* **Redundancy accounting**: N, N+1, N+2, 2N, 2N+1 and 2(N+1) groups, with the
  declared class, the achieved class and the counting posture made explicit.
* **Derating and degradation** as explicit, named, evidence-backed factors.
* **Locality and compatibility class**, so an answer says *where* capacity is
  and *which kind of load* can use it.
* **Deterministic zone, loop and plant rollups**, including shared-plant
  accounting that never counts one pool once per loop and shared-equipment
  accounting that divides rather than duplicates.
* **Bottleneck attribution**: which constraint decided an answer, chosen by a
  total order so the answer is reproducible.
* **Candidate-load evaluation**: the caller proposes a load, the library answers
  admitted, rejected or indeterminate, with an explanation.
* **Commitments**: the authority to occupy cooling capacity, with explicit
  preconditions, an idempotent attempt model and a lifecycle that separates a
  submitted plan from an authoritative commitment.
* **Explanations, generation diffs and conservative revalidation.**
* **Durable persistence** of published generations with a documented commit
  protocol, real cross-process writer authority and recovery that produces one
  whole authoritative state.

### What this repository explicitly does not own

| Area | Owner |
| --- | --- |
| Thermal Control Plane, thermal zones, thermal emergency handling | separate DCCP thermal runtimes |
| Cooling Topology, loop and plant structural topology | Cooling Topology runtime |
| Airflow Control, Liquid Cooling Control, setpoints, actuators, BMS writes | cooling control planes |
| Cooling Failover, Thermal Emergency Manager | dedicated thermal runtimes |
| Facility-wide capacity planning across power, space and cooling | Facility Capacity |
| Placement planning and selection of a zone for a workload | Facility Placement Planner |
| Rack membership, occupancy and allocation policy | Rack Registry |
| Asset inventory, serials, specifications | Asset Registry |
| Stable coordinate, address and naming semantics | Physical Location Registry |
| General cross-service dependency semantics | Facility Dependency Registry |
| Accelerator execution, memory, serving, scheduling | ASI |
| Network topology, paths, transport, congestion, federation | DFI |

This library reads no sensor, writes no setpoint, opens no network connection
and starts no thread of its own. Every cooling fact it holds arrived as an
explicit record. Every answer it gives names the generation, the policy, the
instant and the evidence it used.

### Boundary in one sentence

Cooling Capacity answers *"how much heat can be removed here, now, and why
should that be believed"*; it never answers *"what should be running here"* and
it never touches the plant.

---

## 2. Architecture

```
include/cooling_capacity/     the public API, one self-contained header per concept
  errors.hpp        ErrorCode, ErrorCategory, Error, Result<T>, CCAP_TRY
  text.hpp          Identifier, BoundedText, DocumentRef, strict UTF-8 and decimals
  units.hpp         ThermalPower, Fraction, Temperature, VolumetricFlow, Timestamp
  ids.hpp           typed identities, generations, epochs and revisions
  digest.hpp        SHA-256 (FIPS 180-4) and the streaming hasher
  clock.hpp         injected time source; canonical UTC formatting and parsing
  limits.hpp        every bound, checked before the allocation it bounds
  medium.hpp        media, compatibility classes, equipment kinds, redundancy ladder
  model.hpp         the physical records: site, facility, domain, manifold,
                    equipment, loop, plant, zone
  evidence.hpp      evidence kinds, sources, subjects, values and provenance
  policy.hpp        freshness windows, stale and unknown policy, reserves
  commitment.hpp    the authority to occupy capacity, and its lifecycle
  accounting.hpp    CapacityValue: Known, Unknown, Unsupported, Unavailable
  snapshot.hpp      CoolingSnapshot, SnapshotBuilder, validation findings
  rollup.hpp        CoolingAnalyzer: loop, plant, zone and pool views
  candidate.hpp     candidate loads and admission decisions
  reconciliation.hpp  committed-versus-observed cross-check
  diff.hpp          deterministic generation differences
  explanation.hpp   stable explanation codes and their rendering
  canonical.hpp     the canonical byte format
  store.hpp         the durable, integrity-checked store
  engine.hpp        the composition of a store and an analysis view
src/                          the implementation (not installed)
tools/ccap_cli.cpp            inspection and administration CLI
examples/                     eight programs over the stable public API
benchmarks/benchmark_capacity.cpp   completed-operation benchmarks
tests/                        the proof obligations, one file per area
tests/package_consumer/       an independent find_package consumer
```

### Data flow

```
records ─► SnapshotBuilder ─► validation ─► CoolingSnapshot (immutable, digest)
                                                 │
                          injected clock ────────┤
                                                 ▼
                                          CoolingAnalyzer
                                                 │
      ┌──────────────────┬───────────────────────┼────────────────────┐
      ▼                  ▼                       ▼                    ▼
 loop/plant/zone    candidate load         reconciliation      generation diff
   rollups            evaluation
      │                  │
      └──────────┬───────┘
                 ▼
        CoolingCapacityEngine ──► CoolingCapacityStore ──► MANIFEST + generations
```

---

## 3. State and authority model

### Media are not fungible

`CoolingMedium` is `Air` or `Liquid`. Every capacity value is expressed **per
medium**. Air capacity is never added to liquid capacity and air capacity can
never be spent on a liquid-cooled load. A loop's kind fixes its medium, a
compatibility class fixes its medium, and a zone declares which media it can
accept and which classes it supports.

### The four answers

`CapacityValue` is one of:

| Status | Meaning |
| --- | --- |
| `Known` | the amount is established by usable evidence |
| `Unknown` | the amount cannot be established, so nothing may be concluded |
| `Unsupported` | this medium or class does not exist here at all: a definite negative |
| `Unavailable` | the amount is definitely zero right now, for a named reason |

Zero is `Known(0)`. Collapsing `Unknown` into zero is the single most dangerous
shortcut in capacity accounting, and the type refuses to allow it. `Unknown`
propagates: a group with one unit in an unknown state has unknown capacity, a
loop with an undeclared transport limit has unknown capacity, and a candidate
load against unknown capacity is **Indeterminate**, never admitted and never
rejected.

### Identity, observation and authority are separate

* **Identity** is the record's typed identifier. Mutable metadata (state, label,
  derates) lives in the record and changes only by producing a new generation.
* **Observation** is an evidence record: what was seen, by whom, when, from
  which source generation.
* **Authority** is a writer epoch held as an operating-system lock plus an
  explicit generation precondition on every mutation.
* **Planned** commitments are submitted intent and consume nothing. **Held**
  and **Committed** consume capacity. **Released**, **Expired** and
  **Superseded** are terminal.
* **Acknowledgement** is the outcome of an operation; **verified effect** is an
  observation. They are different records and are compared, never merged.
* **Persisted state** is recovered, not revalidated. A recovered snapshot is
  marked `RecoveredFromStore` and answers are labelled
  `RecoveredPendingRevalidation` until `revalidate()` publishes a new
  generation saying the state was examined against current evidence.
* **Capacity** is a number; **authority to consume it** is a commitment. The
  library never lets one stand in for the other.

### Exact integer units

| Quantity | Canonical unit |
| --- | --- |
| `ThermalPower` | milliwatts |
| `Fraction` | parts per million, authoritative range [0, 1000000] |
| `Temperature` | millidegrees Celsius |
| `VolumetricFlow` | millilitres per second |
| `DurationMs` | milliseconds |
| `Timestamp` | milliseconds since the Unix epoch, UTC |

Multiplication of a capacity by a fraction is
`floor(value * parts_per_million / 1000000)`, computed with a decomposition that
avoids any 128-bit intermediate and cannot overflow silently. Rounding is always
towards zero, so every derived capacity is conservative: a derived figure is
never larger than the exact value. Overflow is reported as
`ErrorCode::ArithmeticOverflow`, never wrapped.

---

## 4. Accounting semantics

### The chain

For each equipment unit and each medium:

```
nominal      the nameplate figure
validated    the commissioned or otherwise validated figure, when one exists
derated      validated x every derate factor, each step rounded down
degraded     derated x the degradation factor
usable       degraded, adjusted for the unit's operating state
```

`validated` is used only when it is backed by evidence that passes the policy's
freshness window. A validated value with no evidence cannot outrank a nameplate:
either the policy requires validation, in which case the unit is `Unknown`, or
the nominal figure is used and the basis is recorded as `Nominal`.

### States

| State | Contributes |
| --- | --- |
| `InService` | full degraded capacity |
| `Standby` | nothing now, but counted for the redundancy formula |
| `Maintenance`, `Faulted`, `Decommissioned` | excluded, and the group's effective class degrades |
| `Unknown` | `Unknown(operating-state-unknown)`, which poisons the group |

### The redundancy formula

Units are the capacity-contributing members of the group. `total` is the sum
over units that are `InService` or `Standby`. `c1` and `c2` are the two largest
capacities in that set.

| Declared | Usable capacity |
| --- | --- |
| N | sum over the in-service units |
| N+1 | `total − c1` |
| N+2 | `total − c1 − c2` |
| 2N | `floor(total / 2)` |
| 2N+1 | `floor((total − c1) / 2)` |
| 2(N+1) | `floor((total − c1 − c2) / 2)` |

The formula is authoritative. The **effective class** reported next to it is a
descriptive label obtained by stepping down the ladder N, N+1, N+2, 2N, 2N+1,
2(N+1) once per excluded unit. Capacity decisions use the formula; a candidate
that demands a redundancy class is checked against both the label and the
formula's result.

### Shared resources are never counted twice

* A **plant** is a pool. Its capacity belongs to every loop that draws on it, so
  it is reported once per pool and a zone's offer is
  `min(local headroom, tightest pool headroom)`. The pool headroom is *shared*
  and is explicitly flagged as not additive across zones: two zones each offered
  200 kW from a 400 kW pool cannot both take 200 kW.
* A **loop fed by two plants** charges its committed load to both plants and
  takes the smaller of the two headrooms. That is conservative by construction
  and is documented as such.
* **Equipment shared between loops** has its usable capacity divided by the
  number of loops that share it, rounding down. Six hundred kilowatts shared by
  two loops is three hundred kilowatts on each, never six hundred on each.

### Bottlenecks

Every zone answer names its binding constraint: the loop's transport path, the
loop's equipment group, a loop's committed load, a shared pool, the reserve, or
an unknown constituent. Ties are broken by the constraint kind and then by the
constraint's identifier, so the answer is total and reproducible.

---

## 5. Persistence and recovery

### Layout

```
<store>/
  MANIFEST   fixed-size, self-digested head pointer: identity, incarnation,
             writer epoch, head generation, head digest, sequence
  LOCK       the file the cross-process writer lock is taken on
  gen-<20 digits>.ccap   one canonical artifact per generation
  staging-*.tmp          transient; never authoritative
```

An artifact is a 64-byte header, a length-prefixed payload and a 32-byte
SHA-256 digest over the header and payload. Every integer is written byte by
byte in little-endian order; the header carries a byte-order marker, so an
artifact written by an opposite-endian host is refused with
`ErrorCode::WrongEndianness` instead of being misread.

### The commit protocol

```
plan -> validate -> check the generation precondition against the manifest on disk
     -> reserve the generation and write staging (create-new, flushed to the device)
     -> re-read the staging file and compare it byte for byte
     -> publish atomically under gen-<n>.ccap (fails if it already exists)
     -> flush the directory (POSIX) / rely on MOVEFILE_WRITE_THROUGH (Windows)
     -> re-read the published file and compare it byte for byte
     -> replace the manifest atomically, writing through
     -> retire generations beyond the retention window, oldest first, never the head
```

The manifest replacement is the commit point. A generation file that was
published but never referenced by the manifest is left alone and stays readable
by explicit generation number; it is never adopted as the head by accident.

### Recovery

Opening a store reads the manifest, verifies its digest and its version, cleans
staging residue, and lists the generations. A damaged or missing manifest
**refuses the open** unless the caller explicitly asks for
`HeadRecovery::ReconstructFromGenerations`, in which case the head is
reconstructed from the highest generation that verifies end to end. The
reconstruction can only ever select a generation that is older than or equal to
the true head, never a newer one, and it records a warning saying it happened.

Recovery produces one whole authoritative state. There is no merging, no
partial adoption and no silent fallback: a store whose head cannot be read is a
store whose head cannot be read.

### What a restart means for authority

* Writer authority is an operating-system lock on `LOCK`. Another process, or
  another store object in this process, is refused with
  `ErrorCode::LockConflict` while it is held.
* The lock is released by the operating system when the holder dies, whether it
  exited cleanly or was killed. The next writer advances the writer epoch, so
  nothing a dead process still believes can be committed.
* A snapshot loaded from a store is `RecoveredFromStore`. Persisted dynamic
  evidence does not become fresh by being reloaded: freshness is recomputed
  against the injected clock and the policy at the instant of evaluation, and a
  recovered snapshot refuses to admit a new load until `revalidate()` publishes
  a generation recording that the state was examined.

---

## 6. Concurrency and process authority

### Ownership and lock order

* `CoolingSnapshot` is immutable. Readers never take a lock: they load an
  atomic `shared_ptr` to the current snapshot, so a reader cannot block a writer
  and a writer cannot block a reader.
* Writers are serialised by exactly one mutex, `CoolingCapacityEngine`'s
  mutation mutex. The complete lock order is:

  ```
  engine mutation mutex  ->  store writer state  ->  operating-system file lock
  ```

  Nothing acquires these in any other order, and no lock is held across a
  callback because the library has no callbacks, no observers, no background
  threads and no asynchronous work. Shutdown ordering therefore cannot go wrong:
  there is no work in flight to wait for.
* The store's own state transitions (open, writer held, closed) are guarded by
  the fact that a store is only reachable through the engine's mutation mutex,
  except for the explicitly documented `CoolingCapacityStore` API, which is
  documented as single-threaded per handle.

### What is proved with real processes

The test binary re-executes itself to obtain independent operating-system
processes. The suite proves, with real processes:

* a second process is refused writer authority while the first holds it;
* a child process takes authority and advances the epoch once the parent
  releases it;
* killing the holder relinquishes authority, the next writer advances the
  epoch, and the store is still whole afterwards;
* a writer killed repeatedly during a commit loop leaves a store whose head
  reads, whose generations all decode and which verifies cleanly.

Threads are never used as a substitute for processes, and processes are never
used as a substitute for crash recovery.

---

## 7. Error model

Failures are values, not exceptions. `Result<T>` carries either a value or an
`Error`; contract violations (reading the value of a failed result) terminate
with a diagnostic rather than producing undefined behaviour.

`ErrorCode` distinguishes, at minimum:

| Category | Codes |
| --- | --- |
| Invalid argument | `invalid_argument`, `invalid_identifier`, `invalid_enum_value`, `invalid_utf8`, `invalid_text`, `duplicate_identity`, `missing_reference`, `dangling_reference`, `self_reference`, `cyclic_reference` |
| Precondition | `not_found`, `already_exists`, `conflict`, `stale_generation`, `stale_authority`, `stale_epoch`, `stale_evidence`, `precondition_failed`, `read_only`, `not_open`, `closed`, `writer_busy`, `idempotency_conflict`, `capacity_exceeded`, `incompatible_class`, `incompatible_medium`, `redundancy_unsatisfied`, `reserve_violation` |
| Integrity | `incompatible_version`, `corruption`, `truncated`, `oversized`, `wrong_endianness`, `digest_mismatch`, `missing_manifest`, `unsupported_format` |
| Resource | `limit_exceeded`, `arithmetic_overflow`, `unsupported`, `unavailable`, `unknown`, `indeterminate` |
| Environment | `permission_denied`, `io_failure`, `lock_conflict`, `path_invalid`, `process_failure` |
| Internal | `invariant_violation`, `internal_error` |

An `Error` carries a message, ordered key/value context, and the expected and
actual generation where those are meaningful. Codes and their names are part of
the public contract: they are serialized, printed by the CLI and asserted by
tests, and are never renumbered.

### Validation precedence

`validate_snapshot` runs a fixed sequence and emits findings in a fixed order:
policy, facilities, domains, manifolds, equipment, loops, plants, zones,
evidence, commitments, then the over-commitment cross-check. Within each stage
records are visited in canonical identifier order and rules are checked in a
fixed order. The precedence is exercised directly by
`tests/test_validation_precedence.cpp`.

---

## 8. The command line tool

```
ccap version
ccap verify       --store DIR
ccap generations  --store DIR
ccap show         --store DIR
ccap capacity     --store DIR --zone Z --medium air|liquid --class C
ccap explain      --store DIR --zone Z --medium air|liquid --class C
ccap evaluate     --store DIR --zone Z --medium M --class C --thermal-mw N
                  [--redundancy n|n+1|n+2|2n|2n+1|2(n+1)] [--loop L]
ccap plan|hold|commit --store DIR --zone Z --medium M --class C --thermal-mw N
                  --commitment ID --attempt ID --actor ID [--expires-at TS]
ccap release|expire   --store DIR --commitment ID --attempt ID --actor ID
ccap revalidate   --store DIR
ccap export       --store DIR [--generation N]
ccap diff         --store DIR --from N --to M
ccap --self-check
ccap --scenario DIR
```

Inspection commands open the store read-only and therefore never contend for
writer authority. Unknown options, malformed values and missing required
options are refused rather than defaulted.

Exit codes are stable: `0` success, `1` usage, `2` not found, `3` refused,
`4` not admitted, `5` indeterminate, `6` integrity or I/O failure.

---

## 9. Examples

| Program | What it shows |
| --- | --- |
| `01_zone_capacity` | the full capacity chain for one zone, with a manual clock |
| `02_shared_plant` | one pool behind two loops: no double counting, and the bottleneck |
| `03_redundancy_and_derating` | the N+1 formula and exact integer derating |
| `04_unknown_is_not_zero` | `Unknown` versus `Unsupported` versus `Unavailable` |
| `05_candidate_load` | admitted, rejected and indeterminate decisions |
| `06_commitments_and_preconditions` | plan, hold, commit, replay and a stale request |
| `07_persistence_and_recovery` | close, reopen, recovered basis, revalidation |
| `08_generation_diff` | the deterministic difference between two generations |

Every example is compiled and run by the test suite.

---

## 10. Build, test and install

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix <prefix>
```

Options: `CCAP_BUILD_TESTS`, `CCAP_BUILD_TOOLS`, `CCAP_BUILD_EXAMPLES`,
`CCAP_BUILD_BENCHMARKS`, `CCAP_WARNINGS_AS_ERRORS`, `CCAP_SANITIZERS`,
`CCAP_ANALYZE`, `BUILD_SHARED_LIBS`.

### Downstream use

```cmake
find_package(CoolingCapacity 1.0 REQUIRED)
target_link_libraries(my_target PRIVATE CoolingCapacity::cooling_capacity)
```

`tests/package_consumer/` is an independent out-of-tree consumer that does
exactly this against an installed package and runs a real minimal lifecycle.

---

## 11. Validation

Everything below was executed on the primary platform for this repository,
Windows with MSVC 19.44 (Visual Studio 2022 Build Tools), x64, Ninja generator.

| Proof | How |
| --- | --- |
| Unit and contract tests | `ccap.tests`, one file per area |
| Deterministic property tests | fixed-seed randomised models checked against an independent reference implementation of the redundancy formula in the test file |
| Adversarial tests | malformed, truncated, oversized, corrupt, wrong-version, wrong-endian and path-manipulating input |
| Persistence | close and reopen through the real store |
| Real process restart | the test binary re-executes itself and reads the store from a separate operating-system process |
| Real multi-process authority | writer-lock contention, handover, and authority relinquished by killing the holder |
| Crash recovery | a writer killed repeatedly during a commit loop, after which the store verifies |
| Concurrency | reader threads against a publishing writer |
| Installed downstream | `find_package` consumer configured against an installed package |
| Warnings | `/W4 /WX /permissive-`, zero first-party warnings in Release and Debug |
| Sanitizers | AddressSanitizer build of the library, the tools, the examples, the benchmarks and the whole test suite; all 12 CTest entries pass |

Every row above is reproducible from a clone with
`cmake -S . -B build && cmake --build build && ctest --test-dir build`.

### What was actually run

* Release (`/O2`, `/W4 /WX /permissive-`) and Debug (`/RTC1`, iterator debugging
  level 2, `/W4 /WX`): the library, the tools, the eight examples, the benchmark
  and the test suite all build with zero warnings, and `ctest` reports
  **12 of 12 passing** in both.
* AddressSanitizer (`/fsanitize=address`) over the same targets: `ctest`
  reports **12 of 12 passing**. A positive control confirms the sanitizer is
  actually active in this environment rather than silently inert: the same
  toolchain flags applied to a program that reads past the end of a heap
  allocation produce an `AddressSanitizer: heap-buffer-overflow` report.
* The test binary reports **202 test functions passing, 0 failing**, including
  the real multiprocess, restart and crash tests.

---

## 12. Benchmarks

`benchmarks/benchmark_capacity.cpp` measures **completed operations** with
`std::chrono::steady_clock` wrapped around the whole operation:

* building and validating a synthetic facility (SYNTHETIC workload),
* a zone capacity rollup,
* candidate-load evaluation,
* a full durable commit, including encoding, the staging write, the device
  flush, the atomic publish, the manifest replacement and the retirement of old
  generations,
* a generation diff between two published generations.

Every measured workload is synthetic and is labelled as such in the output. No
before/after performance pair is published, because no change was isolated by a
controlled alternating measurement.

A full run on the primary platform (Windows 11, MSVC 19.44 x64, Release)
reported, over a synthetic facility of eight loops, eight zones, thirty-two CRAH
units and fifty records:

| Operation | Completed operations | Per operation | Per second |
| --- | --- | --- | --- |
| Zone capacity rollup | 500 | 8.4 µs | 119 000 |
| Candidate-load evaluation | 1 000 | 9.7 µs | 103 000 |
| Full durable commit (flush included) | 25 | 9.97 ms | 100 |
| Generation diff | 250 | 185 µs | 5 400 |

The durable-commit figure is dominated by the device flush and the atomic
manifest replacement, which is the honest cost of a commit that survives a
power loss; it is deliberately measured over the whole `publish` call rather
than around the encoding alone. These are synthetic figures on one machine and
are reported as such.

Run `ccap_benchmark` for the current numbers; the CTest smoke run uses
`--quick`.

---

## 13. Limitations

* **Windows is the exercised platform.** The POSIX branches of the file,
  directory-flush and writer-lock layers are written to the same contract but
  are, on this evidence, unverified.
* **Windows has no directory flush.** There, the durability of a replacement
  comes from `MOVEFILE_WRITE_THROUGH` and the directory-flush function is a
  deliberate no-op rather than a pretence.
* **AddressSanitizer is proven on this platform.** The sanitizer build requires
  the MSVC C++ AddressSanitizer component, which is optional; when it is absent
  `-DCCAP_SANITIZERS=ON` reports that exact reason and refuses to configure
  rather than silently building without sanitizers. The sanitizer run and a
  positive control are described in section 11.
* **The canonical decoder rewrites and compares.** A decoded artifact is
  re-encoded and compared byte for byte with the input; an artifact that is
  structurally readable but not in canonical form is refused. That is a
  deliberate strictness, not an oversight.
* **Recovery is not a repair tool.** A corrupt generation is rejected, not
  fixed. Explicit head reconstruction can only select a generation that
  verifies end to end.
* **Pumps and manifolds carry no thermal capacity.** They can be recorded and
  are visible in a loop's membership, but they contribute no heat removal and
  are excluded from redundancy groups. The library does not model fluid
  hydraulics.
* **Capacity is declared, not derived from fluid physics.** A transport limit
  or a nominal capacity is a declared figure with optional supporting evidence;
  the library performs no flow-versus-delta-T computation and claims none.
* **No telemetry, no observers, no background work.** There are no
  subscriptions, no change streams and no threads; a consumer that wants to
  follow a store polls generations and diffs them.

---

## 14. Repository layout

```
include/cooling_capacity/   the public API, one self-contained header per concept
src/                        the implementation
tools/ccap_cli.cpp          the inspection and administration tool
examples/                   eight programs over the stable public API
benchmarks/                 completed-operation benchmarks
tests/                      the proof obligations, one file per area
tests/package_consumer/     an independent find_package consumer
```

`CONTRIBUTING.md` describes the contribution terms and the engineering
expectations. `NOTICE` records attribution.

---

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
