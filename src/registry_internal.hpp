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

#ifndef FACILITY_DEPENDENCY_REGISTRY_SRC_REGISTRY_INTERNAL_HPP
#define FACILITY_DEPENDENCY_REGISTRY_SRC_REGISTRY_INTERNAL_HPP

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "facility_dependency_registry/clock.hpp"
#include "facility_dependency_registry/persistence.hpp"
#include "facility_dependency_registry/registry.hpp"
#include "facility_dependency_registry/requests.hpp"
#include "graph_state.hpp"

namespace facility_dependency_registry {
namespace detail {

/// The whole mutable state of one registry session.
///
/// One mutex guards mutations; readers never take it, because the authoritative
/// state is reached through one atomic shared pointer. The mutex is never held
/// across a call into code that could re-enter this object: this type has no
/// callbacks and no observers, and every public mutation takes the lock once,
/// at the top, and releases it on return.
struct RegistryImpl {
  mutable std::mutex mutation_mutex{};
  std::atomic<std::shared_ptr<const GraphState>> state{};
  std::unique_ptr<DurableStore> store{};
  RegistryLimits limits{};
  std::shared_ptr<Clock> clock{};
  WriterIncarnation incarnation{};
  RecoveryReport recovery{};
  std::deque<JournalEntry> journal{};
  bool durable = false;
  bool closed = false;

  [[nodiscard]] RegistrySnapshot snapshot() const;
  [[nodiscard]] std::int64_t now_ms() const;
  void record(JournalEntry entry);
};

/// Appends a journal entry, dropping the oldest ones so the ring stays inside
/// the configured bound.
void record_journal(RegistryImpl& impl, JournalEntry entry);

/// Validates the authority context of a mutation. Called with the mutation
/// lock already held.
[[nodiscard]] Status check_authority(const RegistryImpl& impl, const MutationContext& context);

/// Publishes `next` and only then makes it visible. When the registry is
/// durable the publication is the durable one; a failed publication leaves the
/// in-memory state exactly as it was.
[[nodiscard]] Status commit_state(RegistryImpl& impl, std::shared_ptr<const GraphState> next);

/// Builds the next state from a modified edge list and the unchanged
/// declarations, annotating a cycle rejection with what the caller was doing.
[[nodiscard]] Result<std::shared_ptr<const GraphState>> build_next_state(
    const GraphState& current, DependencyGeneration generation, std::uint64_t next_edge_ordinal,
    std::vector<DependencyEdgeRecord> edges, std::string_view what);

/// The same, for a modified declaration list.
[[nodiscard]] Result<std::shared_ptr<const GraphState>> build_next_state_with_refs(
    const GraphState& current, DependencyGeneration generation, std::vector<DeclaredExternalRef> refs,
    std::string_view what);

/// A journal entry that is written when the mutation scope ends, whatever the
/// outcome. Every mutation therefore produces exactly one journal record.
class MutationScope {
 public:
  MutationScope(RegistryImpl& impl, JournalOperation operation);
  ~MutationScope();

  MutationScope(const MutationScope&) = delete;
  MutationScope& operator=(const MutationScope&) = delete;

  void reject(const Status& status);
  void reject(const RegistryError& error);
  void accept(DependencyEdgeId id, std::string detail = {});
  void accept(DependencyNodeRef node, std::string detail = {});
  void set_generation(DependencyGeneration generation) { entry_.generation = generation; }

 private:
  RegistryImpl& impl_;
  JournalEntry entry_{};
};

}  // namespace detail
}  // namespace facility_dependency_registry

#endif  // FACILITY_DEPENDENCY_REGISTRY_SRC_REGISTRY_INTERNAL_HPP
