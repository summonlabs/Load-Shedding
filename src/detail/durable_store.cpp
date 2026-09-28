#include "detail/durable_store.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "detail/codec.hpp"
#include "detail/crc32c.hpp"
#include "detail/platform_io.hpp"
#include "detail/serialization.hpp"
#include "load_shedding/limits.hpp"
#include "load_shedding/text.hpp"

namespace load_shedding::detail {
namespace {

constexpr std::size_t kMarkerBytes = 116;
constexpr std::size_t kWatermarkBytes = 20;
constexpr std::size_t kContainerHeaderBytes = 16;
constexpr std::size_t kMaxMarkerRead = 4096;

std::string zero_padded(std::uint64_t value) {
  std::string digits = std::to_string(value);
  if (digits.size() < 20) {
    digits.insert(digits.begin(), 20 - digits.size(), '0');
  }
  return digits;
}

void apply_crash_point(CrashPoint point, CrashPoint target) {
  if (point != CrashPoint::None && point == target) {
    // Immediate, non-interactive termination: no unwinding, no atexit handlers,
    // no crash dialog.
    terminate_process_now(0xC1);
  }
}

std::string make_container(std::string_view payload) {
  Writer writer;
  writer.bytes(store_format::kStateMagic);
  writer.u32(store_format::kFormatVersion);
  writer.u64(static_cast<std::uint64_t>(payload.size()));
  writer.bytes(payload);
  const Digest digest = Digest::of(writer.bytes_ref());
  writer.raw(digest.bytes().data(), digest.bytes().size());
  return writer.take();
}

Result<std::string_view> unwrap_container(std::string_view content) {
  if (content.size() < kContainerHeaderBytes + Digest::kBytes) {
    return Status::error(StatusCode::Truncated,
                         "state file is " + std::to_string(content.size()) +
                             " bytes, shorter than the minimum container");
  }
  Reader reader(content);
  auto magic = reader.raw(store_format::kStateMagic.size());
  if (!magic.ok()) {
    return magic.status();
  }
  if (magic.value() != store_format::kStateMagic) {
    return Status::error(StatusCode::Corrupt, "state file magic does not match");
  }
  auto version = reader.u32();
  if (!version.ok()) {
    return version.status();
  }
  if (version.value() != store_format::kFormatVersion) {
    return Status::error(StatusCode::Unsupported,
                         "state file format version " + std::to_string(version.value()) +
                             " is not supported by this build");
  }
  auto length = reader.u64();
  if (!length.ok()) {
    return length.status();
  }
  if (length.value() > limits::kMaxStateBytes) {
    return Status::error(StatusCode::Overlong,
                         "state file declares " + std::to_string(length.value()) +
                             " payload bytes, above the bound of " +
                             std::to_string(limits::kMaxStateBytes));
  }
  const std::size_t expected = kContainerHeaderBytes + static_cast<std::size_t>(length.value()) +
                               Digest::kBytes;
  if (content.size() != expected) {
    return Status::error(content.size() < expected ? StatusCode::Truncated : StatusCode::Corrupt,
                         "state file is " + std::to_string(content.size()) +
                             " bytes but declares " + std::to_string(expected));
  }
  auto payload = reader.raw(static_cast<std::size_t>(length.value()));
  if (!payload.ok()) {
    return payload.status();
  }
  auto stored = reader.raw(Digest::kBytes);
  if (!stored.ok()) {
    return stored.status();
  }
  std::array<std::uint8_t, Digest::kBytes> stored_bytes{};
  for (std::size_t index = 0; index < Digest::kBytes; ++index) {
    stored_bytes[index] = static_cast<std::uint8_t>(static_cast<unsigned char>(stored.value()[index]));
  }
  const Digest computed = Digest::of(content.substr(0, kContainerHeaderBytes + length.value()));
  if (!(computed == Digest::from_bytes(stored_bytes))) {
    return Status::error(StatusCode::Corrupt, "state file digest does not match its content");
  }
  const Status end = reader.expect_end();
  if (!end.ok()) {
    return end;
  }
  return payload.value();
}

std::string make_marker(const CommitRecord& record) {
  Writer writer;
  writer.bytes(store_format::kMarkerMagic);
  writer.u32(store_format::kFormatVersion);
  writer.u64(record.generation);
  writer.raw(record.state_digest.bytes().data(), record.state_digest.bytes().size());
  writer.u64(record.state_bytes);
  writer.u64(record.authority_epoch.value());
  writer.u64(record.incarnation.value());
  writer.u64(record.revision.value());
  writer.u64(record.policy_generation.value());
  writer.u64(record.evidence_generation.value());
  writer.u64(record.effect_generation.value());
  writer.u64(record.plan_generation.value());
  const std::uint32_t checksum = crc32c(writer.bytes_ref());
  writer.u32(checksum);
  return writer.take();
}

Result<CommitRecord> parse_marker(std::string_view content) {
  if (content.size() != kMarkerBytes) {
    return Status::error(content.size() < kMarkerBytes ? StatusCode::Truncated : StatusCode::Corrupt,
                         "commit marker is " + std::to_string(content.size()) + " bytes, expected " +
                             std::to_string(kMarkerBytes));
  }
  const std::uint32_t stored_crc =
      crc32c(content.substr(0, kMarkerBytes - sizeof(std::uint32_t)));
  Reader reader(content);
  auto magic = reader.raw(store_format::kMarkerMagic.size());
  if (!magic.ok()) {
    return magic.status();
  }
  if (magic.value() != store_format::kMarkerMagic) {
    return Status::error(StatusCode::Corrupt, "commit marker magic does not match");
  }
  auto version = reader.u32();
  if (!version.ok()) {
    return version.status();
  }
  if (version.value() != store_format::kFormatVersion) {
    return Status::error(StatusCode::Unsupported,
                         "commit marker format version " + std::to_string(version.value()) +
                             " is not supported by this build");
  }
  CommitRecord record;
  auto generation = reader.u64();
  if (!generation.ok()) {
    return generation.status();
  }
  record.generation = generation.value();
  auto digest = reader.raw(Digest::kBytes);
  if (!digest.ok()) {
    return digest.status();
  }
  std::array<std::uint8_t, Digest::kBytes> digest_bytes{};
  for (std::size_t index = 0; index < Digest::kBytes; ++index) {
    digest_bytes[index] = static_cast<std::uint8_t>(static_cast<unsigned char>(digest.value()[index]));
  }
  record.state_digest = Digest::from_bytes(digest_bytes);
  auto length = reader.u64();
  if (!length.ok()) {
    return length.status();
  }
  record.state_bytes = length.value();
  auto epoch = reader.u64();
  if (!epoch.ok()) {
    return epoch.status();
  }
  record.authority_epoch = AuthorityEpoch::from_value(epoch.value());
  auto incarnation = reader.u64();
  if (!incarnation.ok()) {
    return incarnation.status();
  }
  record.incarnation = Incarnation::from_value(incarnation.value());
  auto revision = reader.u64();
  if (!revision.ok()) {
    return revision.status();
  }
  record.revision = StateRevision::from_value(revision.value());
  auto policy = reader.u64();
  if (!policy.ok()) {
    return policy.status();
  }
  record.policy_generation = PolicyGeneration::from_value(policy.value());
  auto evidence = reader.u64();
  if (!evidence.ok()) {
    return evidence.status();
  }
  record.evidence_generation = EvidenceGeneration::from_value(evidence.value());
  auto effect = reader.u64();
  if (!effect.ok()) {
    return effect.status();
  }
  record.effect_generation = EffectGeneration::from_value(effect.value());
  auto plan = reader.u64();
  if (!plan.ok()) {
    return plan.status();
  }
  record.plan_generation = PlanGeneration::from_value(plan.value());
  auto checksum = reader.u32();
  if (!checksum.ok()) {
    return checksum.status();
  }
  if (checksum.value() != stored_crc) {
    return Status::error(StatusCode::Corrupt, "commit marker checksum does not match");
  }
  return record;
}

std::string make_watermark(std::uint64_t generation) {
  Writer writer;
  writer.bytes(store_format::kWatermarkMagic);
  writer.u32(store_format::kFormatVersion);
  writer.u64(generation);
  const std::uint32_t checksum = crc32c(writer.bytes_ref());
  writer.u32(checksum);
  return writer.take();
}

Result<std::uint64_t> parse_watermark(std::string_view content) {
  if (content.size() != kWatermarkBytes) {
    return Status::error(StatusCode::Corrupt, "rollback watermark has an unexpected size");
  }
  Reader reader(content);
  auto magic = reader.raw(store_format::kWatermarkMagic.size());
  if (!magic.ok()) {
    return magic.status();
  }
  if (magic.value() != store_format::kWatermarkMagic) {
    return Status::error(StatusCode::Corrupt, "rollback watermark magic does not match");
  }
  auto version = reader.u32();
  if (!version.ok()) {
    return version.status();
  }
  if (version.value() != store_format::kFormatVersion) {
    return Status::error(StatusCode::Unsupported, "rollback watermark format version is not supported");
  }
  auto generation = reader.u64();
  if (!generation.ok()) {
    return generation.status();
  }
  auto checksum = reader.u32();
  if (!checksum.ok()) {
    return checksum.status();
  }
  if (checksum.value() != crc32c(content.substr(0, kWatermarkBytes - sizeof(std::uint32_t)))) {
    return Status::error(StatusCode::Corrupt, "rollback watermark checksum does not match");
  }
  return generation.value();
}

bool has_suffix(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

std::string_view to_string(CrashPoint point) noexcept {
  switch (point) {
    case CrashPoint::None: return "none";
    case CrashPoint::AfterStagingWrite: return "after-staging-write";
    case CrashPoint::AfterStatePublish: return "after-state-publish";
    case CrashPoint::BeforeHeadCommit: return "before-head-commit";
    case CrashPoint::AfterHeadCommit: return "after-head-commit";
    case CrashPoint::AfterWatermark: return "after-watermark";
  }
  return "unknown";
}

Result<CrashPoint> crash_point_from_string(std::string_view text) {
  if (text.empty() || text == "none") return CrashPoint::None;
  if (text == "after-staging-write") return CrashPoint::AfterStagingWrite;
  if (text == "after-state-publish") return CrashPoint::AfterStatePublish;
  if (text == "before-head-commit") return CrashPoint::BeforeHeadCommit;
  if (text == "after-head-commit") return CrashPoint::AfterHeadCommit;
  if (text == "after-watermark") return CrashPoint::AfterWatermark;
  return Status::error(StatusCode::InvalidArgument,
                       "unknown crash point '" + std::string(text) +
                           "'; expected one of none, after-staging-write, after-state-publish, "
                           "before-head-commit, after-head-commit, after-watermark");
}

Result<std::uint64_t> parse_state_file_name(std::string_view name) {
  if (!has_suffix(name, store_format::kStateSuffix)) {
    return Status::error(StatusCode::InvalidArgument, "not a state file name");
  }
  const std::string_view body = name.substr(0, name.size() - store_format::kStateSuffix.size());
  if (body.size() <= store_format::kStatePrefix.size() ||
      body.substr(0, store_format::kStatePrefix.size()) != store_format::kStatePrefix) {
    return Status::error(StatusCode::InvalidArgument, "not a state file name");
  }
  const std::string_view digits = body.substr(store_format::kStatePrefix.size());
  if (digits.size() > 20) {
    return Status::error(StatusCode::OutOfRange, "state file name carries too many digits");
  }
  std::uint64_t generation = 0;
  for (const char digit : digits) {
    if (digit < '0' || digit > '9') {
      return Status::error(StatusCode::InvalidArgument, "state file name is not numeric");
    }
    const std::uint64_t previous = generation;
    generation = generation * 10u + static_cast<std::uint64_t>(digit - '0');
    if (generation < previous) {
      return Status::error(StatusCode::Overflow, "state file generation overflows");
    }
  }
  return generation;
}

std::string DurableStore::state_file_name(std::uint64_t generation) const {
  return std::string(store_format::kStatePrefix) + zero_padded(generation) +
         std::string(store_format::kStateSuffix);
}

std::string DurableStore::state_file_path(std::uint64_t generation) const {
  return directory_ + "/" + state_file_name(generation);
}

Result<DurableStore> DurableStore::open(const std::string& directory, bool create_if_missing) {
  const Status path_valid = validate_store_path(directory);
  if (!path_valid.ok()) {
    return path_valid;
  }
  const std::filesystem::path root = path_from_utf8(directory);
  if (!file_exists(root)) {
    if (!create_if_missing) {
      return Status::error(StatusCode::NotFound, "store directory '" + directory + "' does not exist");
    }
    const Status created = ensure_directory(root);
    if (!created.ok()) {
      return created;
    }
  } else {
    auto plain = is_plain_directory(root);
    if (!plain.ok()) {
      return plain.status();
    }
    if (!plain.value()) {
      return Status::error(StatusCode::Rejected,
                           "store path '" + directory +
                               "' is not a plain directory (it is a link, reparse point, or file)");
    }
  }

  DurableStore store;
  store.directory_ = directory;

  const std::filesystem::path marker_path = root / std::string(store_format::kCommitMarkerName);
  auto entries = list_directory(root);
  if (!entries.ok()) {
    return entries.status();
  }

  if (!file_exists(marker_path)) {
    // No commit marker: the store has never committed. Any published generation
    // present means a first commit was interrupted, and the protocol refuses to
    // guess whether it was meant to be authoritative.
    for (const DirectoryEntry& entry : entries.value()) {
      if (entry.regular_file && has_suffix(entry.name, store_format::kStateSuffix)) {
        return Status::error(StatusCode::Corrupt,
                             "store '" + directory +
                                 "' has no commit marker but contains published generation '" +
                                 entry.name +
                                 "'; refusing to adopt a partially published state");
      }
    }
    for (const DirectoryEntry& entry : entries.value()) {
      if (entry.regular_file && has_suffix(entry.name, store_format::kStagingSuffix)) {
        const Status removed = remove_file(root / entry.name);
        if (!removed.ok()) {
          return removed;
        }
      }
    }
    store.created_ = true;
    store.commit_ = CommitRecord{};
    return store;
  }

  auto marker_content = read_file_bounded(marker_path, kMaxMarkerRead);
  if (!marker_content.ok()) {
    return marker_content.status();
  }
  auto marker = parse_marker(marker_content.value());
  if (!marker.ok()) {
    return marker.status();
  }
  store.commit_ = marker.value();
  if (store.commit_.generation == 0) {
    return Status::error(StatusCode::Corrupt, "commit marker names generation zero");
  }

  // Rollback boundary, checked before the generation is adopted: the marker may
  // never point behind the watermark. Checking first also means a rolled-back
  // store reports the rollback rather than the missing file it implies.
  const std::filesystem::path watermark_path = root / std::string(store_format::kWatermarkName);
  if (file_exists(watermark_path)) {
    auto content = read_file_bounded(watermark_path, kMaxMarkerRead);
    if (!content.ok()) {
      return content.status();
    }
    auto generation = parse_watermark(content.value());
    if (!generation.ok()) {
      return generation.status();
    }
    store.watermark_ = generation.value();
    if (store.commit_.generation < store.watermark_) {
      return Status::error(StatusCode::Corrupt,
                           "commit marker names generation " +
                               std::to_string(store.commit_.generation) +
                               " but the rollback watermark records " +
                               std::to_string(store.watermark_) +
                               "; refusing to adopt a rolled-back state");
    }
  }

  // Adopt the named generation, verifying it whole.
  auto payload = store.load_committed();
  if (!payload.ok()) {
    return payload.status();
  }

  // Residue: unpublished generations newer than the marker, staging files, and
  // generations outside the retained window.
  const std::uint64_t retain_floor =
      store.commit_.generation > (limits::kGenerationsRetained - 1)
          ? store.commit_.generation - (limits::kGenerationsRetained - 1)
          : 0;
  for (const DirectoryEntry& entry : entries.value()) {
    if (!entry.regular_file) {
      continue;
    }
    if (has_suffix(entry.name, store_format::kStagingSuffix)) {
      const Status removed = remove_file(root / entry.name);
      if (!removed.ok()) {
        return removed;
      }
      continue;
    }
    if (!has_suffix(entry.name, store_format::kStateSuffix)) {
      continue;
    }
    auto generation = parse_state_file_name(entry.name);
    if (!generation.ok()) {
      continue;
    }
    if (generation.value() == store.commit_.generation) {
      continue;
    }
    if (generation.value() > store.commit_.generation || generation.value() < retain_floor) {
      const Status removed = remove_file(root / entry.name);
      if (!removed.ok()) {
        return removed;
      }
    }
  }
  return store;
}

Result<StoreState> DurableStore::load_committed() const {
  if (commit_.generation == 0) {
    return Status::error(StatusCode::NotFound, "the store has no committed generation");
  }
  const std::filesystem::path path = path_from_utf8(state_file_path(commit_.generation));
  auto content = read_file_bounded(path, limits::kMaxStateBytes + 4096);
  if (!content.ok()) {
    return content.status();
  }
  if (content.value().size() != commit_.state_bytes) {
    return Status::error(StatusCode::Corrupt,
                         "committed generation is " + std::to_string(content.value().size()) +
                             " bytes but the commit marker records " +
                             std::to_string(commit_.state_bytes));
  }
  if (!(Digest::of(content.value()) == commit_.state_digest)) {
    return Status::error(StatusCode::Corrupt,
                         "committed generation does not match the digest in the commit marker");
  }
  auto payload = unwrap_container(content.value());
  if (!payload.ok()) {
    return payload.status();
  }
  return decode_state(payload.value());
}

Result<StoreOpenOutcome> DurableStore::read_committed() const {
  StoreOpenOutcome outcome;
  outcome.commit = commit_;
  outcome.created = created_;
  if (commit_.generation == 0) {
    return outcome;
  }
  auto state = load_committed();
  if (!state.ok()) {
    return state.status();
  }
  outcome.state = std::move(state.value());
  return outcome;
}

Result<CommitRecord> DurableStore::commit(const StoreState& state, CrashPoint crash_point) {
  const std::uint64_t generation = commit_.generation + 1;
  auto payload = encode_state(state);
  if (!payload.ok()) {
    return payload.status();
  }
  const std::string container = make_container(payload.value());

  CommitRecord record;
  record.generation = generation;
  record.state_digest = Digest::of(container);
  record.state_bytes = static_cast<std::uint64_t>(container.size());
  record.authority_epoch = state.authority_epoch;
  record.incarnation = state.incarnation;
  record.revision = state.revision;
  record.policy_generation = state.policy_generation;
  record.evidence_generation = state.evidence_generation;
  record.effect_generation = state.effect_generation;
  record.plan_generation = state.plan_generation;

  const std::filesystem::path root = path_from_utf8(directory_);
  const std::filesystem::path final_path = path_from_utf8(state_file_path(generation));
  const std::filesystem::path staging_path =
      path_from_utf8(state_file_path(generation) + std::string(store_format::kStagingSuffix));

  Status status = remove_file(staging_path);
  if (!status.ok()) {
    return status;
  }
  status = write_file_durable(staging_path, container);
  if (!status.ok()) {
    return status;
  }
  apply_crash_point(crash_point, CrashPoint::AfterStagingWrite);

  // Read-back verification: never publish bytes that were not read back whole.
  auto read_back = read_file_bounded(staging_path, limits::kMaxStateBytes + 4096);
  if (!read_back.ok()) {
    return read_back.status();
  }
  if (!(Digest::of(read_back.value()) == record.state_digest) ||
      read_back.value().size() != container.size()) {
    const Status removed = remove_file(staging_path);
    (void)removed;
    return Status::error(StatusCode::IoFailure,
                         "staged generation '" + path_utf8(staging_path) +
                             "' did not read back as written");
  }

  status = replace_file_atomic(staging_path, final_path);
  if (!status.ok()) {
    const Status removed = remove_file(staging_path);
    (void)removed;
    return status;
  }
  apply_crash_point(crash_point, CrashPoint::AfterStatePublish);

  const std::filesystem::path marker_path = root / std::string(store_format::kCommitMarkerName);
  const std::filesystem::path marker_staging =
      root / (std::string(store_format::kCommitMarkerName) + std::string(store_format::kStagingSuffix));
  const std::string marker_bytes = make_marker(record);
  status = write_file_durable(marker_staging, marker_bytes);
  if (!status.ok()) {
    return status;
  }
  apply_crash_point(crash_point, CrashPoint::BeforeHeadCommit);

  status = replace_file_atomic(marker_staging, marker_path);
  if (!status.ok()) {
    return status;
  }
  // ---- COMMIT POINT: the generation named by the marker is now authoritative.
  apply_crash_point(crash_point, CrashPoint::AfterHeadCommit);

  const std::filesystem::path watermark_path = root / std::string(store_format::kWatermarkName);
  const std::filesystem::path watermark_staging =
      root / (std::string(store_format::kWatermarkName) + std::string(store_format::kStagingSuffix));
  status = write_file_durable(watermark_staging, make_watermark(generation));
  if (!status.ok()) {
    return status;
  }
  status = replace_file_atomic(watermark_staging, watermark_path);
  if (!status.ok()) {
    return status;
  }
  apply_crash_point(crash_point, CrashPoint::AfterWatermark);

  // Retire safe residue. A failure here does not invalidate the commit.
  auto entries = list_directory(root);
  if (entries.ok()) {
    const std::uint64_t retain_floor =
        generation > (limits::kGenerationsRetained - 1) ? generation - (limits::kGenerationsRetained - 1) : 0;
    for (const DirectoryEntry& entry : entries.value()) {
      if (!entry.regular_file) {
        continue;
      }
      if (has_suffix(entry.name, store_format::kStagingSuffix)) {
        const Status removed = remove_file(root / entry.name);
        (void)removed;
        continue;
      }
      if (!has_suffix(entry.name, store_format::kStateSuffix)) {
        continue;
      }
      auto parsed = parse_state_file_name(entry.name);
      if (!parsed.ok()) {
        continue;
      }
      if (parsed.value() < retain_floor || parsed.value() > generation) {
        const Status removed = remove_file(root / entry.name);
        (void)removed;
      }
    }
  }

  commit_ = record;
  watermark_ = generation;
  return record;
}

}  // namespace load_shedding::detail
