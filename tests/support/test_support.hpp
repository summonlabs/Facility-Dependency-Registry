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

#ifndef FACILITY_DEPENDENCY_REGISTRY_TESTS_SUPPORT_TEST_SUPPORT_HPP
#define FACILITY_DEPENDENCY_REGISTRY_TESTS_SUPPORT_TEST_SUPPORT_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "facility_dependency_registry/facility_dependency_registry.hpp"
#include "test_harness.hpp"

namespace fdep_test {

using facility_dependency_registry::DependencyEdgeId;
using facility_dependency_registry::DependencyEdgeSpec;
using facility_dependency_registry::DependencyGeneration;
using facility_dependency_registry::DependencyKind;
using facility_dependency_registry::DependencyNodeRef;
using facility_dependency_registry::DependencyRegistry;
using facility_dependency_registry::DependencyStrength;
using facility_dependency_registry::Direction;
using facility_dependency_registry::EdgeRevision;
using facility_dependency_registry::LifecycleState;
using facility_dependency_registry::NodeDomain;
using facility_dependency_registry::ProvenanceRecord;
using facility_dependency_registry::ProvenanceSource;
using facility_dependency_registry::RegistryLimits;
using facility_dependency_registry::RegistrySnapshot;
using facility_dependency_registry::Result;
using facility_dependency_registry::Status;

/// Fails the current test by throwing; the harness reports it as an exception.
[[noreturn]] inline void fail_now(std::string message) { throw std::runtime_error{std::move(message)}; }

inline DependencyNodeRef node(NodeDomain domain, std::string_view id) {
  auto ref = DependencyNodeRef::create(domain, id);
  if (!ref) {
    fail_now("test helper could not build a node reference: " + ref.error().detail());
  }
  return std::move(ref).value();
}

inline DependencyNodeRef asset(std::string_view id) { return node(NodeDomain::Asset, id); }
inline DependencyNodeRef rack(std::string_view id) { return node(NodeDomain::Rack, id); }
inline DependencyNodeRef feed(std::string_view id) { return node(NodeDomain::ElectricalDomain, id); }
inline DependencyNodeRef loop(std::string_view id) { return node(NodeDomain::CoolingDomain, id); }
inline DependencyNodeRef service(std::string_view id) { return node(NodeDomain::FacilityService, id); }
inline DependencyNodeRef asi(std::string_view id) { return node(NodeDomain::AsiDomain, id); }
inline DependencyNodeRef dfi(std::string_view id) { return node(NodeDomain::DfiDomain, id); }

inline ProvenanceRecord provenance(std::string_view source_id = "test-source",
                                   std::string_view principal = "test-principal", std::int64_t when = 0,
                                   std::string_view annotation = {}) {
  auto record = ProvenanceRecord::create(ProvenanceSource::OperatorDeclaration, source_id, principal, when,
                                         annotation, 160, 512);
  if (!record) {
    fail_now("test helper could not build provenance: " + record.error().detail());
  }
  return std::move(record).value();
}

inline DependencyEdgeSpec make_spec(const DependencyNodeRef& from, const DependencyNodeRef& to, DependencyKind kind,
                                    DependencyStrength strength = DependencyStrength::Hard,
                                    Direction direction = Direction::DependsOn,
                                    LifecycleState lifecycle = LifecycleState::Active,
                                    std::vector<facility_dependency_registry::DependencyConstraint> constraints = {},
                                    ProvenanceRecord provenance_record = provenance()) {
  DependencyEdgeSpec spec;
  spec.source = from;
  spec.target = to;
  spec.kind = kind;
  spec.strength = strength;
  spec.direction = direction;
  spec.initial_lifecycle = lifecycle;
  spec.constraints = std::move(constraints);
  spec.provenance = std::move(provenance_record);
  return spec;
}

/// A registry plus the small conveniences every test needs. Helpers that are
/// expected to succeed throw on rejection, so a broken precondition shows up as
/// a failed test rather than as a silently different graph.
class Harness {
 public:
  explicit Harness(DependencyRegistry registry) : registry_(std::move(registry)) {}

  static Harness ephemeral(RegistryLimits limits = RegistryLimits{}) {
    facility_dependency_registry::EphemeralOptions options;
    options.limits = limits;
    options.clock = std::make_shared<facility_dependency_registry::FixedClock>(1'700'000'000'000);
    auto registry = DependencyRegistry::open_ephemeral(options);
    if (!registry) {
      fail_now("could not open an ephemeral registry: " + registry.error().detail());
    }
    return Harness{std::move(registry).value()};
  }

  static Harness durable(const std::filesystem::path& root, RegistryLimits limits = RegistryLimits{},
                         facility_dependency_registry::PublishFaultHooks faults = {}) {
    facility_dependency_registry::RegistryOpenRequest request;
    request.root = root;
    request.store.limits = limits;
    request.store.faults = std::move(faults);
    request.clock = std::make_shared<facility_dependency_registry::FixedClock>(1'700'000'000'000);
    auto registry = DependencyRegistry::open(request);
    if (!registry) {
      fail_now("could not open a durable registry: " + registry.error().detail());
    }
    return Harness{std::move(registry).value()};
  }

  [[nodiscard]] DependencyRegistry& registry() noexcept { return registry_; }
  [[nodiscard]] const DependencyRegistry& registry() const noexcept { return registry_; }
  [[nodiscard]] RegistrySnapshot snapshot() const { return registry_.snapshot(); }
  [[nodiscard]] DependencyGeneration generation() const { return registry_.generation(); }

  Result<facility_dependency_registry::RegisterEdgeOutcome> try_add(const DependencyEdgeSpec& spec,
                                                                   DependencyGeneration expected) {
    facility_dependency_registry::RegisterEdgeRequest request;
    request.context.expected_generation = expected;
    request.spec = spec;
    return registry_.register_edge(request);
  }

  DependencyEdgeId add(const DependencyNodeRef& from, const DependencyNodeRef& to, DependencyKind kind,
                       DependencyStrength strength = DependencyStrength::Hard,
                       Direction direction = Direction::DependsOn,
                       LifecycleState lifecycle = LifecycleState::Active,
                       std::vector<facility_dependency_registry::DependencyConstraint> constraints = {}) {
    auto outcome = try_add(make_spec(from, to, kind, strength, direction, lifecycle, std::move(constraints)),
                           registry_.generation());
    if (!outcome) {
      fail_now("register_edge was rejected with " +
               std::string{facility_dependency_registry::to_token(outcome.error().code())} + ": " +
               outcome.error().detail());
    }
    return outcome.value().id;
  }

  DependencyEdgeId add(const DependencyEdgeSpec& spec) {
    auto outcome = try_add(spec, registry_.generation());
    if (!outcome) {
      fail_now("register_edge was rejected with " +
               std::string{facility_dependency_registry::to_token(outcome.error().code())} + ": " +
               outcome.error().detail());
    }
    return outcome.value().id;
  }

  DependencyEdgeId add_and_declare(const DependencyNodeRef& from, const DependencyNodeRef& to,
                                   DependencyKind kind) {
    declare(from);
    declare(to);
    return add(from, to, kind);
  }

  void declare(const DependencyNodeRef& ref, ProvenanceRecord record = provenance()) {
    facility_dependency_registry::DeclareRefRequest request;
    request.context.expected_generation = registry_.generation();
    request.ref = ref;
    request.provenance = std::move(record);
    auto outcome = registry_.declare_external_ref(request);
    if (!outcome) {
      fail_now("declare_external_ref was rejected with " +
               std::string{facility_dependency_registry::to_token(outcome.error().code())} + ": " +
               outcome.error().detail());
    }
  }

  void declare_all(const std::vector<DependencyNodeRef>& refs) {
    for (const auto& ref : refs) {
      declare(ref);
    }
  }

  Status transition(DependencyEdgeId id, EdgeRevision revision, LifecycleState target) {
    facility_dependency_registry::LifecycleTransitionRequest request;
    request.context.expected_generation = registry_.generation();
    request.id = id;
    request.expected_revision = revision;
    request.target = target;
    auto outcome = registry_.transition_edge_lifecycle(request);
    if (!outcome) {
      return Status::failure(outcome.error());
    }
    return Status::success();
  }

  Status remove(DependencyEdgeId id, EdgeRevision revision) {
    facility_dependency_registry::RemoveEdgeRequest request;
    request.context.expected_generation = registry_.generation();
    request.id = id;
    request.expected_revision = revision;
    auto outcome = registry_.remove_edge(request);
    if (!outcome) {
      return Status::failure(outcome.error());
    }
    return Status::success();
  }

 private:
  DependencyRegistry registry_;
};

/// A deterministic pseudo random generator, so every randomized test is
/// reproducible from the seed it prints.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  [[nodiscard]] std::uint64_t next() {
    // SplitMix64: small, fast, and identical on every platform.
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t value = state_;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
  }

  [[nodiscard]] std::uint32_t below(std::uint32_t bound) {
    return bound == 0 ? 0 : static_cast<std::uint32_t>(next() % bound);
  }

  [[nodiscard]] bool chance(std::uint32_t numerator, std::uint32_t denominator) {
    return below(denominator) < numerator;
  }

 private:
  std::uint64_t state_;
};

inline std::string seed_text(std::uint64_t seed) { return "seed=" + std::to_string(seed); }

}  // namespace fdep_test

#endif  // FACILITY_DEPENDENCY_REGISTRY_TESTS_SUPPORT_TEST_SUPPORT_HPP
