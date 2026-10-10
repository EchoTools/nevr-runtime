// Built -fno-exceptions (callback_thunk.h refuses otherwise). The handler path below (OnSocialHandler,
// SelectSocialObject, FindPnsovr and the loader helpers in got_hook.cpp) is reachable from the thunk
// entry, so the frame sensor (tests/quest TestHookFramesCarryNoPersonality) requires it to be
// personality-free, and it never logs: it only counts.
#include "quest/social/social_install.h"

#include <atomic>
#include <cstring>

#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"
#include "pinned_targets.h"
#include "quest/social/social_abi.h"
#include "quest/social/social_facade.h"
#include "quest/social/social_invite_gate.h"
#include "runtime/compat/social_names.h"

namespace quest_social {
namespace {

using sentinel::LogFields;
using sentinel::LogLevel;

std::atomic<PnsovrLookup> g_lookup{&FindPnsovr};

// The facade's object, published before the callback is armed. The handler reads this and never
// constructs anything.
std::atomic<void*> g_facadeObject{nullptr};

// What the handler counted (constant-initialised: no static initialiser).
std::atomic<std::uint64_t> g_selected{0};
std::atomic<std::uint64_t> g_nullResult{0};
std::atomic<std::uint64_t> g_pnsovrUnavailable{0};
std::atomic<std::uint64_t> g_foreignObject{0};

void Count(std::atomic<std::uint64_t>& counter) noexcept { counter.fetch_add(1, std::memory_order_relaxed); }

}  // namespace

// Runs on the game's thread, once. The frame has no landing pad, so a game exception thrown by the
// original passes through it untouched, and nothing here can throw into the game.
void* OnSocialHandler(SocialThunk::Fn original, std::uint64_t handle) noexcept {
  void* const result = original(handle);
  return SelectSocialObject(result, g_facadeObject.load(std::memory_order_acquire),
                            g_lookup.load(std::memory_order_acquire));
}

NEVR_HOOK_RECORD(kSocialHook, SocialThunk, &OnSocialHandler);

const char* InstallStatusName(InstallStatus status) {
  switch (status) {
    case InstallStatus::kOk: return "ok";
    case InstallStatus::kDisabled: return "disabled";
    case InstallStatus::kHookFailed: return "hook_failed";
  }
  return "unknown";
}

sentinel::GotTarget LibR15Social() {
  return {sentinel::pinned::kLibR15, kSocialSymbol, sentinel::RelocKind::kJumpSlot,
          sentinel::pinned::kLibR15BuildId, kSocialSlotVaddr};
}

PnsovrView FindPnsovr() noexcept {
  PnsovrView view;
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(kLibPnsovr, &image)) return view;
  view.found = true;
  view.loadBias = image.base;
  char id[64] = {};
  view.buildIdMatches = sentinel::ReadBuildId(image, id, sizeof(id)) && std::strcmp(id, kLibPnsovrBuildId) == 0;
  return view;
}

GameJson ResolveGameJson(sentinel::ImageLookup lookup) noexcept {
  GameJson json;
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) return json;
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id)) || std::strcmp(id, sentinel::pinned::kLibR15BuildId) != 0) return json;
  const auto at = [&image](std::uint64_t vaddr) { return image.base + static_cast<std::uintptr_t>(vaddr); };
  const std::uintptr_t reset = at(kLibR15CJsonResetVaddr);
  const std::uintptr_t decode = at(kLibR15CJsonDecodeFromVaddr);
  const std::uintptr_t encode = at(kLibR15CJsonEncodeToCompactVaddr);
  static_assert(sizeof(json.reset) == sizeof(reset), "function pointer size");
  std::memcpy(&json.reset, &reset, sizeof(json.reset));
  std::memcpy(&json.decode, &decode, sizeof(json.decode));
  std::memcpy(&json.encode, &encode, sizeof(json.encode));
  return json;
}

PnsovrLookup SetPnsovrLookup(PnsovrLookup lookup) {
  return g_lookup.exchange(lookup != nullptr ? lookup : &FindPnsovr, std::memory_order_acq_rel);
}

void PublishFacadeObject() { g_facadeObject.store(Facade::Instance().Object(), std::memory_order_release); }

void* SelectSocialObject(void* original, void* facadeObject, PnsovrLookup lookup) noexcept {
  if (original == nullptr) {
    Count(g_nullResult);
    return original;
  }
  if (facadeObject == nullptr || lookup == nullptr) return original;
  const PnsovrView pnsovr = lookup();
  if (!pnsovr.found || !pnsovr.buildIdMatches) {
    Count(g_pnsovrUnavailable);
    return original;
  }
  std::uintptr_t vptr = 0;
  std::memcpy(&vptr, original, sizeof(vptr));
  if (vptr != pnsovr.loadBias + static_cast<std::uintptr_t>(kOvrSocialVptrVaddr)) {
    Count(g_foreignObject);
    return original;
  }
  SetPnsovrBias(pnsovr.loadBias);  // the facade's Initialize checks pnsovr's provider symbol against the login's platform
  Count(g_selected);
  return facadeObject;
}

// ---- rich presence trace (#393) -----------------------------------------------------------------------------

namespace {

// The tracing copy of CNSOVRRichPresence's vtable: offset-to-top and typeinfo, then the slots. Static storage,
// filled once (before any object points at it) and never written again.
constexpr std::size_t kPresenceTableWords = 2 + kOvrRichPresenceSlotCount;
std::uintptr_t g_presenceTable[kPresenceTableWords];
std::atomic<bool> g_presenceTableBuilt{false};
std::uintptr_t g_presenceOrigCount = 0;
std::uintptr_t g_presenceOrigName = 0;
std::uintptr_t g_presenceOrigDestination = 0;
std::uintptr_t g_presenceOrigSet = 0;

std::atomic<std::uint64_t> g_presenceSelected{0};
std::atomic<std::uint64_t> g_presencePassThrough{0};
std::atomic<bool> g_presenceSlotCheck{true};
std::atomic<bool> g_presenceNames{false};
std::atomic<int> g_presenceCurrent{-1};  // table position of the game_type of the last Set (-1: none, unknown)
std::atomic<CJsonEncodeToCompactFn> g_presenceEncode{nullptr};
std::atomic<bool> g_presenceEncodeResolved{false};

// What the wrappers logged last, so only a change is a line (the game asks on every party and lobby event).
std::atomic<bool> g_destSeen{false};
std::atomic<int> g_lastDestIndex{0};
std::atomic<unsigned> g_lastDestCount{0};
std::atomic<std::uint64_t> g_lastNameKey{0};
std::atomic<std::uint64_t> g_lastSetKey{0};

using CountFn = unsigned (*)(const void*);
using DestinationFn = int (*)(const void*);
using NameFn = const char* (*)(const void*, unsigned);
using SetFn = void (*)(void*, const void*);

template <typename Fn>
Fn AsFn(std::uintptr_t address) noexcept {
  Fn fn;
  static_assert(sizeof(fn) == sizeof(address), "function pointer size");
  std::memcpy(&fn, &address, sizeof(fn));
  return fn;
}

struct PresenceName {
  const char* apiName;
  const char* display;
};
// The game's game_type values (symbol corpus: echo_arena, echo_arena_private, ...; the log's "Social_2.0") and
// what the player sees. A game type that is not here is left to the game.
constexpr PresenceName kPresenceNames[] = {
    {"social_2.0", "Social Lobby"},         {"echo_arena", "Arena"},
    {"echo_combat", "Combat"},              {"echo_arena_private", "Private Match"},
    {"echo_combat_private", "Private Match"}, {"social_2.0_private", "Private Match"},
};
constexpr int kPresenceNameCount = static_cast<int>(sizeof(kPresenceNames) / sizeof(kPresenceNames[0]));

char Lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

int PresenceNameIndex(const char* gameType) noexcept {
  if (gameType == nullptr || gameType[0] == '\0') return -1;
  for (int i = 0; i < kPresenceNameCount; ++i) {
    const char* a = gameType;
    const char* b = kPresenceNames[i].apiName;
    while (*a != '\0' && *b != '\0' && Lower(*a) == *b) {
      ++a;
      ++b;
    }
    if (*a == '\0' && *b == '\0') return i;
  }
  return -1;
}

// The value of "game_type" in the compact JSON text the game's encoder wrote (a plain string: no escapes, short).
bool GameTypeOf(const char* text, char (&out)[48]) noexcept {
  static const char kKey[] = "\"game_type\":\"";
  const char* at = std::strstr(text, kKey);
  if (at == nullptr) return false;
  at += sizeof(kKey) - 1;
  std::size_t n = 0;
  while (at[n] != '\0' && at[n] != '"') {
    if (at[n] == '\\' || n + 1 >= sizeof(out)) return false;
    out[n] = at[n];
    ++n;
  }
  if (at[n] != '"') return false;
  out[n] = '\0';
  return true;
}

std::uint64_t Fnv(const char* text, std::uint64_t seed) noexcept {
  std::uint64_t h = 1469598103934665603ULL ^ seed;
  for (; text != nullptr && *text != '\0'; ++text) h = (h ^ static_cast<unsigned char>(*text)) * 1099511628211ULL;
  return h;
}

// Slot 9: the index of the destination the presence's game_type names, -1 when none (the empty list of a failed
// GetDestinations). The answer is the original's.
int TracedDestination(const void* self) noexcept {
  int index = AsFn<DestinationFn>(g_presenceOrigDestination)(self);
  const unsigned count = AsFn<CountFn>(g_presenceOrigCount)(self);
  bool fromTable = false;
  // The game found none (Meta's list is empty or lacks this game type): answer from the table when enabled and
  // the table knows the game type of the presence just set. Anything the game found is left alone.
  if (index == -1 && g_presenceNames.load(std::memory_order_relaxed)) {
    const int known = g_presenceCurrent.load(std::memory_order_relaxed);
    if (known >= 0) {
      index = kPresenceNameBase + known;
      fromTable = true;
    }
  }
  const bool seen = g_destSeen.exchange(true, std::memory_order_relaxed);
  if (!seen || g_lastDestIndex.load(std::memory_order_relaxed) != index ||
      g_lastDestCount.load(std::memory_order_relaxed) != count) {
    g_lastDestIndex.store(index, std::memory_order_relaxed);
    g_lastDestCount.store(count, std::memory_order_relaxed);
    LogFields(LogLevel::kInfo, "rich_presence_destination",
              {{"index", index}, {"count", static_cast<long long>(count)}, {"source", fromTable ? "table" : "game"}});
  }
  return index;
}

// Slot 8: the display name of destination `index`. Only asked for when slot 9 found one.
const char* TracedName(const void* self, unsigned index) noexcept {
  const bool ours = index >= static_cast<unsigned>(kPresenceNameBase) &&
                    index < static_cast<unsigned>(kPresenceNameBase + kPresenceNameCount);
  const char* const name = ours ? kPresenceNames[index - static_cast<unsigned>(kPresenceNameBase)].display
                                : AsFn<NameFn>(g_presenceOrigName)(self, index);
  const std::uint64_t key = Fnv(name, index + 1U);
  if (g_lastNameKey.exchange(key, std::memory_order_relaxed) != key) {
    LogFields(LogLevel::kInfo, "rich_presence_name",
              {{"index", static_cast<long long>(index)}, {"name", name != nullptr ? name : "(null)"}});
  }
  return name;
}

// Slot 15: the presence document the game set (game_type, lobby_id, party_id, capacities, joinable). The
// original copies it into the object; the line carries the document's compact text.
void TracedSet(void* self, const void* json) noexcept {
  AsFn<SetFn>(g_presenceOrigSet)(self, json);
  if (!g_presenceEncodeResolved.load(std::memory_order_acquire)) {
    if (g_presenceEncode.load(std::memory_order_relaxed) == nullptr) {
      g_presenceEncode.store(ResolveGameJson(&sentinel::FindLoadedImage).encode, std::memory_order_relaxed);
    }
    g_presenceEncodeResolved.store(true, std::memory_order_release);
  }
  const CJsonEncodeToCompactFn encode = g_presenceEncode.load(std::memory_order_relaxed);
  g_presenceCurrent.store(-1, std::memory_order_relaxed);
  if (encode == nullptr || json == nullptr) {
    if (g_lastSetKey.exchange(1, std::memory_order_relaxed) != 1) {
      LogFields(LogLevel::kWarn, "rich_presence_set", {{"result", "game_json_unavailable"}});
    }
    return;
  }
  char text[512];
  unsigned long long size = sizeof(text);
  if (encode(json, text, &size, 1, "") != 0 || size >= sizeof(text)) {
    // A document the encoder cannot read out fails the same way on every Set: one line, then silence until
    // a Set reads out again (its text changes the key).
    if (g_lastSetKey.exchange(2, std::memory_order_relaxed) != 2) {
      LogFields(LogLevel::kWarn, "rich_presence_set", {{"result", "encode_failed"}});
    }
    return;
  }
  text[size] = '\0';
  char gameType[48];
  if (GameTypeOf(text, gameType)) g_presenceCurrent.store(PresenceNameIndex(gameType), std::memory_order_relaxed);
  const std::uint64_t key = Fnv(text, 7);
  if (g_lastSetKey.exchange(key, std::memory_order_relaxed) != key) {
    LogFields(LogLevel::kInfo, "rich_presence_set", {{"json", text}});
  }
}

// The slots we wrap must hold the functions of the pinned build, or the wrappers would call into something else.
bool SlotsAreThePinnedOnes(const std::uintptr_t* live, std::uintptr_t bias) noexcept {
  if (!g_presenceSlotCheck.load(std::memory_order_relaxed)) return true;
  return live[kRichPresenceSlotDestinationCount] == bias + kOvrRichPresenceDestinationCountVaddr &&
         live[kRichPresenceSlotDestinationName] == bias + kOvrRichPresenceDestinationNameVaddr &&
         live[kRichPresenceSlotDestination] == bias + kOvrRichPresenceDestinationVaddr &&
         live[kRichPresenceSlotSet] == bias + kOvrRichPresenceSetVaddr;
}

}  // namespace

void* OnPresenceHandler(PresenceThunk::Fn original, std::uint64_t handle) noexcept {
  void* const result = original(handle);
  return SelectRichPresenceObject(result, g_lookup.load(std::memory_order_acquire));
}

NEVR_HOOK_RECORD(kPresenceHook, PresenceThunk, &OnPresenceHandler);

sentinel::GotTarget LibR15RichPresence() {
  return {sentinel::pinned::kLibR15, kRichPresenceSymbol, sentinel::RelocKind::kJumpSlot,
          sentinel::pinned::kLibR15BuildId, kRichPresenceSlotVaddr};
}

void* SelectRichPresenceObject(void* original, PnsovrLookup lookup) noexcept {
  if (original == nullptr || lookup == nullptr) {
    Count(g_presencePassThrough);
    return original;
  }
  const PnsovrView pnsovr = lookup();
  if (!pnsovr.found || !pnsovr.buildIdMatches) {
    Count(g_presencePassThrough);
    return original;
  }
  std::uintptr_t vptr = 0;
  std::memcpy(&vptr, original, sizeof(vptr));
  if (vptr == reinterpret_cast<std::uintptr_t>(&g_presenceTable[2])) return original;  // already traced
  if (vptr != pnsovr.loadBias + static_cast<std::uintptr_t>(kOvrRichPresenceVptrVaddr)) {
    Count(g_presencePassThrough);
    return original;
  }
  const std::uintptr_t* const live = reinterpret_cast<const std::uintptr_t*>(vptr);
  if (!SlotsAreThePinnedOnes(live, pnsovr.loadBias)) {
    Count(g_presencePassThrough);
    return original;
  }
  if (!g_presenceTableBuilt.load(std::memory_order_acquire)) {
    g_presenceOrigCount = live[kRichPresenceSlotDestinationCount];
    g_presenceOrigName = live[kRichPresenceSlotDestinationName];
    g_presenceOrigDestination = live[kRichPresenceSlotDestination];
    g_presenceOrigSet = live[kRichPresenceSlotSet];
    g_presenceTable[0] = live[-2];
    g_presenceTable[1] = live[-1];
    for (std::size_t i = 0; i < kOvrRichPresenceSlotCount; ++i) g_presenceTable[2 + i] = live[i];
    const auto word = [](auto* fn) { return reinterpret_cast<std::uintptr_t>(fn); };
    g_presenceTable[2 + kRichPresenceSlotDestinationName] = word(&TracedName);
    g_presenceTable[2 + kRichPresenceSlotDestination] = word(&TracedDestination);
    g_presenceTable[2 + kRichPresenceSlotSet] = word(&TracedSet);
    g_presenceTableBuilt.store(true, std::memory_order_release);
  }
  const std::uintptr_t table = reinterpret_cast<std::uintptr_t>(&g_presenceTable[2]);
  std::memcpy(original, &table, sizeof(table));
  Count(g_presenceSelected);
  return original;
}

const char* PresenceDisplayName(const char* gameType) noexcept {
  const int i = PresenceNameIndex(gameType);
  return i >= 0 ? kPresenceNames[i].display : nullptr;
}

void SetPresenceNames(bool enabled) noexcept { g_presenceNames.store(enabled, std::memory_order_relaxed); }

PresenceCounters PresenceCountersView() noexcept { return PresenceCounters{g_presenceSelected, g_presencePassThrough}; }

void ResetPresenceForTest() noexcept {
  g_presenceSelected.store(0, std::memory_order_relaxed);
  g_presencePassThrough.store(0, std::memory_order_relaxed);
  g_presenceSlotCheck.store(true, std::memory_order_relaxed);
  g_presenceNames.store(false, std::memory_order_relaxed);
  g_presenceCurrent.store(-1, std::memory_order_relaxed);
  g_presenceEncode.store(nullptr, std::memory_order_relaxed);
  g_presenceEncodeResolved.store(false, std::memory_order_relaxed);
  g_destSeen.store(false, std::memory_order_relaxed);
  g_lastNameKey.store(0, std::memory_order_relaxed);
  g_lastSetKey.store(0, std::memory_order_relaxed);
  g_presenceTableBuilt.store(false, std::memory_order_relaxed);
}

void SetPresenceSeamsForTest(bool slotCheck, CJsonEncodeToCompactFn encode) noexcept {
  g_presenceSlotCheck.store(slotCheck, std::memory_order_relaxed);
  g_presenceEncode.store(encode, std::memory_order_relaxed);
  g_presenceEncodeResolved.store(encode != nullptr, std::memory_order_release);
}

sentinel::GotStatus InstallPresenceTrace() {
  static sentinel::GotHook hook;
  PresenceThunk::Arm(kPresenceHook);
  const sentinel::GotStatus got = sentinel::InstallThunk<PresenceThunk>(hook, LibR15RichPresence());
  if (got != sentinel::GotStatus::kOk) {
    PresenceThunk::Disarm();
    LogFields(LogLevel::kError, "rich_presence_install", {{"status", "hook_failed"}, {"got", sentinel::GotStatusName(got)}});
  } else {
    LogFields(LogLevel::kInfo, "rich_presence_install", {{"status", "ok"}});
  }
  return got;
}

SocialCounters Counters() noexcept {
  return SocialCounters{g_selected, g_nullResult, g_pnsovrUnavailable, g_foreignObject};
}

void ResetCountersForTest() noexcept {
  g_selected.store(0, std::memory_order_relaxed);
  g_nullResult.store(0, std::memory_order_relaxed);
  g_pnsovrUnavailable.store(0, std::memory_order_relaxed);
  g_foreignObject.store(0, std::memory_order_relaxed);
}

bool RegisterSocialReportCounters() {
  bool ok = true;
  ok = sentinel::RegisterReportCounter("social_calls", &SocialThunk::CallCounter()) && ok;
  ok = sentinel::RegisterReportCounter("social_facade_selected", &g_selected) && ok;
  ok = sentinel::RegisterReportCounter("social_null_result", &g_nullResult, sentinel::ReportKind::kFaults) && ok;
  ok = sentinel::RegisterReportCounter("social_pnsovr_unavailable", &g_pnsovrUnavailable,
                                       sentinel::ReportKind::kFaults) && ok;
  ok = sentinel::RegisterReportCounter("social_foreign_object", &g_foreignObject, sentinel::ReportKind::kFaults) && ok;
  ok = sentinel::RegisterReportCounter("social_thunk_faults", &SocialThunk::FaultCounter(),
                                       sentinel::ReportKind::kFaults) && ok;
  const FacadeCounters facade = FacadeCountersView();
  ok = sentinel::RegisterReportCounter("social_members_hidden", &facade.membersHidden,
                                       sentinel::ReportKind::kFaults) && ok;
  ok = sentinel::RegisterReportCounter("social_events_dropped", &facade.eventsDropped,
                                       sentinel::ReportKind::kFaults) && ok;
  ok = sentinel::RegisterReportCounter("social_send_failed", &facade.sendFailed, sentinel::ReportKind::kFaults) && ok;
  ok = sentinel::RegisterReportCounter("social_join_deferred", &facade.joinDeferred) && ok;
  ok = sentinel::RegisterReportCounter("social_request_timeout", &facade.requestTimeout,
                                       sentinel::ReportKind::kFaults) && ok;
  // Callback deliveries by class: a headset run shows PartyCreatedCB, MemberJoined, JoinFailed and the rest.
  ok = sentinel::RegisterReportCounter("social_cb_created", &facade.cbCreated) && ok;
  ok = sentinel::RegisterReportCounter("social_cb_member_joined", &facade.cbMemberJoined) && ok;
  ok = sentinel::RegisterReportCounter("social_cb_join_failed", &facade.cbJoinFailed) && ok;
  ok = sentinel::RegisterReportCounter("social_cb_other", &facade.cbOther) && ok;
  ok = sentinel::RegisterReportCounter("social_json_failed", &facade.jsonFailed, sentinel::ReportKind::kFaults) && ok;
  ok = sentinel::RegisterReportCounter("social_frames_ignored", &facade.framesIgnored, sentinel::ReportKind::kFaults) && ok;
  ok = RegisterInviteGateCounters() && ok;
  ok = sentinel::RegisterReportCounter("presence_calls", &PresenceThunk::CallCounter()) && ok;
  ok = sentinel::RegisterReportCounter("presence_selected", &g_presenceSelected) && ok;
  ok = sentinel::RegisterReportCounter("presence_pass_through", &g_presencePassThrough,
                                       sentinel::ReportKind::kFaults) && ok;
  ok = sentinel::RegisterReportCounter("presence_thunk_faults", &PresenceThunk::FaultCounter(),
                                       sentinel::ReportKind::kFaults) && ok;
  return ok;
}

InstallResult InstallSocialHook(bool enabled) {
  InstallResult result;
  if (!enabled) {
    LogFields(LogLevel::kInfo, "social_install", {{"status", "disabled"}});
    return result;
  }
  static sentinel::GotHook hook;
  // The friend-name decoder (zstd) is registered by this explicit call: the PC registers it from a static
  // initializer, which the sentinel may not carry. Without it no profile is requested and rows show ids.
  nevr_social_names::RegisterDefaultDecoder();
  // Allocate and wire the models before any game thread can reach the handler.
  PublishFacadeObject();
  const GameJson gameJson = ResolveGameJson(&sentinel::FindLoadedImage);
  SetGameJson(gameJson);
  LogFields(gameJson.reset != nullptr ? LogLevel::kInfo : LogLevel::kWarn, "social_install",
            {{"game_json", gameJson.reset != nullptr ? "resolved" : "unavailable"}});
  SocialThunk::Arm(kSocialHook);
  result.got = sentinel::InstallThunk<SocialThunk>(hook, LibR15Social());
  if (result.got != sentinel::GotStatus::kOk) {
    SocialThunk::Disarm();
    result.status = InstallStatus::kHookFailed;
    LogFields(LogLevel::kError, "social_install",
              {{"status", "hook_failed"}, {"got", sentinel::GotStatusName(result.got)}});
    return result;
  }
  result.status = InstallStatus::kOk;
  LogFields(LogLevel::kInfo, "social_install", {{"status", "ok"}});
  // The facade is live: let the invite "+" reach it (the gate logs its own outcome; the facade works without it).
  InstallInviteGate();
  // The rich presence trace: the object pnsovr returns for RichPresence() answers as before and says what it answered.
  InstallPresenceTrace();
  return result;
}

}  // namespace quest_social
