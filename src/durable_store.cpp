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

#include "facility_dependency_registry/persistence.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "facility_dependency_registry/version.hpp"
#include "file_lock.hpp"
#include "file_ops.hpp"
#include "graph_state.hpp"
#include "sha256.hpp"

namespace facility_dependency_registry {
namespace {

constexpr std::size_t kPointerMaxBytes = 4096;
constexpr std::size_t kGenerationDigits = 20;

std::string hex_of(const ContentDigest& digest) { return digest.to_hex(); }

std::string generation_file_name(DependencyGeneration generation) {
  std::string digits = std::to_string(generation.value());
  std::string name{kGenerationFilePrefix};
  if (digits.size() < kGenerationDigits) {
    name.append(kGenerationDigits - digits.size(), '0');
  }
  name.append(digits);
  name.append(kGenerationFileSuffix);
  return name;
}

std::optional<DependencyGeneration> parse_generation_file_name(std::string_view name) {
  if (name.size() != kGenerationFilePrefix.size() + kGenerationDigits + kGenerationFileSuffix.size()) {
    return std::nullopt;
  }
  if (name.substr(0, kGenerationFilePrefix.size()) != kGenerationFilePrefix) {
    return std::nullopt;
  }
  if (name.substr(name.size() - kGenerationFileSuffix.size()) != kGenerationFileSuffix) {
    return std::nullopt;
  }
  const std::string_view digits =
      name.substr(kGenerationFilePrefix.size(), name.size() - kGenerationFilePrefix.size() -
                                                     kGenerationFileSuffix.size());
  // The digit field is zero padded to a fixed width, so it deliberately accepts
  // leading zeros; `parse_generation` stays strict for identity text, and the
  // canonical spelling check below is what pins the field to exactly this
  // width. A digit field that is not all digits, or that does not fit in 64
  // bits, is rejected here.
  std::uint64_t value = 0;
  for (const char character : digits) {
    if (character < '0' || character > '9') {
      return std::nullopt;
    }
    const auto digit = static_cast<std::uint64_t>(character - '0');
    if (value > (UINT64_MAX - digit) / 10) {
      return std::nullopt;
    }
    value = value * 10 + digit;
  }
  const DependencyGeneration generation = DependencyGeneration::from_value(value);
  if (generation_file_name(generation) != name) {
    return std::nullopt;  // not the canonical spelling of that generation
  }
  return generation;
}

struct PointerRecord {
  DependencyGeneration generation{};
  ContentDigest digest{};
};

std::string render_pointer(DependencyGeneration generation, const ContentDigest& digest) {
  std::string text{kPointerMagic};
  text.push_back('\n');
  text.append("generation ");
  text.append(std::to_string(generation.value()));
  text.push_back('\n');
  text.append("sha256 ");
  text.append(hex_of(digest));
  text.push_back('\n');
  return text;
}

Result<PointerRecord> parse_pointer(std::string_view text) {
  const auto next_line = [&text](std::size_t& offset, std::string_view& line) {
    if (offset >= text.size()) {
      return false;
    }
    const std::size_t end = text.find('\n', offset);
    if (end == std::string_view::npos) {
      return false;
    }
    line = text.substr(offset, end - offset);
    offset = end + 1;
    return true;
  };

  std::size_t offset = 0;
  std::string_view line;
  if (!next_line(offset, line) || line != kPointerMagic) {
    return Result<PointerRecord>::failure(ErrorCode::StoreLayoutInvalid,
                                          "the pointer file does not start with its magic");
  }
  if (!next_line(offset, line) || line.substr(0, 11) != "generation ") {
    return Result<PointerRecord>::failure(ErrorCode::StoreLayoutInvalid,
                                          "the pointer file has no generation line");
  }
  PointerRecord record;
  if (!parse_generation(line.substr(11), record.generation)) {
    return Result<PointerRecord>::failure(ErrorCode::StoreLayoutInvalid,
                                          "the pointer file names a generation that is not a number");
  }
  if (!next_line(offset, line) || line.substr(0, 7) != "sha256 ") {
    return Result<PointerRecord>::failure(ErrorCode::StoreLayoutInvalid,
                                          "the pointer file has no digest line");
  }
  auto digest = ContentDigest::from_hex(line.substr(7));
  if (!digest) {
    return Result<PointerRecord>::failure(ErrorCode::StoreLayoutInvalid,
                                          "the pointer file names a digest that is not a content digest");
  }
  record.digest = digest.value();
  if (offset != text.size()) {
    return Result<PointerRecord>::failure(ErrorCode::StoreLayoutInvalid,
                                          "the pointer file has trailing bytes");
  }
  return record;
}

std::vector<std::byte> encode_container(std::span<const std::byte> payload) {
  std::vector<std::byte> file;
  file.reserve(static_cast<std::size_t>(kContainerHeaderSize) + payload.size());
  const auto append_u32 = [&file](std::uint32_t value) {
    for (unsigned shift = 0; shift < 32u; shift += 8u) {
      file.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
    }
  };
  const auto append_u64 = [&file](std::uint64_t value) {
    for (unsigned shift = 0; shift < 64u; shift += 8u) {
      file.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
    }
  };
  for (const char character : kContainerMagic) {
    file.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
  append_u32(kContainerFormatVersion);
  append_u32(0);  // flags
  append_u64(payload.size());
  append_u32(crc32(payload));
  append_u32(0);  // reserved
  const auto digest = sha256(payload);
  for (const auto byte : digest.bytes()) {
    file.push_back(byte);
  }
  file.insert(file.end(), payload.begin(), payload.end());
  return file;
}

struct ContainerView {
  std::uint64_t payload_length = 0;
  std::uint32_t crc = 0;
  ContentDigest digest{};
  std::span<const std::byte> payload{};
};

Result<ContainerView> verify_container(std::span<const std::byte> file, std::uint64_t max_payload_bytes) {
  if (file.size() < kContainerHeaderSize) {
    return Result<ContainerView>::failure(ErrorCode::StoreTruncated,
                                          "the file is shorter than its fixed header");
  }
  const auto read_u32 = [file](std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) {
      value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(file[offset + index])) << (8u * index);
    }
    return value;
  };
  const auto read_u64 = [file](std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
      value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(file[offset + index])) << (8u * index);
    }
    return value;
  };

  for (std::size_t index = 0; index < kContainerMagic.size(); ++index) {
    if (static_cast<char>(file[index]) != kContainerMagic[index]) {
      return Result<ContainerView>::failure(ErrorCode::StoreCorrupt, "the file magic does not match");
    }
  }
  const std::uint32_t version = read_u32(8);
  if (version != kContainerFormatVersion) {
    return Result<ContainerView>::failure(ErrorCode::StoreVersionUnsupported,
                                          "the file declares container format version " +
                                              std::to_string(version));
  }
  const std::uint32_t flags = read_u32(12);
  if (flags != 0) {
    return Result<ContainerView>::failure(ErrorCode::StoreCorrupt,
                                          "the file declares flags this build does not implement");
  }
  const std::uint64_t payload_length = read_u64(16);
  if (payload_length > max_payload_bytes) {
    return Result<ContainerView>::failure(ErrorCode::PayloadTooLarge,
                                          "the file declares a payload larger than the configured maximum");
  }
  if (payload_length != static_cast<std::uint64_t>(file.size()) - kContainerHeaderSize) {
    return Result<ContainerView>::failure(ErrorCode::StoreTruncated,
                                          "the file length does not match the length in its header");
  }
  ContainerView view;
  view.payload_length = payload_length;
  view.crc = read_u32(24);
  std::array<std::byte, ContentDigest::byte_size> digest_bytes{};
  for (std::size_t index = 0; index < ContentDigest::byte_size; ++index) {
    digest_bytes[index] = file[32 + index];
  }
  view.digest = ContentDigest::from_bytes(digest_bytes);
  view.payload = file.subspan(static_cast<std::size_t>(kContainerHeaderSize));

  if (crc32(view.payload) != view.crc) {
    return Result<ContainerView>::failure(ErrorCode::StoreIntegrityFailure,
                                          "the payload failed its CRC-32 check");
  }
  if (sha256(view.payload) != view.digest) {
    return Result<ContainerView>::failure(ErrorCode::StoreIntegrityFailure,
                                          "the payload failed its SHA-256 check");
  }
  return view;
}

ContentDigest digest_of_file(std::span<const std::byte> file) { return sha256(file); }

}  // namespace

// ---------------------------------------------------------------------------
// WriterIncarnation
// ---------------------------------------------------------------------------

WriterIncarnation WriterIncarnation::generate() {
  WriterIncarnation incarnation;
  incarnation.process_id_ = detail::current_process_id();
  std::random_device device;
  std::mt19937_64 generator{static_cast<std::uint64_t>(device()) ^
                            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
  incarnation.nonce_ = generator() | 1u;
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  incarnation.started_at_unix_ms_ =
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  return incarnation;
}

std::string WriterIncarnation::to_text() const {
  std::string result{"pid="};
  result.append(std::to_string(process_id_));
  result.append(" nonce=");
  static constexpr char digits[] = "0123456789abcdef";
  for (int shift = 60; shift >= 0; shift -= 4) {
    result.push_back(digits[(nonce_ >> shift) & 0x0Fu]);
  }
  result.append(" started-ms=");
  result.append(std::to_string(started_at_unix_ms_));
  return result;
}

Result<WriterIncarnation> WriterIncarnation::parse(std::string_view text) {
  WriterIncarnation incarnation;
  const auto take_field = [&text](std::size_t& offset, std::string_view prefix, std::string_view& value) {
    if (text.substr(offset, prefix.size()) != prefix) {
      return false;
    }
    offset += prefix.size();
    const std::size_t end = text.find(' ', offset);
    value = end == std::string_view::npos ? text.substr(offset) : text.substr(offset, end - offset);
    offset = end == std::string_view::npos ? text.size() : end + 1;
    return !value.empty();
  };

  std::size_t offset = 0;
  std::string_view value;
  if (!take_field(offset, "pid=", value)) {
    return Result<WriterIncarnation>::failure(ErrorCode::StoreLayoutInvalid,
                                              "the writer incarnation has no process id");
  }
  for (const char character : value) {
    if (character < '0' || character > '9') {
      return Result<WriterIncarnation>::failure(ErrorCode::StoreLayoutInvalid,
                                                "the writer incarnation process id is not a number");
    }
  }
  incarnation.process_id_ = std::strtoull(std::string{value}.c_str(), nullptr, 10);

  if (!take_field(offset, "nonce=", value) || value.size() != 16) {
    return Result<WriterIncarnation>::failure(ErrorCode::StoreLayoutInvalid,
                                              "the writer incarnation has no nonce");
  }
  std::uint64_t nonce = 0;
  for (const char character : value) {
    nonce <<= 4;
    if (character >= '0' && character <= '9') {
      nonce |= static_cast<std::uint64_t>(character - '0');
    } else if (character >= 'a' && character <= 'f') {
      nonce |= static_cast<std::uint64_t>(character - 'a' + 10);
    } else {
      return Result<WriterIncarnation>::failure(ErrorCode::StoreLayoutInvalid,
                                                "the writer incarnation nonce is not hexadecimal");
    }
  }
  incarnation.nonce_ = nonce;

  if (!take_field(offset, "started-ms=", value)) {
    return Result<WriterIncarnation>::failure(ErrorCode::StoreLayoutInvalid,
                                              "the writer incarnation has no start time");
  }
  std::size_t consumed = 0;
  try {
    incarnation.started_at_unix_ms_ = std::stoll(std::string{value}, &consumed);
  } catch (const std::exception&) {
    return Result<WriterIncarnation>::failure(ErrorCode::StoreLayoutInvalid,
                                              "the writer incarnation start time is not a number");
  }
  if (consumed != value.size()) {
    return Result<WriterIncarnation>::failure(ErrorCode::StoreLayoutInvalid,
                                              "the writer incarnation start time is not a number");
  }
  return incarnation;
}

std::string_view to_token(OpenMode mode) noexcept {
  switch (mode) {
    case OpenMode::ReadOnly:
      return "read-only";
    case OpenMode::ReadWrite:
      return "read-write";
  }
  return "unknown-open-mode";
}

std::string_view to_token(PublishStep step) noexcept {
  switch (step) {
    case PublishStep::Begun:
      return "begun";
    case PublishStep::TransientWritten:
      return "transient-written";
    case PublishStep::TransientSynced:
      return "transient-synced";
    case PublishStep::TransientVerified:
      return "transient-verified";
    case PublishStep::GenerationRenamed:
      return "generation-renamed";
    case PublishStep::DirectorySynced:
      return "directory-synced";
    case PublishStep::PointerReplaced:
      return "pointer-replaced";
    case PublishStep::PointerSynced:
      return "pointer-synced";
    case PublishStep::Pruned:
      return "pruned";
    case PublishStep::Completed:
      return "completed";
  }
  return "unknown-publish-step";
}

std::string_view to_token(RecoveryOutcome outcome) noexcept {
  switch (outcome) {
    case RecoveryOutcome::FreshEmpty:
      return "fresh-empty";
    case RecoveryOutcome::LoadedCurrent:
      return "loaded-current";
    case RecoveryOutcome::LoadedFallback:
      return "loaded-fallback";
    case RecoveryOutcome::RefusedCorrupt:
      return "refused-corrupt";
  }
  return "unknown-recovery-outcome";
}

std::string RecoveryReport::to_text() const {
  std::string result{"recovery outcome="};
  result.append(to_token(outcome));
  result.append(" generation=");
  result.append(facility_dependency_registry::to_text(loaded_generation));
  result.append(" rejected=");
  if (rejected_generations.empty()) {
    result.append("none");
  } else {
    for (std::size_t index = 0; index < rejected_generations.size(); ++index) {
      if (index > 0) {
        result.push_back(',');
      }
      result.append(facility_dependency_registry::to_text(rejected_generations[index]));
    }
  }
  result.append(" transient-removed=");
  result.append(std::to_string(transient_files_removed));
  result.append(" pruned=");
  result.append(std::to_string(generations_pruned));
  for (const auto& note : diagnostics) {
    result.append(" | ");
    result.append(note);
  }
  return result;
}

std::string StoreStatus::to_text() const {
  std::string result{"store root="};
  result.append(detail::to_utf8(root));
  result.append(" mode=");
  result.append(read_only ? "read-only" : "read-write");
  result.append(" lock-held=");
  result.append(writer_lock_held ? "true" : "false");
  if (lock_incarnation.valid()) {
    result.append(" lock-holder={");
    result.append(lock_incarnation.to_text());
    result.push_back('}');
  }
  result.append(" pointer=");
  result.append(pointer_present ? facility_dependency_registry::to_text(pointer_generation) : std::string{"absent"});
  result.append(" retained=");
  for (std::size_t index = 0; index < retained_generations.size(); ++index) {
    if (index > 0) {
      result.push_back(',');
    }
    result.append(facility_dependency_registry::to_text(retained_generations[index]));
  }
  result.append(" bytes=");
  result.append(std::to_string(total_bytes));
  result.append(" transient=");
  result.append(std::to_string(transient_files));
  return result;
}

// ---------------------------------------------------------------------------
// DurableStore
// ---------------------------------------------------------------------------

struct DurableStore::Impl {
  std::filesystem::path root{};
  std::filesystem::path lock_path{};
  std::filesystem::path pointer_path{};
  StoreOptions options{};
  detail::FileLock lock{};
  WriterIncarnation incarnation{};
  DependencyGeneration committed{};
  bool read_only = false;
  bool closed = false;
};

DurableStore::DurableStore() noexcept = default;
DurableStore::~DurableStore() = default;
DurableStore::DurableStore(DurableStore&&) noexcept = default;
DurableStore& DurableStore::operator=(DurableStore&&) noexcept = default;

const std::filesystem::path& DurableStore::root() const noexcept { return impl_->root; }
bool DurableStore::read_only() const noexcept { return impl_->read_only; }
bool DurableStore::lock_held() const noexcept { return impl_->lock.held(); }
const WriterIncarnation& DurableStore::incarnation() const noexcept { return impl_->incarnation; }
DependencyGeneration DurableStore::committed_generation() const noexcept { return impl_->committed; }

namespace {

/// Reads, verifies and decodes one generation file. The generation encoded in
/// the file name is cross-checked against the generation inside the payload, so
/// a file cannot be renamed into a different place in history. `digest_out`
/// receives the SHA-256 of the whole file, which is what the pointer names.
Result<RegistrySnapshot> load_generation_file(const std::filesystem::path& path, DependencyGeneration expected,
                                              const RegistryLimits& limits, ContentDigest* digest_out) {
  const std::uint64_t max_file_bytes = limits.max_persisted_bytes;
  auto bytes = detail::read_file_bounded(path, max_file_bytes);
  if (!bytes) {
    return Result<RegistrySnapshot>::failure(bytes.error());
  }
  if (digest_out != nullptr) {
    *digest_out = sha256(std::span<const std::byte>{bytes.value()});
  }
  auto container = verify_container(bytes.value(), limits.max_persisted_bytes);
  if (!container) {
    return Result<RegistrySnapshot>::failure(container.error());
  }
  auto snapshot = RegistrySnapshot::decode(container.value().payload, limits);
  if (!snapshot) {
    return Result<RegistrySnapshot>::failure(
        RegistryError{snapshot.error().code(),
                      detail::to_utf8(path.filename()) + ": " + snapshot.error().detail()});
  }
  if (snapshot.value().generation() != expected) {
    return Result<RegistrySnapshot>::failure(
        ErrorCode::StoreCorrupt,
        detail::to_utf8(path.filename()) + " contains generation " +
            facility_dependency_registry::to_text(snapshot.value().generation()) + " under the name of generation " +
            facility_dependency_registry::to_text(expected));
  }
  return snapshot;
}

struct RetainedFile {
  DependencyGeneration generation{};
  std::filesystem::path path{};
  std::uint64_t bytes = 0;
};

/// Removes generation files outside the retention window.
///
/// The window always contains `must_keep`, which is the generation that was
/// loaded or just published: a store never prunes away the state it is about to
/// name in its pointer. The remaining places are filled from the newest
/// generations downwards, so a superseded generation survives exactly as long
/// as it stays inside the window and no longer.
std::uint32_t prune_generations(const std::vector<RetainedFile>& newest_first, DependencyGeneration must_keep,
                                std::size_t max_retained) {
  std::vector<DependencyGeneration> keep;
  keep.reserve(max_retained);
  keep.push_back(must_keep);
  for (const auto& file : newest_first) {
    if (keep.size() >= max_retained) {
      break;
    }
    if (file.generation != must_keep) {
      keep.push_back(file.generation);
    }
  }
  std::uint32_t removed = 0;
  for (const auto& file : newest_first) {
    if (std::find(keep.begin(), keep.end(), file.generation) != keep.end()) {
      continue;
    }
    if (detail::remove_file(file.path).ok()) {
      ++removed;
    }
  }
  return removed;
}

Result<std::vector<RetainedFile>> scan_generation_files(const std::filesystem::path& root,
                                                        std::vector<std::filesystem::path>* transient_files) {
  std::vector<RetainedFile> files;
  std::error_code error;
  std::filesystem::directory_iterator iterator{root, error};
  if (error) {
    return Result<std::vector<RetainedFile>>::failure(
        ErrorCode::StoreIoError, "cannot list " + detail::to_utf8(root) + ": " + error.message());
  }
  for (const auto& entry : iterator) {
    const std::string name = detail::to_utf8(entry.path().filename());
    std::error_code type_error;
    if (!entry.is_regular_file(type_error)) {
      continue;
    }
    if (name.rfind(std::string{kTransientFilePrefix}, 0) == 0) {
      if (transient_files != nullptr) {
        transient_files->push_back(entry.path());
      }
      continue;
    }
    const auto generation = parse_generation_file_name(name);
    if (!generation.has_value()) {
      continue;
    }
    RetainedFile file;
    file.generation = *generation;
    file.path = entry.path();
    std::error_code size_error;
    const auto size = std::filesystem::file_size(entry.path(), size_error);
    file.bytes = size_error ? 0 : static_cast<std::uint64_t>(size);
    files.push_back(std::move(file));
  }
  std::sort(files.begin(), files.end(), [](const RetainedFile& lhs, const RetainedFile& rhs) {
    return lhs.generation > rhs.generation;
  });
  return files;
}

}  // namespace

Result<std::unique_ptr<DurableStore>> DurableStore::open(const std::filesystem::path& root,
                                                         const StoreOptions& options,
                                                         RegistrySnapshot& out_snapshot,
                                                         RecoveryReport& out_report) {
  out_report = RecoveryReport{};
  if (root.empty()) {
    return Result<std::unique_ptr<DurableStore>>::failure(ErrorCode::InvalidStorePath,
                                                          "the store root is empty");
  }
  if (const auto status = options.limits.validate(); !status.ok()) {
    return Result<std::unique_ptr<DurableStore>>::failure(status.error());
  }

  auto store = std::unique_ptr<DurableStore>{new DurableStore{}};
  store->impl_ = std::make_unique<Impl>();
  store->impl_->root = root;
  store->impl_->lock_path = root / std::filesystem::path{std::string{kWriterLockFileName}};
  store->impl_->pointer_path = root / std::filesystem::path{std::string{kPointerFileName}};
  store->impl_->options = options;
  store->impl_->read_only = options.mode == OpenMode::ReadOnly;

  std::error_code error;
  const bool exists = std::filesystem::exists(root, error);
  if (!exists) {
    if (store->impl_->read_only || !options.create_if_missing) {
      return Result<std::unique_ptr<DurableStore>>::failure(ErrorCode::StoreNotFound,
                                                            "the store root " + detail::to_utf8(root) +
                                                                " does not exist");
    }
    if (const auto status = detail::ensure_directory(root); !status.ok()) {
      return Result<std::unique_ptr<DurableStore>>::failure(status.error());
    }
  } else if (!std::filesystem::is_directory(root, error)) {
    return Result<std::unique_ptr<DurableStore>>::failure(ErrorCode::StoreLayoutInvalid,
                                                          detail::to_utf8(root) + " is not a directory");
  }

  if (!store->impl_->read_only) {
    const WriterIncarnation incarnation = WriterIncarnation::generate();
    auto lock = detail::FileLock::acquire(store->impl_->lock_path, incarnation.to_text());
    if (!lock) {
      if (lock.error().code() == ErrorCode::StoreLocked) {
        std::string detail = lock.error().detail();
        const auto holder = detail::read_lock_record(store->impl_->lock_path);
        if (holder && !holder.value().empty()) {
          detail.append("; the recorded holder is ");
          detail.append(holder.value());
        }
        return Result<std::unique_ptr<DurableStore>>::failure(ErrorCode::StoreLocked, std::move(detail));
      }
      return Result<std::unique_ptr<DurableStore>>::failure(lock.error());
    }
    store->impl_->lock = std::move(lock).value();
    store->impl_->incarnation = incarnation;
  }

  std::vector<std::filesystem::path> transient_files;
  auto files = scan_generation_files(root, &transient_files);
  if (!files) {
    return Result<std::unique_ptr<DurableStore>>::failure(files.error());
  }
  if (!store->impl_->read_only) {
    for (const auto& path : transient_files) {
      if (const auto status = detail::remove_file(path); status.ok()) {
        ++out_report.transient_files_removed;
      }
    }
  } else {
    out_report.transient_files_removed = 0;
  }

  bool pointer_present = detail::path_exists(store->impl_->pointer_path);
  std::optional<PointerRecord> pointer;
  if (pointer_present) {
    const auto text = detail::read_file_bounded(store->impl_->pointer_path, kPointerMaxBytes);
    if (text) {
      const std::string_view view{reinterpret_cast<const char*>(text.value().data()), text.value().size()};
      auto parsed = parse_pointer(view);
      if (parsed) {
        pointer = parsed.value();
      } else {
        out_report.diagnostics.push_back("the pointer file was rejected: " + parsed.error().detail());
      }
    } else {
      out_report.diagnostics.push_back("the pointer file could not be read: " + text.error().detail());
    }
  }

  const auto& retained = files.value();
  std::optional<RegistrySnapshot> loaded;
  ContentDigest loaded_file_digest{};
  if (pointer.has_value()) {
    const auto position =
        std::find_if(retained.begin(), retained.end(), [&pointer](const RetainedFile& file) {
          return file.generation == pointer->generation;
        });
    if (position == retained.end()) {
      out_report.diagnostics.push_back("the pointer names generation " +
                                       facility_dependency_registry::to_text(pointer->generation) +
                                       " but no such generation file is present");
      out_report.rejected_generations.push_back(pointer->generation);
    } else {
      ContentDigest file_digest{};
      auto candidate = load_generation_file(position->path, pointer->generation, options.limits, &file_digest);
      if (candidate && file_digest != pointer->digest) {
        // The pointer's digest is part of the pointer, not of the generation:
        // when the two disagree the pointer is not trustworthy, and the store
        // recovers through the scan below rather than believing it.
        out_report.diagnostics.push_back("the pointer's digest does not describe generation " +
                                         facility_dependency_registry::to_text(pointer->generation));
        out_report.rejected_generations.push_back(pointer->generation);
      } else if (candidate) {
        loaded = std::move(candidate).value();
        loaded_file_digest = file_digest;
        out_report.outcome = RecoveryOutcome::LoadedCurrent;
      } else {
        out_report.diagnostics.push_back("generation " +
                                         facility_dependency_registry::to_text(pointer->generation) +
                                         " named by the pointer was rejected: " + candidate.error().detail());
        out_report.rejected_generations.push_back(pointer->generation);
      }
    }
  }

  if (!loaded.has_value()) {
    for (const auto& file : retained) {
      ContentDigest file_digest{};
      auto candidate = load_generation_file(file.path, file.generation, options.limits, &file_digest);
      if (candidate) {
        loaded = std::move(candidate).value();
        loaded_file_digest = file_digest;
        // Reaching this loop at all means the pointer did not name a usable
        // generation, so the state is recovered from history rather than from
        // the pointer: that is a degraded recovery even though it succeeded.
        out_report.outcome = RecoveryOutcome::LoadedFallback;
        break;
      }
      out_report.diagnostics.push_back("generation " + facility_dependency_registry::to_text(file.generation) +
                                       " was rejected: " + candidate.error().detail());
      out_report.rejected_generations.push_back(file.generation);
    }
  }

  if (!loaded.has_value()) {
    // A store root with neither a generation file nor a pointer file has never
    // held state, and opening it as empty invents nothing. Anything else is
    // refused: an empty graph is never presented in place of state that exists
    // but cannot be read.
    if (retained.empty() && !pointer_present) {
      out_report.outcome = RecoveryOutcome::FreshEmpty;
      out_snapshot = RegistrySnapshot::empty(options.limits);
      out_report.loaded_generation = kInitialGeneration;
      store->impl_->committed = kInitialGeneration;
      return store;
    }
    out_report.outcome = RecoveryOutcome::RefusedCorrupt;
    std::string detail{"no usable generation was found under " + detail::to_utf8(root)};
    for (const auto& note : out_report.diagnostics) {
      detail.append("; ");
      detail.append(note);
    }
    return Result<std::unique_ptr<DurableStore>>::failure(ErrorCode::StoreCorrupt, std::move(detail));
  }

  out_report.loaded_generation = loaded->generation();
  store->impl_->committed = loaded->generation();
  out_snapshot = std::move(*loaded);

  if (!store->impl_->read_only) {
    out_report.generations_pruned =
        prune_generations(retained, out_report.loaded_generation, options.limits.max_retained_generations);
  }
  // A read-only open never repairs the pointer, even when a fallback was used:
  // inspection must not change what it inspects.
  if (!store->impl_->read_only && out_report.outcome == RecoveryOutcome::LoadedFallback &&
      !loaded_file_digest.is_zero()) {
    // The digest the pointer carries is the digest of the generation file, so
    // the repair reuses the digest of the file that was actually loaded rather
    // than of some re-encoding of it.
    const std::string pointer_text = render_pointer(out_snapshot.generation(), loaded_file_digest);
    const std::filesystem::path temporary =
        root / std::filesystem::path{std::string{kTransientFilePrefix} + detail::random_token() + ".ptr"};
    const auto bytes = std::span<const std::byte>{reinterpret_cast<const std::byte*>(pointer_text.data()),
                                                  pointer_text.size()};
    if (const auto status = detail::write_new_file(temporary, bytes); status.ok()) {
      if (const auto replaced = detail::replace_file(temporary, store->impl_->pointer_path); !replaced.ok()) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        out_report.diagnostics.push_back("the pointer could not be repaired: " + replaced.error().detail());
      }
    } else {
      out_report.diagnostics.push_back("the pointer could not be repaired: " + status.error().detail());
    }
  }
  return store;
}

Result<RegistrySnapshot> DurableStore::load() const {
  if (impl_ == nullptr) {
    return Result<RegistrySnapshot>::failure(ErrorCode::StoreLayoutInvalid, "the store is not open");
  }
  const auto pointer_text = detail::read_file_bounded(impl_->pointer_path, kPointerMaxBytes);
  if (!pointer_text) {
    return Result<RegistrySnapshot>::failure(pointer_text.error());
  }
  const std::string_view view{reinterpret_cast<const char*>(pointer_text.value().data()),
                              pointer_text.value().size()};
  auto pointer = parse_pointer(view);
  if (!pointer) {
    return Result<RegistrySnapshot>::failure(pointer.error());
  }
  return load_generation(pointer.value().generation);
}

Result<RegistrySnapshot> DurableStore::load_generation(DependencyGeneration generation) const {
  if (generation.value() > impl_->options.limits.max_generation) {
    return Result<RegistrySnapshot>::failure(ErrorCode::InvalidGeneration,
                                             "the requested generation is beyond the configured maximum");
  }
  const auto path = impl_->root / std::filesystem::path{generation_file_name(generation)};
  if (!detail::path_exists(path)) {
    return Result<RegistrySnapshot>::failure(ErrorCode::StoreNotFound,
                                             "generation " + facility_dependency_registry::to_text(generation) +
                                                 " is not retained in this store");
  }
  return load_generation_file(path, generation, impl_->options.limits, nullptr);
}

Result<std::vector<DependencyGeneration>> DurableStore::retained_generations() const {
  auto files = scan_generation_files(impl_->root, nullptr);
  if (!files) {
    return Result<std::vector<DependencyGeneration>>::failure(files.error());
  }
  std::vector<DependencyGeneration> result;
  result.reserve(files.value().size());
  for (const auto& file : files.value()) {
    result.push_back(file.generation);
  }
  return result;
}

Result<StoreStatus> DurableStore::status() const {
  StoreStatus status;
  status.root = impl_->root;
  status.read_only = impl_->read_only;
  status.writer_lock_held = impl_->lock.held();
  if (const auto record = detail::read_lock_record(impl_->lock_path); record && !record.value().empty()) {
    if (auto parsed = WriterIncarnation::parse(record.value()); parsed) {
      status.lock_incarnation = parsed.value();
    }
  }
  auto files = scan_generation_files(impl_->root, nullptr);
  if (!files) {
    return Result<StoreStatus>::failure(files.error());
  }
  for (const auto& file : files.value()) {
    status.retained_generations.push_back(file.generation);
    status.total_bytes += file.bytes;
  }
  const auto pointer_text = detail::read_file_bounded(impl_->pointer_path, kPointerMaxBytes);
  if (pointer_text) {
    const std::string_view view{reinterpret_cast<const char*>(pointer_text.value().data()),
                                pointer_text.value().size()};
    if (auto pointer = parse_pointer(view); pointer) {
      status.pointer_present = true;
      status.pointer_generation = pointer.value().generation;
    }
  }
  std::error_code error;
  std::filesystem::directory_iterator iterator{impl_->root, error};
  if (!error) {
    for (const auto& entry : iterator) {
      const std::string name = detail::to_utf8(entry.path().filename());
      if (name.rfind(std::string{kTransientFilePrefix}, 0) == 0) {
        ++status.transient_files;
      }
    }
  }
  return status;
}

Status DurableStore::publish(const RegistrySnapshot& next) {
  if (impl_ == nullptr) {
    return Status::failure(ErrorCode::StoreLayoutInvalid, "the store is not open");
  }
  if (impl_->read_only) {
    return Status::failure(ErrorCode::StoreReadOnly, "a read-only store cannot publish a generation");
  }
  if (!impl_->lock.held()) {
    return Status::failure(ErrorCode::WriterFenced,
                           "this session no longer holds the writer lock on " + detail::to_utf8(impl_->root));
  }
  DependencyGeneration expected_next = impl_->committed;
  if (!try_increment(expected_next)) {
    return Status::failure(ErrorCode::GenerationExhausted, "the committed generation cannot advance further");
  }
  if (next.generation() != expected_next) {
    return Status::failure(ErrorCode::StaleGeneration,
                           "publication expected generation " +
                               facility_dependency_registry::to_text(expected_next) + " but was offered " +
                               facility_dependency_registry::to_text(next.generation()));
  }

  // Re-read the pointer: if it no longer names the generation this session
  // committed, another writer has taken over and this one is fenced off.
  if (impl_->committed.value() != 0) {
    const auto pointer_text = detail::read_file_bounded(impl_->pointer_path, kPointerMaxBytes);
    if (!pointer_text) {
      return Status::failure(ErrorCode::WriterFenced,
                             "the store pointer could not be read: " + pointer_text.error().detail());
    }
    const std::string_view view{reinterpret_cast<const char*>(pointer_text.value().data()),
                                pointer_text.value().size()};
    auto pointer = parse_pointer(view);
    if (!pointer) {
      return Status::failure(ErrorCode::WriterFenced,
                             "the store pointer is no longer readable by this writer: " +
                                 pointer.error().detail());
    }
    if (pointer.value().generation != impl_->committed) {
      return Status::failure(ErrorCode::WriterFenced,
                             "the store pointer names generation " +
                                 facility_dependency_registry::to_text(pointer.value().generation) +
                                 " but this writer committed " +
                                 facility_dependency_registry::to_text(impl_->committed));
    }
  }

  const PublishFaultHooks& hooks = impl_->options.faults;
  const auto fire = [&hooks](PublishStep step, DependencyGeneration generation) {
    if (hooks.after_step) {
      hooks.after_step(step, generation);
    }
  };

  fire(PublishStep::Begun, next.generation());

  const auto payload = next.encode();
  if (!payload) {
    return Status::failure(payload.error());
  }
  const auto file_bytes = encode_container(payload.value());
  if (file_bytes.size() > impl_->options.limits.max_persisted_bytes) {
    return Status::failure(ErrorCode::PayloadTooLarge,
                           "the encoded generation is larger than the configured maximum of " +
                               std::to_string(impl_->options.limits.max_persisted_bytes) + " bytes");
  }

  const std::string token = detail::random_token();
  const std::filesystem::path temporary =
      impl_->root / std::filesystem::path{std::string{kTransientFilePrefix} + token + ".tmp"};
  if (const auto status = detail::write_new_file(temporary, file_bytes); !status.ok()) {
    return status;
  }
  fire(PublishStep::TransientWritten, next.generation());

  if (const auto status = detail::sync_file(temporary); !status.ok()) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return status;
  }
  fire(PublishStep::TransientSynced, next.generation());

  const auto reread = detail::read_file_bounded(temporary, impl_->options.limits.max_persisted_bytes);
  if (!reread) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return Status::failure(reread.error());
  }
  if (reread.value().size() != file_bytes.size() ||
      std::memcmp(reread.value().data(), file_bytes.data(), file_bytes.size()) != 0) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return Status::failure(ErrorCode::StoreIntegrityFailure,
                           "the transient generation file does not match the bytes that were written");
  }
  if (const auto verified = verify_container(reread.value(), impl_->options.limits.max_persisted_bytes);
      !verified.has_value()) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return Status::failure(verified.error());
  }
  fire(PublishStep::TransientVerified, next.generation());

  const std::filesystem::path generation_path =
      impl_->root / std::filesystem::path{generation_file_name(next.generation())};
  if (const auto status = detail::replace_file(temporary, generation_path); !status.ok()) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return status;
  }
  fire(PublishStep::GenerationRenamed, next.generation());

  if (const auto status = detail::sync_directory(impl_->root); !status.ok()) {
    return status;
  }
  fire(PublishStep::DirectorySynced, next.generation());

  const auto digest = digest_of_file(reread.value());
  const std::string pointer_text = render_pointer(next.generation(), digest);
  const std::filesystem::path pointer_temporary =
      impl_->root / std::filesystem::path{std::string{kTransientFilePrefix} + token + ".ptr"};
  const auto pointer_bytes = std::span<const std::byte>{reinterpret_cast<const std::byte*>(pointer_text.data()),
                                                        pointer_text.size()};
  if (const auto status = detail::write_new_file(pointer_temporary, pointer_bytes); !status.ok()) {
    return status;
  }
  if (const auto status = detail::replace_file(pointer_temporary, impl_->pointer_path); !status.ok()) {
    std::error_code ignored;
    std::filesystem::remove(pointer_temporary, ignored);
    return status;
  }
  fire(PublishStep::PointerReplaced, next.generation());

  if (const auto status = detail::sync_directory(impl_->root); !status.ok()) {
    return status;
  }
  fire(PublishStep::PointerSynced, next.generation());

  impl_->committed = next.generation();

  auto retained = scan_generation_files(impl_->root, nullptr);
  if (retained) {
    // The generation just published is the newest one, so it is inside the
    // window by construction; naming it explicitly keeps the rule in one place.
    static_cast<void>(prune_generations(retained.value(), next.generation(),
                                        impl_->options.limits.max_retained_generations));
  }
  fire(PublishStep::Pruned, next.generation());
  fire(PublishStep::Completed, next.generation());
  return Status::success();
}

Status DurableStore::close() {
  if (impl_ == nullptr) {
    return Status::success();
  }
  impl_->lock.release();
  impl_->closed = true;
  return Status::success();
}

}  // namespace facility_dependency_registry
