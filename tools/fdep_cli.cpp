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

// The inspection and operation tool for a Facility Dependency Registry store.
//
// Every command goes through the same public API a consumer uses. Read-only
// commands open the store read-only, take no writer lock and never write,
// repair or prune anything. Mutation commands are addressed to an expected
// generation that the caller must supply: there is no flag that skips the
// authority check, because the check is the point.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "facility_dependency_registry/facility_dependency_registry.hpp"

namespace {

using namespace facility_dependency_registry;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitRejected = 2;
constexpr int kExitIo = 3;

struct Options {
  std::map<std::string, std::vector<std::string>> values;
  std::vector<std::string> positional;

  [[nodiscard]] bool has(std::string_view name) const { return values.find(std::string{name}) != values.end(); }

  [[nodiscard]] std::optional<std::string> one(std::string_view name) const {
    const auto position = values.find(std::string{name});
    if (position == values.end() || position->second.empty()) {
      return std::nullopt;
    }
    return position->second.back();
  }

  [[nodiscard]] std::vector<std::string> all(std::string_view name) const {
    const auto position = values.find(std::string{name});
    if (position == values.end()) {
      return {};
    }
    return position->second;
  }
};

bool g_json = false;
bool g_quiet = false;

void say(const std::string& line) {
  if (!g_quiet) {
    std::cout << line << "\n";
  }
}

void json_string(std::ostream& stream, std::string_view text) { stream << '"' << json_escape(text) << '"'; }

/// Reports a registry rejection. The stable token is printed first so that a
/// script can match on it without parsing prose.
int report_rejection(std::string_view what, const RegistryError& error) {
  std::cout << "rejected " << what << " " << to_token(error.code()) << ": " << error.detail() << "\n";
  return kExitRejected;
}

int report_io_failure(std::string_view what, const RegistryError& error) {
  std::cout << "failed " << what << " " << to_token(error.code()) << ": " << error.detail() << "\n";
  return kExitIo;
}

int usage(std::string_view message) {
  std::cerr << "fdep: " << message << "\n";
  std::cerr << R"(usage: fdep <command> [arguments] [options]

read-only commands (they take no writer lock and write nothing):
  inspect      <root>
  verify       <root>
  edges        <root> [--kind K] [--strength S] [--lifecycle L] [--include-inactive]
  deps         <root> <ref> [--depth N] [--max-nodes N] [--include-inactive]
  dependents   <root> <ref> [--depth N] [--max-nodes N] [--include-inactive]
  impact       <root> <ref> [--max-depth N] [--max-nodes N] [--all-strengths]
  path         <root> <from> <to> [--reverse] [--depth N]
  scc          <root> [--singletons]
  cycles       <root> [--max-length N] [--max-cycles N]
  unresolved   <root>
  export       <root>
  diff         <root> --from-generation N [--to-generation M]

mutation commands (all of them require an explicit expected generation):
  register     <root> --from <ref> --to <ref> --kind K --expect-generation N
                      [--strength S] [--direction D] [--lifecycle L] [--constraint KIND=VALUE]...
                      [--source-id ID] [--principal ID] [--annotation TEXT] [--recorded-at-ms N]
  update       <root> --edge N --expect-generation N --expect-revision R --strength S
                      [--direction D] [--constraint KIND=VALUE]... [--source-id ID] [--principal ID]
  transition   <root> --edge N --expect-generation N --expect-revision R --to STATE [--reason TEXT]
  remove       <root> --edge N --expect-generation N --expect-revision R [--reason TEXT]
  declare-ref  <root> --ref <ref> --expect-generation N [--source-id ID] [--principal ID]
  withdraw-ref <root> --ref <ref> --expect-generation N [--allow-referenced]

self validation (used by the test suite; both clean up after themselves):
  <root> --self-check
  <root> --scenario

global options:
  --json       machine readable output where the command produces records
  --quiet      suppress informational output

exit codes: 0 accepted, 1 usage error, 2 rejected by the registry, 3 store failure
)";
  return kExitUsage;
}

Options parse_options(const std::vector<std::string>& arguments, std::size_t first) {
  Options options;
  for (std::size_t index = first; index < arguments.size(); ++index) {
    const std::string& token = arguments[index];
    if (token.rfind("--", 0) != 0) {
      options.positional.push_back(token);
      continue;
    }
    std::string name = token.substr(2);
    std::optional<std::string> value;
    const std::size_t equals = name.find('=');
    if (equals != std::string::npos) {
      value = name.substr(equals + 1);
      name = name.substr(0, equals);
    } else if (index + 1 < arguments.size() && arguments[index + 1].rfind("--", 0) != 0) {
      value = arguments[index + 1];
      ++index;
    }
    if (value.has_value()) {
      options.values[name].push_back(*value);
    } else {
      options.values[name];
    }
  }
  return options;
}

std::optional<DependencyNodeRef> parse_ref(std::string_view text) {
  const auto ref = DependencyNodeRef::parse(text);
  if (!ref) {
    return std::nullopt;
  }
  return ref.value();
}

template <class Enum, class Parse>
std::optional<Enum> parse_enum(std::string_view name, const Options& options, Parse parse) {
  const auto text = options.one(name);
  if (!text.has_value()) {
    return std::nullopt;
  }
  const auto value = parse(*text);
  if (!value.has_value()) {
    return std::nullopt;
  }
  return *value;
}

/// Builds the edge filter the command line asked for.
///
/// An unrecognised kind, strength or lifecycle is a usage error. It is never
/// dropped: silently widening or narrowing a query because a token was
/// misspelled would answer a different question than the one that was asked.
std::optional<EdgeFilter> build_filter(const Options& options, std::string& error) {
  EdgeFilter filter;
  if (options.has("include-inactive")) {
    filter.lifecycles = LifecycleMask::all();
  } else if (options.has("lifecycle")) {
    const auto lifecycle = parse_lifecycle_state(options.one("lifecycle").value_or(""));
    if (!lifecycle.has_value()) {
      error = "--lifecycle is not one of the declared states";
      return std::nullopt;
    }
    filter.lifecycles = LifecycleMask::of(*lifecycle);
  }
  DependencyKindMask kinds = DependencyKindMask::none();
  for (const auto& text : options.all("kind")) {
    const auto kind = parse_dependency_kind(text);
    if (!kind.has_value()) {
      error = "unknown dependency kind '" + text + "'";
      return std::nullopt;
    }
    kinds = kinds.with(*kind);
  }
  if (!kinds.empty()) {
    filter.kinds = kinds;
  }
  DependencyStrengthMask strengths = DependencyStrengthMask::none();
  for (const auto& text : options.all("strength")) {
    const auto strength = parse_dependency_strength(text);
    if (!strength.has_value()) {
      error = "unknown dependency strength '" + text + "'";
      return std::nullopt;
    }
    strengths = strengths.with(*strength);
  }
  if (!strengths.empty()) {
    filter.strengths = strengths;
  }
  return filter;
}

/// Numeric option parsing that never throws. `std::stoul` and friends raise on
/// malformed input, and a command line is untrusted input, so every numeric
/// argument goes through here and a malformed one becomes a usage error.
bool parse_u32(std::string_view text, std::uint32_t& out) noexcept {
  if (text.empty()) {
    return false;
  }
  std::uint32_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return false;
  }
  out = value;
  return true;
}

bool parse_i64(std::string_view text, std::int64_t& out) noexcept {
  if (text.empty()) {
    return false;
  }
  std::int64_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return false;
  }
  out = value;
  return true;
}

/// Reads one numeric option, or reports a usage error naming it.
bool read_u32_option(const Options& options, std::string_view name, std::uint32_t fallback, std::uint32_t& out,
                     std::string& error) {
  const auto text = options.one(name);
  if (!text.has_value()) {
    out = fallback;
    return true;
  }
  if (!parse_u32(*text, out)) {
    error.assign("--");
    error.append(name);
    error.append(" is not a number");
    return false;
  }
  return true;
}

/// Opens the store read-only. Every read-only command uses this, so the tool
/// can inspect a store that another process is writing.
struct ReadOnlySession {
  std::unique_ptr<DurableStore> store;
  RegistrySnapshot snapshot;
  RecoveryReport report;
  RegistryLimits limits{};
};

std::optional<ReadOnlySession> open_read_only(const std::string& root, int& exit_code) {
  StoreOptions options;
  options.mode = OpenMode::ReadOnly;
  ReadOnlySession session;
  session.limits = options.limits;
  // The durable store is opened directly rather than through a registry, so
  // that no mutation surface is even constructed for a read-only command.
  auto handle = DurableStore::open(std::filesystem::path{root}, options, session.snapshot, session.report);
  if (!handle) {
    exit_code = report_io_failure("to open the store", handle.error());
    return std::nullopt;
  }
  session.store = std::move(handle).value();
  return session;
}

std::string render_edge(const DependencyEdgeRecord& record) { return record.to_text(); }

void print_edges(const std::vector<DependencyEdgeRecord>& edges) {
  if (g_json) {
    std::cout << "[";
    for (std::size_t index = 0; index < edges.size(); ++index) {
      if (index > 0) {
        std::cout << ",";
      }
      std::cout << "\n  {\"id\": " << edges[index].id().value() << ", \"revision\": "
                << edges[index].revision().value() << ", \"kind\": ";
      json_string(std::cout, to_token(edges[index].kind()));
      std::cout << ", \"strength\": ";
      json_string(std::cout, to_token(edges[index].strength()));
      std::cout << ", \"direction\": ";
      json_string(std::cout, to_token(edges[index].direction()));
      std::cout << ", \"lifecycle\": ";
      json_string(std::cout, to_token(edges[index].lifecycle()));
      std::cout << ", \"source\": ";
      json_string(std::cout, edges[index].source().to_canonical());
      std::cout << ", \"target\": ";
      json_string(std::cout, edges[index].target().to_canonical());
      std::cout << "}";
    }
    std::cout << (edges.empty() ? "]\n" : "\n]\n");
    return;
  }
  for (const auto& record : edges) {
    std::cout << render_edge(record) << "\n";
  }
}

/// Reports the outcome of a mutation in one line, or as one JSON object.
void print_mutation(std::string_view command, DependencyEdgeId id, EdgeRevision revision,
                    DependencyGeneration generation, bool advanced, std::string_view note) {
  if (g_json) {
    std::cout << "{\"command\": ";
    json_string(std::cout, command);
    if (is_assigned(id)) {
      std::cout << ", \"edge\": " << id.value() << ", \"revision\": " << revision.value();
    }
    std::cout << ", \"generation\": " << generation.value() << ", \"generation_advanced\": "
              << (advanced ? "true" : "false") << ", \"outcome\": ";
    json_string(std::cout, note);
    std::cout << "}\n";
    return;
  }
  std::cout << command << " accepted";
  if (is_assigned(id)) {
    std::cout << " edge=" << to_text(id) << " revision=" << to_text(revision);
  }
  std::cout << " generation=" << to_text(generation) << (advanced ? "" : " (no change)") << " " << note << "\n";
}

// -- self validation ---------------------------------------------------------

int run_self_check(const std::filesystem::path& root) {
  std::error_code error;
  std::filesystem::remove_all(root, error);
  RegistryOpenRequest request;
  request.root = root;
  auto registry = DependencyRegistry::open(request);
  if (!registry) {
    return report_io_failure("to open the store", registry.error());
  }

  const auto make_spec = [](const DependencyNodeRef& from, const DependencyNodeRef& to, DependencyKind kind) {
    DependencyEdgeSpec spec;
    spec.source = from;
    spec.target = to;
    spec.kind = kind;
    spec.strength = DependencyStrength::Hard;
    spec.direction = Direction::DependsOn;
    spec.initial_lifecycle = LifecycleState::Active;
    spec.provenance =
        ProvenanceRecord::create(ProvenanceSource::OperatorDeclaration, "self-check", "fdep", 0, "",
                                 kHardMaxIdLength, kHardMaxAnnotationLength)
            .value();
    return spec;
  };
  const auto node = [](NodeDomain domain, std::string_view id) {
    return DependencyNodeRef::create(domain, id).value();
  };

  RegisterEdgeRequest first;
  first.context.expected_generation = registry.value().generation();
  first.spec = make_spec(node(NodeDomain::Asset, "self-check-node"), node(NodeDomain::ElectricalDomain, "feed-a"),
                         DependencyKind::RequiresPowerFrom);
  const auto registered = registry.value().register_edge(first);
  if (!registered) {
    return report_rejection("register", registered.error());
  }
  const DependencyEdgeId id = registered.value().id;

  // A stale mutation must be refused.
  RegisterEdgeRequest stale = first;
  stale.spec = make_spec(node(NodeDomain::Asset, "other"), node(NodeDomain::ElectricalDomain, "feed-a"),
                         DependencyKind::RequiresPowerFrom);
  const auto refused = registry.value().register_edge(stale);
  if (refused || refused.error().code() != ErrorCode::StaleGeneration) {
    std::cout << "failed: a stale generation was not refused\n";
    return kExitRejected;
  }

  // A prohibited cycle must be refused.
  RegisterEdgeRequest cycle;
  cycle.context.expected_generation = registry.value().generation();
  cycle.spec = make_spec(node(NodeDomain::ElectricalDomain, "feed-a"), node(NodeDomain::ElectricalDomain, "feed-b"),
                         DependencyKind::RequiresPowerFrom);
  const auto cycle_first = registry.value().register_edge(cycle);
  if (!cycle_first) {
    return report_rejection("register", cycle_first.error());
  }
  RegisterEdgeRequest closing;
  closing.context.expected_generation = registry.value().generation();
  closing.spec = make_spec(node(NodeDomain::ElectricalDomain, "feed-b"),
                           node(NodeDomain::ElectricalDomain, "feed-a"), DependencyKind::RequiresPowerFrom);
  const auto prohibited = registry.value().register_edge(closing);
  if (prohibited || prohibited.error().code() != ErrorCode::ProhibitedCycle) {
    std::cout << "failed: a prohibited cycle was not refused\n";
    return kExitRejected;
  }

  const DependencyGeneration generation = registry.value().generation();
  const ContentDigest digest = registry.value().snapshot().state_digest();
  if (!registry.value().close().ok()) {
    std::cout << "failed: the store did not close\n";
    return kExitIo;
  }

  RegistryOpenRequest reopen;
  reopen.root = root;
  auto second = DependencyRegistry::open(reopen);
  if (!second) {
    return report_io_failure("to reopen the store", second.error());
  }
  const bool intact = second.value().generation() == generation &&
                      second.value().snapshot().state_digest() == digest &&
                      second.value().snapshot().edge_count() == 2 &&
                      second.value().recovery_report().outcome == RecoveryOutcome::LoadedCurrent;
  if (!second.value().close().ok()) {
    std::cout << "failed: the reopened store did not close\n";
    return kExitIo;
  }
  std::filesystem::remove_all(root, error);
  if (!intact) {
    std::cout << "failed: the reopened store did not match the state that was committed\n";
    return kExitRejected;
  }
  say("self-check ok generation=" + to_text(generation) + " digest=" + digest.to_hex());
  return kExitOk;
}

int run_scenario(const std::filesystem::path& root) {
  std::error_code error;
  std::filesystem::remove_all(root, error);
  RegistryOpenRequest request;
  request.root = root;
  auto registry = DependencyRegistry::open(request);
  if (!registry) {
    return report_io_failure("to open the store", registry.error());
  }
  DependencyRegistry& live = registry.value();

  const auto node = [](NodeDomain domain, std::string_view id) {
    return DependencyNodeRef::create(domain, id).value();
  };
  const auto provenance_record = [](std::string_view id) {
    return ProvenanceRecord::create(ProvenanceSource::OperatorDeclaration, id, "fdep-scenario", 0, "",
                                    kHardMaxIdLength, kHardMaxAnnotationLength)
        .value();
  };
  const auto make_spec = [&](const DependencyNodeRef& from, const DependencyNodeRef& to, DependencyKind kind,
                             DependencyStrength strength) {
    DependencyEdgeSpec spec;
    spec.source = from;
    spec.target = to;
    spec.kind = kind;
    spec.strength = strength;
    spec.direction = Direction::DependsOn;
    spec.initial_lifecycle = LifecycleState::Active;
    spec.provenance = provenance_record("scenario");
    return spec;
  };

  DeclareRefRequest declaration;
  declaration.context.expected_generation = live.generation();
  declaration.ref = node(NodeDomain::Asset, "scenario-node");
  declaration.provenance = provenance_record("scenario");
  const auto declared = live.declare_external_ref(declaration);
  if (!declared) {
    return report_rejection("declare-ref", declared.error());
  }

  RegisterEdgeRequest edge;
  edge.context.expected_generation = live.generation();
  edge.spec = make_spec(node(NodeDomain::Asset, "scenario-node"), node(NodeDomain::ElectricalDomain, "feed-a"),
                        DependencyKind::RequiresPowerFrom, DependencyStrength::Hard);
  const auto registered = live.register_edge(edge);
  if (!registered) {
    return report_rejection("register", registered.error());
  }

  UpdateEdgeRequest update;
  update.context.expected_generation = live.generation();
  update.id = registered.value().id;
  update.expected_revision = registered.value().revision;
  update.strength = DependencyStrength::Soft;
  update.provenance = provenance_record("scenario-2");
  const auto updated = live.update_edge(update);
  if (!updated) {
    return report_rejection("update", updated.error());
  }

  LifecycleTransitionRequest transition;
  transition.context.expected_generation = live.generation();
  transition.id = registered.value().id;
  transition.expected_revision = updated.value().revision;
  transition.target = LifecycleState::Suspended;
  const auto transitioned = live.transition_edge_lifecycle(transition);
  if (!transitioned) {
    return report_rejection("transition", transitioned.error());
  }

  const DependencyGeneration generation = live.generation();
  const ContentDigest digest = live.snapshot().state_digest();
  const std::size_t edges = live.snapshot().edge_count();
  if (!live.close().ok()) {
    std::cout << "failed: the store did not close\n";
    return kExitIo;
  }

  // Inspect the result through the read-only path, exactly as an operator
  // would, and check that it agrees with what was committed.
  int read_exit = kExitOk;
  auto session = open_read_only(root.string(), read_exit);
  if (!session.has_value()) {
    return read_exit;
  }
  const bool agrees = session->snapshot.generation() == generation &&
                      session->snapshot.state_digest() == digest &&
                      session->snapshot.edge_count() == edges &&
                      session->report.outcome == RecoveryOutcome::LoadedCurrent;
  const auto text = export_text(session->snapshot);
  const bool exported = text.has_value() && text.value().find("format facility-dependency-registry/1") == 0;
  static_cast<void>(session->store->close());
  std::filesystem::remove_all(root, error);
  if (!agrees || !exported) {
    std::cout << "failed: the scenario did not read back what it committed\n";
    return kExitRejected;
  }
  say("scenario ok generation=" + to_text(generation) + " edges=" + std::to_string(edges) +
      " digest=" + digest.to_hex());
  return kExitOk;
}

// -- commands ----------------------------------------------------------------

int command_inspect(const std::string& root) {
  int exit_code = kExitOk;
  auto session = open_read_only(root, exit_code);
  if (!session.has_value()) {
    return exit_code;
  }
  const RegistrySnapshot& snapshot = session->snapshot;
  const auto status = session->store->status();
  if (!status) {
    return report_io_failure("to read the store status", status.error());
  }
  if (g_json) {
    std::cout << "{\"generation\": " << snapshot.generation().value() << ", \"edges\": " << snapshot.edge_count()
              << ", \"declarations\": " << snapshot.declared_ref_count()
              << ", \"nodes\": " << snapshot.node_count()
              << ", \"state_digest\": ";
    json_string(std::cout, snapshot.state_digest().to_hex());
    std::cout << ", \"content_digest\": ";
    json_string(std::cout, snapshot.content_digest().to_hex());
    std::cout << ", \"recovery\": ";
    json_string(std::cout, to_token(session->report.outcome));
    std::cout << ", \"retained_generations\": [";
    for (std::size_t index = 0; index < status.value().retained_generations.size(); ++index) {
      if (index > 0) {
        std::cout << ", ";
      }
      std::cout << status.value().retained_generations[index].value();
    }
    std::cout << "]}\n";
    return kExitOk;
  }
  std::cout << "root " << root << "\n";
  std::cout << "generation " << to_text(snapshot.generation()) << "\n";
  std::cout << "edges " << snapshot.edge_count() << "\n";
  std::cout << "declarations " << snapshot.declared_ref_count() << "\n";
  std::cout << "nodes " << snapshot.node_count() << "\n";
  std::cout << "state-digest " << snapshot.state_digest().to_hex() << "\n";
  std::cout << "content-digest " << snapshot.content_digest().to_hex() << "\n";
  std::cout << "recovery " << to_token(session->report.outcome) << "\n";
  std::cout << "retained";
  for (const auto generation : status.value().retained_generations) {
    std::cout << " " << to_text(generation);
  }
  std::cout << "\n";
  std::cout << "bytes " << status.value().total_bytes << "\n";
  if (status.value().lock_incarnation.valid()) {
    // A read-only session cannot tell whether the lock is held right now
    // without taking it, so the record is reported for what it is: the writer
    // that last held the store.
    std::cout << "last-writer pid " << status.value().lock_incarnation.process_id() << "\n";
  }
  return kExitOk;
}

int command_verify(const std::string& root) {
  int exit_code = kExitOk;
  auto session = open_read_only(root, exit_code);
  if (!session.has_value()) {
    return exit_code;
  }
  const RegistrySnapshot& snapshot = session->snapshot;
  std::size_t problems = 0;

  const auto obligation = snapshot.verify_acyclic_obligation();
  if (!obligation) {
    return report_io_failure("to check the acyclic obligation", obligation.error());
  }
  if (!obligation.value().satisfied) {
    ++problems;
    std::cout << "problem prohibited-cycle ";
    for (const auto& violation : obligation.value().violations) {
      std::cout << violation.to_text() << " ";
    }
    std::cout << "\n";
  }

  const auto encoded = snapshot.encode();
  if (!encoded) {
    ++problems;
    std::cout << "problem unencodable " << encoded.error().detail() << "\n";
  } else {
    const ContentDigest recomputed = sha256(std::span<const std::byte>{encoded.value()});
    if (recomputed != snapshot.state_digest()) {
      ++problems;
      std::cout << "problem digest-mismatch\n";
    }
    const auto round_trip =
        RegistrySnapshot::decode(std::span<const std::byte>{encoded.value()}, session->limits);
    if (!round_trip) {
      ++problems;
      std::cout << "problem undecodable " << round_trip.error().to_string() << "\n";
    } else if (round_trip.value().state_digest() != snapshot.state_digest()) {
      ++problems;
      std::cout << "problem round-trip-digest-mismatch\n";
    }
  }

  // The stored order is the canonical order: by key, then by identity. Any
  // other order would mean the state was assembled by something other than
  // this library, so the check uses the same comparison the library uses and
  // not a rendering of it.
  std::size_t ordering_problems = 0;
  const DependencyEdgeRecord* previous = nullptr;
  for (const auto& record : snapshot.edges()) {
    if (previous != nullptr && !(*previous < record)) {
      ++ordering_problems;
    }
    previous = &record;
  }
  if (ordering_problems != 0) {
    problems += ordering_problems;
    std::cout << "problem duplicate-or-unordered-edges " << ordering_problems << "\n";
  }

  const auto unresolved = snapshot.unresolved_endpoints();
  if (!unresolved) {
    return report_io_failure("to list unresolved references", unresolved.error());
  }
  if (!unresolved.value().empty()) {
    // Not a defect: an unresolved endpoint is an honest answer. It is reported
    // so an operator can decide whether the declaration is missing.
    std::cout << "note unresolved-references " << unresolved.value().size() << "\n";
  }

  if (problems == 0) {
    std::cout << "ok generation=" << to_text(snapshot.generation()) << " edges=" << snapshot.edge_count()
              << " digest=" << snapshot.state_digest().to_hex() << "\n";
    return kExitOk;
  }
  return kExitRejected;
}

int command_edges(const std::string& root, const Options& options) {
  int exit_code = kExitOk;
  auto session = open_read_only(root, exit_code);
  if (!session.has_value()) {
    return exit_code;
  }
  std::string filter_error;
  const auto built = build_filter(options, filter_error);
  if (!built.has_value()) {
    return usage(filter_error);
  }
  const EdgeFilter filter = built.value();
  std::vector<DependencyEdgeRecord> edges;
  for (const auto& record : session->snapshot.edges()) {
    if (filter.matches(record)) {
      edges.push_back(record);
    }
  }
  print_edges(edges);
  return kExitOk;
}

int command_lookup(const std::string& root, const std::string& reference, const Options& options, bool dependents) {
  const auto ref = parse_ref(reference);
  if (!ref.has_value()) {
    return usage("the reference is not a canonical <domain>:<identifier>");
  }
  int exit_code = kExitOk;
  auto session = open_read_only(root, exit_code);
  if (!session.has_value()) {
    return exit_code;
  }
  if (options.has("depth") || options.has("max-nodes")) {
    TraversalRequest request;
    request.root = *ref;
    request.direction = dependents ? TraversalDirection::Dependents : TraversalDirection::Dependencies;
    std::string filter_error;
    const auto built = build_filter(options, filter_error);
    if (!built.has_value()) {
      return usage(filter_error);
    }
    request.filter = built.value();
    std::string numeric_error;
    if (!read_u32_option(options, "depth", request.max_depth, request.max_depth, numeric_error) ||
        !read_u32_option(options, "max-nodes", request.max_nodes, request.max_nodes, numeric_error)) {
      return usage(numeric_error);
    }
    const auto closure = session->snapshot.transitive_closure(request);
    if (!closure) {
      return report_rejection("traversal", closure.error());
    }
    if (!closure.value().truncated) {
      say(std::string{"stop "} + std::string{to_token(closure.value().stop)});
    } else {
      say(std::string{"stop "} + std::string{to_token(closure.value().stop)} + " (truncated)");
    }
    for (const auto& entry : closure.value().entries) {
      std::cout << entry.node.to_canonical() << " depth=" << entry.depth
                << " via-edge=" << to_text(entry.via_edge) << " via=" << entry.via_node.to_canonical() << "\n";
    }
    return kExitOk;
  }

  std::string filter_error;
  const auto built = build_filter(options, filter_error);
  if (!built.has_value()) {
    return usage(filter_error);
  }
  const auto result = dependents ? session->snapshot.direct_dependents(*ref, built.value())
                                 : session->snapshot.direct_dependencies(*ref, built.value());
  if (!result) {
    return report_rejection("lookup", result.error());
  }
  print_edges(result.value());
  return kExitOk;
}

int command_impact(const std::string& root, const std::string& reference, const Options& options) {
  const auto ref = parse_ref(reference);
  if (!ref.has_value()) {
    return usage("the reference is not a canonical <domain>:<identifier>");
  }
  int exit_code = kExitOk;
  auto session = open_read_only(root, exit_code);
  if (!session.has_value()) {
    return exit_code;
  }
  ImpactConeRequest request;
  request.origin = *ref;
  if (options.has("all-strengths")) {
    request.strengths = DependencyStrengthMask::all();
  }
  std::string filter_error;
  const auto kinds = build_filter(options, filter_error);
  if (!kinds.has_value()) {
    return usage(filter_error);
  }
  request.kinds = kinds.value().kinds;
  request.lifecycles = kinds.value().lifecycles;
  std::string numeric_error;
  if (!read_u32_option(options, "max-depth", request.max_depth, request.max_depth, numeric_error) ||
      !read_u32_option(options, "max-nodes", request.max_nodes, request.max_nodes, numeric_error)) {
    return usage(numeric_error);
  }
  const auto cone = session->snapshot.impact_cone(request);
  if (!cone) {
    return report_rejection("impact", cone.error());
  }
  say(std::string{"origin "} + ref->to_canonical() + " stop " + std::string{to_token(cone.value().stop)} +
      (cone.value().truncated ? " (truncated)" : ""));
  for (const auto& entry : cone.value().entries) {
    std::cout << entry.node.to_canonical() << " depth=" << entry.depth << " via-edge=" << to_text(entry.via_edge)
              << " via=" << entry.via_node.to_canonical() << "\n";
  }
  return kExitOk;
}

int command_path(const std::string& root, const std::string& from, const std::string& to, const Options& options) {
  const auto start = parse_ref(from);
  const auto goal = parse_ref(to);
  if (!start.has_value() || !goal.has_value()) {
    return usage("both endpoints must be canonical <domain>:<identifier>");
  }
  int exit_code = kExitOk;
  auto session = open_read_only(root, exit_code);
  if (!session.has_value()) {
    return exit_code;
  }
  PathRequest request;
  request.from = *start;
  request.to = *goal;
  request.direction = options.has("reverse") ? TraversalDirection::Dependents : TraversalDirection::Dependencies;
  std::string filter_error;
  const auto built = build_filter(options, filter_error);
  if (!built.has_value()) {
    return usage(filter_error);
  }
  request.filter = built.value();
  std::string numeric_error;
  if (!read_u32_option(options, "depth", request.max_depth, request.max_depth, numeric_error)) {
    return usage(numeric_error);
  }
  const auto result = session->snapshot.explain_path(request);
  if (!result) {
    return report_rejection("path", result.error());
  }
  if (!result.value().found) {
    std::cout << "no path from " << start->to_canonical() << " to " << goal->to_canonical() << " ("
              << to_token(result.value().stop) << ")\n";
    return kExitRejected;
  }
  for (const auto& step : result.value().steps) {
    std::cout << step.to_text() << "\n";
  }
  say("hops " + std::to_string(result.value().hops()));
  return kExitOk;
}

int command_scc(const std::string& root, const Options& options) {
  int exit_code = kExitOk;
  auto session = open_read_only(root, exit_code);
  if (!session.has_value()) {
    return exit_code;
  }
  ComponentRequest request;
  request.include_singletons = options.has("singletons");
  std::string filter_error;
  const auto built = build_filter(options, filter_error);
  if (!built.has_value()) {
    return usage(filter_error);
  }
  request.filter = built.value();
  const auto result = session->snapshot.strongly_connected_components(request);
  if (!result) {
    return report_rejection("components", result.error());
  }
  say("components " + std::to_string(result.value().components.size()) + " cyclic " +
      std::to_string(result.value().cyclic_components) + " nodes " +
      std::to_string(result.value().nodes_examined));
  for (const auto& component : result.value().components) {
    std::cout << (component.cyclic ? "cyclic size=" : "single size=") << component.size() << " members";
    for (const auto& member : component.members) {
      std::cout << " " << member.to_canonical();
    }
    std::cout << "\n";
  }
  return kExitOk;
}

int command_cycles(const std::string& root, const Options& options) {
  int exit_code = kExitOk;
  auto session = open_read_only(root, exit_code);
  if (!session.has_value()) {
    return exit_code;
  }
  CycleRequest request;
  std::string filter_error;
  const auto built = build_filter(options, filter_error);
  if (!built.has_value()) {
    return usage(filter_error);
  }
  request.filter = built.value();
  std::string numeric_error;
  if (!read_u32_option(options, "max-length", request.max_length, request.max_length, numeric_error) ||
      !read_u32_option(options, "max-cycles", request.max_cycles, request.max_cycles, numeric_error)) {
    return usage(numeric_error);
  }
  const auto report = session->snapshot.enumerate_cycles(request);
  if (!report) {
    return report_rejection("cycles", report.error());
  }
  if (report.value().truncated) {
    say(std::string{"stop "} + std::string{to_token(report.value().stop)} + " (truncated)");
  }
  for (const auto& cycle : report.value().cycles) {
    std::cout << "cycle length=" << cycle.length() << " nodes";
    for (const auto& node : cycle.nodes) {
      std::cout << " " << node.to_canonical();
    }
    std::cout << " edges";
    for (const auto& edge : cycle.edges) {
      std::cout << " " << to_text(edge);
    }
    std::cout << "\n";
  }
  return kExitOk;
}

int command_unresolved(const std::string& root) {
  int exit_code = kExitOk;
  auto session = open_read_only(root, exit_code);
  if (!session.has_value()) {
    return exit_code;
  }
  const auto unresolved = session->snapshot.unresolved_endpoints();
  if (!unresolved) {
    return report_rejection("unresolved references", unresolved.error());
  }
  const auto unreferenced = session->snapshot.unreferenced_declared_refs();
  if (!unreferenced) {
    return report_rejection("unreferenced declarations", unreferenced.error());
  }
  for (const auto& ref : unresolved.value().refs()) {
    std::cout << "unresolved " << ref.to_canonical() << "\n";
  }
  for (const auto& ref : unreferenced.value().refs()) {
    std::cout << "unreferenced " << ref.to_canonical() << "\n";
  }
  return kExitOk;
}

int command_export(const std::string& root, const Options& options) {
  int exit_code = kExitOk;
  auto session = open_read_only(root, exit_code);
  if (!session.has_value()) {
    return exit_code;
  }
  const auto text = g_json || options.has("json") ? export_json(session->snapshot) : export_text(session->snapshot);
  if (!text) {
    return report_rejection("export", text.error());
  }
  std::cout << text.value();
  return kExitOk;
}

int command_diff(const std::string& root, const Options& options) {
  const auto from_text = options.one("from-generation");
  if (!from_text.has_value()) {
    return usage("diff needs --from-generation");
  }
  DependencyGeneration from{};
  if (!parse_generation(*from_text, from)) {
    return usage("--from-generation is not a generation number");
  }
  int exit_code = kExitOk;
  auto session = open_read_only(root, exit_code);
  if (!session.has_value()) {
    return exit_code;
  }

  RegistrySnapshot to = session->snapshot;
  if (const auto to_text_value = options.one("to-generation"); to_text_value.has_value()) {
    DependencyGeneration to_generation{};
    if (!parse_generation(*to_text_value, to_generation)) {
      return usage("--to-generation is not a generation number");
    }
    const auto loaded = session->store->load_generation(to_generation);
    if (!loaded) {
      return report_io_failure("to load that generation", loaded.error());
    }
    to = loaded.value();
  }

  const auto from_snapshot = session->store->load_generation(from);
  if (!from_snapshot) {
    return report_io_failure("to load the from generation", from_snapshot.error());
  }
  const auto diff = diff_snapshots(from_snapshot.value(), to, 4096);
  if (!diff) {
    return report_rejection("diff", diff.error());
  }
  say("from " + to_text(diff.value().from) + " to " + to_text(diff.value().to) + " added " +
      std::to_string(diff.value().added()) + " removed " + std::to_string(diff.value().removed()) + " modified " +
      std::to_string(diff.value().modified()) + " unchanged " + std::to_string(diff.value().unchanged_edges) +
      (diff.value().truncated ? " (truncated)" : ""));
  for (const auto& change : diff.value().edge_changes) {
    std::cout << change.to_text() << "\n";
  }
  for (const auto& change : diff.value().ref_changes) {
    std::cout << change.to_text() << "\n";
  }
  return kExitOk;
}

/// Builds the declared provenance, or reports why the arguments cannot be one.
/// A rejected annotation or identifier becomes a usage error rather than an
/// exception, because every argument here came from a command line.
std::optional<ProvenanceRecord> make_provenance(const Options& options, std::string_view default_source_id,
                                                std::string& error) {
  const std::string source_id = options.one("source-id").value_or(std::string{default_source_id});
  const std::string principal = options.one("principal").value_or("fdep");
  std::int64_t recorded_at = 0;
  if (const auto value = options.one("recorded-at-ms"); value.has_value()) {
    if (!parse_i64(*value, recorded_at)) {
      error = "--recorded-at-ms is not a number";
      return std::nullopt;
    }
  }
  const std::string annotation = options.one("annotation").value_or("");
  auto record = ProvenanceRecord::create(ProvenanceSource::OperatorDeclaration, source_id, principal, recorded_at,
                                         annotation, kHardMaxIdLength, kHardMaxAnnotationLength);
  if (!record) {
    error = std::string{"provenance rejected: "} + record.error().to_string();
    return std::nullopt;
  }
  return std::move(record).value();
}

std::optional<std::vector<DependencyConstraint>> make_constraints(const Options& options, std::string& error) {
  std::vector<DependencyConstraint> constraints;
  for (const auto& text : options.all("constraint")) {
    const std::size_t equals = text.find('=');
    if (equals == std::string::npos) {
      error = "a constraint must be written KIND=VALUE";
      return std::nullopt;
    }
    const auto kind = parse_constraint_kind(text.substr(0, equals));
    if (!kind.has_value()) {
      error = "unknown constraint kind '" + text.substr(0, equals) + "'";
      return std::nullopt;
    }
    const std::string value = text.substr(equals + 1);
    const auto type = value_type_of(*kind);
    if (type == ConstraintValueType::Integer) {
      std::int64_t number = 0;
      if (!parse_i64(value, number)) {
        error = "constraint " + std::string{to_token(*kind)} + " takes an integer";
        return std::nullopt;
      }
      auto constraint = DependencyConstraint::make(*kind, number);
      if (!constraint) {
        error = "constraint rejected: " + constraint.error().to_string();
        return std::nullopt;
      }
      constraints.push_back(std::move(constraint).value());
    } else if (type == ConstraintValueType::Token) {
      auto constraint = DependencyConstraint::make(*kind, value);
      if (!constraint) {
        error = "constraint rejected: " + constraint.error().to_string();
        return std::nullopt;
      }
      constraints.push_back(std::move(constraint).value());
    } else {
      error = "constraint " + std::string{to_token(*kind)} + " takes no value";
      return std::nullopt;
    }
  }
  return constraints;
}

int open_writer(const std::string& root, std::unique_ptr<DependencyRegistry>& registry) {
  RegistryOpenRequest request;
  request.root = root;
  auto opened = DependencyRegistry::open(request);
  if (!opened) {
    return report_io_failure("to open the store", opened.error());
  }
  registry = std::make_unique<DependencyRegistry>(std::move(opened).value());
  return kExitOk;
}

int command_register(const std::string& root, const Options& options) {
  const auto from = options.one("from");
  const auto to = options.one("to");
  const auto kind_text = options.one("kind");
  const auto expected = options.one("expect-generation");
  if (!from.has_value() || !to.has_value() || !kind_text.has_value() || !expected.has_value()) {
    return usage("register needs --from, --to, --kind and --expect-generation");
  }
  const auto from_ref = parse_ref(*from);
  const auto to_ref = parse_ref(*to);
  const auto kind = parse_dependency_kind(*kind_text);
  DependencyGeneration expected_generation{};
  if (!from_ref.has_value() || !to_ref.has_value() || !kind.has_value() ||
      !parse_generation(*expected, expected_generation)) {
    return usage("the arguments are not a valid declaration");
  }

  std::unique_ptr<DependencyRegistry> registry;
  if (const int code = open_writer(root, registry); code != kExitOk) {
    return code;
  }
  DependencyEdgeSpec spec;
  spec.source = *from_ref;
  spec.target = *to_ref;
  spec.kind = *kind;
  if (const auto strength = parse_enum<DependencyStrength>("strength", options, &parse_dependency_strength);
      strength.has_value()) {
    spec.strength = *strength;
  } else if (options.has("strength")) {
    static_cast<void>(registry->close());
    return usage("--strength is not one of the declared strengths");
  }
  if (const auto direction = parse_enum<Direction>("direction", options, &parse_direction);
      direction.has_value()) {
    spec.direction = *direction;
  } else if (options.has("direction")) {
    static_cast<void>(registry->close());
    return usage("--direction is not one of the declared directions");
  }
  if (const auto lifecycle = parse_enum<LifecycleState>("lifecycle", options, &parse_lifecycle_state);
      lifecycle.has_value()) {
    spec.initial_lifecycle = *lifecycle;
  } else if (options.has("lifecycle")) {
    static_cast<void>(registry->close());
    return usage("--lifecycle is not one of the declared states");
  }
  std::string build_error;
  const auto constraints = make_constraints(options, build_error);
  if (!constraints.has_value()) {
    static_cast<void>(registry->close());
    return usage(build_error);
  }
  spec.constraints = constraints.value();
  const auto provenance_record = make_provenance(options, "cli-register", build_error);
  if (!provenance_record.has_value()) {
    static_cast<void>(registry->close());
    return usage(build_error);
  }
  spec.provenance = provenance_record.value();

  RegisterEdgeRequest request;
  request.context.expected_generation = expected_generation;
  request.spec = spec;
  const auto outcome = registry->register_edge(request);
  if (!outcome) {
    static_cast<void>(registry->close());
    return report_rejection("register", outcome.error());
  }
  const DependencyGeneration generation = registry->generation();
  static_cast<void>(registry->close());
  print_mutation("register", outcome.value().id, outcome.value().revision, generation,
                 outcome.value().generation_advanced,
                 outcome.value().already_present ? "already-present" : "declared");
  return kExitOk;
}

int command_update(const std::string& root, const Options& options) {
  const auto edge_text = options.one("edge");
  const auto expected = options.one("expect-generation");
  const auto revision_text = options.one("expect-revision");
  const auto strength_text = options.one("strength");
  if (!edge_text.has_value() || !expected.has_value() || !revision_text.has_value() || !strength_text.has_value()) {
    return usage("update needs --edge, --expect-generation, --expect-revision and --strength");
  }
  DependencyEdgeId id{};
  DependencyGeneration expected_generation{};
  EdgeRevision expected_revision{};
  const auto strength = parse_dependency_strength(*strength_text);
  if (!parse_edge_id(*edge_text, id) || !parse_generation(*expected, expected_generation) ||
      !parse_edge_revision(*revision_text, expected_revision) || !strength.has_value()) {
    return usage("the arguments are not a valid update");
  }

  std::unique_ptr<DependencyRegistry> registry;
  if (const int code = open_writer(root, registry); code != kExitOk) {
    return code;
  }
  UpdateEdgeRequest request;
  request.context.expected_generation = expected_generation;
  request.id = id;
  request.expected_revision = expected_revision;
  request.strength = *strength;
  if (const auto direction = parse_enum<Direction>("direction", options, &parse_direction);
      direction.has_value()) {
    request.direction = *direction;
  } else if (options.has("direction")) {
    static_cast<void>(registry->close());
    return usage("--direction is not one of the declared directions");
  }
  std::string build_error;
  const auto constraints = make_constraints(options, build_error);
  if (!constraints.has_value()) {
    static_cast<void>(registry->close());
    return usage(build_error);
  }
  request.constraints = constraints.value();
  const auto provenance_record = make_provenance(options, "cli-update", build_error);
  if (!provenance_record.has_value()) {
    static_cast<void>(registry->close());
    return usage(build_error);
  }
  request.provenance = provenance_record.value();
  const auto outcome = registry->update_edge(request);
  if (!outcome) {
    static_cast<void>(registry->close());
    return report_rejection("update", outcome.error());
  }
  const DependencyGeneration generation = registry->generation();
  static_cast<void>(registry->close());
  print_mutation("update", outcome.value().id, outcome.value().revision, generation,
                 outcome.value().generation_advanced, outcome.value().changed ? "updated" : "unchanged");
  return kExitOk;
}

int command_transition(const std::string& root, const Options& options) {
  const auto edge_text = options.one("edge");
  const auto expected = options.one("expect-generation");
  const auto revision_text = options.one("expect-revision");
  const auto target_text = options.one("to");
  if (!edge_text.has_value() || !expected.has_value() || !revision_text.has_value() || !target_text.has_value()) {
    return usage("transition needs --edge, --expect-generation, --expect-revision and --to");
  }
  DependencyEdgeId id{};
  DependencyGeneration expected_generation{};
  EdgeRevision expected_revision{};
  const auto target = parse_lifecycle_state(*target_text);
  if (!parse_edge_id(*edge_text, id) || !parse_generation(*expected, expected_generation) ||
      !parse_edge_revision(*revision_text, expected_revision) || !target.has_value()) {
    return usage("the arguments are not a valid transition");
  }

  std::unique_ptr<DependencyRegistry> registry;
  if (const int code = open_writer(root, registry); code != kExitOk) {
    return code;
  }
  LifecycleTransitionRequest request;
  request.context.expected_generation = expected_generation;
  request.id = id;
  request.expected_revision = expected_revision;
  request.target = *target;
  request.reason = options.one("reason").value_or("");
  const auto outcome = registry->transition_edge_lifecycle(request);
  if (!outcome) {
    static_cast<void>(registry->close());
    return report_rejection("transition", outcome.error());
  }
  const DependencyGeneration generation = registry->generation();
  static_cast<void>(registry->close());
  print_mutation("transition", outcome.value().id, outcome.value().revision, generation,
                 outcome.value().generation_advanced,
                 outcome.value().already_in_state ? "already-in-state" : to_token(*target));
  return kExitOk;
}

int command_remove(const std::string& root, const Options& options) {
  const auto edge_text = options.one("edge");
  const auto expected = options.one("expect-generation");
  const auto revision_text = options.one("expect-revision");
  if (!edge_text.has_value() || !expected.has_value() || !revision_text.has_value()) {
    return usage("remove needs --edge, --expect-generation and --expect-revision");
  }
  DependencyEdgeId id{};
  DependencyGeneration expected_generation{};
  EdgeRevision expected_revision{};
  if (!parse_edge_id(*edge_text, id) || !parse_generation(*expected, expected_generation) ||
      !parse_edge_revision(*revision_text, expected_revision)) {
    return usage("the arguments are not a valid removal");
  }

  std::unique_ptr<DependencyRegistry> registry;
  if (const int code = open_writer(root, registry); code != kExitOk) {
    return code;
  }
  RemoveEdgeRequest request;
  request.context.expected_generation = expected_generation;
  request.id = id;
  request.expected_revision = expected_revision;
  request.reason = options.one("reason").value_or("");
  const auto outcome = registry->remove_edge(request);
  if (!outcome) {
    static_cast<void>(registry->close());
    return report_rejection("remove", outcome.error());
  }
  const DependencyGeneration generation = registry->generation();
  static_cast<void>(registry->close());
  print_mutation("remove", outcome.value().id, outcome.value().removed_revision, generation,
                 outcome.value().generation_advanced,
                 outcome.value().already_absent ? "already-absent" : "removed");
  return kExitOk;
}

int command_declare_ref(const std::string& root, const Options& options) {
  const auto ref_text = options.one("ref");
  const auto expected = options.one("expect-generation");
  if (!ref_text.has_value() || !expected.has_value()) {
    return usage("declare-ref needs --ref and --expect-generation");
  }
  const auto ref = parse_ref(*ref_text);
  DependencyGeneration expected_generation{};
  if (!ref.has_value() || !parse_generation(*expected, expected_generation)) {
    return usage("the arguments are not a valid declaration");
  }
  std::unique_ptr<DependencyRegistry> registry;
  if (const int code = open_writer(root, registry); code != kExitOk) {
    return code;
  }
  DeclareRefRequest request;
  request.context.expected_generation = expected_generation;
  request.ref = *ref;
  std::string build_error;
  const auto provenance_record = make_provenance(options, "cli-declare", build_error);
  if (!provenance_record.has_value()) {
    static_cast<void>(registry->close());
    return usage(build_error);
  }
  request.provenance = provenance_record.value();
  const auto outcome = registry->declare_external_ref(request);
  if (!outcome) {
    static_cast<void>(registry->close());
    return report_rejection("declare-ref", outcome.error());
  }
  const DependencyGeneration generation = registry->generation();
  static_cast<void>(registry->close());
  print_mutation("declare-ref", DependencyEdgeId{}, EdgeRevision{}, generation,
                 outcome.value().generation_advanced,
                 outcome.value().already_declared ? "already-declared" : "declared");
  return kExitOk;
}

int command_withdraw_ref(const std::string& root, const Options& options) {
  const auto ref_text = options.one("ref");
  const auto expected = options.one("expect-generation");
  if (!ref_text.has_value() || !expected.has_value()) {
    return usage("withdraw-ref needs --ref and --expect-generation");
  }
  const auto ref = parse_ref(*ref_text);
  DependencyGeneration expected_generation{};
  if (!ref.has_value() || !parse_generation(*expected, expected_generation)) {
    return usage("the arguments are not a valid withdrawal");
  }
  std::unique_ptr<DependencyRegistry> registry;
  if (const int code = open_writer(root, registry); code != kExitOk) {
    return code;
  }
  WithdrawRefRequest request;
  request.context.expected_generation = expected_generation;
  request.ref = *ref;
  request.allow_referenced = options.has("allow-referenced");
  const auto outcome = registry->withdraw_external_ref(request);
  if (!outcome) {
    static_cast<void>(registry->close());
    return report_rejection("withdraw-ref", outcome.error());
  }
  const DependencyGeneration generation = registry->generation();
  static_cast<void>(registry->close());
  print_mutation("withdraw-ref", DependencyEdgeId{}, EdgeRevision{}, generation,
                 outcome.value().generation_advanced,
                 outcome.value().already_absent ? "already-absent" : "withdrawn");
  return kExitOk;
}

}  // namespace

int run(int argc, char** argv) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  if (arguments.empty()) {
    return usage("no command given");
  }

  for (const auto& argument : arguments) {
    if (argument == "--json") {
      g_json = true;
    } else if (argument == "--quiet") {
      g_quiet = true;
    }
  }
  if (g_json) {
    g_quiet = true;
  }

  const std::string command = arguments[0];
  const Options options = parse_options(arguments, 1);

  if (options.has("self-check") || options.has("scenario")) {
    if (command.rfind("--", 0) == 0) {
      return usage("the self validation modes take the store directory first");
    }
    return options.has("self-check") ? run_self_check(command) : run_scenario(command);
  }

  const auto root = [&options, &command]() -> std::optional<std::string> {
    if (!options.positional.empty()) {
      return options.positional.front();
    }
    return std::nullopt;
  }();

  if (command == "inspect" || command == "verify" || command == "edges" || command == "dependents" ||
      command == "deps" || command == "impact" || command == "path" || command == "scc" ||
      command == "cycles" || command == "unresolved" || command == "export" || command == "diff" ||
      command == "register" || command == "update" || command == "transition" || command == "remove" ||
      command == "declare-ref" || command == "withdraw-ref") {
    if (!root.has_value()) {
      return usage("this command needs a store root");
    }
  }

  const std::string store_root = root.value_or("");

  if (command == "inspect") {
    return command_inspect(store_root);
  }
  if (command == "verify") {
    return command_verify(store_root);
  }
  if (command == "edges") {
    return command_edges(store_root, options);
  }
  if (command == "deps") {
    if (options.positional.size() < 2) {
      return usage("deps needs a reference");
    }
    return command_lookup(store_root, options.positional[1], options, false);
  }
  if (command == "dependents") {
    if (options.positional.size() < 2) {
      return usage("dependents needs a reference");
    }
    return command_lookup(store_root, options.positional[1], options, true);
  }
  if (command == "impact") {
    if (options.positional.size() < 2) {
      return usage("impact needs a reference");
    }
    return command_impact(store_root, options.positional[1], options);
  }
  if (command == "path") {
    if (options.positional.size() < 3) {
      return usage("path needs two references");
    }
    return command_path(store_root, options.positional[1], options.positional[2], options);
  }
  if (command == "scc") {
    return command_scc(store_root, options);
  }
  if (command == "cycles") {
    return command_cycles(store_root, options);
  }
  if (command == "unresolved") {
    return command_unresolved(store_root);
  }
  if (command == "export") {
    return command_export(store_root, options);
  }
  if (command == "diff") {
    return command_diff(store_root, options);
  }
  if (command == "register") {
    return command_register(store_root, options);
  }
  if (command == "update") {
    return command_update(store_root, options);
  }
  if (command == "transition") {
    return command_transition(store_root, options);
  }
  if (command == "remove") {
    return command_remove(store_root, options);
  }
  if (command == "declare-ref") {
    return command_declare_ref(store_root, options);
  }
  if (command == "withdraw-ref") {
    return command_withdraw_ref(store_root, options);
  }
  return usage("unknown command '" + command + "'");
}

int main(int argc, char** argv) {
  // Every argument below is untrusted input. The command implementations
  // validate rather than assume, and this guard exists so that an unforeseen
  // failure is reported as a distinct internal error instead of as an abort
  // that a caller could mistake for a store failure.
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::cout << "internal-error: " << error.what() << "\n";
    return 4;
  } catch (...) {
    std::cout << "internal-error: unrecognised failure\n";
    return 4;
  }
}
