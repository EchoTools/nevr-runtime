#pragma once
// Self-check "party_data_share" (#398, docs/engine/remote-log.md): the game service answers every party data
// share the facade sends, and an answer to a share that FAILED is the PartyUpdateFailure of #398.
//
// The answer symbols are not exclusive to shares. The party-scope pair (PartyUpdateSuccess / PartyUpdateFailure)
// also answers the leader's set-join-policy request and a party metadata update (nakama
// evr_pipeline_party_policy.go, evr_pipeline_party.go), so an answer cannot be read without the request behind it.
// The service answers one session's requests in the order it received them, so each family keeps a FIFO of what
// was sent: OnClientMessage records the runtime's own requests (and the game's), OnServerMessage pops the next
// entry of the family the answer belongs to and counts the answer only when that entry is a share. The member-scope
// pair (PartyUpdateMemberSuccess / PartyUpdateMemberFailure) answers only a member-scope share.
//
// The game's own party metadata updates are answered with the same symbols (SNSPartyUpdateRequest ->
// PartyUpdateSuccess/Failure, SNSPartyUpdateMemberRequest -> PartyUpdateMemberSuccess/Failure; nakama
// evr_pipeline_party.go snsPartyUpdateRequest / snsPartyUpdateMemberRequest), so those two are recorded too, as
// requests that are not shares.
//
// What is reported: the FIRST success of a session once, EVERY failure (the unit's per-check cap then emits
// `capped`), each with the running counts; and a separate `party_data_share_totals` result when the number of
// answers reaches 1, 2, 4, 8, ... and when a session ends, so the counts survive the cap. An answer with no
// request behind it (the request was sent before this build's observer saw it, or a session boundary dropped it)
// is counted as `unmatched` and reports nothing.
//
// Header only on top of the self-check unit (compat/self_check.h) and the party symbol table
// (compat/social_party.h); the caller links self_check.cpp.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

#include "runtime/compat/self_check.h"
#include "runtime/compat/social_party.h"

namespace nevr_party_share_check {

struct Answer {
  bool member;        // member-scope share (the sender's own member data), else the party family
  bool success;
  const char* name;   // the answer's symbol name, without the SNS prefix
};

struct Totals {
  std::uint64_t ok = 0;         // answers to shares that succeeded
  std::uint64_t failed = 0;     // answers to shares that failed
  std::uint64_t other = 0;      // answers to a request that was not a share (set-join-policy)
  std::uint64_t unmatched = 0;  // answers with no request behind them
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

constexpr std::size_t kMaxPending = 64;       // requests remembered per family; the oldest is dropped
// The game's metadata updates, answered with the same symbols as a share (CSymbol64 of the SNS names,
// tools/gen_symbol_corpus.py csymbol64_hash).
constexpr std::uint64_t kPartyUpdateRequest = 0xdee761a021a5278aULL;        // SNSPartyUpdateRequest
constexpr std::uint64_t kPartyUpdateMemberRequest = 0x4edeeb8ddecc8736ULL;  // SNSPartyUpdateMemberRequest
constexpr std::size_t kTargetParamOffset = 32;  // payload: 8 zero, self UUID(16), 8 zero, TargetParam(8)

enum class Kind : std::uint8_t { kShare, kOther };

struct State {
  std::mutex mutex;
  std::deque<Kind> party;   // requests the party-scope pair will answer, oldest first
  std::deque<Kind> member;  // requests the member-scope pair will answer
  Totals totals;
  bool firstSuccessReported = false;
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
inline nevr_self_check::CheckId TotalsCheck() {
  return nevr_self_check::Register(
      {"party_data_share_totals", "no party data share was answered with a failure (failed=0)", nullptr});
}

inline std::uint64_t Le64(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

inline void Push(std::deque<Kind>* queue, Kind kind) {
  if (queue->size() >= kMaxPending) queue->pop_front();
  queue->push_back(kind);
}

inline void ResetSessionImpl();  // below; registered with the unit the first time a message of ours is seen

inline void EnsureSessionResetRegistered() { nevr_self_check::RegisterSessionReset(&ResetSessionImpl); }

inline std::string TotalsText(const Totals& t) {
  return "answers=" + std::to_string(t.ok + t.failed) + " ok=" + std::to_string(t.ok) + " failed=" +
         std::to_string(t.failed) + " other=" + std::to_string(t.other) + " unmatched=" + std::to_string(t.unmatched);
}

inline bool IsPowerOfTwo(std::uint64_t n) { return n != 0 && (n & (n - 1)) == 0; }

// A session boundary (the unit calls it on every SetLoggedIn, before the boundary moves): the totals of the
// session that is ending are reported, then the pending requests, the counts and the first-success latch start
// over.
inline void ResetSessionImpl() {
  State& s = S();
  std::string totalsText;
  bool failedAny = false;
  bool report = false;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    const Totals& t = s.totals;
    report = t.ok + t.failed + t.other + t.unmatched != 0;
    totalsText = TotalsText(t);
    failedAny = t.failed != 0;
    s.party.clear();
    s.member.clear();
    s.totals = Totals();
    s.firstSuccessReported = false;
  }
  if (report) nevr_self_check::Report(TotalsCheck(), totalsText, !failedAny);
}


}  // namespace detail

inline Totals Counts() {
  detail::State& s = detail::S();
  std::lock_guard<std::mutex> lock(s.mutex);
  return s.totals;
}

/// One message the runtime (or the game) sent to the service, with its payload. Records the requests whose
/// answer is a PartyUpdate* symbol: a party data share (scope in TargetParam), the set-join-policy request and
/// the game's own party / member metadata updates.
inline void OnClientMessage(std::uint64_t symbol, const std::uint8_t* payload, std::size_t length) {
  using detail::Kind;
  detail::State& s = detail::S();
  if (symbol == nevr_social_party::kPartyDataUpdateRequest) {
    if (payload == nullptr || length < detail::kTargetParamOffset + 8) return;
    detail::EnsureSessionResetRegistered();
    const std::uint64_t scope = detail::Le64(payload + detail::kTargetParamOffset);
    std::lock_guard<std::mutex> lock(s.mutex);
    if (scope == nevr_social_party::kPartyDataScopeParty) detail::Push(&s.party, Kind::kShare);
    else if (scope == nevr_social_party::kPartyDataScopeMember) detail::Push(&s.member, Kind::kShare);
  } else if (symbol == nevr_social_party::kSetJoinPolicyRequest || symbol == detail::kPartyUpdateRequest) {
    detail::EnsureSessionResetRegistered();
    std::lock_guard<std::mutex> lock(s.mutex);
    detail::Push(&s.party, Kind::kOther);
  } else if (symbol == detail::kPartyUpdateMemberRequest) {
    detail::EnsureSessionResetRegistered();
    std::lock_guard<std::mutex> lock(s.mutex);
    detail::Push(&s.member, Kind::kOther);
  }
}

/// One message symbol the service sent to the game. Acts only on the four answer symbols.
inline void OnServerMessage(std::uint64_t symbol) {
  using detail::Kind;
  const std::optional<Answer> answer = AnswerOf(symbol);
  if (!answer) return;
  detail::EnsureSessionResetRegistered();
  detail::State& s = detail::S();
  std::string observed;
  std::string totalsText;
  bool reportAnswer = false;
  bool reportTotals = false;
  bool failedAny = false;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    std::deque<Kind>& queue = answer->member ? s.member : s.party;
    if (queue.empty()) {
      ++s.totals.unmatched;
      return;
    }
    const Kind kind = queue.front();
    queue.pop_front();
    if (kind == Kind::kOther) {
      ++s.totals.other;
      return;
    }
    if (answer->success) ++s.totals.ok;
    else ++s.totals.failed;
    const bool first = answer->success && !s.firstSuccessReported;
    if (first) s.firstSuccessReported = true;
    reportAnswer = !answer->success || first;
    observed = std::string("scope=") + (answer->member ? "member" : "party") + " answer=" + answer->name +
               " ok=" + std::to_string(s.totals.ok) + " failed=" + std::to_string(s.totals.failed);
    reportTotals = detail::IsPowerOfTwo(s.totals.ok + s.totals.failed);
    totalsText = detail::TotalsText(s.totals);
    failedAny = s.totals.failed != 0;
  }
  if (reportAnswer) nevr_self_check::Report(detail::Check(), observed, answer->success);
  if (reportTotals) nevr_self_check::Report(detail::TotalsCheck(), totalsText, !failedAny);
}

// Test seam: forgets everything without reporting (the checks stay registered with the unit).
inline void ResetForTest() {
  detail::State& s = detail::S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.party.clear();
  s.member.clear();
  s.totals = Totals();
  s.firstSuccessReported = false;
}

}  // namespace nevr_party_share_check
