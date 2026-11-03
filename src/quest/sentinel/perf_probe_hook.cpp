// Built with -fno-exceptions (callback_thunk.h refuses otherwise). What this probe measures and why: perf_probe_hook.h.
#include "perf_probe_hook.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "callback_thunk.h"
#include "hook_install.h"
#include "hook_log.h"
#include "pinned_targets.h"
#include "sentinel_log.h"

namespace nevr_quest::perf_probe {
namespace {

enum Slot : int { kMicAvailable, kMicRead, kHeadsetType, kPartyMemberHeadsetType, kPartyPermissions, kBotActorId, kSlotCount };

struct SlotRow {
  const char* name;    // the log name
  const char* symbol;  // the relocation's symbol
  std::uint64_t slot;  // link-time slot address in the pinned file
};
constexpr SlotRow kSlots[kSlotCount] = {
    {"mic_available", "_ZN10NRadEngine11CNSProvider12MicAvailableEm", 0x36e8fa0ULL},
    {"mic_read", "_ZN10NRadEngine11CNSProvider7MicReadEmPsy", 0x36e06e8ULL},
    {"headset_type", "_ZN10NRadEngine3CVR11HeadsetTypeEv", 0x36f9fd8ULL},
    {"party_member_headset_type", "_ZNK10NRadEngine8NRadGame11CR15NetGame22PartyMemberHeadsetTypeEj", 0x36fd880ULL},
    {"party_permissions", "_ZNK10NRadEngine8NRadGame33CR15NetPartyPermissionsExpressionclERNS1_7SOutputERNS_13SScriptThreadE",
     0x36c6ee8ULL},
    {"bot_actor_id", "_ZNK10NRadEngine8NRadGame11CR15NetGame10BotActorIdENS_9CSymbol64E", 0x36c1878ULL},
};

// The two callers of CVR::HeadsetType the q15t gates tell apart (ELF vaddr and .dynsym size in the pinned file).
constexpr std::uint64_t kBindRva = 0x202ba74ULL, kBindSize = 184;    // BindExpression<CR15HeadsetTypeExpression>
constexpr std::uint64_t kBotRva = 0x124dd38ULL, kBotSize = 716;      // CR15NetGame::BotActorId(CSymbol64) const
constexpr std::uint64_t kHeadsetTypeRva = 0x1a6f164ULL;              // CVR::HeadsetType(), to find the load base

sentinel::GotTarget Target(Slot s) {
  return sentinel::GotTarget{sentinel::pinned::kLibR15, kSlots[s].symbol, sentinel::RelocKind::kJumpSlot,
                             sentinel::pinned::LibR15PatchedBuildId(), kSlots[s].slot}
      .ServiceSide();
}

constexpr int kHookCount = 6;
static_assert(kHookCount == kSlotCount, "one GotHook per slot");
sentinel::GotHook g_hooks[kHookCount];

constexpr std::uint64_t kWindowNs = 10ULL * 1000 * 1000 * 1000;
constexpr std::uint64_t kGapLimitNs = 1000ULL * 1000 * 1000;  // a longer pause between reads is a new recording, not a gap

std::uintptr_t g_base = 0;                 // libr15's load base, written once before the first install
std::uint64_t g_micOverride = 0;           // 0: pass the provider's answer through; written once before the first install
std::atomic<std::uint64_t> g_windowStart{0};

std::uint64_t NowNs() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<std::uint64_t>(ts.tv_nsec);
}

struct FnStats {
  std::atomic<std::uint64_t> calls{0};
  std::atomic<std::uint64_t> ns{0};
  std::atomic<std::uint64_t> nsMax{0};
};
FnStats g_stats[kSlotCount];

void Raise(std::atomic<std::uint64_t>& slot, std::uint64_t v) noexcept {
  std::uint64_t cur = slot.load(std::memory_order_relaxed);
  while (v > cur && !slot.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}

void Count(Slot s, std::uint64_t dt) noexcept {
  g_stats[s].calls.fetch_add(1, std::memory_order_relaxed);
  g_stats[s].ns.fetch_add(dt, std::memory_order_relaxed);
  Raise(g_stats[s].nsMax, dt);
}

// CVR::HeadsetType callers.
std::atomic<std::uint64_t> g_fromBind{0}, g_fromBot{0}, g_fromOther{0};
// One timed dladdr per window.
std::atomic<bool> g_dladdrTaken{false};
std::atomic<long long> g_dladdrNs{-1};

// Microphone.
std::atomic<std::uint64_t> g_micGame{0};      // the provider's last MicAvailable answer
constexpr std::uint64_t kSizes[] = {0, 120, 240, 480, 960, 1920, 2880};
constexpr int kSizeCount = sizeof kSizes / sizeof kSizes[0];
std::atomic<std::uint64_t> g_got[kSizeCount + 1] = {};  // the last bucket: any other count
std::atomic<std::uint64_t> g_gotOtherLast{0};
std::atomic<std::uint64_t> g_requested{0}, g_gotTotal{0};
std::atomic<std::uint64_t> g_lastReadNs{0}, g_gapMaxNs{0}, g_gapSumNs{0}, g_gaps{0};

// The address of the most recently loaded script library (lib<16 hex>.so), for the dladdr sample.
struct NewestScriptLib {
  std::uintptr_t addr = 0;
};
bool IsScriptLibName(const char* path) noexcept {
  const char* slash = std::strrchr(path, '/');
  const char* name = slash != nullptr ? slash + 1 : path;
  if (std::strlen(name) != 22 || std::strncmp(name, "lib", 3) != 0 || std::strcmp(name + 19, ".so") != 0) return false;
  for (int i = 3; i < 19; ++i) {
    const char c = name[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}
int NewestScriptLibCallback(dl_phdr_info* info, std::size_t, void* data) noexcept {
  if (info->dlpi_name != nullptr && IsScriptLibName(info->dlpi_name)) {
    for (int i = 0; i < info->dlpi_phnum; ++i) {
      const ElfW(Phdr)& p = info->dlpi_phdr[i];
      if (p.p_type == PT_LOAD && (p.p_flags & PF_X) != 0) {
        static_cast<NewestScriptLib*>(data)->addr = info->dlpi_addr + p.p_vaddr;
        break;
      }
    }
  }
  return 0;  // walk them all: the last one wins
}

long long TimeOneDladdr() noexcept {
  NewestScriptLib lib;
  dl_iterate_phdr(&NewestScriptLibCallback, &lib);
  if (lib.addr == 0) return -1;
  Dl_info di{};
  const std::uint64_t t0 = NowNs();
  dladdr(reinterpret_cast<void*>(lib.addr), &di);
  return static_cast<long long>(NowNs() - t0);
}

long long Mean(std::uint64_t ns, std::uint64_t calls) noexcept {
  return calls == 0 ? 0 : static_cast<long long>(ns / calls);
}

void LogWindow(std::uint64_t windowNs) noexcept {
  std::uint64_t calls[kSlotCount], ns[kSlotCount], nsMax[kSlotCount];
  for (int s = 0; s < kSlotCount; ++s) {
    calls[s] = g_stats[s].calls.exchange(0, std::memory_order_relaxed);
    ns[s] = g_stats[s].ns.exchange(0, std::memory_order_relaxed);
    nsMax[s] = g_stats[s].nsMax.exchange(0, std::memory_order_relaxed);
  }
  std::uint64_t got[kSizeCount + 1];
  for (int i = 0; i <= kSizeCount; ++i) got[i] = g_got[i].exchange(0, std::memory_order_relaxed);
  const std::uint64_t gaps = g_gaps.exchange(0, std::memory_order_relaxed);
  const std::uint64_t gapSum = g_gapSumNs.exchange(0, std::memory_order_relaxed);
  const std::uint64_t gapMax = g_gapMaxNs.exchange(0, std::memory_order_relaxed);
  const long long dladdrNs = g_dladdrNs.exchange(-1, std::memory_order_relaxed);
  g_dladdrTaken.store(false, std::memory_order_relaxed);
  const long long windowMs = static_cast<long long>(windowNs / 1000000);
  // Two lines: the sentinel's line holds 768 bytes (hook_log.cpp kLineCapacity), and one line with every field would not fit.
  sentinel::LogFields(
      sentinel::LogLevel::kInfo, "perf_probe_calls",
      {{"window_ms", windowMs},
       {"headset_type_calls", calls[kHeadsetType]},
       {"headset_type_from_bind", g_fromBind.exchange(0, std::memory_order_relaxed)},
       {"headset_type_from_bot_actor_id", g_fromBot.exchange(0, std::memory_order_relaxed)},
       {"headset_type_from_other", g_fromOther.exchange(0, std::memory_order_relaxed)},
       {"headset_type_mean_ns", Mean(ns[kHeadsetType], calls[kHeadsetType])},
       {"headset_type_max_ns", nsMax[kHeadsetType]},
       {"party_member_headset_type_calls", calls[kPartyMemberHeadsetType]},
       {"party_member_headset_type_mean_ns", Mean(ns[kPartyMemberHeadsetType], calls[kPartyMemberHeadsetType])},
       {"party_permissions_calls", calls[kPartyPermissions]},
       {"party_permissions_mean_ns", Mean(ns[kPartyPermissions], calls[kPartyPermissions])},
       {"party_permissions_max_ns", nsMax[kPartyPermissions]},
       {"bot_actor_id_calls", calls[kBotActorId]},
       {"bot_actor_id_mean_ns", Mean(ns[kBotActorId], calls[kBotActorId])},
       {"dladdr_script_lib_ns", dladdrNs},
       {"meaning", "how often the functions a combat rewrite replaces run"}});
  if (calls[kMicAvailable] == 0 && calls[kMicRead] == 0) return;
  sentinel::LogFields(
      sentinel::LogLevel::kInfo, "perf_probe_mic",
      {{"window_ms", windowMs},
       {"mic_available_calls", calls[kMicAvailable]},
       {"mic_available_game", g_micGame.load(std::memory_order_relaxed)},
       {"mic_available_answered", g_micOverride != 0 ? g_micOverride : g_micGame.load(std::memory_order_relaxed)},
       {"mic_read_calls", calls[kMicRead]},
       {"mic_read_requested", g_requested.exchange(0, std::memory_order_relaxed)},
       {"mic_read_got", g_gotTotal.exchange(0, std::memory_order_relaxed)},
       {"got_0", got[0]},
       {"got_120", got[1]},
       {"got_240", got[2]},
       {"got_480", got[3]},
       {"got_960", got[4]},
       {"got_1920", got[5]},
       {"got_2880", got[6]},
       {"got_other", got[kSizeCount]},
       {"got_other_last", g_gotOtherLast.load(std::memory_order_relaxed)},
       {"read_gap_mean_us", gaps == 0 ? 0LL : static_cast<long long>(gapSum / gaps / 1000)},
       {"read_gap_max_us", static_cast<long long>(gapMax / 1000)},
       {"meaning", "Opus takes only 120 to 2880-sample frames: got_other counts updates that sent no voice"}});
}

void MaybeFlush(std::uint64_t now) noexcept {
  std::uint64_t start = g_windowStart.load(std::memory_order_relaxed);
  if (now - start >= kWindowNs && g_windowStart.compare_exchange_strong(start, now, std::memory_order_relaxed)) {
    LogWindow(now - start);
  }
}

struct MicAvailableTag {};
struct MicReadTag {};
struct HeadsetTypeTag {};
struct PartyMemberHeadsetTypeTag {};
struct PartyPermissionsTag {};
struct BotActorIdTag {};
using MicAvailableThunk = sentinel::CallbackThunk<MicAvailableTag, std::uint64_t(std::uint64_t)>;
using MicReadThunk = sentinel::CallbackThunk<MicReadTag, std::uint64_t(std::uint64_t, std::int16_t*, std::uint64_t)>;
using HeadsetTypeThunk = sentinel::CallbackThunk<HeadsetTypeTag, std::int32_t(), /*kCaller=*/true>;
using PartyMemberHeadsetTypeThunk = sentinel::CallbackThunk<PartyMemberHeadsetTypeTag, std::int32_t(const void*, std::uint32_t)>;
// operator() and BotActorId: x0 is passed back unchanged, whatever the game's return type is (void, bool or a CSymbol64).
using PartyPermissionsThunk = sentinel::CallbackThunk<PartyPermissionsTag, std::uint64_t(const void*, void*, void*)>;
using BotActorIdThunk = sentinel::CallbackThunk<BotActorIdTag, std::uint64_t(const void*, std::uint64_t)>;

std::uint64_t HookedMicAvailable(MicAvailableThunk::Fn original, std::uint64_t handle) noexcept {
  const std::uint64_t t0 = NowNs();
  const std::uint64_t game = original(handle);
  const std::uint64_t t1 = NowNs();
  Count(kMicAvailable, t1 - t0);
  g_micGame.store(game, std::memory_order_relaxed);
  MaybeFlush(t1);
  return g_micOverride != 0 ? g_micOverride : game;
}

std::uint64_t HookedMicRead(MicReadThunk::Fn original, std::uint64_t handle, std::int16_t* buf, std::uint64_t n) noexcept {
  const std::uint64_t t0 = NowNs();
  const std::uint64_t got = original(handle, buf, n);
  const std::uint64_t t1 = NowNs();
  Count(kMicRead, t1 - t0);
  g_requested.fetch_add(n, std::memory_order_relaxed);
  g_gotTotal.fetch_add(got, std::memory_order_relaxed);
  int bucket = kSizeCount;
  for (int i = 0; i < kSizeCount; ++i) {
    if (got == kSizes[i]) bucket = i;
  }
  g_got[bucket].fetch_add(1, std::memory_order_relaxed);
  if (bucket == kSizeCount) g_gotOtherLast.store(got, std::memory_order_relaxed);
  const std::uint64_t last = g_lastReadNs.exchange(t0, std::memory_order_relaxed);
  if (last != 0 && t0 > last && t0 - last < kGapLimitNs) {
    g_gaps.fetch_add(1, std::memory_order_relaxed);
    g_gapSumNs.fetch_add(t0 - last, std::memory_order_relaxed);
    Raise(g_gapMaxNs, t0 - last);
  }
  MaybeFlush(t1);
  return got;
}

std::int32_t HookedHeadsetType(HeadsetTypeThunk::Fn original, const void* caller) noexcept {
  const std::uint64_t t0 = NowNs();
  const std::int32_t answer = original();
  const std::uint64_t t1 = NowNs();
  Count(kHeadsetType, t1 - t0);
  const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(caller) - g_base;
  if (at - kBindRva < kBindSize) {
    g_fromBind.fetch_add(1, std::memory_order_relaxed);
    if (!g_dladdrTaken.exchange(true, std::memory_order_relaxed)) g_dladdrNs.store(TimeOneDladdr(), std::memory_order_relaxed);
  } else if (at - kBotRva < kBotSize) {
    g_fromBot.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_fromOther.fetch_add(1, std::memory_order_relaxed);
  }
  MaybeFlush(t1);
  return answer;
}

std::int32_t HookedPartyMemberHeadsetType(PartyMemberHeadsetTypeThunk::Fn original, const void* self, std::uint32_t index) noexcept {
  const std::uint64_t t0 = NowNs();
  const std::int32_t answer = original(self, index);
  const std::uint64_t t1 = NowNs();
  Count(kPartyMemberHeadsetType, t1 - t0);
  MaybeFlush(t1);
  return answer;
}

std::uint64_t HookedPartyPermissions(PartyPermissionsThunk::Fn original, const void* self, void* out, void* thread) noexcept {
  const std::uint64_t t0 = NowNs();
  const std::uint64_t r = original(self, out, thread);
  const std::uint64_t t1 = NowNs();
  Count(kPartyPermissions, t1 - t0);
  MaybeFlush(t1);
  return r;
}

std::uint64_t HookedBotActorId(BotActorIdThunk::Fn original, const void* self, std::uint64_t symbol) noexcept {
  const std::uint64_t t0 = NowNs();
  const std::uint64_t r = original(self, symbol);
  const std::uint64_t t1 = NowNs();
  Count(kBotActorId, t1 - t0);
  MaybeFlush(t1);
  return r;
}

NEVR_HOOK_RECORD(kMicAvailableHook, MicAvailableThunk, &HookedMicAvailable);
NEVR_HOOK_RECORD(kMicReadHook, MicReadThunk, &HookedMicRead);
NEVR_HOOK_RECORD(kHeadsetTypeHook, HeadsetTypeThunk, &HookedHeadsetType);
NEVR_HOOK_RECORD(kPartyMemberHeadsetTypeHook, PartyMemberHeadsetTypeThunk, &HookedPartyMemberHeadsetType);
NEVR_HOOK_RECORD(kPartyPermissionsHook, PartyPermissionsThunk, &HookedPartyPermissions);
NEVR_HOOK_RECORD(kBotActorIdHook, BotActorIdThunk, &HookedBotActorId);

template <typename Thunk>
bool InstallOne(Slot s, const sentinel::HookRecord<Thunk>& record) noexcept {
  Thunk::Arm(record);
  const sentinel::GotStatus status = sentinel::InstallThunk<Thunk>(g_hooks[s], Target(s));
  if (status != sentinel::GotStatus::kOk) Thunk::Disarm();
  char slot[19];
  sentinel::LogFields(status == sentinel::GotStatus::kOk ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "perf_probe_install",
                      {{"function", kSlots[s].name},
                       {"symbol", kSlots[s].symbol},
                       {"slot", sentinel::HexString(slot, kSlots[s].slot)},
                       {"status", sentinel::GotStatusName(status)}});
  return status == sentinel::GotStatus::kOk;
}

// The switch file's sample count, 0 when it is absent; `state` says which.
std::uint64_t ReadMicSwitch(const char** state) noexcept {
  const std::string path = sentinel::LogDir() + "/perf_probe_mic_available";
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    *state = "absent";
    return 0;
  }
  char text[16] = {};
  const ssize_t n = ::read(fd, text, sizeof text - 1);
  ::close(fd);
  if (n <= 0) {
    *state = "unreadable";
    return 0;
  }
  char* end = nullptr;
  const unsigned long long v = std::strtoull(text, &end, 10);
  if (end == text || v == 0 || v > 5760) {
    *state = "out_of_range";
    return 0;
  }
  *state = "present";
  return static_cast<std::uint64_t>(v);
}

std::uintptr_t ResolveBase() noexcept {
  void* const h = dlopen("libr15.so", RTLD_NOW | RTLD_NOLOAD);
  if (h == nullptr) return 0;
  void* const fn = dlsym(h, kSlots[kHeadsetType].symbol);
  dlclose(h);
  if (fn == nullptr) return 0;
  return reinterpret_cast<std::uintptr_t>(fn) - kHeadsetTypeRva;
}

std::atomic<bool> g_done{false};

}  // namespace

bool InstallForServiceOnly() noexcept {
  if (g_done.exchange(true, std::memory_order_acq_rel)) return true;
  const char* micState = "absent";
  g_micOverride = ReadMicSwitch(&micState);
  g_base = ResolveBase();
  if (g_base == 0) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "perf_probe",
                        {{"status", "off"}, {"reason", "libr15_base_unresolved"},
                         {"consequence", "no call counts or microphone sizes are logged this run"}});
    return false;
  }
  g_windowStart.store(NowNs(), std::memory_order_relaxed);
  int ok = 0;
  ok += InstallOne(kMicAvailable, kMicAvailableHook);
  ok += InstallOne(kMicRead, kMicReadHook);
  ok += InstallOne(kHeadsetType, kHeadsetTypeHook);
  ok += InstallOne(kPartyMemberHeadsetType, kPartyMemberHeadsetTypeHook);
  ok += InstallOne(kPartyPermissions, kPartyPermissionsHook);
  ok += InstallOne(kBotActorId, kBotActorIdHook);
  sentinel::LogFields(ok == kSlotCount ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "perf_probe",
                      {{"status", ok == kSlotCount ? "on" : "partial"},
                       {"installed", ok},
                       {"of", static_cast<int>(kSlotCount)},
                       {"mic_available_switch", micState},
                       {"mic_available_override", static_cast<long long>(g_micOverride)},
                       {"consequence", g_micOverride != 0
                                           ? "the voice path is told the switch file's sample count instead of the provider's"
                                           : "measurement only: every probed call returns what the game's own function returned"}});
  return ok == kSlotCount;
}

}  // namespace nevr_quest::perf_probe
