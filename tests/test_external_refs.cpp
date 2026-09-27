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

#include <algorithm>
#include <filesystem>
#include <string>

#include "facility_dependency_registry/facility_dependency_registry.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

using namespace facility_dependency_registry;
using fdep_test::asi;
using fdep_test::asset;
using fdep_test::dfi;
using fdep_test::feed;
using fdep_test::Harness;
using fdep_test::loop;
using fdep_test::provenance;
using fdep_test::rack;
using fdep_test::service;

namespace {

DeclareRefRequest declare_request(const Harness& harness, const DependencyNodeRef& ref,
                                  ProvenanceRecord record = provenance()) {
  DeclareRefRequest request;
  request.context.expected_generation = harness.generation();
  request.ref = ref;
  request.provenance = std::move(record);
  return request;
}

}  // namespace

FDEP_TEST(external_refs, an_edge_may_reference_an_identity_nobody_declared) {
  Harness harness = Harness::ephemeral();
  const DependencyEdgeId id = harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);

  // The registry stays explicitly undecided rather than inventing the object.
  FDEP_CHECK(harness.snapshot().resolution_of(asset("a")) == RefResolution::Unresolved);
  FDEP_CHECK(harness.snapshot().resolution_of(feed("f1")) == RefResolution::Unresolved);
  FDEP_CHECK(harness.snapshot().has_node(asset("a")));
  FDEP_CHECK_EQ(harness.snapshot().declared_ref_count(), std::size_t{0});
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{1});
  FDEP_CHECK(harness.snapshot().find_edge(id) != nullptr);

  const auto unresolved = harness.snapshot().unresolved_endpoints();
  FDEP_REQUIRE_OK(unresolved);
  FDEP_CHECK_EQ(unresolved.value().size(), std::size_t{2});
  FDEP_CHECK(unresolved.value().refs()[0] == asset("a"));
  FDEP_CHECK(unresolved.value().refs()[1] == feed("f1"));
}

FDEP_TEST(external_refs, declaring_an_identity_changes_only_its_resolution) {
  Harness harness = Harness::ephemeral();
  harness.add(asset("a"), feed("f1"), DependencyKind::RequiresPowerFrom);
  const ContentDigest before = harness.snapshot().content_digest();

  harness.declare(asset("a"), provenance("inventory-2026", "importer", 11, "seen in inventory"));
  FDEP_CHECK(harness.snapshot().resolution_of(asset("a")) == RefResolution::Declared);
  FDEP_CHECK(harness.snapshot().resolution_of(feed("f1")) == RefResolution::Unresolved);
  FDEP_CHECK(harness.snapshot().content_digest() != before);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{2});

  const auto* declaration = harness.snapshot().find_declared_ref(asset("a"));
  FDEP_REQUIRE(declaration != nullptr);
  FDEP_CHECK_EQ(declaration->declared_generation().value(), std::uint64_t{2});
  FDEP_CHECK_EQ(declaration->provenance().source_id(), std::string{"inventory-2026"});
  FDEP_CHECK(declaration->provenance().source() == ProvenanceSource::OperatorDeclaration);
  FDEP_CHECK(declaration->to_text().find("ref=asset:a") != std::string::npos);
}

FDEP_TEST(external_refs, declaration_validation_rejects_before_mutating) {
  Harness harness = Harness::ephemeral();

  DeclareRefRequest invalid_ref = declare_request(harness, DependencyNodeRef{});
  FDEP_CHECK_CODE(harness.registry().declare_external_ref(invalid_ref), ErrorCode::InvalidNodeReference);

  DeclareRefRequest invalid_provenance = declare_request(harness, asset("a"));
  invalid_provenance.provenance = ProvenanceRecord{};
  FDEP_CHECK_CODE(harness.registry().declare_external_ref(invalid_provenance),
                  ErrorCode::InvalidProvenanceSource);

  DeclareRefRequest no_source = declare_request(harness, asset("b"));
  no_source.provenance = ProvenanceRecord{};
  FDEP_CHECK_CODE(harness.registry().declare_external_ref(no_source), ErrorCode::InvalidProvenanceSource);

  // A negative timestamp and an annotation with a control character cannot even
  // be constructed, which is the point: the value type refuses them before a
  // request can exist.
  const auto impossible = ProvenanceRecord::create(ProvenanceSource::OperatorDeclaration, "src", "prin", -5, "", 160,
                                                   512);
  FDEP_CHECK_CODE(impossible, ErrorCode::InvalidTimestamp);
  const auto bad_annotation = ProvenanceRecord::create(ProvenanceSource::OperatorDeclaration, "src", "prin", 0,
                                                       "line\nbreak", 160, 512);
  FDEP_CHECK_CODE(bad_annotation, ErrorCode::InvalidAnnotation);

  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{0});
  FDEP_CHECK_EQ(harness.snapshot().declared_ref_count(), std::size_t{0});
}

FDEP_TEST(external_refs, declarations_are_canonically_ordered_and_bounded) {
  RegistryLimits limits;
  limits.max_declared_refs = 64;
  Harness harness = Harness::ephemeral(limits);
  harness.declare(rack("r1"));
  harness.declare(asset("z"));
  harness.declare(asset("a"));
  harness.declare(feed("f1"));

  const auto refs = harness.snapshot().declared_refs();
  FDEP_REQUIRE(refs.size() == 4);
  // Canonical order is by domain ordinal first: asset (1), then rack (2), then
  // electrical domain (3).
  FDEP_CHECK(refs[0].ref() == asset("a"));
  FDEP_CHECK(refs[1].ref() == asset("z"));
  FDEP_CHECK(refs[2].ref() == rack("r1"));
  FDEP_CHECK(refs[3].ref() == feed("f1"));
  for (std::size_t index = 1; index < refs.size(); ++index) {
    FDEP_CHECK(refs[index - 1].ref() < refs[index].ref());
  }

  // Identical declarations are idempotent and do not reorder anything.
  DeclareRefRequest repeat = declare_request(harness, asset("a"));
  const auto outcome = harness.registry().declare_external_ref(repeat);
  FDEP_REQUIRE_OK(outcome);
  FDEP_CHECK(outcome.value().already_declared);
  FDEP_CHECK_EQ(harness.snapshot().declared_ref_count(), std::size_t{4});
  FDEP_CHECK(harness.snapshot().declared_refs()[0].ref() == asset("a"));
}

FDEP_TEST(external_refs, withdrawal_is_explicit_about_the_edges_it_leaves_behind) {
  Harness harness = Harness::ephemeral();
  harness.declare(asset("leaf"));
  harness.declare(feed("root"));
  harness.add(asset("leaf"), feed("root"), DependencyKind::RequiresPowerFrom);

  WithdrawRefRequest blocked;
  blocked.context.expected_generation = harness.generation();
  blocked.ref = asset("leaf");
  FDEP_CHECK_CODE(harness.registry().withdraw_external_ref(blocked), ErrorCode::DeclaredRefReferenced);
  FDEP_CHECK_EQ(harness.snapshot().declared_ref_count(), std::size_t{2});

  WithdrawRefRequest allowed = blocked;
  allowed.allow_referenced = true;
  const auto outcome = harness.registry().withdraw_external_ref(allowed);
  FDEP_REQUIRE_OK(outcome);
  FDEP_CHECK(!outcome.value().already_absent);
  FDEP_CHECK_EQ(harness.snapshot().declared_ref_count(), std::size_t{1});
  FDEP_CHECK(harness.snapshot().resolution_of(asset("leaf")) == RefResolution::Unresolved);
  FDEP_CHECK_EQ(harness.snapshot().edge_count(), std::size_t{1});

  // Withdrawing an undeclared reference is an idempotent no-op.
  WithdrawRefRequest missing;
  missing.context.expected_generation = harness.generation();
  missing.ref = loop("never-declared");
  const auto absent = harness.registry().withdraw_external_ref(missing);
  FDEP_REQUIRE_OK(absent);
  FDEP_CHECK(absent.value().already_absent);
  FDEP_CHECK(!absent.value().generation_advanced);
  FDEP_CHECK_EQ(harness.generation().value(), std::uint64_t{4});
}

FDEP_TEST(external_refs, unreferenced_declarations_are_reported) {
  Harness harness = Harness::ephemeral();
  harness.declare(asset("used"));
  harness.declare(asset("unused"));
  harness.declare(feed("unused-feed"));
  harness.add(asset("used"), feed("target"), DependencyKind::RequiresPowerFrom);

  const auto unreferenced = harness.snapshot().unreferenced_declared_refs();
  FDEP_REQUIRE_OK(unreferenced);
  FDEP_CHECK_EQ(unreferenced.value().size(), std::size_t{2});
  FDEP_CHECK(unreferenced.value().refs()[0] == asset("unused"));
  FDEP_CHECK(unreferenced.value().refs()[1] == feed("unused-feed"));

  const auto unresolved = harness.snapshot().unresolved_endpoints();
  FDEP_REQUIRE_OK(unresolved);
  FDEP_CHECK_EQ(unresolved.value().size(), std::size_t{1});
  FDEP_CHECK(unresolved.value().refs()[0] == feed("target"));
}

FDEP_TEST(external_refs, declarations_survive_a_store_round_trip) {
  const std::filesystem::path root = fdep_test::make_temp_directory("external-refs-store");
  ContentDigest digest;
  {
    Harness harness = Harness::durable(root);
    harness.declare(asset("declared"), provenance("inventory-2026", "importer", 12, "seen"));
    harness.declare(asi("composed"));
    harness.add(asi("composed"), rack("r9"), DependencyKind::HousedIn);
    digest = harness.snapshot().state_digest();
    FDEP_REQUIRE(harness.registry().close().ok());
  }
  {
    Harness harness = Harness::durable(root);
    FDEP_CHECK_EQ(harness.snapshot().state_digest(), digest);
    FDEP_CHECK_EQ(harness.snapshot().declared_ref_count(), std::size_t{2});
    FDEP_CHECK(harness.snapshot().resolution_of(asset("declared")) == RefResolution::Declared);
    FDEP_CHECK(harness.snapshot().resolution_of(asi("composed")) == RefResolution::Declared);
    FDEP_CHECK(harness.snapshot().resolution_of(rack("r9")) == RefResolution::Unresolved);
    const auto* declaration = harness.snapshot().find_declared_ref(asset("declared"));
    FDEP_REQUIRE(declaration != nullptr);
    FDEP_CHECK_EQ(declaration->provenance().source_id(), std::string{"inventory-2026"});
    FDEP_CHECK_EQ(declaration->provenance().recorded_at_unix_ms(), std::int64_t{12});
    FDEP_CHECK_EQ(declaration->declared_generation().value(), std::uint64_t{1});
    FDEP_REQUIRE(harness.registry().close().ok());
  }
  fdep_test::remove_tree(root);
}

FDEP_TEST(external_refs, composed_domain_references_are_opaque) {
  // This registry never inspects an ASI or DFI domain: it records the identity,
  // the kind of relation and the declared semantics, and nothing else.
  Harness harness = Harness::ephemeral();
  harness.declare(asi("inference-cluster-eu1"));
  harness.declare(dfi("fabric-pod-3"));
  harness.add(asi("inference-cluster-eu1"), dfi("fabric-pod-3"), DependencyKind::ComposedDomainDependsOn,
              DependencyStrength::Soft);
  harness.add(asi("inference-cluster-eu1"), rack("r1"), DependencyKind::HousedIn);

  const auto dependencies = harness.snapshot().direct_dependencies(asi("inference-cluster-eu1"));
  FDEP_REQUIRE_OK(dependencies);
  FDEP_REQUIRE(dependencies.value().size() == 2);
  FDEP_CHECK(dependencies.value()[0].target() == rack("r1"));
  FDEP_CHECK(dependencies.value()[1].target() == dfi("fabric-pod-3"));
  FDEP_CHECK(dependencies.value()[1].kind() == DependencyKind::ComposedDomainDependsOn);
  FDEP_CHECK_EQ(dependencies.value()[1].strength(), DependencyStrength::Soft);

  // A composed domain reference carries no payload beyond its identity.
  FDEP_CHECK_EQ(asi("inference-cluster-eu1").to_canonical(), std::string{"asi-domain:inference-cluster-eu1"});
  FDEP_CHECK_EQ(asi("inference-cluster-eu1").id(), std::string{"inference-cluster-eu1"});
  FDEP_CHECK(asi("inference-cluster-eu1").domain() == NodeDomain::AsiDomain);
}

FDEP_TEST(external_refs, service_and_rack_references_follow_the_declared_domains) {
  Harness harness = Harness::ephemeral();
  harness.declare(service("chilled-water"));
  harness.declare(rack("row-a-rack-07"));
  harness.declare(loop("loop-2"));

  harness.add(rack("row-a-rack-07"), service("chilled-water"), DependencyKind::ServedBy);
  harness.add(rack("row-a-rack-07"), loop("loop-2"), DependencyKind::CooledBy);
  harness.add(asset("node-1"), rack("row-a-rack-07"), DependencyKind::HousedIn);

  const auto dependencies = harness.snapshot().direct_dependencies(rack("row-a-rack-07"));
  FDEP_REQUIRE_OK(dependencies);
  FDEP_REQUIRE(dependencies.value().size() == 2);
  FDEP_CHECK(dependencies.value()[0].kind() == DependencyKind::CooledBy);
  FDEP_CHECK(dependencies.value()[1].kind() == DependencyKind::ServedBy);

  const auto dependents = harness.snapshot().direct_dependents(rack("row-a-rack-07"));
  FDEP_REQUIRE_OK(dependents);
  FDEP_REQUIRE(dependents.value().size() == 1);
  FDEP_CHECK(dependents.value()[0].source() == asset("node-1"));
  FDEP_CHECK(dependents.value()[0].kind() == DependencyKind::HousedIn);
}
