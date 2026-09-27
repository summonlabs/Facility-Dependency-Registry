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

#ifndef FACILITY_DEPENDENCY_REGISTRY_REGISTRY_HPP
#define FACILITY_DEPENDENCY_REGISTRY_REGISTRY_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "facility_dependency_registry/cancel.hpp"
#include "facility_dependency_registry/clock.hpp"
#include "facility_dependency_registry/errors.hpp"
#include "facility_dependency_registry/ids.hpp"
#include "facility_dependency_registry/limits.hpp"
#include "facility_dependency_registry/persistence.hpp"
#include "facility_dependency_registry/requests.hpp"
#include "facility_dependency_registry/snapshot.hpp"

namespace facility_dependency_registry {

namespace detail {
struct RegistryImpl;
}  // namespace detail

/// How to open a durable registry.
struct RegistryOpenRequest {
  /// The store root directory. Created when missing, unless the store is
  /// opened read-only.
  std::filesystem::path root{};
  StoreOptions store{};
  /// The clock used to stamp journal entries. Defaults to the system clock;
  /// tests inject a fixed clock so that every recorded timestamp is
  /// reproducible.
  std::shared_ptr<Clock> clock{};
};

/// How to open a non-durable registry.
struct EphemeralOptions {
  RegistryLimits limits{};
  std::shared_ptr<Clock> clock{};
};

/// The authoritative dependency registry.
///
/// Concurrent access model, stated exactly:
///
/// * Exactly one mutation is in flight at a time. Mutations take one internal
///   mutex, and that mutex is never held across a call back into user code,
///   because this type has no callbacks and no observers.
/// * Readers never take that mutex. A reader takes an immutable snapshot, which
///   is one atomic shared-pointer load, and then queries the snapshot.
/// * Publication is ordered: the mutation is applied to a new immutable state,
///   that state is made durable (when the registry is durable), and only then
///   is it published in memory. A failed or cancelled publish leaves the
///   registry exactly as it was.
/// * `close()` refuses new mutations, waits for an in-flight mutation to
///   finish, and then releases the writer lock. It never cancels work that has
///   already crossed the commit boundary.
class DependencyRegistry {
 public:
  DependencyRegistry() noexcept;
  ~DependencyRegistry();
  DependencyRegistry(DependencyRegistry&&) noexcept;
  DependencyRegistry& operator=(DependencyRegistry&&) noexcept;
  DependencyRegistry(const DependencyRegistry&) = delete;
  DependencyRegistry& operator=(const DependencyRegistry&) = delete;

  /// Opens the durable store at `request.root` and loads its authoritative
  /// state. Recovery is conservative: the newest intact generation is loaded,
  /// and a corrupt store is refused rather than replaced with an empty one.
  [[nodiscard]] static Result<DependencyRegistry> open(const RegistryOpenRequest& request);

  /// Opens a registry that keeps its state only in memory. Every mutation
  /// behaves identically except that nothing is published to disk; persistence
  /// specific operations report NotDurable.
  [[nodiscard]] static Result<DependencyRegistry> open_ephemeral(const EphemeralOptions& options = {});

  // -- authority -------------------------------------------------------
  /// The current authoritative generation.
  [[nodiscard]] DependencyGeneration generation() const noexcept;
  [[nodiscard]] bool durable() const noexcept;
  [[nodiscard]] bool closed() const noexcept;
  [[nodiscard]] const RegistryLimits& limits() const noexcept;
  /// How the durable state was established at open time. FreshEmpty for an
  /// ephemeral registry.
  [[nodiscard]] const RecoveryReport& recovery_report() const noexcept;
  /// The writer incarnation of this session. Invalid for an ephemeral registry.
  [[nodiscard]] WriterIncarnation writer_incarnation() const;
  [[nodiscard]] std::shared_ptr<const Clock> clock() const noexcept;

  // -- immutable view --------------------------------------------------
  /// The current authoritative state. Cheap: one atomic load and one shared
  /// pointer copy. The returned snapshot stays valid and unchanged forever,
  /// whatever happens to the registry afterwards, including close.
  [[nodiscard]] RegistrySnapshot snapshot() const;

  // -- mutation commands ------------------------------------------------
  [[nodiscard]] Result<RegisterEdgeOutcome> register_edge(const RegisterEdgeRequest& request);
  [[nodiscard]] Result<UpdateEdgeOutcome> update_edge(const UpdateEdgeRequest& request);
  [[nodiscard]] Result<LifecycleTransitionOutcome> transition_edge_lifecycle(
      const LifecycleTransitionRequest& request);
  [[nodiscard]] Result<RemoveEdgeOutcome> remove_edge(const RemoveEdgeRequest& request);
  [[nodiscard]] Result<DeclareRefOutcome> declare_external_ref(const DeclareRefRequest& request);
  [[nodiscard]] Result<WithdrawRefOutcome> withdraw_external_ref(const WithdrawRefRequest& request);

  // -- diagnostics -------------------------------------------------------
  /// The bounded in-memory journal, oldest first. It records committed changes
  /// and rejected attempts. It is a diagnostic, not authoritative state, and
  /// it does not survive a restart.
  [[nodiscard]] std::vector<JournalEntry> journal() const;
  /// What the durable store currently holds, without mutating anything.
  [[nodiscard]] Result<StoreStatus> store_status() const;

  /// Refuses further mutations, waits for any in-flight mutation, then
  /// releases the writer lock. Idempotent. Snapshots taken before the close
  /// remain valid.
  [[nodiscard]] Status close();

 private:
  std::unique_ptr<detail::RegistryImpl> impl_{};
};

}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_REGISTRY_HPP
