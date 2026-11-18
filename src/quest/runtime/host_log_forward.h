#pragma once

// How the Quest runtime library hands its records to the shim's log (runtime_entry.cpp): the sentinel's level numbering mapped to
// the ABI's (core/quest_runtime_abi.h, NevrQuestLogFn) and the update engine's to the sentinel's, and the forwarding of one
// record. Split out of runtime_entry.cpp, which has a library entry point and a constructor sequence no host test can start, so
// the host test (quest/tests/host_log_forward_test.cpp) calls them with a fake NevrQuestLogFn.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/quest_runtime_abi.h"
#include "core/update/engine_check.h"
#include "quest/sentinel/hook_log.h"
#include "quest/shim/runtime_load.h"

namespace nevr_quest::host_log_forward {

// The most fields one forwarded record carries, which sizes the stack arrays of ForwardRecord; a longer list is cut and the record
// says so (a `truncated` field).
inline constexpr std::size_t kMaxForwardedFields = 48;
// Digits of a long long, a sign and the terminator, with room to spare: an integer field's text.
inline constexpr std::size_t kIntegerTextBytes = 24;
// The keys the shim's RuntimeLog (sentinel/entry.cpp) turns back into the record's tag and message, and the field that marks a cut.
inline constexpr char kTagKey[] = "tag";
inline constexpr char kMessageKey[] = "message";
inline constexpr char kTruncatedKey[] = "truncated";
inline constexpr char kTruncatedValue[] = "true";

// The level number of NevrQuestLogFn (core/quest_runtime_abi.h) for a sentinel level.
inline std::uint32_t HostLevel(sentinel::LogLevel level) noexcept {
  namespace rl = nevr_quest::runtime_load;
  switch (level) {
    case sentinel::LogLevel::kDebug: return rl::kLogLevelDebug;
    case sentinel::LogLevel::kWarn: return rl::kLogLevelWarning;
    case sentinel::LogLevel::kError: return rl::kLogLevelError;
    case sentinel::LogLevel::kInfo: break;
  }
  return rl::kLogLevelInfo;
}

// The sentinel level of an update engine level (core/update/engine_check.h: 0 debug, 1 info, 2 warning, 3 error).
inline sentinel::LogLevel UpdateLevel(int level) noexcept {
  switch (level) {
    case nevr_update::kDebug: return sentinel::LogLevel::kDebug;
    case nevr_update::kWarning: return sentinel::LogLevel::kWarn;
    case nevr_update::kError: return sentinel::LogLevel::kError;
    default: break;
  }
  return sentinel::LogLevel::kInfo;
}

// One record to the shim's log `hostLog`. The first field keyed "tag" is the call's tag and the first keyed "message" its message,
// which is how the shim's RuntimeLog puts them back; every other field follows in order. The ABI carries text only, so an integer
// field arrives as its decimal text. Returns true only when `hostLog` was called; false (a null `hostLog`) leaves the record to the
// copy's own writer, which here is logcat.
inline bool ForwardRecord(NevrQuestLogFn hostLog, sentinel::LogLevel level, const char* event, const sentinel::LogField* fields,
                          std::size_t count) {
  if (hostLog == nullptr) return false;
  NevrQuestLogField forwarded[kMaxForwardedFields + 1];  // one more than the cap: the cut marker
  char integers[kMaxForwardedFields][kIntegerTextBytes];
  const char* tag = nullptr;
  const char* message = nullptr;
  std::uint32_t kept = 0;
  bool truncated = false;
  for (std::size_t i = 0; i < count; ++i) {
    if (i >= kMaxForwardedFields) {
      truncated = true;
      break;
    }
    const sentinel::LogField& field = fields[i];
    if (field.key == nullptr) continue;
    const char* value = field.text;
    if (value == nullptr) {
      std::snprintf(integers[i], sizeof(integers[i]), "%lld", field.number);
      value = integers[i];
    }
    if (tag == nullptr && std::strcmp(field.key, kTagKey) == 0) {
      tag = value;
    } else if (message == nullptr && std::strcmp(field.key, kMessageKey) == 0) {
      message = value;
    } else {
      forwarded[kept++] = NevrQuestLogField{field.key, value};
    }
  }
  if (truncated) forwarded[kept++] = NevrQuestLogField{kTruncatedKey, kTruncatedValue};
  hostLog(HostLevel(level), tag, event, message, forwarded, kept);
  return true;
}

}  // namespace nevr_quest::host_log_forward
