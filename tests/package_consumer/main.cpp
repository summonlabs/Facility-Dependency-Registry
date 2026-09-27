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

// An independent consumer of the installed Facility Dependency Registry
// package. It uses only the public headers and the exported CMake target, and
// it exercises the parts of the surface a composing DCCP layer would use: open
// a durable store, declare references, register edges under an expected
// generation, explain impact, close, reopen and compare digests.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

#include "facility_dependency_registry/facility_dependency_registry.hpp"

namespace {

using namespace facility_dependency_registry;

int fail(const std::string& message) {
  std::cerr << "consumer failure: " << message << "\n";
  return 1;
}

DependencyNodeRef node(NodeDomain domain, const std::string& id) {
  auto ref = DependencyNodeRef::create(domain, id);
  if (!ref) {
    std::cerr << "consumer failure: " << ref.error().detail() << "\n";
    std::exit(1);
  }
  return std::move(ref).value();
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path root =
      argc > 1 ? std::filesystem::path{argv[1]}
               : std::filesystem::temp_directory_path() / "fdep-consumer-store";
  std::error_code error;
  std::filesystem::remove_all(root, error);

  auto provenance = ProvenanceRecord::create(ProvenanceSource::FacilityInventoryImport, "consumer-import",
                                             "consumer-principal", 1'700'000'000'000, "from the consumer",
                                             kHardMaxIdLength, kHardMaxAnnotationLength);
  if (!provenance) {
    return fail(provenance.error().detail());
  }

  const auto asset = node(NodeDomain::Asset, "consumer-node");
  const auto feed = node(NodeDomain::ElectricalDomain, "consumer-feed");
  const auto rack = node(NodeDomain::Rack, "consumer-rack");

  ContentDigest digest;
  DependencyGeneration generation{};
  {
    RegistryOpenRequest request;
    request.root = root;
    auto registry = DependencyRegistry::open(request);
    if (!registry) {
      return fail("open: " + registry.error().to_string());
    }

    DeclareRefRequest declaration;
    declaration.context.expected_generation = registry.value().generation();
    declaration.ref = asset;
    declaration.provenance = provenance.value();
    const auto declared = registry.value().declare_external_ref(declaration);
    if (!declared) {
      return fail("declare: " + declared.error().to_string());
    }

    DependencyEdgeSpec spec;
    spec.source = asset;
    spec.target = feed;
    spec.kind = DependencyKind::RequiresPowerFrom;
    spec.strength = DependencyStrength::Hard;
    spec.direction = Direction::DependsOn;
    spec.initial_lifecycle = LifecycleState::Active;
    spec.provenance = provenance.value();
    RegisterEdgeRequest registration;
    registration.context.expected_generation = registry.value().generation();
    registration.spec = spec;
    const auto registered = registry.value().register_edge(registration);
    if (!registered) {
      return fail("register: " + registered.error().to_string());
    }

    spec.target = rack;
    spec.kind = DependencyKind::HousedIn;
    RegisterEdgeRequest housing;
    housing.context.expected_generation = registry.value().generation();
    housing.spec = spec;
    const auto housed = registry.value().register_edge(housing);
    if (!housed) {
      return fail("register housing: " + housed.error().to_string());
    }

    // A stale attempt must be refused, exactly as it is for any consumer.
    RegisterEdgeRequest stale = registration;
    stale.spec.target = rack;
    const auto refused = registry.value().register_edge(stale);
    if (refused || refused.error().code() != ErrorCode::StaleGeneration) {
      return fail("a stale generation was not refused");
    }

    const RegistrySnapshot snapshot = registry.value().snapshot();
    ImpactConeRequest cone_request;
    cone_request.origin = feed;
    cone_request.max_depth = 4;
    cone_request.max_nodes = 64;
    const auto cone = snapshot.impact_cone(cone_request);
    if (!cone || !cone.value().reached(asset)) {
      return fail("the impact cone did not contain the dependent asset");
    }

    digest = snapshot.state_digest();
    generation = snapshot.generation();
    if (!registry.value().close().ok()) {
      return fail("close");
    }
  }

  {
    RegistryOpenRequest request;
    request.root = root;
    auto registry = DependencyRegistry::open(request);
    if (!registry) {
      return fail("reopen: " + registry.error().to_string());
    }
    const RegistrySnapshot snapshot = registry.value().snapshot();
    if (snapshot.generation() != generation || snapshot.state_digest() != digest) {
      return fail("the reopened state does not match the committed one");
    }
    if (snapshot.edge_count() != 2) {
      return fail("the reopened state does not hold both edges");
    }
    const auto exported = export_json(snapshot);
    if (!exported) {
      return fail("export: " + exported.error().to_string());
    }
    std::cout << "consumer ok version=" << version_string() << " generation=" << to_text(generation)
              << " edges=" << snapshot.edge_count() << " digest=" << digest.to_hex() << "\n";
    if (!registry.value().close().ok()) {
      return fail("close after reopen");
    }
  }

  std::filesystem::remove_all(root, error);
  return 0;
}
