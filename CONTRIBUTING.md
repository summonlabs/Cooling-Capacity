# Contributing to Cooling Capacity

Thank you for your interest in contributing to Cooling Capacity. This document
describes the contribution terms and the engineering expectations for this
repository.

## License

By contributing to this project, you agree that your contributions are licensed
under the **Apache License, Version 2.0**. See the `LICENSE` file for the full
license text and the `NOTICE` file for attribution and license notices. There is
**no separate Contributor License Agreement (CLA)** requirement: you retain
ownership of your contributions and grant the project a license to use them
under the terms of the Apache License 2.0.

## License headers

New source files should carry the following header:

```
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
```

## Coding standards

- C++20 and CMake only. No third-party dependencies, no network access during
  configure, build or test.
- Build cleanly with `/W4 /WX` on MSVC and `-Wall -Wextra -Wpedantic
  -Wconversion -Wsign-conversion -Werror` elsewhere. Fix warning causes rather
  than suppressing warnings.
- Public identities, generations, revisions, epochs and attempt identifiers are
  distinct strong types. Do not add implicit conversions between them.
- Authoritative thermal accounting is exact integer arithmetic in fixed units.
  Floating point must not appear in any authoritative capacity computation.
- All external input is untrusted. Validate before allocating, use checked
  arithmetic for externally influenced sizes, and reject malformed input instead
  of normalizing it.
- Zero and unknown are different answers, and so are unsupported and
  unavailable. Do not collapse them.
- Where iteration order is public or serialized, it is documented, total and
  tested. Do not rely on hash-container iteration order for anything that is
  observable.

## Architecture boundaries

- This repository owns **cooling-capacity accounting**: nominal, validated,
  usable and committed thermal-removal capacity, reserve, redundancy, derating,
  degradation, locality, compatibility class, freshness, provenance and explicit
  unknown, together with deterministic zone, loop and plant rollups, bottleneck
  attribution, candidate-load evaluation, explanations and generation diffs.
- This repository does **not** own cooling actuation, airflow control, liquid
  cooling control, thermal zone management, cooling failover, thermal emergency
  handling, facility-wide capacity planning, placement planning or reservations
  outside its own cooling-capacity commitments. Those are separate runtimes in
  the Data Center Control Plane and in Accelerated Systems Infrastructure and
  Distributed Fabric Infrastructure.
- Facility structure, equipment identity and dependency semantics are referenced
  through opaque typed identifiers and evidence records. Do not add knowledge of
  other runtimes' internals.
- Do not add actuation, setpoint writing, telemetry emission, background threads
  or observer callbacks.

## Testing

Every behavioral change needs a test that would fail without it. The repository
expects, at minimum:

- deterministic rollup, redundancy, derating and validation-precedence tests;
- fixed-seed property tests that compare the implementation against an
  independent reference model;
- adversarial tests for malformed, truncated, oversized, corrupt, wrong-version
  and wrong-endian input, and for stale authority;
- persistence tests that close and reopen the durable store, that restart a real
  operating-system process, and that kill a writer mid-commit;
- concurrency tests for concurrent readers and writers;
- independent multi-process tests for writer authority and fencing.

Tests must pass on their own. A hanging test is a defect to diagnose and fix,
not to bound with a timeout. Do not add test timeouts.

## Pull requests

Keep changes focused, add the tests that prove the change, and make sure the
repository builds and tests cleanly in both Release and Debug with warnings
treated as errors. Do not include generated build output, install trees,
benchmark residue or editor files.
