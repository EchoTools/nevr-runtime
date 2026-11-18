// The decisions of the PC's CDN start that need no Windows, no game and no network (lifecycle/cdn_start.cpp reads the file, the running
// runtime and the published sets, and asks these): whether a pending self-update makes this start the trial (PLAN-v7 5.4 "The
// self-update trial", G9), and whether a mounted set is one the read hook can serve. Header-only, so a host test drives it.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/update/engine_start.h"
#include "core/update/fs.h"

namespace nevr_cdn_start {

struct TrialDecision {
  bool trial = false;
  /// Non-null when the start is taken as the trial because pending.json exists and cannot be used: `unreadable`, `too_big`, `unparsed` or
  /// `runtime_unhashed` (the log's `cause`). Null when the answer comes from the record itself, or there is no file.
  const char* cause = nullptr;
};

/// `read` and `text` are what the edge read of update\pending.json; `hash_runtime` answers the running runtime's sha256 (false when it cannot
/// be hashed) and is asked only once the file parses. A file that is absent is not a trial; one that exists and cannot be used is.
inline TrialDecision DecideLastUpdateTrial(nevr_update::ReadResult read, const std::string& text,
                                           const std::function<bool(std::string* sha256)>& hash_runtime) {
  TrialDecision out;
  if (read == nevr_update::ReadResult::kAbsent) {
    out.trial = nevr_update::LastUpdateTrial(std::nullopt, std::string_view());
    return out;
  }
  std::string runtime_sha256;
  nlohmann::json doc;
  nevr_update::Pending pending;
  if (read == nevr_update::ReadResult::kUnreadable) {
    out.cause = "unreadable";
  } else if (read == nevr_update::ReadResult::kTooBig) {
    out.cause = "too_big";
  } else if (!nevr_update::ParseJsonText(text, &doc) || !nevr_update::ParsePending(doc, &pending)) {
    out.cause = "unparsed";
  } else if (!hash_runtime(&runtime_sha256)) {
    out.cause = "runtime_unhashed";
  }
  if (out.cause != nullptr) {
    out.trial = true;
    return out;
  }
  out.trial = nevr_update::LastUpdateTrial(std::string_view(text), runtime_sha256);
  return out;
}

/// A server's set (its boot set or a session's demand set) is handed to the read hook only when it holds a shadow: a set with nothing
/// mounted is not published, and the one before it stays.
template <typename Set>
bool HoldsShadow(const Set* set) {
  return set != nullptr && set->shadow != nullptr;
}

}  // namespace nevr_cdn_start
