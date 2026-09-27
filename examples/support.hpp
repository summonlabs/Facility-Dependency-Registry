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

#ifndef FACILITY_DEPENDENCY_REGISTRY_EXAMPLES_SUPPORT_HPP
#define FACILITY_DEPENDENCY_REGISTRY_EXAMPLES_SUPPORT_HPP

/// Shared helpers for the Facility Dependency Registry example programs.
///
/// This is example scaffolding, not library code. Every helper is `inline`, so
/// the header can be included by any number of example translation units, and
/// every helper that is handed input the library would reject reports the
/// rejection and exits instead of letting an example continue on a failed call.
///
/// Nothing here is deterministic by accident: declarations are stamped with one
/// fixed instant, so no example output depends on the host clock.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "facility_dependency_registry/facility_dependency_registry.hpp"

namespace fdep_examples {

using facility_dependency_registry::DeclareRefOutcome;
using facility_dependency_registry::DeclareRefRequest;
using facility_dependency_registry::DependencyConstraint;
using facility_dependency_registry::DependencyEdgeSpec;
using facility_dependency_registry::DependencyKind;
using facility_dependency_registry::DependencyNodeRef;
using facility_dependency_registry::DependencyRegistry;
using facility_dependency_registry::DependencyStrength;
using facility_dependency_registry::Direction;
using facility_dependency_registry::LifecycleState;
using facility_dependency_registry::NodeDomain;
using facility_dependency_registry::ProvenanceRecord;
using facility_dependency_registry::ProvenanceSource;
using facility_dependency_registry::RegisterEdgeOutcome;
using facility_dependency_registry::RegisterEdgeRequest;
using facility_dependency_registry::Result;
using facility_dependency_registry::Status;

/// 2026-01-01T00:00:00Z in milliseconds. Every example declaration is stamped
/// with this instant, so a report never carries a wall-clock value.
inline constexpr std::int64_t kExampleTimestampUnixMs = 1'767'225'600'000;

/// Reports a condition an example asserts is impossible and terminates with a
/// failure status. Examples are not library code: a broken example should stop
/// loudly rather than continue with a value it did not expect.
[[noreturn]] inline void fail(const char* what) {
  std::cerr << "example failure: " << what << '\n';
  std::exit(1);
}

/// The required example assertion helper.
inline void check(bool condition, const char* what) {
  if (!condition) {
    fail(what);
  }
}

/// Unwraps a successful `Result`, or prints the rejection and exits.
template <class T>
inline T unwrap(Result<T> result, const char* what) {
  if (!result) {
    std::cerr << "example failure: " << what << ": " << result.error().to_string() << '\n';
    std::exit(1);
  }
  return std::move(result).value();
}

/// Checks a `Status`, or prints the rejection and exits.
inline void expect_ok(Status status, const char* what) {
  if (!status.ok()) {
    std::cerr << "example failure: " << what << ": " << status.error().to_string() << '\n';
    std::exit(1);
  }
}

/// Builds an operator declaration provenance record.
inline ProvenanceRecord make_provenance(std::string_view source_id, std::string_view principal,
                                        std::string_view annotation) {
  return unwrap(ProvenanceRecord::create(ProvenanceSource::OperatorDeclaration, source_id, principal,
                                         kExampleTimestampUnixMs, annotation,
                                         facility_dependency_registry::kHardMaxIdLength,
                                         facility_dependency_registry::kHardMaxAnnotationLength),
                "make_provenance");
}

/// The provenance every `make_spec` declaration carries unless the caller
/// replaces it.
inline ProvenanceRecord default_provenance() {
  return make_provenance("fdep-examples", "example-operator", "example declaration");
}

/// Builds one validated reference in `domain`. Invalid example input aborts.
inline DependencyNodeRef make_node(NodeDomain domain, std::string_view id) {
  return unwrap(DependencyNodeRef::create(domain, id), "make_node");
}

inline DependencyNodeRef asset(std::string_view id) { return make_node(NodeDomain::Asset, id); }
inline DependencyNodeRef rack(std::string_view id) { return make_node(NodeDomain::Rack, id); }
inline DependencyNodeRef feed(std::string_view id) { return make_node(NodeDomain::ElectricalDomain, id); }
inline DependencyNodeRef cooling_loop(std::string_view id) { return make_node(NodeDomain::CoolingDomain, id); }
inline DependencyNodeRef service(std::string_view id) { return make_node(NodeDomain::FacilityService, id); }
inline DependencyNodeRef asi(std::string_view id) { return make_node(NodeDomain::AsiDomain, id); }
inline DependencyNodeRef dfi(std::string_view id) { return make_node(NodeDomain::DfiDomain, id); }

/// Builds one edge declaration. The provenance is the shared example
/// declaration; a caller that needs its own overwrites `spec.provenance`.
inline DependencyEdgeSpec make_spec(DependencyNodeRef from, DependencyNodeRef to, DependencyKind kind,
                                    DependencyStrength strength, Direction direction, LifecycleState lifecycle,
                                    std::vector<DependencyConstraint> constraints = {}) {
  DependencyEdgeSpec spec;
  spec.source = std::move(from);
  spec.target = std::move(to);
  spec.kind = kind;
  spec.strength = strength;
  spec.direction = direction;
  spec.initial_lifecycle = lifecycle;
  spec.constraints = std::move(constraints);
  spec.provenance = default_provenance();
  return spec;
}

/// Declares one external reference at the current generation.
inline DeclareRefOutcome declare_ref(DependencyRegistry& registry, const DependencyNodeRef& ref,
                                     std::string_view annotation) {
  DeclareRefRequest request;
  request.context.expected_generation = registry.generation();
  request.ref = ref;
  request.provenance = make_provenance("facility-inventory", "example-operator", annotation);
  return unwrap(registry.declare_external_ref(request), "declare_external_ref");
}

/// Registers one edge at the current generation.
inline RegisterEdgeOutcome add_edge(DependencyRegistry& registry, const DependencyEdgeSpec& spec) {
  RegisterEdgeRequest request;
  request.context.expected_generation = registry.generation();
  request.spec = spec;
  return unwrap(registry.register_edge(request), "register_edge");
}

/// `true`/`false` for a `bool`, so reports read unambiguously.
inline const char* yes_no(bool value) noexcept { return value ? "true" : "false"; }

}  // namespace fdep_examples

#endif  // FACILITY_DEPENDENCY_REGISTRY_EXAMPLES_SUPPORT_HPP
