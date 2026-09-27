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

// Benchmarks for the Facility Dependency Registry.
//
// Every measurement here is of a completed operation: a lookup that returned
// its result, a traversal that finished, a mutation whose generation was
// published and, when the registry is durable, flushed. Nothing measures
// submission or enqueue latency, because this library has no queue.
//
// The numbers are produced by a synthetic graph built in this process from a
// fixed seed. They are measurements of this library on the machine that ran
// them, and they are labelled as such: they say nothing about any particular
// facility, disk or production workload.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "facility_dependency_registry/facility_dependency_registry.hpp"

namespace {

using namespace facility_dependency_registry;

constexpr std::uint64_t kSeed = 0x5CA1AB1Eull;
constexpr std::string_view kEvidenceClass = "SYNTHETIC";

struct Scale {
  const char* label;
  std::uint32_t edges;
};

constexpr Scale kScales[] = {{"small", 1'000}, {"medium", 3'000}, {"large", 8'000}};
constexpr Scale kQuickScales[] = {{"quick", 400}};

std::uint64_t g_rng_state = kSeed;

std::uint64_t next_random() {
  g_rng_state += 0x9E3779B97F4A7C15ull;
  std::uint64_t value = g_rng_state;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

using Clock = std::chrono::steady_clock;

double milliseconds_since(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void report(std::string_view name, std::string_view scale, std::string_view metric, double value,
            std::string_view unit, std::string_view context) {
  std::cout << "benchmark " << std::left << std::setw(22) << name << " scale=" << std::setw(7) << scale
            << " " << metric << "=" << std::fixed << std::setprecision(3) << value << " " << unit;
  if (!context.empty()) {
    std::cout << " context=\"" << context << "\"";
  }
  std::cout << " evidence=" << kEvidenceClass << "\n";
}

DependencyNodeRef node(NodeDomain domain, std::string_view id) {
  auto ref = DependencyNodeRef::create(domain, id);
  if (!ref) {
    std::cerr << "benchmark cannot build a node reference: " << ref.error().detail() << "\n";
    std::exit(1);
  }
  return std::move(ref).value();
}

ProvenanceRecord provenance_record() {
  auto record = ProvenanceRecord::create(ProvenanceSource::OperatorDeclaration, "benchmark", "benchmark", 0, "",
                                         kHardMaxIdLength, kHardMaxAnnotationLength);
  if (!record) {
    std::cerr << "benchmark cannot build provenance\n";
    std::exit(1);
  }
  return std::move(record).value();
}

DependencyEdgeSpec edge_spec(const DependencyNodeRef& from, const DependencyNodeRef& to, DependencyKind kind,
                             DependencyStrength strength) {
  DependencyEdgeSpec spec;
  spec.source = from;
  spec.target = to;
  spec.kind = kind;
  spec.strength = strength;
  spec.direction = Direction::DependsOn;
  spec.initial_lifecycle = LifecycleState::Active;
  spec.provenance = provenance_record();
  return spec;
}

/// A facility shaped graph: every asset is powered from a feed, half of them
/// are cooled from a loop, a third sit in a rack, and every fifth asset has a
/// control dependency on an earlier asset.
struct Graph {
  DependencyRegistry registry;
  RegistryLimits limits;
  std::uint32_t feeds = 0;
  std::uint32_t loops = 0;
  std::uint32_t racks = 0;
  std::uint32_t target_edges = 0;
  std::uint32_t accepted = 0;
  double build_ms = 0.0;
};

Graph build_graph(std::uint32_t target_edges) {
  RegistryLimits limits;
  limits.max_edges = 1'000'000;
  limits.max_declared_refs = 1'000'000;
  limits.max_traversal_nodes = 1'000'000;
  limits.max_analysis_nodes = 1'000'000;
  limits.max_cycle_length = 32;

  EphemeralOptions options;
  options.limits = limits;
  auto registry = DependencyRegistry::open_ephemeral(options);
  if (!registry) {
    std::cerr << "benchmark cannot open a registry: " << registry.error().detail() << "\n";
    std::exit(1);
  }

  Graph graph;
  graph.registry = std::move(registry).value();
  graph.limits = limits;
  graph.target_edges = target_edges;
  graph.feeds = std::max<std::uint32_t>(1, target_edges / 32);
  graph.loops = std::max<std::uint32_t>(1, target_edges / 64);
  graph.racks = std::max<std::uint32_t>(1, target_edges / 24);

  const auto start = Clock::now();
  std::uint32_t index = 0;
  while (graph.accepted < target_edges) {
    const std::string asset_id = "asset-" + std::to_string(index);
    const auto asset = node(NodeDomain::Asset, asset_id);
    RegisterEdgeRequest request;
    request.context.expected_generation = graph.registry.generation();

    switch (index % 5) {
      case 0:
      case 1:
      case 2:
        request.spec = edge_spec(asset, node(NodeDomain::ElectricalDomain,
                                             "feed-" + std::to_string(index % graph.feeds)),
                                 DependencyKind::RequiresPowerFrom, DependencyStrength::Hard);
        break;
      case 3:
        // CooledBy and HousedIn targets repeat, so these attempts collide with
        // earlier ones; the collision is a duplicate and is counted, not
        // committed, which keeps the benchmark honest about what it measured.
        request.spec = edge_spec(asset,
                                 node(index % 2 == 0 ? NodeDomain::CoolingDomain : NodeDomain::Rack,
                                      index % 2 == 0 ? "loop-" + std::to_string(index % graph.loops)
                                                     : "rack-" + std::to_string(index % graph.racks)),
                                 index % 2 == 0 ? DependencyKind::CooledBy : DependencyKind::HousedIn,
                                 DependencyStrength::Soft);
        break;
      default:
        if (index < 10) {
          ++index;
          continue;
        }
        request.spec = edge_spec(asset, node(NodeDomain::Asset, "asset-" + std::to_string(index - 10)),
                                 DependencyKind::ControlDependsOn, DependencyStrength::Soft);
        break;
    }
    const auto outcome = graph.registry.register_edge(request);
    if (outcome) {
      ++graph.accepted;
    } else if (outcome.error().code() != ErrorCode::DuplicateEdge) {
      std::cerr << "benchmark could not build the graph: " << outcome.error().to_string() << "\n";
      std::exit(1);
    }
    ++index;
  }
  graph.build_ms = milliseconds_since(start);
  return graph;
}

void bench_direct_lookup(const Graph& graph, const Scale& scale) {
  const RegistrySnapshot snapshot = graph.registry.snapshot();
  const std::uint32_t iterations = 200'000;
  std::size_t found = 0;
  const auto start = Clock::now();
  for (std::uint32_t index = 0; index < iterations; ++index) {
    const auto ref = node(NodeDomain::ElectricalDomain, "feed-" + std::to_string(index % graph.feeds));
    const auto result = snapshot.direct_dependents(ref);
    if (!result) {
      std::cerr << "direct lookup failed: " << result.error().to_string() << "\n";
      std::exit(1);
    }
    found += result.value().size();
  }
  const double elapsed = milliseconds_since(start);
  report("direct_lookup", scale.label, "ops_per_second", static_cast<double>(iterations) / (elapsed / 1000.0),
         "ops/s", "completed direct dependents lookups");
  report("direct_lookup", scale.label, "records_per_second", static_cast<double>(found) / (elapsed / 1000.0),
         "records/s", "edges returned across those lookups");
}

void bench_impact_traversal(const Graph& graph, const Scale& scale) {
  const RegistrySnapshot snapshot = graph.registry.snapshot();
  ImpactConeRequest request;
  request.origin = node(NodeDomain::ElectricalDomain, "feed-0");
  request.strengths = DependencyStrengthMask::all();
  request.max_depth = 8;
  request.max_nodes = 100'000;

  const std::uint32_t iterations = 2'000;
  std::size_t visited = 0;
  const auto start = Clock::now();
  for (std::uint32_t index = 0; index < iterations; ++index) {
    const auto cone = snapshot.impact_cone(request);
    if (!cone) {
      std::cerr << "impact cone failed: " << cone.error().to_string() << "\n";
      std::exit(1);
    }
    visited += cone.value().size();
  }
  const double elapsed = milliseconds_since(start);
  report("impact_traversal", scale.label, "ops_per_second", static_cast<double>(iterations) / (elapsed / 1000.0),
         "ops/s", "completed bounded impact traversals");
  report("impact_traversal", scale.label, "nodes_per_second", static_cast<double>(visited) / (elapsed / 1000.0),
         "nodes/s", "impacted nodes reported");
}

void bench_spec_validation(const Scale& scale) {
  const auto spec = edge_spec(node(NodeDomain::Asset, "asset-1"),
                              node(NodeDomain::ElectricalDomain, "feed-1"), DependencyKind::RequiresPowerFrom,
                              DependencyStrength::Hard);
  const RegistryLimits limits;
  const std::uint32_t iterations = 500'000;
  const auto start = Clock::now();
  for (std::uint32_t index = 0; index < iterations; ++index) {
    if (!spec.validate(limits).ok()) {
      std::cerr << "validation rejected a well formed specification\n";
      std::exit(1);
    }
  }
  const double elapsed = milliseconds_since(start);
  report("declaration_validation", scale.label, "ops_per_second",
         static_cast<double>(iterations) / (elapsed / 1000.0), "ops/s",
         "completed validations of one declaration");
}

void bench_duplicate_rejection(Graph& graph, const Scale& scale) {
  // Registering a key that already exists exercises the full declaration
  // validation plus the duplicate check, and commits nothing.
  DependencyRegistry& registry = graph.registry;
  const DependencyEdgeRecord& existing = registry.snapshot().edges().front();
  DependencyEdgeSpec spec = edge_spec(existing.source(), existing.target(), existing.kind(), existing.strength());
  spec.provenance = existing.provenance();
  const std::uint32_t iterations = 20'000;
  std::size_t rejected = 0;
  const auto start = Clock::now();
  for (std::uint32_t index = 0; index < iterations; ++index) {
    RegisterEdgeRequest request;
    request.context.expected_generation = registry.generation();
    request.spec = spec;
    const auto outcome = registry.register_edge(request);
    if (outcome) {
      ++rejected;  // an idempotent no-op, which is also a completed operation
    } else if (outcome.error().code() == ErrorCode::DuplicateEdge) {
      ++rejected;
    } else {
      std::cerr << "unexpected rejection: " << outcome.error().to_string() << "\n";
      std::exit(1);
    }
  }
  const double elapsed = milliseconds_since(start);
  report("duplicate_rejection", scale.label, "ops_per_second", static_cast<double>(iterations) / (elapsed / 1000.0),
         "ops/s", "completed duplicate checks that committed nothing");
}

void bench_scc(const Graph& graph, const Scale& scale) {
  const RegistrySnapshot snapshot = graph.registry.snapshot();
  ComponentRequest request;
  request.include_singletons = false;
  request.max_nodes = graph.limits.max_analysis_nodes;

  const std::uint32_t iterations = 20;
  std::size_t components = 0;
  const auto start = Clock::now();
  for (std::uint32_t index = 0; index < iterations; ++index) {
    const auto result = snapshot.strongly_connected_components(request);
    if (!result) {
      std::cerr << "component analysis failed: " << result.error().to_string() << "\n";
      std::exit(1);
    }
    components += result.value().components.size();
  }
  const double elapsed = milliseconds_since(start);
  report("scc_analysis", scale.label, "ops_per_second", static_cast<double>(iterations) / (elapsed / 1000.0),
         "ops/s", "completed whole graph decompositions");
  report("scc_analysis", scale.label, "nodes_per_second",
         static_cast<double>(snapshot.node_count()) * iterations / (elapsed / 1000.0), "nodes/s",
         "graph nodes examined");
  report("scc_analysis", scale.label, "cyclic_components_found", static_cast<double>(components / iterations),
         "components", "cyclic components reported per run");
}

void bench_cycle_analysis(const Graph& graph, const Scale& scale) {
  const RegistrySnapshot snapshot = graph.registry.snapshot();
  const std::uint32_t iterations = 20;
  const auto start = Clock::now();
  for (std::uint32_t index = 0; index < iterations; ++index) {
    const auto obligation = snapshot.verify_acyclic_obligation();
    if (!obligation || !obligation.value().satisfied) {
      std::cerr << "the acyclic obligation was not satisfied by the benchmark graph\n";
      std::exit(1);
    }
  }
  const double elapsed = milliseconds_since(start);
  report("acyclic_obligation", scale.label, "ops_per_second", static_cast<double>(iterations) / (elapsed / 1000.0),
         "ops/s", "completed whole graph obligation checks");
}

void bench_generation_diff(Graph& graph, const Scale& scale) {
  // Two snapshots one mutation apart, diffed repeatedly. The snapshot pair is
  // taken once so the measurement is of the diff, not of the mutation.
  DependencyRegistry& registry = graph.registry;
  RegistrySnapshot before = registry.snapshot();
  RegisterEdgeRequest request;
  request.context.expected_generation = registry.generation();
  request.spec = edge_spec(node(NodeDomain::Asset, "diff-probe-1"), node(NodeDomain::ElectricalDomain, "feed-0"),
                           DependencyKind::RequiresPowerFrom, DependencyStrength::Hard);
  if (!registry.register_edge(request).has_value()) {
    std::cerr << "the diff probe edge was rejected\n";
    std::exit(1);
  }
  const RegistrySnapshot after = registry.snapshot();

  const std::uint32_t iterations = 200;
  const auto start = Clock::now();
  for (std::uint32_t index = 0; index < iterations; ++index) {
    const auto diff = diff_snapshots(before, after, graph.limits.max_diff_changes);
    if (!diff) {
      std::cerr << "diff failed: " << diff.error().to_string() << "\n";
      std::exit(1);
    }
  }
  const double elapsed = milliseconds_since(start);
  report("generation_diff", scale.label, "ops_per_second", static_cast<double>(iterations) / (elapsed / 1000.0),
         "ops/s", "completed merge joins over both generations");
}

void bench_canonical_encoding(const Graph& graph, const Scale& scale) {
  const RegistrySnapshot snapshot = graph.registry.snapshot();
  const std::uint32_t iterations = 50;
  std::size_t bytes = 0;
  const auto start = Clock::now();
  for (std::uint32_t index = 0; index < iterations; ++index) {
    const auto encoded = snapshot.encode();
    if (!encoded) {
      std::cerr << "encoding failed\n";
      std::exit(1);
    }
    bytes = encoded.value().size();
  }
  const double elapsed = milliseconds_since(start);
  report("canonical_encoding", scale.label, "ops_per_second", static_cast<double>(iterations) / (elapsed / 1000.0),
         "ops/s", "completed encodings of the whole state");
  report("canonical_encoding", scale.label, "mebibytes_per_second",
         static_cast<double>(bytes) * iterations / (1024.0 * 1024.0) / (elapsed / 1000.0), "MiB/s",
         "payload bytes written");
}

void bench_durable_mutation(const Scale& scale, std::uint32_t mutations) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("fdep-benchmark-" + std::string{scale.label});
  std::error_code error;
  std::filesystem::remove_all(root, error);

  RegistryLimits limits;
  limits.max_edges = 1'000'000;
  limits.max_retained_generations = 2;

  RegistryOpenRequest request;
  request.root = root;
  request.store.limits = limits;
  auto registry = DependencyRegistry::open(request);
  if (!registry) {
    std::cerr << "benchmark cannot open a durable store: " << registry.error().detail() << "\n";
    std::exit(1);
  }

  // One committed durable mutation per measured operation, including the
  // generation rewrite, the file flush and the pointer replacement.
  double total_ms = 0.0;
  std::uint64_t bytes = 0;
  for (std::uint32_t index = 0; index < mutations; ++index) {
    RegisterEdgeRequest register_request;
    register_request.context.expected_generation = registry.value().generation();
    register_request.spec =
        edge_spec(node(NodeDomain::Asset, "durable-" + std::to_string(index)),
                  node(NodeDomain::ElectricalDomain, "feed-" + std::to_string(index % 8)),
                  DependencyKind::RequiresPowerFrom, DependencyStrength::Hard);
    const auto start = Clock::now();
    const auto outcome = registry.value().register_edge(register_request);
    total_ms += milliseconds_since(start);
    if (!outcome) {
      std::cerr << "durable mutation failed: " << outcome.error().to_string() << "\n";
      std::exit(1);
    }
  }
  const auto status = registry.value().store_status();
  if (status) {
    bytes = status.value().total_bytes;
  }
  const double mean_ms = total_ms / static_cast<double>(mutations);
  if (!registry.value().close().ok()) {
    std::cerr << "the durable store did not close\n";
    std::exit(1);
  }

  // Verify the durable result by reopening it: the benchmark's integrity
  // result is that the committed state came back.
  RegistryOpenRequest reopen;
  reopen.root = root;
  reopen.store.limits = limits;
  auto second = DependencyRegistry::open(reopen);
  const bool intact = second && second.value().generation().value() == mutations &&
                      second.value().snapshot().edge_count() == mutations;
  if (second) {
    static_cast<void>(second.value().close());
  }
  std::filesystem::remove_all(root, error);
  if (!intact) {
    std::cerr << "the durable benchmark could not read back what it committed\n";
    std::exit(1);
  }

  report("durable_mutation", scale.label, "mean_latency_ms", mean_ms, "ms",
         "completed durable mutations including flush and pointer replacement");
  report("durable_mutation", scale.label, "ops_per_second", 1000.0 / mean_ms, "ops/s",
         "durable mutations with fsync, single writer");
  report("durable_mutation", scale.label, "store_bytes_on_disk", static_cast<double>(bytes), "bytes",
         "after the measured mutations");
}

void print_environment() {
  std::cout << "benchmark evidence=" << kEvidenceClass
            << " note=\"synthetic graph built in this process; timings are of this machine\"\n";
  std::cout << "benchmark seed=" << kSeed << " library_version=" << version_string() << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  bool quick = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument{argv[index]};
    if (argument == "--quick") {
      quick = true;
    }
  }

  print_environment();
  const std::span<const Scale> scales = quick ? std::span<const Scale>{kQuickScales} : std::span<const Scale>{kScales};

  for (const Scale& scale : scales) {
    Graph graph = build_graph(scale.edges);
    report("graph_build", scale.label, "elapsed_ms", graph.build_ms, "ms",
           "context only: committed registrations used to reach the scale");
    report("graph_build", scale.label, "committed_edges", static_cast<double>(graph.accepted), "edges",
           "edges in the measured graph");
    report("graph_build", scale.label, "nodes", static_cast<double>(graph.registry.snapshot().node_count()), "nodes",
           "distinct nodes in the measured graph");

    bench_direct_lookup(graph, scale);
    bench_impact_traversal(graph, scale);
    bench_spec_validation(scale);
    bench_duplicate_rejection(graph, scale);
    bench_scc(graph, scale);
    bench_cycle_analysis(graph, scale);
    bench_generation_diff(graph, scale);
    bench_canonical_encoding(graph, scale);

    std::cout << "benchmark graph_integrity scale=" << scale.label << " digest="
              << graph.registry.snapshot().state_digest().to_hex() << " edges=" << graph.accepted
              << " acyclic_obligation=satisfied evidence=" << kEvidenceClass << "\n";
  }

  const Scale durable_scale = quick ? Scale{"quick-durable", 200} : Scale{"medium-durable", 400};
  bench_durable_mutation(durable_scale, durable_scale.edges);
  return 0;
}
