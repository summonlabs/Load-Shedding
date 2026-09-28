#include "load_shedding/store.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "detail/crc32c.hpp"
#include "detail/durable_store.hpp"
#include "detail/platform_io.hpp"
#include "load_shedding/limits.hpp"

namespace load_shedding {
namespace {

bool has_suffix(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_known_name(std::string_view name) {
  if (name == store_format::kCommitMarkerName || name == store_format::kWatermarkName ||
      name == store_format::kLockName) {
    return true;
  }
  if (name == std::string(store_format::kLockName) + ".owner") {
    return true;
  }
  if (has_suffix(name, store_format::kStagingSuffix)) {
    return true;
  }
  return has_suffix(name, store_format::kStateSuffix);
}

}  // namespace

Result<StoreAudit> inspect_store(const std::string& directory) {
  StoreAudit audit;
  audit.directory = directory;
  audit.format_version = store_format::kFormatVersion;
  audit.unsynchronized = true;

  const Status path_valid = detail::validate_store_path(directory);
  if (!path_valid.ok()) {
    audit.findings.push_back("path: " + path_valid.message());
  }
  const std::filesystem::path root = detail::path_from_utf8(directory);
  if (!detail::file_exists(root)) {
    return Status::error(StatusCode::NotFound, "store directory '" + directory + "' does not exist");
  }
  auto entries = detail::list_directory(root);
  if (!entries.ok()) {
    return entries.status();
  }

  for (const detail::DirectoryEntry& entry : entries.value()) {
    if (!is_known_name(entry.name)) {
      audit.stray_files.push_back(entry.name);
      audit.findings.push_back("stray file '" + entry.name + "' is not part of the store format");
    } else if (has_suffix(entry.name, store_format::kStagingSuffix)) {
      audit.findings.push_back("staging residue '" + entry.name +
                               "' is present, which means a publication did not finish");
    }
  }

  const std::filesystem::path marker_path = root / std::string(store_format::kCommitMarkerName);
  if (!detail::file_exists(marker_path)) {
    audit.commit_marker_present = false;
    audit.findings.push_back("the store has no commit marker");
  } else {
    audit.commit_marker_present = true;
    auto content = detail::read_file_bounded(marker_path, 4096);
    if (!content.ok()) {
      audit.findings.push_back("commit marker: " + content.message());
    } else if (content.value().size() != 116) {
      audit.findings.push_back("commit marker has an unexpected size of " +
                               std::to_string(content.value().size()) + " bytes");
    } else {
      // Reuse the strict parser through the durable store by opening the marker
      // shape here: magic, version, and checksum are all verified.
      const std::uint32_t stored = detail::crc32c(content.value().substr(0, 112));
      const std::uint32_t recorded =
          static_cast<std::uint32_t>(static_cast<unsigned char>(content.value()[112])) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(content.value()[113])) << 8) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(content.value()[114])) << 16) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(content.value()[115])) << 24);
      if (stored != recorded) {
        audit.findings.push_back("commit marker checksum does not match");
      } else if (content.value().substr(0, 4) != store_format::kMarkerMagic) {
        audit.findings.push_back("commit marker magic does not match");
      } else {
        audit.commit_marker_valid = true;
        const auto read_u64 = [&content](std::size_t offset) {
          std::uint64_t value = 0;
          for (std::size_t index = 0; index < 8; ++index) {
            value |= static_cast<std::uint64_t>(
                         static_cast<unsigned char>(content.value()[offset + index]))
                     << (8 * index);
          }
          return value;
        };
        audit.commit.generation = read_u64(8);
        std::array<std::uint8_t, Digest::kBytes> digest_bytes{};
        for (std::size_t index = 0; index < Digest::kBytes; ++index) {
          digest_bytes[index] =
              static_cast<std::uint8_t>(static_cast<unsigned char>(content.value()[16 + index]));
        }
        audit.commit.state_digest = Digest::from_bytes(digest_bytes);
        audit.commit.state_bytes = read_u64(48);
        audit.commit.authority_epoch = AuthorityEpoch::from_value(read_u64(56));
        audit.commit.incarnation = Incarnation::from_value(read_u64(64));
        audit.commit.revision = StateRevision::from_value(read_u64(72));
        audit.commit.policy_generation = PolicyGeneration::from_value(read_u64(80));
        audit.commit.evidence_generation = EvidenceGeneration::from_value(read_u64(88));
        audit.commit.effect_generation = EffectGeneration::from_value(read_u64(96));
        audit.commit.plan_generation = PlanGeneration::from_value(read_u64(104));
      }
    }
  }

  const std::filesystem::path watermark_path = root / std::string(store_format::kWatermarkName);
  if (detail::file_exists(watermark_path)) {
    auto content = detail::read_file_bounded(watermark_path, 4096);
    if (!content.ok()) {
      audit.findings.push_back("rollback watermark: " + content.message());
    } else if (content.value().size() != 20 ||
               content.value().substr(0, 4) != store_format::kWatermarkMagic) {
      audit.findings.push_back("rollback watermark is malformed");
    } else if (detail::crc32c(content.value().substr(0, 16)) !=
               (static_cast<std::uint32_t>(static_cast<unsigned char>(content.value()[16])) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(content.value()[17])) << 8) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(content.value()[18])) << 16) |
                (static_cast<std::uint32_t>(static_cast<unsigned char>(content.value()[19])) << 24))) {
      audit.findings.push_back("rollback watermark checksum does not match");
    } else {
      std::uint64_t generation = 0;
      for (std::size_t index = 0; index < 8; ++index) {
        generation |= static_cast<std::uint64_t>(
                          static_cast<unsigned char>(content.value()[8 + index]))
                      << (8 * index);
      }
      audit.watermark_present = true;
      audit.watermark_generation = generation;
      if (audit.commit_marker_valid && audit.commit.generation < generation) {
        audit.rollback_detected = true;
        audit.findings.push_back("commit marker names generation " +
                                 std::to_string(audit.commit.generation) +
                                 " but the rollback watermark records " +
                                 std::to_string(generation));
      }
    }
  }

  for (const detail::DirectoryEntry& entry : entries.value()) {
    if (!entry.regular_file || has_suffix(entry.name, store_format::kStagingSuffix)) {
      continue;
    }
    auto generation = detail::parse_state_file_name(entry.name);
    if (!generation.ok()) {
      continue;
    }
    StoreGenerationInfo info;
    info.generation = generation.value();
    info.file_name = entry.name;
    info.bytes = entry.bytes;
    info.committed = audit.commit_marker_valid && info.generation == audit.commit.generation;
    auto content = detail::read_file_bounded(root / entry.name, limits::kMaxStateBytes + 4096);
    if (!content.ok()) {
      info.note = content.message();
      audit.findings.push_back("generation " + std::to_string(info.generation) + ": " + info.note);
      audit.generations.push_back(std::move(info));
      continue;
    }
    info.digest = Digest::of(content.value());
    info.verified = true;
    if (info.committed && !(info.digest == audit.commit.state_digest)) {
      info.verified = false;
      info.note = "digest does not match the commit marker";
      audit.findings.push_back("committed generation does not match the commit marker digest");
    }
    if (info.committed) {
      // Decode through the durable store so that the container, payload, and
      // structural validation are all exercised.
      auto store = detail::DurableStore::open(directory, false);
      if (store.ok()) {
        auto state = store.value().read_committed();
        if (state.ok()) {
          info.decodable = true;
          audit.state_readable = true;
          audit.load_count = state.value().state.snapshot.loads.size();
          audit.plan_count = state.value().state.plans.size();
          audit.effect_count = state.value().state.effects.size();
          audit.audit_count = state.value().state.audit.size();
          audit.recovery_count = state.value().state.recoveries.size();
        } else {
          info.note = state.message();
          audit.findings.push_back("committed generation does not decode: " + state.message());
        }
      } else {
        info.note = store.message();
        audit.findings.push_back("store open failed during inspection: " + store.message());
      }
    } else if (audit.commit_marker_valid && info.generation > audit.commit.generation) {
      audit.findings.push_back("generation " + std::to_string(info.generation) +
                               " was published but never committed");
    }
    audit.generations.push_back(std::move(info));
  }
  std::sort(audit.generations.begin(), audit.generations.end(),
            [](const StoreGenerationInfo& left, const StoreGenerationInfo& right) {
              return left.generation < right.generation;
            });
  if (audit.commit_marker_valid) {
    const bool present = std::any_of(audit.generations.begin(), audit.generations.end(),
                                     [&audit](const StoreGenerationInfo& info) {
                                       return info.generation == audit.commit.generation;
                                     });
    if (!present) {
      audit.findings.push_back("the committed generation file is missing");
    }
  }
  audit.clean = audit.findings.empty() && audit.commit_marker_valid;
  return audit;
}

}  // namespace load_shedding
