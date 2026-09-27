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

#ifndef FACILITY_DEPENDENCY_REGISTRY_PERSISTENCE_HPP
#define FACILITY_DEPENDENCY_REGISTRY_PERSISTENCE_HPP

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "facility_dependency_registry/errors.hpp"
#include "facility_dependency_registry/ids.hpp"
#include "facility_dependency_registry/limits.hpp"
#include "facility_dependency_registry/snapshot.hpp"

namespace facility_dependency_registry {

/// Identifies one writer session of one durable store.
///
/// The incarnation is recorded inside the writer lock file and inside every
/// published generation. It exists so an operator can see who last wrote a
/// store, and so a stale writer can be recognized after the fact. It is *not*
/// the fencing mechanism: authority over a store comes from the operating
/// system lock on the lock file, which disappears with the process that holds
/// it. A leftover incarnation record therefore never blocks or authorizes
/// anything.
class WriterIncarnation {
 public:
  WriterIncarnation() = default;

  [[nodiscard]] static WriterIncarnation generate();

  [[nodiscard]] std::uint64_t process_id() const noexcept { return process_id_; }
  [[nodiscard]] std::uint64_t nonce() const noexcept { return nonce_; }
  [[nodiscard]] std::int64_t started_at_unix_ms() const noexcept { return started_at_unix_ms_; }
  [[nodiscard]] bool valid() const noexcept { return nonce_ != 0; }

  /// `pid=<n> nonce=<hex> started-ms=<n>`
  [[nodiscard]] std::string to_text() const;

  friend bool operator==(const WriterIncarnation& lhs, const WriterIncarnation& rhs) noexcept {
    return lhs.process_id_ == rhs.process_id_ && lhs.nonce_ == rhs.nonce_ &&
           lhs.started_at_unix_ms_ == rhs.started_at_unix_ms_;
  }

  /// Parses the text form produced by `to_text()`. Strict: any deviation is
  /// rejected rather than partially accepted.
  [[nodiscard]] static Result<WriterIncarnation> parse(std::string_view text);

 private:
  std::uint64_t process_id_ = 0;
  std::uint64_t nonce_ = 0;
  std::int64_t started_at_unix_ms_ = 0;
};

/// How a store is opened.
enum class OpenMode : std::uint8_t {
  /// No lock is taken and nothing is written, repaired or pruned. The caller
  /// sees exactly the committed state of the store, or a deterministic
  /// rejection. This is the mode the inspection tool uses by default.
  ReadOnly = 1,
  /// The exclusive writer lock is taken for the lifetime of the store. Only
  /// one process at a time may hold it.
  ReadWrite = 2,
};

[[nodiscard]] std::string_view to_token(OpenMode mode) noexcept;

/// The named steps of the publication protocol, in order.
///
/// plan -> validate -> reserve generation -> write transient file -> sync ->
/// re-read and verify -> rename to the generation file -> sync the directory ->
/// atomically replace the pointer -> sync -> prune superseded generations.
enum class PublishStep : std::uint8_t {
  Begun = 1,
  TransientWritten = 2,
  TransientSynced = 3,
  TransientVerified = 4,
  GenerationRenamed = 5,
  DirectorySynced = 6,
  PointerReplaced = 7,
  PointerSynced = 8,
  Pruned = 9,
  Completed = 10,
};

[[nodiscard]] std::string_view to_token(PublishStep step) noexcept;

/// Injection points for durability testing. A hook runs immediately after the
/// named step has completed, so a test can terminate the process at an exact
/// point of the protocol and then reopen the store.
///
/// Hooks are inert by default. A hook runs inside the publication path, and
/// when the publication was requested by a `DependencyRegistry` that registry
/// holds its mutation lock for the duration: a hook must therefore not call
/// back into that registry. The crash tests use hooks for exactly one thing,
/// which is recording the step and terminating the process.
struct PublishFaultHooks {
  std::function<void(PublishStep step, DependencyGeneration generation)> after_step{};

  [[nodiscard]] bool active() const noexcept { return static_cast<bool>(after_step); }
};

/// Configuration of one durable store.
struct StoreOptions {
  OpenMode mode = OpenMode::ReadWrite;
  RegistryLimits limits{};
  /// Create the store root directory when it does not exist. Only honoured in
  /// ReadWrite mode; a read-only open of a missing store fails.
  bool create_if_missing = true;
  PublishFaultHooks faults{};
};

/// How a store's authoritative state was established at open time.
enum class RecoveryOutcome : std::uint8_t {
  /// The store root held no generation and no pointer. A new, empty store was
  /// created in ReadWrite mode, or reported empty in ReadOnly mode.
  FreshEmpty = 1,
  /// The pointer was valid and the generation it named passed integrity and
  /// semantic validation.
  LoadedCurrent = 2,
  /// The pointer was missing, malformed, or named a generation that failed
  /// validation, and a lower intact generation was found and loaded instead.
  /// Newer unusable generations were rejected, not repaired.
  LoadedFallback = 3,
  /// Files existed but none could be validated. Nothing was loaded and the
  /// open failed: an empty graph is never presented in place of corrupt
  /// authoritative state.
  RefusedCorrupt = 4,
};

[[nodiscard]] std::string_view to_token(RecoveryOutcome outcome) noexcept;

/// What happened while opening a store.
struct RecoveryReport {
  RecoveryOutcome outcome = RecoveryOutcome::FreshEmpty;
  DependencyGeneration loaded_generation{};
  /// Generations that were present but failed validation, newest first.
  std::vector<DependencyGeneration> rejected_generations{};
  /// Bounded, human readable notes: what was rejected and why.
  std::vector<std::string> diagnostics{};
  std::uint32_t transient_files_removed = 0;
  std::uint32_t generations_pruned = 0;

  /// True when the loaded state is not simply the newest intact generation.
  [[nodiscard]] bool is_degraded() const noexcept {
    return outcome == RecoveryOutcome::LoadedFallback || outcome == RecoveryOutcome::RefusedCorrupt;
  }

  [[nodiscard]] std::string to_text() const;
};

/// A read-only description of what is on disk.
struct StoreStatus {
  std::filesystem::path root{};
  bool read_only = false;
  bool writer_lock_held = false;
  /// The incarnation recorded in the lock file, whether or not this process
  /// holds the lock.
  WriterIncarnation lock_incarnation{};
  bool pointer_present = false;
  DependencyGeneration pointer_generation{};
  /// Retained generation files, newest first.
  std::vector<DependencyGeneration> retained_generations{};
  std::uint64_t total_bytes = 0;
  std::uint32_t transient_files = 0;

  [[nodiscard]] std::string to_text() const;
};

/// A versioned, integrity checked, single-writer durable store.
///
/// Layout of a store root:
///
/// ```
/// <root>/writer.lock                  exclusive lock file, holds an incarnation
/// <root>/CURRENT                      atomically replaced pointer to the newest generation
/// <root>/gen-00000000000000000001.fdepstate
/// <root>/gen-00000000000000000002.fdepstate
/// ```
///
/// Only `CURRENT` names the authoritative generation. A generation file is
/// authoritative only when `CURRENT` names it *and* it passes its own length,
/// CRC-32 and SHA-256 checks *and* it decodes into a semantically valid graph.
/// Transient `tmp-` files are removed, never interpreted. Publication is
/// atomic: the generation file appears complete before the pointer names it,
/// and the pointer is replaced by an atomic rename, so a crash leaves either
/// the previous or the new generation authoritative and never a partial one.
class DurableStore {
 public:
  DurableStore() noexcept;
  ~DurableStore();
  DurableStore(DurableStore&&) noexcept;
  DurableStore& operator=(DurableStore&&) noexcept;
  DurableStore(const DurableStore&) = delete;
  DurableStore& operator=(const DurableStore&) = delete;

  /// Opens a store and loads its authoritative snapshot.
  ///
  /// In ReadWrite mode the writer lock is acquired first; if another process
  /// holds it the open fails with StoreLocked and the holder's incarnation is
  /// reported. Transient files are then removed, the pointer and its
  /// generation are validated, and a lower intact generation is used only if
  /// the pointer's target cannot be used.
  [[nodiscard]] static Result<std::unique_ptr<DurableStore>> open(const std::filesystem::path& root,
                                                                  const StoreOptions& options,
                                                                  RegistrySnapshot& out_snapshot,
                                                                  RecoveryReport& out_report);

  [[nodiscard]] const std::filesystem::path& root() const noexcept;
  [[nodiscard]] bool read_only() const noexcept;
  [[nodiscard]] bool lock_held() const noexcept;
  [[nodiscard]] const WriterIncarnation& incarnation() const noexcept;
  /// The generation this store last validated or published.
  [[nodiscard]] DependencyGeneration committed_generation() const noexcept;

  /// Re-reads and re-validates `CURRENT`, returning the authoritative state as
  /// it is on disk right now.
  [[nodiscard]] Result<RegistrySnapshot> load() const;
  /// Reads one retained generation directly. Read-only in both modes.
  [[nodiscard]] Result<RegistrySnapshot> load_generation(DependencyGeneration generation) const;
  /// Retained generations, newest first.
  [[nodiscard]] Result<std::vector<DependencyGeneration>> retained_generations() const;
  [[nodiscard]] Result<StoreStatus> status() const;

  /// Publishes exactly `committed_generation() + 1`.
  ///
  /// Rejects a snapshot whose generation is not the next one, a store opened
  /// read-only, a store that no longer holds the writer lock, and a pointer
  /// that changed underneath the store since it was last validated. On success
  /// the new generation is durable and `CURRENT` names it.
  [[nodiscard]] Status publish(const RegistrySnapshot& next);

  /// Releases the writer lock and forgets the session. Idempotent.
  [[nodiscard]] Status close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_{};
};

}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_PERSISTENCE_HPP
