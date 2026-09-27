# Contributing to Facility Dependency Registry

Thank you for your interest in contributing to Facility Dependency Registry.
This document describes the contribution terms and the engineering
expectations for this repository.

## License

By contributing to this project, you agree that your contributions are
licensed under the **Apache License, Version 2.0**. See the `LICENSE` file for
the full license text and the `NOTICE` file for attribution and license
notices. There is **no separate Contributor License Agreement (CLA)**
requirement: you retain ownership of your contributions and grant the project
a license to use them under the terms of the Apache License 2.0.

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
- Public identities, generations, revisions and authority tokens are distinct
  strong types. Do not add implicit conversions between them.
- All external input is untrusted. Validate before allocating, use checked
  arithmetic for externally influenced sizes, and reject malformed input
  instead of normalizing it.
- Where iteration order is public or serialized, it is documented, total and
  tested. Do not rely on hash-container iteration order for anything that is
  observable.
- Traversals and graph analyses must be bounded and iterative. Recursion on
  attacker-influenced graph depth is not acceptable.

## Architecture boundaries

- This repository owns explicit facility dependency relationships and their
  canonical graph semantics. It does not own the lifecycle of the objects it
  references, capacity calculation, failure recovery, or DFI network
  dependency semantics.
- External objects (assets, racks, electrical domains, cooling domains,
  facility services, composed ASI and DFI domains) are referenced through
  opaque typed references. Do not add knowledge of their internals.
- Do not implement capacity planning, placement, reservations, actuation,
  maintenance orchestration, tenancy, incident response, dashboards or
  multi-site federation here.

## Testing

Every behavioral change needs a test that would fail without it. The
repository expects, at minimum:

- deterministic state-machine tests for lifecycle and generation rules;
- fixed-seed property tests over randomly generated graphs;
- adversarial tests for malformed, truncated, oversized and corrupt input;
- persistence tests that close and reopen the durable store, plus at least one
  real operating-system-process test for shared-writer behaviour;
- concurrency tests for concurrent readers and writers.

Tests must pass on their own. A hanging test is a defect to diagnose and fix,
not to bound with a timeout.

## Pull requests

Keep changes focused, add the tests that prove the change, and make sure the
repository builds and tests cleanly in both Release and Debug with warnings
treated as errors. Do not include generated build output, install trees,
benchmark residue or editor files.
