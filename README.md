# Facility Dependency Registry

Facility Dependency Registry is the canonical, explicit dependency graph of the
Data Center Control Plane (DCCP), Tranche 1: Canonical Facility State. It answers
one question and refuses to answer any other:

> What depends on what, with which declared semantics, in which generation, and
> which of those declarations may a higher control layer trust?

It is a vendor-neutral C++20 library with one inspection tool. It has no
third-party dependencies: it builds with a C++20 compiler, CMake and the
operating system alone.

Version 1.0.0. State format version 1. Container format version 1. Store layout
version 1.

---

## 1. Systems boundary

This repository owns **explicit cross-domain facility dependency relationships
and their canonical graph semantics**. It records which declared dependency
exists between which pair of external facility objects, what that dependency
means, how strong it is declared to be, whether it is in force, who declared it
and from what, and which generation of the graph is authoritative.

### It owns

* The dependency edge as a first-class record: identity, revision, kind,
  declared strength, direction, lifecycle, declared requirement metadata and
  provenance.
* Endpoint references as typed opaque identities, with a canonical syntax and a
  canonical order.
* The natural key of an edge and the rules that make it unique.
* The rules that decide which declarations are accepted: endpoint type
  compatibility per kind, direction compatibility per kind, the constraint
  schema per kind, self-dependency prohibition, and the acyclic obligation.
* The canonical graph semantics: direct dependencies and dependents, bounded
  transitive closure, bounded impact cones, strongly connected components,
  deterministic path explanations, bounded elementary cycle enumeration, and
  generation diffs.
* Declared external reference observations, and the distinction between a
  declared endpoint and an unresolved one.
* Authority: the generation a mutation is addressed to, per-edge revisions,
  monotonic generation progression, and the rejection of stale writes.
* Versioned, integrity-checked, atomically published durable state, with
  conservative recovery and single-writer fencing.
* Immutable snapshots, a canonical byte encoding, a canonical text export, a
  canonical JSON export and content digests.

### It explicitly does not own

| Adjacent system | What it owns, and this repository does not |
| --- | --- |
| Asset Registry | Whether an asset exists, its class, its lifecycle, its attributes |
| Rack Registry | Rack identity, geometry, slot occupancy, rack state |
| Electrical domain and cooling domain control | Feeds, busways, loops, their live state, their switching |
| Facility services catalogue | What a service is, whether it is up, how it is delivered |
| Composed ASI domains | Accelerator execution, memory, serving, scheduling, state |
| Composed DFI domains | Network topology, paths, transport, congestion, recovery |
| Facility topology | Structural containment, physical adjacency, connectivity |
| Facility State Ledger | Authoritative state history and epochs |
| Capacity planning, placement, reservations | What fits, what to place, what to reserve |
| Failure recovery and failover | What should recover, in what order, how |
| Maintenance orchestration, tenancy, incident response, dashboards | Their own decisions |

Every object this registry refers to belongs to somebody else. This repository
holds an identity, a role in a declared dependency, and nothing about the object
itself. It never creates, mutates, retires or resurrects a referenced object,
and it never derives a recovery, failover or capacity decision from a
declaration.

---

## 2. Domain model

### 2.1 Endpoints

A `DependencyNodeRef` is a domain tag plus an opaque identifier:

| Domain token | Refers to | Owned by |
| --- | --- | --- |
| `asset` | A physical or logical asset | Asset Registry |
| `rack` | A rack | Rack Registry |
| `electrical-domain` | A feed, busway, distribution path or source | Facility electrical control |
| `cooling-domain` | A loop, distribution path or source | Facility cooling control |
| `facility-service` | A catalogue service | Facility service catalogue |
| `asi-domain` | A composed ASI domain | ASI |
| `dfi-domain` | A composed DFI domain | DFI |

The identifier syntax is deliberately narrow: 1 to 160 bytes of `A-Z a-z 0-9 . _
-`, starting and ending with an alphanumeric character, with `.` and `-` used as
single separators (`a..b` and `a--b` are rejected). An identifier that does not
match is **rejected, never trimmed, case-folded or otherwise normalised**, and
identifiers are compared byte for byte. The canonical text form is
`<domain>:<identifier>`, for example `asset:row-a-rack-07-node-3`.

References compare and order structurally: by domain ordinal, then by identifier
bytes. That order is the canonical node order used everywhere this repository
reports nodes.

### 2.2 Dependency kinds

Six kinds exist. There is no "other" and no "custom": an untyped edge bag is
exactly what this repository exists to avoid.

| Kind | Reading | Source domains | Target domain | Directions | Acyclic | Allowed constraints |
| --- | --- | --- | --- | --- | --- | --- |
| `requires-power-from` | The source needs electrical supply from the target | any | `electrical-domain` | directed | **required** | redundancy class, redundancy count, failover mode |
| `cooled-by` | The source is cooled by the target | any | `cooling-domain` | directed | **required** | max latency, redundancy class, redundancy count, failover mode |
| `housed-in` | The source is housed in the target (reference only) | asset, facility service, ASI, DFI | `rack` | directed | **required** | none |
| `served-by` | The source is served by the target | any except facility service | `facility-service` | directed | permitted | max latency, min bandwidth, redundancy class |
| `control-depends-on` | The source's control path depends on the target | any | any | directed or mutual | permitted | max latency, failover mode |
| `composed-domain-depends-on` | A composed ASI or DFI domain depends on another composed domain or on a facility object | ASI, DFI | any | directed or mutual | permitted | all five |

`housed-in` is the reference-only form of housing. It records the declaration and
never derives physical containment, slot occupancy or topology from it: that
remains the Rack Registry's and facility topology's business.

A **mutual** edge is a symmetric relation. It is stored once, with its endpoints
in canonical order, and is traversed in both directions. A mutual edge never
participates in the acyclic obligation, because a symmetric relation is a
two-cycle by definition; declaring one endpoint order and then the other is the
same edge and the second declaration is a duplicate.

### 2.3 Strength, lifecycle and requirement metadata

`DependencyStrength` is a **declaration about consequences**, not a computation
of them: `hard` (loss of the target is declared to be an outage of the source),
`soft` (declared degradation) or `advisory` (recorded, no declared operational
consequence). This repository never derives recovery, failover or capacity
behaviour from it.

`LifecycleState` is the authority a declaration currently carries:

| From | Legal targets |
| --- | --- |
| `proposed` | `active`, `retired` |
| `active` | `suspended`, `retired` |
| `suspended` | `active`, `retired` |
| `retired` | none: terminal |

A new edge may only be born `proposed` or `active`. `suspended` and `retired`
are reached through the transition command, which is itself audited. Only
`active` edges are followed by a default traversal; a proposed or suspended edge
is stored, validated, visible and cycle-checked, but not in force. A retired
edge is retained for audit, never traversed, cannot be changed again, and
reserves its key until it is removed outright.

Requirement metadata is a bounded schema of **declarations**: maximum added
latency in microseconds, minimum bandwidth in megabits per second, redundancy
class (`N`, `N+1`, `2N`, `2N+1`), redundancy count and failover mode
(`automatic`, `manual`, `none`). Values are validated against the declared
domain of their kind, at most `max_constraints_per_edge` may be attached, and a
constraint kind that a dependency kind does not allow is rejected. The registry
stores, reports and diffs these declarations; it never evaluates them, never
reserves against them and never plans with them.

### 2.4 Provenance

Every authoritative declaration carries a mandatory `ProvenanceRecord`: a source
category (operator declaration, facility inventory import, per-domain export,
change record, registry migration), a source identifier, the acting principal,
a caller-supplied recording timestamp and an optional annotation. Provenance
records the origin of a declaration; it never grants authority. Identifiers use
the same canonical syntax as node identifiers, annotations must be well-formed
UTF-8 with no control characters (including no newline or tab), and timestamps
must fall inside `[0, 2100-01-01)`.

No wall clock is read anywhere in the library except through the injected
`Clock`, which stamps journal entries only. Time never orders mutations and
never appears in a state digest, so a registry behaves identically under a
system clock and a fixed test clock.

---

## 3. Authority model

### 3.1 Generations and revisions

* `DependencyGeneration` is the graph generation. Generation zero is the empty
  graph; **every committed mutation advances the generation by exactly one**.
* `DependencyEdgeId` is a registry-assigned ordinal. Identities are never
  reused, not even after the edge is removed, so an identity a consumer observed
  always refers to at most one declaration in the history of a generation chain.
* `EdgeRevision` is per-edge. The first revision is 1 and every accepted change
  advances it by exactly one.

These are three distinct types. There are no implicit conversions between them,
or between any of them and their representation.

### 3.2 The natural key and duplicate semantics

The natural key of an edge is `(source, target, kind)`. It appears at most once
among stored edges, whatever their lifecycle. Re-declaring an existing key has
exactly two outcomes:

* **Identical payload** (same strength, direction, lifecycle, constraints as a
  set in canonical order, and provenance): an **idempotent no-op**. The outcome
  reports `already_present == true`, the existing identity and revision, and
  `generation_advanced == false`. A retried registration therefore publishes
  nothing, which matters for a durable store: a retry does not write a
  generation.
* **Any difference**: rejected as `duplicate-edge`, naming the holder and its
  revision. The registry never silently adopts the newer declaration; a
  conflicting declaration must go through the update command under the revision
  it expects.

The key of an edge is immutable. Changing endpoints or kind is a removal
followed by a registration, so identity, provenance and history stay
unambiguous.

### 3.3 Stale writes

Every mutation carries `expected_generation`, which must equal the current
generation or the mutation is rejected with `stale-generation`. Updates,
transitions and removals additionally carry `expected_revision`, checked with
`stale-edge-revision`. There is no flag that skips either check, in the library
or in the tool.

A generation above the current one is not "fresh": it is rejected the same way.
A default-constructed context expects generation zero and is therefore rejected
by any registry that has committed anything.

Consumer-side idempotency is explicit rather than implicit:

* `remove` of an edge that is absent is an accepted no-op (`already_absent`),
  provided the generation still matches; a removal retry must not fail.
* a lifecycle transition to the state an edge is already in is an accepted
  no-op (`already_in_state`).
* an update that describes the stored state exactly is accepted with
  `changed == false` and publishes nothing.

### 3.4 The acyclic obligation

One subgraph is checked: every **directed** edge whose kind is acyclic-required
(`requires-power-from`, `cooled-by`, `housed-in`), regardless of lifecycle
state. Adding an edge that would close a cycle in that subgraph is rejected with
`prohibited-cycle` and the offending cycle in the rejection detail.

The obligation is one subgraph, not one per kind, because a mixed-kind cycle
cannot be written down in the first place: `housed-in` can only end at a rack and
only start at a non-rack, and `requires-power-from` and `cooled-by` can only end
at an electrical or cooling domain, so nothing can point back at a rack or an
asset through an acyclic-required kind. The property tests check that claim on
random graphs rather than trusting the argument.

Legal cycles are represented, not accidental: `served-by`, `control-depends-on`
and `composed-domain-depends-on` may form cycles, mutual edges are two-cycles,
and both are reported by the component analysis and the bounded cycle
enumeration. Every traversal is iterative and every traversal is bounded, so a
cycle can never cause unbounded recursion.

---

## 4. Queries

All queries are methods of an immutable `RegistrySnapshot`. A snapshot owns its
state, is cheap to copy, never changes, and stays valid after the registry that
produced it is closed.

| Query | What it answers | Bounds |
| --- | --- | --- |
| `direct_dependencies` / `direct_dependents` | Edges incident to one node, in canonical edge order | `max_direct_results` |
| `transitive_closure` | The bounded reachable set, breadth-first layer by layer | `max_traversal_depth`, `max_traversal_nodes` |
| `impact_cone` | What transitively depends on a node through chains inside a strength set | `max_traversal_depth`, `max_traversal_nodes` |
| `strongly_connected_components` | The decomposition, cyclic components first-class | `max_analysis_nodes` |
| `explain_path` | The shortest chain between two nodes, hop by hop | `max_traversal_depth`, `max_traversal_nodes` |
| `enumerate_cycles` | Elementary cycles up to a length and a count | `max_cycle_length`, `max_cycles`, `max_analysis_nodes` |
| `verify_acyclic_obligation` | Whether the acyclic obligation holds, with the first violating cycle | `max_analysis_nodes` |
| `diff_snapshots` | Added, removed and modified edges plus declaration changes, by merge join | `max_diff_changes` |
| `unresolved_endpoints`, `unreferenced_declared_refs` | Which identities are referenced but never declared, and declared but never referenced | `max_query_roots` |

Determinism is a contract, not an accident:

* **Nodes** are ordered by domain ordinal then identifier bytes.
* **Edges** are ordered by key, then identity. That order is what is stored,
  exported and hashed.
* **A traversal** reports nodes in breadth-first layer order and, within a
  layer, in canonical node order. A node is reported once, at its minimal depth.
  When several edges of the same layer could discover it, the recorded
  discovering edge is the one found first while expanding that layer in
  canonical node order. The node bound is applied **after** each layer is
  ordered canonically, so a truncated result is always the canonically first
  part of the reachable set.
* **Components** are ordered by their smallest member; members are sorted.
* **Cycles** are ordered by length, then by member list, and each is rotated so
  that its smallest member comes first.
* **Diffs** are merge joins over the canonical order, so their output does not
  depend on insertion history.

Truncation is never silent. A bounded traversal that stopped early reports
`truncated == true` and a stop reason (`depth-limit`, `node-limit`,
`result-limit`, `cancelled`), and `truncated` is computed exactly: a frontier
that turned out to be a leaf does not claim to have been cut off.

---

## 5. Errors

`ErrorCode` is a stable, machine-readable enumeration. Its numeric values and
its lower-case tokens (`duplicate-edge`, `stale-generation`,
`store-integrity-failure`, …) are part of the public contract and never change
meaning; new codes are only ever added with unused values. Every rejection also
carries a specific human-readable detail, and `RegistryError::explain()` renders
both.

| Category | Codes | Meaning |
| --- | --- | --- |
| Input (1xx) | `invalid-node-domain`, `invalid-node-id-syntax`, `invalid-constraint-value`, `invalid-annotation`, `invalid-timestamp`, `invalid-limits`, … | A supplied value is not in its declared domain |
| Semantics (2xx) | `duplicate-edge`, `endpoint-domain-not-allowed`, `direction-not-allowed-for-kind`, `prohibited-cycle`, `invalid-lifecycle-transition`, `edge-retired`, … | The declaration or the transition is not a legal dependency |
| Authority (3xx) | `stale-generation`, `stale-edge-revision`, `registry-closed`, `writer-fenced`, `not-durable`, `generation-exhausted` | The request was addressed to authority the caller does not have |
| Bounds (4xx) | `edge-capacity-exceeded`, `request-limit-exceeded`, `query-result-limit-exceeded`, `payload-too-large`, `analysis-limit-exceeded`, `cancelled` | A configured or requested bound was reached |
| Persistence (5xx) | `store-locked`, `store-corrupt`, `store-truncated`, `store-integrity-failure`, `store-version-unsupported`, `store-layout-invalid`, `store-publish-failed`, … | The durable state could not be read, trusted or published |

Every bound in the library is enforced **before** allocation or before a walk
expands, never after. A resource bound that is reached produces a typed
rejection, never a partial answer presented as a complete one.

---

## 6. Persistence and recovery

### 6.1 Store layout

```
<root>/writer.lock                        exclusive lock file, holds an incarnation record
<root>/CURRENT                            atomically replaced pointer to the newest generation
<root>/gen-00000000000000000001.fdepstate
<root>/gen-00000000000000000002.fdepstate
<root>/tmp-<random>.tmp                   transient, never authoritative, removed on open
```

A generation file is a 64-byte header (magic `FDEPSTA1`, container version,
flags, payload length, CRC-32 of the payload, reserved word, SHA-256 of the
payload) followed by the canonical state payload. `CURRENT` names a generation
and the SHA-256 of the whole generation file.

The store never builds a file name from anything a caller supplied: generation
file names are formatted from the generation number, and transient names from a
random token created with an exclusive create. A node identifier that looks like
a path is an ordinary opaque identifier, and nothing it contains can influence
where a file is written.

### 6.2 Publication

```
plan -> validate -> reserve generation -> write transient file -> flush
     -> re-read and verify -> rename to the generation file -> sync the directory
     -> atomically replace the pointer -> sync -> prune superseded generations
```

A crash at any point leaves either the previous or the new generation
authoritative, never a partial one: the generation file appears complete before
the pointer names it, and the pointer is replaced by an atomic rename. The
publish path is exercised at every one of those ten steps by a test that
terminates a real child process at each step, and by a test that kills a child
from outside while it is mid-publication.

Publication rejects, in this order: a read-only store (`store-read-only`), a
session that no longer holds the writer lock (`writer-fenced`), a generation
that is not the next one (`stale-generation`), and a pointer that no longer
names what this writer committed (`writer-fenced`). A failed publication leaves
the in-memory state exactly as it was.

### 6.3 Recovery

Opening a store is conservative and explicit:

| Outcome | Meaning |
| --- | --- |
| `fresh-empty` | The root holds neither a generation file nor a pointer. A new store, or an empty one being inspected |
| `loaded-current` | The pointer named a generation that passed its length, CRC-32, SHA-256 and semantic validation |
| `loaded-fallback` | The pointer was missing, malformed, or named an unusable generation, and a lower intact generation was loaded instead. Newer unusable generations are rejected, not repaired |
| `refused-corrupt` | Files existed but none could be validated. Nothing is loaded and the open fails: **an empty graph is never presented in place of authoritative state** |

A payload is put through the same validation a mutation would have to pass:
enumeration domains, identifier syntax, length bounds, canonical ordering,
uniqueness, ordinal consistency, the per-kind endpoint and direction rules, the
constraint schema and the acyclic obligation. A payload this library could not
have produced is rejected rather than normalised into surprising state. The name
of a generation file is cross-checked against the generation inside it, so a
file cannot be renamed into a different place in history, and the pointer's
digest must describe the file it names or the pointer is not believed.

On a fallback in read-write mode the pointer is repaired to name the generation
that was actually loaded. A read-only open never repairs, prunes or removes
anything: inspection must not change what it inspects.

Pruning keeps at most `max_retained_generations` generation files, always
including the one the pointer names. `max_retained_generations = 1` is legal but
removes the ability to fall back.

### 6.4 Single writer, many readers

Authority over a store comes from an **operating system lock** on the lock file,
held for the whole lifetime of a read-write session and released by the
operating system even when the process dies without cleanup. A second
read-write open fails with `store-locked`. Read-only opens take no lock, so
inspection works while another process is writing.

The incarnation record inside the lock file names the writer for an operator. It
is a diagnostic and never authority: a leftover record neither authorises nor
blocks anything, which is why a crashed writer leaves no stale fence behind.

Durability of a replacement comes from `MoveFileEx` with
`MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH` on Windows and from
`rename(2)` plus a directory `fsync` on POSIX.

---

## 7. Concurrency model

* **Exactly one mutation at a time.** Mutations take one internal mutex. The
  mutex is never held across a call into user code, because this type has no
  callbacks, no observers and no subscriptions.
* **Readers never take that mutex.** A reader takes a snapshot, which is one
  atomic shared-pointer load over an immutable state, and then queries the
  snapshot. Readers cannot observe a half-applied mutation because there is
  nothing half way to observe: a mutation builds a new immutable state and
  publishes it in one store.
* **Publication is ordered.** The mutation is applied to a new state, that state
  is made durable when the registry is durable, and only then is it published in
  memory. A failed or cancelled publication leaves the registry exactly as it
  was.
* **`close()` refuses new mutations, waits for an in-flight mutation to finish,
  and then releases the writer lock.** It never cancels work that has already
  crossed the commit boundary. Snapshots taken before a close stay valid.
* **Cancellation is cooperative and real.** A cancelled traversal returns the
  `cancelled` category rather than a partial answer; a cancelled mutation
  publishes nothing and writes no generation file. Work that already crossed the
  commit boundary cannot be cancelled and is reported as committed.
* **No background work exists.** There are no workers, queues or timers, so
  there is no shutdown ordering problem to get wrong, and cancellation is
  observed at defined points rather than by racing a thread.

Two audit notes are part of the contract rather than footnotes: a publication
fault hook and a clock both run while the calling registry holds its mutation
lock, so neither may call back into that registry. Both headers say so where the
type is declared.

---

## 8. Building, installing and using

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix <prefix>
```

Options: `FDEP_BUILD_TESTS`, `FDEP_BUILD_TOOLS`, `FDEP_BUILD_EXAMPLES`,
`FDEP_BUILD_BENCHMARKS`, `FDEP_WARNINGS_AS_ERRORS` (default `ON`),
`FDEP_SANITIZERS`, `FDEP_ANALYZE`, and the standard `BUILD_SHARED_LIBS`.

### Using it from another project

```cmake
find_package(FacilityDependencyRegistry 1.0 REQUIRED)
target_link_libraries(my_target PRIVATE FacilityDependencyRegistry::facility_dependency_registry)
```

`tests/package_consumer/` is an independent project that does exactly this
against an installed prefix, from outside the source tree. It is built and run
as part of the validation recorded below.

### A first program

```cpp
#include "facility_dependency_registry/facility_dependency_registry.hpp"

using namespace facility_dependency_registry;

RegistryOpenRequest open;
open.root = "/var/lib/dccp/facility-dependencies";
auto registry = DependencyRegistry::open(open).value();

DependencyEdgeSpec spec;
spec.source = DependencyNodeRef::create(NodeDomain::Asset, "row-a-rack-07-node-3").value();
spec.target = DependencyNodeRef::create(NodeDomain::ElectricalDomain, "feed-a1").value();
spec.kind = DependencyKind::RequiresPowerFrom;
spec.strength = DependencyStrength::Hard;
spec.provenance = ProvenanceRecord::create(ProvenanceSource::OperatorDeclaration, "change-4711",
                                           "operator-alice", 1'770'000'000'000, "declared during change window",
                                           kHardMaxIdLength, kHardMaxAnnotationLength).value();

RegisterEdgeRequest request;
request.context.expected_generation = registry.generation();
request.spec = spec;
const auto registered = registry.register_edge(request);

ImpactConeRequest cone;
cone.origin = spec.target;
cone.max_depth = 4;
cone.max_nodes = 4096;
const auto impacted = registry.snapshot().impact_cone(cone);
```

The eight programs in `examples/` are compiled and run as part of the test
suite, and each one demonstrates a different part of the public API.

---

## 9. The inspection tool

`fdep` is built by `FDEP_BUILD_TOOLS` and installed into `bin/`. Every command
goes through the same public API a consumer uses; the tool has no privileged
path. Read-only commands open the store read-only, take no writer lock and write
nothing. Mutation commands are addressed to an explicit expected generation:
there is no flag that skips the authority check, because the check is the point.

```
fdep inspect   <root>                          generation, digests, counts, retention, last writer
fdep verify    <root>                          integrity, round trip and obligations; exit 2 on a problem
fdep edges     <root> [--kind K] [--strength S] [--lifecycle L] [--include-inactive]
fdep deps      <root> <ref> [--depth N] [--max-nodes N]
fdep dependents<root> <ref> [--depth N] [--max-nodes N]
fdep impact    <root> <ref> [--max-depth N] [--max-nodes N] [--all-strengths]
fdep path      <root> <from> <to> [--reverse] [--depth N]
fdep scc       <root> [--singletons]
fdep cycles    <root> [--max-length N] [--max-cycles N]
fdep unresolved<root>
fdep export    <root> [--json]
fdep diff      <root> --from-generation N [--to-generation M]

fdep register  <root> --from <ref> --to <ref> --kind K --expect-generation N
                      [--strength S] [--direction D] [--lifecycle L] [--constraint KIND=VALUE]...
                      [--source-id ID] [--principal ID] [--annotation TEXT] [--recorded-at-ms N]
fdep update    <root> --edge N --expect-generation N --expect-revision R --strength S [...]
fdep transition<root> --edge N --expect-generation N --expect-revision R --to STATE [--reason TEXT]
fdep remove    <root> --edge N --expect-generation N --expect-revision R [--reason TEXT]
fdep declare-ref  <root> --ref <ref> --expect-generation N
fdep withdraw-ref <root> --ref <ref> --expect-generation N [--allow-referenced]
```

Exit codes: `0` accepted, `1` usage error, `2` rejected by the registry, `3`
store failure, `4` internal error. Every rejection prints its stable token
first, so a script can match on it without parsing prose.

`--json` produces machine-readable output for `inspect`, `edges` and `export`.
`--self-check` and `--scenario` run scripted end-to-end exercises of the real
library against a store the tool creates and removes; the test suite runs both.

An unrecognised kind, strength, lifecycle or numeric argument is a usage error,
never something that is silently dropped: a query is never quietly widened or
narrowed because a token was misspelled.

---

## 10. Validation performed

All of the following was run on one Windows 11 x64 machine with MSVC 19.44
(Visual Studio 2022 build tools) and Ninja, from the committed sources.

**Evidence classes.** Everything below is **REAL**: the library, the tests, the
tool and the benchmarks were compiled and executed. Hardware claims are absent
by construction: no accelerator, network, PDU, UPS or cooling hardware was
touched, and this repository integrates with none. Benchmark numbers are
**SYNTHETIC** in the sense that the graph they measure is generated in-process
from a fixed seed; they are real measurements of this library doing real work on
that graph.

| Validation | Result |
| --- | --- |
| Release build, `/W4 /WX` | Clean, zero warnings |
| Debug build, `/W4 /WX`, `_ITERATOR_DEBUG_LEVEL=2`, `/RTC1` | Clean, zero warnings |
| Debug test suite | 245 tests, 245 passed |
| Release test suite | 245 tests, 245 passed |
| AddressSanitizer (Debug, MSVC `/fsanitize=address`) | 245 tests, 245 passed, no sanitizer report |
| `ctest` (Release) | 12/12 entries: tests, 8 examples, 2 CLI exercises, benchmark smoke |
| Multiprocess tests | Real second processes: writer fencing, read-only inspection while another process writes, crash without cleanup, repeated restart, monotonic generations |
| Crash tests | A real child process terminated at each of the ten publication steps, and terminated from outside mid-publication; on every reopen the state was the previous or the new generation, never a partial one |
| Corruption tests | Damaged magic, version, flags, length, CRC-32, SHA-256, truncation, trailing bytes, empty file, oversized file, misnamed generation, malformed pointers, damaged lock record, a directory named like a generation, wrong-width names, and a byte-flip sweep across the whole newest generation |
| Property tests | 8 suites over fixed seeds, graphs up to 200 declarations, both legal and illegal cycles, deletion and reinsertion, stale generations, bounded traversals, reachability agreement, component partition, cycle enumeration and impact-cone monotonicity |
| Adversarial tests | Path-like identifiers, non-directory roots, 200 unrelated files in a store root, capacity and generation ceilings, hostile annotations in both exports, oversized declarations, eight concurrent store opens, a fenced writer, oversized files, hostile header counts, a 600-deep chain, and 400 randomly corrupted payloads |
| Install and downstream consumer | `cmake --install` into a prefix, then an independent `find_package` project built and run from outside the source tree |
| Benchmarks | `fdep_benchmark` over 1 000 / 3 000 / 8 000 edge graphs plus durable mutation; see below |

### Benchmarks

Measured with `fdep_benchmark` (Release, MSVC 19.44, one machine, synthetic
facility-shaped graph from seed 1554098974). Throughput is of **completed**
operations; the durable figure includes the flush and the pointer replacement.

| Measurement | 1 000 edges | 3 000 edges | 8 000 edges |
| --- | --- | --- | --- |
| Direct lookup (completed) | ~373 k ops/s | ~221 k ops/s | ~562 k ops/s |
| Bounded impact traversal | ~8.8 k ops/s | ~133 k ops/s | ~138 k ops/s |
| Declaration validation | ~8.7 M ops/s | ~5.1 M ops/s | ~8.5 M ops/s |
| Duplicate check (commits nothing) | ~162 k ops/s | ~1.46 M ops/s | ~2.35 M ops/s |
| Whole-graph SCC | ~702 ops/s | ~178 ops/s | ~128 ops/s |
| Acyclic obligation check | ~16.1 k ops/s | ~4.3 k ops/s | ~1.7 k ops/s |
| Generation diff | ~2.7 k ops/s | ~4.6 k ops/s | ~2.2 k ops/s |
| Canonical encoding | ~530 MiB/s | ~725 MiB/s | ~917 MiB/s |

Durable mutation, measured separately on a 400-edge store because each
committed operation rewrites and flushes a whole generation: **mean 16.9 ms per
committed mutation, about 59 durable mutations per second, single writer.**

The spread between scales is measurement noise on a shared machine, not a
scaling claim. The numbers that matter are the ones that say what this library
is and is not for: declarations are validated tens of millions of times a
second, lookups and bounded traversals are sub-millisecond, whole-graph
analyses are milliseconds, and a **durable** mutation costs milliseconds because
it rewrites and flushes an entire generation.

---

## 11. Genuine limitations

* **A committed mutation is O(E) in the size of the graph.** A new generation
  serialises the whole graph, and the in-memory state is rebuilt and fully
  re-validated on every commit. The durable path additionally rewrites and
  flushes the whole generation file. This is a deliberate consequence of
  "one generation is one authoritative state", and it is why this repository is
  a registry for facility dependencies that change on maintenance and change
  windows, not a stream processor. Building the 8 000-edge benchmark graph takes
  about 71 seconds of committed mutations; answering queries against it takes
  microseconds.
* **The state digest is sensitive to allocation history.** Edge identities are
  part of authoritative state, so two registries that reach the same graph by
  registering edges in different orders have different state and content
  digests. Two registries that perform the same mutations in the same order
  produce byte-identical states.
* **Removals leave no durable tombstone.** A removed edge is gone; it is visible
  only through a diff between retained generations. Provenance is preserved for
  authoritative state, not for state that no longer exists.
* **The journal is in memory.** It records committed changes and rejections, is
  bounded by `max_journal_entries`, and does not survive a restart. The durable
  record of a change is the generation it produced.
* **The POSIX branch is implemented but was not exercised on this machine.**
  File locking, atomic replacement, `fsync` and directory `fsync` have separate
  POSIX implementations selected at compile time; the validation above ran the
  Windows implementations. The POSIX branch is written to the same protocol but
  is, on this evidence, unverified.
* **Windows has no directory `fsync`.** There, the durability of a replacement
  comes from `MOVEFILE_WRITE_THROUGH`, and `sync_directory` deliberately does
  nothing rather than pretending.
* **Recovery is not a repair tool.** A corrupt generation is rejected, not
  fixed, and a store whose every generation is unreadable refuses to open. The
  registry never reconstructs state it cannot verify.
* **Annotations are restricted to printable text.** Control characters,
  including newline and tab, are rejected so that the canonical exports stay
  unambiguous. That is a deliberate restriction, not an oversight.
* **Requirement metadata is a declaration.** Redundancy classes, latency and
  bandwidth bounds are stored, validated, diffed and exported. Nothing computes
  with them.
* **No tombstones, no observers, no asynchronous work.** There are no
  subscriptions, no change streams and no background threads; a consumer that
  wants to follow a registry polls snapshots and diffs generations.

---

## 12. Repository layout

```
include/facility_dependency_registry/   the public API, one self-contained header per concept
src/                                    the implementation, including the internal graph state
tools/fdep_cli.cpp                      the inspection and operation tool
examples/                               eight programs over the stable public API
benchmarks/benchmark_registry.cpp       completed-operation benchmarks
tests/                                  the proof obligations, one file per area
tests/package_consumer/                 an independent find_package consumer
```

`CONTRIBUTING.md` describes the contribution terms and the engineering
expectations. `NOTICE` records attribution.

---

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
