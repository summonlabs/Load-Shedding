#include "load_shedding/history.hpp"

#include <string>

namespace load_shedding {
namespace {

struct AuditKindName {
  const char* name;
  AuditKind kind;
};

constexpr AuditKindName kAuditNames[] = {
    {"store-opened", AuditKind::StoreOpened},
    {"store-recovered", AuditKind::StoreRecovered},
    {"authority-taken", AuditKind::AuthorityTaken},
    {"policy-installed", AuditKind::PolicyInstalled},
    {"load-upserted", AuditKind::LoadUpserted},
    {"load-removed", AuditKind::LoadRemoved},
    {"obligation-upserted", AuditKind::ObligationUpserted},
    {"obligation-removed", AuditKind::ObligationRemoved},
    {"snapshot-replaced", AuditKind::SnapshotReplaced},
    {"plan-committed", AuditKind::PlanCommitted},
    {"plan-replayed", AuditKind::PlanReplayed},
    {"plan-refused", AuditKind::PlanRefused},
    {"recovery-decided", AuditKind::RecoveryDecided},
    {"effect-observed", AuditKind::EffectObserved},
    {"effect-refused", AuditKind::EffectRefused},
};

}  // namespace

std::string_view to_string(AuditKind value) noexcept {
  for (const AuditKindName& entry : kAuditNames) {
    if (entry.kind == value) {
      return entry.name;
    }
  }
  return "unknown";
}

Result<AuditKind> audit_kind_from_string(std::string_view text) {
  for (const AuditKindName& entry : kAuditNames) {
    if (text == entry.name) {
      return entry.kind;
    }
  }
  return Status::error(StatusCode::InvalidArgument, "unknown audit kind '" + std::string(text) + "'");
}

}  // namespace load_shedding
