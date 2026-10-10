#pragma once
// The platform-neutral parts of the hardware dump's report (#335): when to dump, the libr15_queries section,
// and the one log line per write. Host-tested.

#include "quest/diag/hwdump_install.h"
#include "quest/diag/hwdump_records.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace nevr_quest::hwdump {

// The dump waits for the game: 30 s after vrapi_Initialize was seen, or 180 s after the sentinel started when
// it never is (the VrApi fields then say why).
inline constexpr std::uint64_t kSettleAfterInitializeMs = 30 * 1000;
inline constexpr std::uint64_t kFallbackAfterStartMs = 180 * 1000;

struct DumpDecision {
  bool due = false;
  const char* reason = "waiting";  // "after_vrapi_initialize" | "fallback_no_vrapi_initialize" | "waiting"
};

DumpDecision Decide(std::uint64_t now_ms, std::uint64_t start_ms, bool initialize_seen, std::uint64_t initialize_ms);

// libr15_queries: every hook's install state and counters, every recorded query, and what is not hooked.
nlohmann::json ComposeQueries(const RecordSnapshot* records, std::size_t num_records, const HookState* hooks,
                              std::size_t num_hooks, std::uint64_t overflow, std::uint64_t contended);

struct WriteSummary {
  std::string stage;
  std::string path;
  bool written = false;
  std::string write_error;  // set when written is false (AtomicWrite's error text, no value)
  std::size_t fields = 0;
  std::size_t failed = 0;
  std::size_t hooks_installed = 0;
  std::size_t hooks_total = 0;
  std::uint64_t hook_faults = 0;
  std::size_t records = 0;
  std::uint64_t overflow = 0;
};

// The one line logged per write. Counts and the path only: never a value from the dump.
std::string LogLine(const WriteSummary& s);

}  // namespace nevr_quest::hwdump
