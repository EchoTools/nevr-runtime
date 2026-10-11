#pragma once
// Self-check "party_data_share" (#398, docs/engine/remote-log.md): the game service answers every party data
// share the facade sends, and the answer's symbol names the scope it answers (PartyUpdateSuccess/Failure for
// the party's data, PartyUpdateMemberSuccess/MemberFailure for the sender's own member data;
// nakama server/evr_pipeline_party_data.go). A failure here is the PartyUpdateFailure of #398.
//
// OnServerMessage is called once for each server-to-game message symbol by the platform's existing frame
// observer (PC compat/ws_bridge.cpp ObserveSocialFrames, Quest integration/production_steps.cpp). It needs no
// record of the requests: the first success is reported once, every failure is reported, and both carry the
// running counts so a capped run still says how many shares failed.
//
// Header only on top of the self-check unit (compat/self_check.h) and the party symbol table
// (compat/social_party.h); the caller links self_check.cpp.

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

#include "runtime/compat/self_check.h"
#include "runtime/compat/social_party.h"

namespace nevr_party_share_check {

struct Answer {
  bool member;        // member-scope share (the sender's own member data), else the party's data
  bool success;
  const char* name;   // the answer's symbol name, without the SNS prefix
};

struct Totals {
  std::uint64_t ok = 0;
  std::uint64_t failed = 0;
};

inline std::optional<Answer> AnswerOf(std::uint64_t symbol) {
  static const std::uint64_t kSuccess = nevr_social_party::ReplySymbol("PartyUpdateSuccess");
  static const std::uint64_t kFailure = nevr_social_party::ReplySymbol("PartyUpdateFailure");
  static const std::uint64_t kMemberSuccess = nevr_social_party::ReplySymbol("PartyUpdateMemberSuccess");
  static const std::uint64_t kMemberFailure = nevr_social_party::ReplySymbol("PartyUpdateMemberFailure");
  if (symbol == 0) return std::nullopt;
  if (symbol == kSuccess) return Answer{false, true, "PartyUpdateSuccess"};
  if (symbol == kFailure) return Answer{false, false, "PartyUpdateFailure"};
  if (symbol == kMemberSuccess) return Answer{true, true, "PartyUpdateMemberSuccess"};
  if (symbol == kMemberFailure) return Answer{true, false, "PartyUpdateMemberFailure"};
  return std::nullopt;
}

namespace detail {
struct State {
  std::atomic<std::uint64_t> ok{0};
  std::atomic<std::uint64_t> failed{0};
  std::atomic<bool> firstSuccessReported{false};
};
inline State& S() {
  static State* const state = new State();  // never destroyed (the Quest sentinel registers no atexit)
  return *state;
}
// Register is idempotent by name, so asking again returns the same check (and survives the unit's test reset).
inline nevr_self_check::CheckId Check() {
  return nevr_self_check::Register(
      {"party_data_share", "every party data share is answered PartyUpdateSuccess (member data: PartyUpdateMemberSuccess)",
       nullptr});
}
}  // namespace detail

inline Totals Counts() {
  Totals t;
  t.ok = detail::S().ok.load();
  t.failed = detail::S().failed.load();
  return t;
}

inline void OnServerMessage(std::uint64_t symbol) {
  const std::optional<Answer> answer = AnswerOf(symbol);
  if (!answer) return;
  detail::State& s = detail::S();
  std::uint64_t ok = 0;
  std::uint64_t failed = 0;
  if (answer->success) {
    ok = s.ok.fetch_add(1) + 1;
    failed = s.failed.load();
    if (s.firstSuccessReported.exchange(true)) return;  // later successes are only counted
  } else {
    failed = s.failed.fetch_add(1) + 1;
    ok = s.ok.load();
  }
  const std::string observed = std::string("scope=") + (answer->member ? "member" : "party") + " answer=" + answer->name +
                               " ok=" + std::to_string(ok) + " failed=" + std::to_string(failed);
  nevr_self_check::Report(detail::Check(), observed, answer->success);
}

// Test seam: forgets the counts and the first-success latch (the check stays registered with the unit).
inline void ResetForTest() {
  detail::State& s = detail::S();
  s.ok.store(0);
  s.failed.store(0);
  s.firstSuccessReported.store(false);
}

}  // namespace nevr_party_share_check
