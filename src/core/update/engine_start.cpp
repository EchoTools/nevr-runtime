#include "core/update/engine_start.h"

#include "core/update/names.h"

#include <cstddef>
#include <string>
#include <utility>

namespace nevr_update {
namespace {

namespace names = nevr_update_names;
using nlohmann::json;

/// The longest reason word IsReasonToken accepts (ADR 0024, section 4.9's journal and pending.json reasons).
constexpr std::size_t kMaxReasonChars = 40;

/// The reasons the decision gives on its own account, before a load outcome or the host names one.
constexpr char kReasonCrashed[] = "crashed";
constexpr char kReasonUnconfirmed[] = "unconfirmed";

// True when the text's arrays and objects, outside strings, nest at most `max` deep (see ParseJsonText).
bool NestedAtMost(std::string_view text, std::size_t max) {
  std::size_t depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (const char c : text) {
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
    } else if (c == '"') {
      in_string = true;
    } else if (c == '[' || c == '{') {
      if (++depth > max) return false;
    } else if ((c == ']' || c == '}') && depth > 0) {
      --depth;
    }
  }
  return true;
}

}  // namespace

bool IsSha256Hex(std::string_view text) noexcept {
  if (text.size() != kSha256HexChars) return false;
  for (const char c : text) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

bool IsReasonToken(std::string_view text) noexcept {
  if (text.empty() || text.size() > kMaxReasonChars) return false;
  for (const char c : text) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

bool ParseJsonText(std::string_view text, json* out) {
  if (text.size() > names::kMaxJsonBytes || !NestedAtMost(text, names::kMaxJsonDepth)) return false;
  *out = json::parse(std::string(text), nullptr, false);
  return !out->is_discarded();
}

bool ParsePending(const json& doc, Pending* out) {
  if (!doc.is_object() || !doc.contains("to") || !doc["to"].is_object() || !doc.contains("boot_attempts") ||
      !doc["boot_attempts"].is_number_unsigned()) {
    return false;
  }
  const json& to = doc["to"];
  if (!to.contains("version") || !to["version"].is_string() || !to.contains("sha256") || !to["sha256"].is_string()) return false;
  out->to_version = to["version"].get<std::string>();
  out->to_sha256 = to["sha256"].get<std::string>();
  if (!names::IsVersionName(out->to_version) || !IsSha256Hex(out->to_sha256)) return false;
  out->boot_attempts = doc["boot_attempts"].get<std::uint64_t>();
  if (doc.contains("from") && !doc["from"].is_null()) {
    const json& from = doc["from"];
    if (!from.is_object() || !from.contains("version") || !from["version"].is_string() || !from.contains("sha256") ||
        !from["sha256"].is_string()) {
      return false;
    }
    out->from_version = from["version"].get<std::string>();
    out->from_sha256 = from["sha256"].get<std::string>();
    if (!names::IsVersionName(out->from_version) || !IsSha256Hex(out->from_sha256)) return false;
    out->has_from = true;
  }
  if (doc.contains("failed_reason") && !doc["failed_reason"].is_null()) {
    if (!doc["failed_reason"].is_string()) return false;
    out->failed_reason = doc["failed_reason"].get<std::string>();
    if (!IsReasonToken(out->failed_reason)) return false;
  }
  return true;
}

PendingFacts ReadPendingFacts(const UpdateFs& fs, const std::string& update_dir, std::string root_runtime_sha256,
                              bool finished_swap) {
  PendingFacts facts;
  const std::string separator(1, fs.Separator());
  facts.pending_read = fs.ReadSmallFile(update_dir + separator + names::kPendingJson, &facts.pending_text);
  // The error of this read is taken before the marker is asked for: a later call on the same UpdateFs may replace it.
  if (facts.pending_read == ReadResult::kUnreadable) facts.pending_os_error = static_cast<std::uint32_t>(fs.LastOsError());
  facts.crashed = fs.Exists(update_dir + separator + names::kCrashedFile);
  facts.root_runtime_sha256 = std::move(root_runtime_sha256);
  facts.finished_swap = finished_swap;
  return facts;
}

PendingDecision DecidePending(const PendingFacts& facts, const std::function<WriteAccess()>& write_access) {
  PendingDecision out;
  switch (facts.pending_read) {
    case ReadResult::kAbsent:
      out.action = PendingAction::kNone;
      return out;
    case ReadResult::kUnreadable:
      // Not damage: the record is there and something keeps this start from it. Nothing is judged, counted or swapped.
      out.action = PendingAction::kUnreadable;
      out.os_error = facts.pending_os_error;
      return out;
    case ReadResult::kTooBig:
      out.action = PendingAction::kDamaged;
      return out;
    case ReadResult::kOk:
      break;
  }
  json doc;
  // A record the host did not write names nothing the host can put back: it is dropped by the edge (see kDamaged).
  if (!ParseJsonText(facts.pending_text, &doc) || !ParsePending(doc, &out.pending)) {
    out.action = PendingAction::kDamaged;
    return out;
  }
  const Pending& pending = out.pending;
  // The root holds a runtime this record is not about: the host moves no file it did not place (ADR 0024, section 4.7).
  if (!facts.root_runtime_sha256.empty() && facts.root_runtime_sha256 != pending.to_sha256) {
    out.action = PendingAction::kStale;
    return out;
  }
  std::string reason = pending.failed_reason;
  if (reason.empty() && facts.crashed) reason = kReasonCrashed;
  if (reason.empty() && pending.boot_attempts >= kBootAttemptsToRollBack) reason = kReasonUnconfirmed;
  // The write access is asked once, after the record is judged not stale, in every branch that follows.
  const WriteAccess runtime = write_access();
  if (!reason.empty()) {
    out.reason = reason;
    if (runtime.kind == WriteAccess::Kind::kInUse) {
      // Another process runs the pending release, perhaps inside its ten seconds: it is not condemned (R2).
      out.action = PendingAction::kDeferredInUse;
    } else if (runtime.kind == WriteAccess::Kind::kNotWritable) {
      out.action = PendingAction::kBlockedNotWritable;
      out.os_error = runtime.os_error;
    } else {
      out.action = PendingAction::kRollBack;
    }
    return out;
  }
  // One more try. A start beside a running one is not a try, and neither is the load that follows a swap whose cleanup
  // this start finished: the swap's own start never got to load.
  if (runtime.kind == WriteAccess::Kind::kInUse) {
    out.action = PendingAction::kNotCountedInUse;
  } else if (runtime.kind == WriteAccess::Kind::kNotWritable) {
    out.action = PendingAction::kNotCountedNotWritable;
    out.os_error = runtime.os_error;
  } else if (facts.finished_swap) {
    out.action = PendingAction::kNotCountedSwapFinished;
  } else {
    out.action = PendingAction::kCountTry;
    out.counted = doc;
    out.counted["boot_attempts"] = pending.boot_attempts + 1;
  }
  return out;
}

}  // namespace nevr_update
