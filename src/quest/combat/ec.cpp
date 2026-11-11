#include "quest/combat/ec.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "quest/combat/combat_level.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/ec_core.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/safe_read.h"
#include "quest/integration/post_load.h"
#include "quest/sentinel/ec_page.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"
#include "runtime/compat/server_features.h"

namespace nevr_quest::combat::ec {

namespace {

namespace pinned = sentinel::pinned;
using namespace core;
using sentinel::LogLevel;

// ---- the thunks: one per slot (callback_thunk.h, "One thunk per hooked slot") ----
struct RecvTag {};
struct SendTag {};
template <int I>
struct LogVTag {};
struct ReloadTag {};
struct UnloadTag {};
struct LobbySettingsTag {};
using RecvThunk = sentinel::CallbackThunk<RecvTag, RecvfromSig>;
using SendThunk = sentinel::CallbackThunk<SendTag, SendtoSig>;
template <int I>
using LogVThunk = sentinel::CallbackThunk<LogVTag<I>, WriteLogVSig>;
using ReloadThunk = sentinel::CallbackThunk<ReloadTag, GearSig>;
using UnloadThunk = sentinel::CallbackThunk<UnloadTag, GearSig>;
using LobbySettingsThunk = sentinel::CallbackThunk<LobbySettingsTag, LobbySettingsSig>;

sentinel::GotHook g_recvHook;
sentinel::GotHook g_sendHook;
sentinel::GotHook g_logHook[kLogLibs];
sentinel::GotHook g_reloadHook;
sentinel::GotHook g_unloadHook;
sentinel::GotHook g_lobbySettingsHook;

constexpr int kLineCap = 48;
constexpr std::uint64_t kScopeEveryMs = 250;  // the level is read at most this often; the thread's period out of scope
constexpr unsigned kStepUs = 5000;            // the thread's period in scope

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
State g_st;  // under g_mu
std::atomic<std::uint64_t> g_sends{0}, g_passed{0}, g_consumed{0};
std::atomic<int> g_killLines{0}, g_tokenLines{0}, g_selLines{0};
std::atomic<DroneKilledFn> g_droneKilled{nullptr};
constexpr int kNetTicks = 4;
std::atomic<NetTickFn> g_netTick[kNetTicks] = {};
std::atomic<int> g_gameFd{-1};            // the game's own socket (from the server pick; -1 = none)
std::atomic<std::uint64_t> g_gameSrc{0};  // that server's address << 16 | port (0 = none)
std::atomic<std::uint32_t> g_gameTid{0};  // the thread its datagrams arrive on
double g_locTokenS = 0;
std::atomic<std::uintptr_t> g_tacListWord{0};  // absolute address of the tac-mod list word
bool g_hideWrite = false;   // set before the thread starts
bool g_equipStore = false;  // set before the thread starts
bool g_listOn = false;      // both gate thunks in
bool g_settingsOn = false;  // the lobby settings callback hooked (#2333): without it no server's statement is read
std::atomic<bool> g_installed{false};
std::atomic<bool> g_ran{false};
std::atomic<bool> g_logOn[kLogLibs] = {};
bool g_noThread = false;  // test seam

// the scope
std::atomic<bool> g_inScope{false};
std::atomic<std::uintptr_t> g_game{0};
std::atomic<int> g_forceScope{-1};
std::uint64_t g_nextScopeMs = 0;  // the ec thread only (and tests)
level::Kind g_lastKind = level::Kind::kNone;
bool g_scopeLogged = false;

std::atomic<sentinel::ImageLookup> g_lookup{nullptr};
std::atomic<const Pins*> g_pinsOverride{nullptr};
std::atomic<const LogLib*> g_logLibsOverride{nullptr};
const LogLib* g_logLibs = kLogLib;  // what the install used, for the observer
pthread_mutex_t g_platformMu = PTHREAD_MUTEX_INITIALIZER;
int g_platformLogged[kLogLibs] = {-1, -1, -1};  // the outcome last logged per library (under g_platformMu)

GearGate g_gear;

struct LockG {
  LockG() { pthread_mutex_lock(&g_mu); }
  ~LockG() { pthread_mutex_unlock(&g_mu); }
  LockG(const LockG&) = delete;
  LockG& operator=(const LockG&) = delete;
};

std::uint64_t (*g_clock)() = nullptr;  // test seam

std::uint64_t Now100() noexcept {
  if (g_clock != nullptr) return g_clock();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 10000000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 100;
}
std::uint32_t Tid() noexcept { return static_cast<std::uint32_t>(syscall(SYS_gettid)); }

sentinel::ImageLookup Lookup() noexcept {
  const sentinel::ImageLookup l = g_lookup.load(std::memory_order_acquire);
  return l != nullptr ? l : &sentinel::FindLoadedImage;
}

const char* LogLibName(int i) noexcept { return i == 0 ? "libr15" : i == 1 ? "libpnsovr" : "libpnsrad"; }

// ---- the handlers ----

// The in-scope halves are out of line, so the out-of-scope path of a handler is the scope load and a tail call (ADR 0038: it runs per datagram
// in arena).
__attribute__((noinline)) ssize_t SendInScope(SendThunk::Fn real, int fd, const void* buf, size_t len, int flags, const sockaddr* to,
                                              socklen_t tolen) noexcept {
  if (to != nullptr && tolen >= sizeof(sockaddr_in) && to->sa_family == AF_INET) {
    const sockaddr_in* a = reinterpret_cast<const sockaddr_in*>(to);
    LockG l;
    g_st.SawSend(fd, a->sin_addr.s_addr, a->sin_port);
  }
  g_sends.fetch_add(1, std::memory_order_relaxed);
  return real(fd, buf, len, flags, to, tolen);
}
ssize_t SendHandler(SendThunk::Fn real, int fd, const void* buf, size_t len, int flags, const sockaddr* to, socklen_t tolen) noexcept {
  if (!InCombatScope()) return real(fd, buf, len, flags, to, tolen);  // arena and every other level: libc, untouched
  return SendInScope(real, fd, buf, len, flags, to, tolen);
}

// The real recvfrom; a datagram of exactly 16 bytes with both magics and a known type that was not peeked is ours and is NOT returned:
// the real recvfrom is called again with the same arguments (a non-blocking socket gives the game the next datagram or EAGAIN, a blocking
// one waits as the game's own call would). Everything else, MSG_PEEK, and a call with from != null and fromlen == null pass through.
constexpr int kMaxConsumePerCall = 256;

void LogKill(std::uint32_t drone, bool handled) noexcept {
  if (g_killLines.fetch_add(1, std::memory_order_relaxed) >= 16) return;
  sentinel::LogFields(LogLevel::kInfo, "combat_ec_kill",
                      {{"drone", drone}, {"handler", handled ? "yes" : "none (dropped)"},
                       {"msg", handled ? "a drone was killed on the server: handed to the drone module"
                                       : "a drone was killed on the server, and no drone module listens: the kill is dropped"}});
}
void LogToken(std::uint8_t type, std::uint32_t id, std::uint64_t tokens) noexcept {
  if (g_tokenLines.fetch_add(1, std::memory_order_relaxed) >= kLineCap) return;
  const char t[2] = {static_cast<char>(type), 0};
  char idText[19];
  sentinel::LogFields(LogLevel::kInfo, "combat_ec_token", {{"type", t}, {"id", sentinel::HexString(idText, id)}, {"tokens", tokens}});
}

__attribute__((noinline)) ssize_t RecvInScope(RecvThunk::Fn real, int fd, void* buf, size_t len, int flags, sockaddr* from,
                                              socklen_t* fromlen) noexcept {
  for (int consumed = 0;; ++consumed) {
    sockaddr_storage own;
    socklen_t ownLen = sizeof own;
    sockaddr* f = from != nullptr ? from : reinterpret_cast<sockaddr*>(&own);
    socklen_t* fl = from != nullptr ? fromlen : &ownLen;
    const socklen_t saved = fl != nullptr ? *fl : 0;
    const ssize_t n = real(fd, buf, len, flags, f, fl);
    Packet p;
    // len: a MSG_TRUNC call can return 16 into a shorter buffer. consumed: one call never swallows more than kMaxConsumePerCall of the
    // network's datagrams in a row; past that the next one passes through to the game untouched.
    if ((flags & MSG_PEEK) || fl == nullptr || len < sizeof(Packet) || consumed >= kMaxConsumePerCall || !Ours(buf, static_cast<long>(n), &p)) {
      if (n >= 0) g_passed.fetch_add(1, std::memory_order_relaxed);
      // only the game's own socket (CR15NetGame::UpdateBroadcaster), and only on the thread that received a datagram of the current server
      // on it (an fd number can be reused)
      if (fd == g_gameFd.load(std::memory_order_relaxed)) {
        const std::uint32_t tid = Tid();
        if (n > 0 && *fl >= sizeof(sockaddr_in) && f->sa_family == AF_INET) {
          const sockaddr_in* a = reinterpret_cast<const sockaddr_in*>(f);
          const std::uint64_t src = (static_cast<std::uint64_t>(a->sin_addr.s_addr) << 16) | a->sin_port;
          if (src == g_gameSrc.load(std::memory_order_relaxed)) g_gameTid.store(tid, std::memory_order_relaxed);
        }
        if (tid == g_gameTid.load(std::memory_order_relaxed)) {
          for (auto& t : g_netTick) {
            if (const NetTickFn tick = t.load(std::memory_order_acquire)) tick();
          }
        }
      }
      return n;
    }
    if (*fl >= sizeof(sockaddr_in) && f->sa_family == AF_INET) {
      std::uint32_t killed;
      std::uint64_t tokensBefore, tokensAfter;
      {
        LockG l;
        tokensBefore = g_st.tokens;
        const std::uint64_t now = Now100();
        killed = g_st.Handle(sentinel::EcPage(), p, now);
        tokensAfter = g_st.tokens;
      }
      if (killed != 0) {  // outside the lock: the handler is another module's
        const DroneKilledFn handler = g_droneKilled.load(std::memory_order_acquire);
        if (handler != nullptr) handler(killed - 1);
        LogKill(killed - 1, handler != nullptr);
      }
      if (tokensAfter != tokensBefore) LogToken(p.type, p.id, tokensAfter);
    }
    *fl = saved;
    g_consumed.fetch_add(1, std::memory_order_relaxed);
  }
}
ssize_t RecvHandler(RecvThunk::Fn real, int fd, void* buf, size_t len, int flags, sockaddr* from, socklen_t* fromlen) noexcept {
  if (!InCombatScope()) return real(fd, buf, len, flags, from, fromlen);  // arena and every other level: libc, untouched
  return RecvInScope(real, fd, buf, len, flags, from, fromlen);
}

// Runs on whichever thread logs the profile update: two atomic page stores and capped lines.
void SawProfile(const char* json, int lib) noexcept {
  char name[24];
  const std::uint64_t sym = SelectedFromJson(json, name, sizeof name);
  if (name[0] == 0) return;  // a profile update without a tac mod
  if (sym != 0 && g_equipStore) StoreSelected(sentinel::EcPage(), sym);
  if (g_selLines.fetch_add(1, std::memory_order_relaxed) >= kLineCap) return;
  char symText[19];
  sentinel::LogFields(LogLevel::kInfo, "combat_ec_sel",
                      {{"name", name}, {"sym", sentinel::HexString(symText, sym)}, {"lib", LogLibName(lib)},
                       {"result", sym == 0 ? "not a known tac mod, nothing stored" : g_equipStore ? "stored at +e80" : "seen, NOT stored (noequip word)"}});
}

// The game's WriteLog built this va_list and calls WriteLogV through the slot (ec_targets.h): one look at the format, then WriteLogV with
// the same four arguments, its result passed back. Every scope: the store is into re33_ec_page only (ec.h, "Arena").
template <int I>
std::uintptr_t LogVHandler(typename LogVThunk<I>::Fn real, int level, unsigned long long flags, const char* fmt, va_list ap) noexcept {
  if (IsProfileFmt(fmt)) {
    va_list c;
    va_copy(c, ap);
    const char* const json = va_arg(c, const char*);
    va_end(c);
    SawProfile(json, I);
  }
  return real(level, flags, fmt, ap);
}

void ReloadHandler(ReloadThunk::Fn real) noexcept {
  g_gear.Enter();
  real();
  g_gear.Leave();
}
void UnloadHandler(UnloadThunk::Fn real) noexcept {
  g_gear.Enter();
  real();
  g_gear.Leave();
}

// #2333: the server states what it runs in the lobby settings it gives its clients (runtime/compat/server_features.h). The handler only
// keeps the message's text (the same +0x18 / +0x20 the game's callback reads) and forwards the call unchanged: the document is read with
// nlohmann::json, which is not on a hook frame's path (tests/quest TestHookFramesCarryNoPersonality), so the EC thread reads it
// (DrainLobbySettings). Once per document the game receives (a session's start, a join), never per frame.
constexpr std::size_t kSettingsCap = 256 * 1024;  // the service's documents are a few KiB; a larger one is read as malformed
pthread_mutex_t g_settingsMu = PTHREAD_MUTEX_INITIALIZER;
char g_settingsText[kSettingsCap];  // under g_settingsMu
std::uint64_t g_settingsSize = 0;   // under g_settingsMu: the size the message gave
bool g_settingsBad = false;         // under g_settingsMu: no text, or more than kSettingsCap
std::uint64_t g_settingsSeq = 0;    // under g_settingsMu: one more per message
std::uint64_t g_settingsRead = 0;   // the EC thread's: the last message it read

void LobbySettingsHandler(LobbySettingsThunk::Fn real, std::uintptr_t netGame, const void* msg) noexcept {
  const char* text = nullptr;
  std::uint64_t size = 0;
  if (msg != nullptr) {
    std::memcpy(&text, static_cast<const std::uint8_t*>(msg) + nevr_server_features::kMsgTextOffset, sizeof(text));
    std::memcpy(&size, static_cast<const std::uint8_t*>(msg) + nevr_server_features::kMsgSizeOffset, sizeof(size));
  }
  pthread_mutex_lock(&g_settingsMu);
  g_settingsBad = text == nullptr || size > kSettingsCap;
  if (!g_settingsBad) std::memcpy(g_settingsText, text, static_cast<std::size_t>(size));
  g_settingsSize = size;
  ++g_settingsSeq;
  pthread_mutex_unlock(&g_settingsMu);
  real(netGame, msg);
}

// On the EC thread: the newest document the game received, read once; the gate the tac-mod hide follows set from it.
void DrainLobbySettings() noexcept {
  std::string text;
  std::uint64_t size = 0;
  bool bad = false;
  pthread_mutex_lock(&g_settingsMu);
  const bool fresh = g_settingsSeq != g_settingsRead;
  if (fresh) {
    g_settingsRead = g_settingsSeq;
    bad = g_settingsBad;
    size = g_settingsSize;
    if (!bad) text.assign(g_settingsText, static_cast<std::size_t>(size));
  }
  pthread_mutex_unlock(&g_settingsMu);
  if (!fresh) return;
  nevr_server_features::Statement statement;
  if (bad) {
    statement.kind = nevr_server_features::Statement::Kind::kMalformed;
  } else {
    statement = nevr_server_features::ReadDocument(text.data(), text.size());
  }
  const nevr_server_features::Gate gate = nevr_server_features::GateOf(statement);
  const bool changed = nevr_server_features::Client().Apply(gate);
  sentinel::LogFields(statement.kind == nevr_server_features::Statement::Kind::kMalformed ? LogLevel::kWarn : LogLevel::kInfo,
                      "combat_ec_lobby_settings",
                      {{"statement", nevr_server_features::KindName(statement.kind)},
                       {"features", nevr_server_features::Joined(statement.features).c_str()},
                       {"dropped", static_cast<long long>(statement.dropped)},
                       {"bytes", static_cast<long long>(size)},
                       {"changed", changed ? 1 : 0},
                       {"msg", gate.echoCombat ? "the server states EchoCombat: UMBRAL VEIL, OVERCHARGE and POINT are shown"
                                               : "the server states no EchoCombat: UMBRAL VEIL, OVERCHARGE and POINT stay hidden"}});
}

NEVR_HOOK_RECORD(kRecvRecord, RecvThunk, &RecvHandler);
NEVR_HOOK_RECORD(kSendRecord, SendThunk, &SendHandler);
NEVR_HOOK_RECORD(kLogV0Record, LogVThunk<0>, &LogVHandler<0>);
NEVR_HOOK_RECORD(kLogV1Record, LogVThunk<1>, &LogVHandler<1>);
NEVR_HOOK_RECORD(kLogV2Record, LogVThunk<2>, &LogVHandler<2>);
NEVR_HOOK_RECORD(kReloadRecord, ReloadThunk, &ReloadHandler);
NEVR_HOOK_RECORD(kUnloadRecord, UnloadThunk, &UnloadHandler);
NEVR_HOOK_RECORD(kLobbySettingsRecord, LobbySettingsThunk, &LobbySettingsHandler);

const char* StatusOf(sentinel::GotStatus status) noexcept {
  return status == sentinel::GotStatus::kOk ? "installed" : sentinel::GotStatusName(status);
}

void LogHook(const char* hook, const char* module, const char* symbol, const char* status, bool ok) noexcept {
  sentinel::LogFields(ok ? LogLevel::kInfo : LogLevel::kError, "combat_ec_hook",
                      {{"hook", hook}, {"module", module}, {"symbol", symbol}, {"status", status}});
}

template <typename Thunk, typename Record>
sentinel::GotStatus InstallOne(sentinel::GotHook& hook, const sentinel::GotTarget& target, const Record& record, sentinel::ImageLookup lookup) noexcept {
  Thunk::Arm(record);
  const sentinel::GotStatus got = sentinel::InstallThunk<Thunk>(hook, target, lookup);
  if (got != sentinel::GotStatus::kOk) Thunk::Disarm();
  return got;
}

template <typename T>
bool ReadWord(std::uintptr_t address, T* out) noexcept {
  return SafeRead(address, out, sizeof(T));
}

// The words that prove `lib`'s WriteLog calls WriteLogV through the pinned slot: the 25-word body (its `bl` included) and the PLT stub.
const char* LogLibWordsHold(const LogLib& lib, std::uintptr_t base) noexcept {
  for (std::size_t i = 0; i < kWriteLogWords; ++i) {
    std::uint32_t have = 0;
    const std::uint32_t want = i == kWriteLogBlIndex ? static_cast<std::uint32_t>(lib.blWord) : kWriteLogBody[i];
    if (!ReadWord(base + lib.writeLogFn + 4 * i, &have) || have != want) return "writelog_body_not_known";
  }
  for (std::size_t i = 0; i < 4; ++i) {
    std::uint32_t have = 0;
    if (!ReadWord(base + lib.stubAt + 4 * i, &have) || have != lib.stub[i]) return "writelogv_stub_not_known";
  }
  return nullptr;
}

template <int I>
sentinel::GotStatus InstallLogVThunk(const sentinel::GotTarget& target, sentinel::ImageLookup lookup) noexcept;
template <>
sentinel::GotStatus InstallLogVThunk<0>(const sentinel::GotTarget& t, sentinel::ImageLookup l) noexcept {
  return InstallOne<LogVThunk<0>>(g_logHook[0], t, kLogV0Record, l);
}
template <>
sentinel::GotStatus InstallLogVThunk<1>(const sentinel::GotTarget& t, sentinel::ImageLookup l) noexcept {
  return InstallOne<LogVThunk<1>>(g_logHook[1], t, kLogV1Record, l);
}
template <>
sentinel::GotStatus InstallLogVThunk<2>(const sentinel::GotTarget& t, sentinel::ImageLookup l) noexcept {
  return InstallOne<LogVThunk<2>>(g_logHook[2], t, kLogV2Record, l);
}
void* LogVEntry(int i) noexcept {
  return i == 0 ? LogVThunk<0>::EntryAddress() : i == 1 ? LogVThunk<1>::EntryAddress() : LogVThunk<2>::EntryAddress();
}

// One log library's WriteLogV hook. A platform library's slot must hold its own WriteLogV or libr15's (the loader may bind either); a
// slot that already holds our entry (this mapping was hooked) is kept. Returns a fixed token; "installed" when the hook is in.
const char* InstallLogLib(int i, const LogLib& lib, sentinel::ImageLookup lookup, std::uintptr_t r15Base) noexcept {
  sentinel::ElfImage image;
  if (!lookup(lib.module, &image)) return "module_not_loaded";
  if (const char* why = LogLibWordsHold(lib, image.base)) return why;
  std::uintptr_t word = 0;
  if (!ReadWord(image.base + lib.slot, &word)) return "slot_unreadable";
  if (word == reinterpret_cast<std::uintptr_t>(LogVEntry(i))) return "installed";
  if (i != 0) {
    if (word != image.base + lib.fn && (r15Base == 0 || word != r15Base + g_logLibs[0].fn)) return "slot_not_writelogv";
    g_logHook[i].Forget();  // a hook left from a mapping that is gone
  }
  const sentinel::GotTarget target = LogLibTarget(lib, image.base);
  const sentinel::GotStatus got = i == 0 ? InstallLogVThunk<0>(target, lookup) : i == 1 ? InstallLogVThunk<1>(target, lookup) : InstallLogVThunk<2>(target, lookup);
  return StatusOf(got);
}

// The platform libraries (1, 2): hooked when loaded; one line per change of outcome.
void EnsurePlatform(int i, sentinel::ImageLookup lookup) noexcept {
  sentinel::ElfImage r15;
  const std::uintptr_t r15Base = lookup(pinned::kLibR15, &r15) ? r15.base : 0;
  pthread_mutex_lock(&g_platformMu);
  const char* const status = InstallLogLib(i, g_logLibs[i], lookup, r15Base);
  const bool ok = std::strcmp(status, "installed") == 0;
  g_logOn[i].store(ok, std::memory_order_release);
  const int code = ok ? 1 : std::strcmp(status, "module_not_loaded") == 0 ? 2 : 3;
  const bool changed = g_platformLogged[i] != code;
  g_platformLogged[i] = code;
  pthread_mutex_unlock(&g_platformMu);
  if (!changed || code == 2) return;  // a library that is not loaded yet is not news
  sentinel::LogFields(ok ? LogLevel::kInfo : LogLevel::kWarn, "combat_ec_platform",
                      {{"lib", LogLibName(i)}, {"status", status},
                       {"msg", ok ? "the library's log lines are watched for the selected tac mod"
                                  : "the library's log lines are not watched: a tac mod it reports is not stored"}});
}

void SendPacket(const Dest& d, std::uint8_t type, std::uint32_t id) noexcept {
  const Packet p = {kMagic, type, {0, 0, 0}, id, kMagic2};
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = d.addr;
  to.sin_port = d.port;
  const SendThunk::Fn real = SendThunk::Original();
  if (real != nullptr) real(d.sock, &p, sizeof p, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
}

bool Pow2(std::uint32_t n) noexcept { return n != 0 && (n & (n - 1)) == 0; }

// The source's Main loop locals.
struct Loop {
  std::uint32_t seenUse[4] = {};
  std::uint32_t seenSteps = 0, seenLocShown = 0, seenVeils = 0, seenBarriers = 0, seenSkips = 0;
  std::uint64_t nextPick = 0, nextHello = 0, nextSummary = 0, nextLocToken = 0, nextList = 0;
  TacList tacList;
  int listWas = 0;
  bool keepWas = false;
  std::uint64_t selWas = 0, selChangedMs = 0;
  Dest logged = {};
  bool answered = false, pageAns = false;
  int lines = 0, summaries = 0, locLines = 0;
  std::uint32_t pid = 0;
};
Loop g_loop;

void LoopInit() noexcept {
  g_loop = Loop();
  std::uint8_t* const page = sentinel::EcPage();
  for (int k = 0; k < 4; ++k) g_loop.seenUse[k] = Load32(page + UseOf(k));
  g_loop.seenSteps = Load32(page + kDroneBlock + kDroneSteps);
  g_loop.seenLocShown = Load32(page + kLocTok + 0x10);
  g_loop.seenVeils = Load32(page + kVeilTok + 0x10);
  g_loop.seenBarriers = Load32(page + kVeilTok + 0x14);
  g_loop.seenSkips = Load32(page + kEquipSkips);
  g_loop.pid = static_cast<std::uint32_t>(getpid());
}

// A capped line of the step.
bool StepLine() noexcept {
  if (g_loop.lines >= kLineCap) return false;
  ++g_loop.lines;
  return true;
}

void* Main(void*) {
  for (;;) {
    DrainLobbySettings();  // #2333: in scope or not, so a document read before a combat map loads is in force when it does
    const std::uint64_t now100 = Now100();
    if (RefreshScope(now100 / 10000)) {
      Step(now100);
      usleep(kStepUs);
    } else {
      usleep(static_cast<useconds_t>(kScopeEveryMs * 1000));  // out of scope: at most 4 looks a second, nothing sent
    }
  }
  return nullptr;
}

void WarnLegacySwitches() noexcept {
  static constexpr const char* kFiles[] = {"ec.on", "ec.off", "ec_preset1.on", "ec_preset2.on", "ec_preset3.on"};
  for (const char* file : kFiles) {
    if (!legacy::Present(file)) continue;
    sentinel::LogFields(LogLevel::kWarn, "combat_ec_legacy_switch",
                        {{"file", file}, {"replaced_by", std::strcmp(file, "ec.off") == 0 ? "combat.ec: false" : "combat.ec_words"},
                         {"msg", "the plugin's switch file is no longer read and changes nothing; set the combat key in config.yaml instead"}});
  }
}

void LogPage() noexcept {
  bool reopened = false;
  const void* seen = nullptr;
  const sentinel::EcPageState st = sentinel::PublishEcPage(&reopened, &seen);
  char pageText[19], seenText[19];
  sentinel::LogFields(st == sentinel::EcPageState::kVisible ? LogLevel::kInfo : st == sentinel::EcPageState::kInvisible ? LogLevel::kWarn : LogLevel::kError,
                      "combat_ec_page",
                      {{"page", sentinel::HexString(pageText, reinterpret_cast<std::uintptr_t>(sentinel::EcPage()))},
                       {"global_lookup", sentinel::HexString(seenText, reinterpret_cast<std::uintptr_t>(seen))},
                       {"reopened_global", reopened ? 1 : 0},
                       {"state", st == sentinel::EcPageState::kVisible ? "visible" : st == sentinel::EcPageState::kInvisible ? "invisible" : "other"},
                       {"msg", st == sentinel::EcPageState::kVisible ? "ec: page published: the EC script libraries find re33_ec_page by dlsym"
                               : st == sentinel::EcPageState::kInvisible
                                   ? "re33_ec_page is not visible to dlsym(RTLD_DEFAULT): EC scripts decide as on a PC without the EC network client"
                                   : "another re33_ec_page is already global: the scripts use that one, not the sentinel's"}});
}

ModuleResult InstallLine(const char* result, const char* reason, const char* net, const char* gear, bool ok, const char* consequence) noexcept {
  sentinel::LogFields(ok ? LogLevel::kInfo : LogLevel::kError, "combat_ec_install",
                      {{"result", result}, {"reason", reason}, {"net", net}, {"gear_gate", gear},
                       {"writelogv_libr15", g_logOn[0].load() ? "installed" : "not_installed"},
                       {"writelogv_pnsovr", g_logOn[1].load() ? "installed" : "not_loaded_or_refused"},
                       {"writelogv_pnsrad", g_logOn[2].load() ? "installed" : "not_loaded_or_refused"}, {"consequence", consequence}});
  return {ok, result};
}

ModuleResult Refused(const char* token) noexcept {
  InstallLine("refused", token, "not_tried", "not_tried", false,
                     "the EC client is not installed: EC scripts decide as on a PC without the EC network client, and the game's traffic is untouched");
  return {false, token};
}

}  // namespace

bool InCombatScope() noexcept { return g_inScope.load(std::memory_order_acquire); }

void Tick(std::uintptr_t game) noexcept {
  if (g_game.load(std::memory_order_relaxed) != game) g_game.store(game, std::memory_order_release);
}

bool RefreshScope(std::uint64_t nowMs) noexcept {
  if (g_nextScopeMs != 0 && nowMs < g_nextScopeMs) return InCombatScope();
  g_nextScopeMs = nowMs + kScopeEveryMs;
  const int forced = g_forceScope.load(std::memory_order_acquire);
  level::Kind kind = level::Kind::kNone;
  std::uint64_t levelId = 0;
  const std::uintptr_t game = g_game.load(std::memory_order_acquire);
  if (forced >= 0) {
    kind = forced != 0 ? level::Kind::kCombatOrLobby : level::Kind::kNone;
  } else if (game != 0) {
    kind = level::Classify(game, /*lobby=*/true, &levelId);
  }
  const bool in = kind == level::Kind::kCombatOrLobby;
  const bool was = g_inScope.exchange(in, std::memory_order_acq_rel);
  if (in != was || kind != g_lastKind || !g_scopeLogged) {
    g_scopeLogged = true;
    g_lastKind = kind;
    if (!in) {  // leaving: no server, no net tick, the page's answer word cleared
      g_gameFd.store(-1, std::memory_order_relaxed);
      g_gameSrc.store(0, std::memory_order_relaxed);
      Store32(sentinel::EcPage() + kQuestEcServer, 0);
      g_loop.pageAns = false;
    }
    char levelText[19];
    sentinel::LogFields(LogLevel::kInfo, "combat_ec_scope",
                        {{"in_scope", in ? 1 : 0},
                         {"level_class", kind == level::Kind::kCombatOrLobby ? "combat_or_lobby" : kind == level::Kind::kEchoArena ? "echo_arena" : game == 0 && forced < 0 ? "no_game" : "other"},
                         {"level", sentinel::HexString(levelText, levelId)},
                         {"msg", in ? "a combat map or the combat lobby is loaded: the EC client acts"
                                    : "no combat map or combat lobby is loaded: the EC client passes every call to the game and sends nothing"}});
  }
  return in;
}

bool OnNetTick(NetTickFn tick) noexcept {
  if (tick == nullptr) return false;
  for (auto& t : g_netTick) {
    NetTickFn empty = nullptr;
    if (t.compare_exchange_strong(empty, tick, std::memory_order_acq_rel)) return true;
  }
  return false;
}

void OnDroneKilled(DroneKilledFn handler) noexcept { g_droneKilled.store(handler, std::memory_order_release); }

bool Installed() noexcept { return g_installed.load(std::memory_order_acquire); }

void OnModuleLoaded(const char* name, void* handle) noexcept {
  if (name == nullptr || handle == nullptr || !Installed()) return;
  const char* const slash = std::strrchr(name, '/');
  const char* const base = slash != nullptr ? slash + 1 : name;
  for (int i = 1; i < static_cast<int>(kLogLibs); ++i) {
    if (std::strcmp(base, g_logLibs[i].module) == 0) EnsurePlatform(i, Lookup());
  }
}

ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, const LogLib* logLibs, const Switches& switches) noexcept {
  WarnLegacySwitches();
  LogPage();
  if (!switches.on) {
    InstallLine("off", "off_config", "not_tried", "not_tried", true,
                "combat.ec is false: EC scripts decide as on a PC without the EC network client, and the game's traffic is untouched");
    return {false, "off", true};  // off by its key: counted apart on the phase line, neither installed nor failed
  }
  if (g_ran.exchange(true, std::memory_order_acq_rel)) return Refused("already_ran");
  const char* const words = switches.words != nullptr ? switches.words : kDefaultWords;
  g_locTokenS = LocTokenFromWords(words);
  {
    LockG l;
    g_st.types = TypesFromWords(words);
  }
  g_hideWrite = WholeWord(words, "hide");
  g_equipStore = EquipFromWords(words);
  char typesText[19];
  sentinel::LogFields(LogLevel::kInfo, "combat_ec_words",
                      {{"source", switches.words != nullptr ? "config" : "default"}, {"types_mask", sentinel::HexString(typesText, g_st.types)},
                       {"hide_write", g_hideWrite ? 1 : 0}, {"equip_store", g_equipStore ? 1 : 0},
                       {"loctoken_s", static_cast<long long>(g_locTokenS)}});

  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(pinned::kLibR15, &image)) return Refused("module_not_loaded");
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused("no_build_id");
  if (std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0) return Refused("build_id_mismatch");
  const std::uintptr_t base = image.base;
  g_logLibs = logLibs;

  // The scope reads the game the tick hands it: with no tick the client could never act, so it installs nothing.
  if (!tick::Add(&Tick)) return Refused("tick_table_full");

  // recvfrom + sendto: both or none.
  const sentinel::GotStatus recv = InstallOne<RecvThunk>(g_recvHook, LibcTarget(kRecvfromSymbol, pins.recvfromSlot), kRecvRecord, lookup);
  LogHook("recvfrom", pinned::kLibR15, kRecvfromSymbol, StatusOf(recv), recv == sentinel::GotStatus::kOk);
  if (recv != sentinel::GotStatus::kOk) return Refused(StatusOf(recv));
  const sentinel::GotStatus send = InstallOne<SendThunk>(g_sendHook, LibcTarget(kSendtoSymbol, pins.sendtoSlot), kSendRecord, lookup);
  LogHook("sendto", pinned::kLibR15, kSendtoSymbol, StatusOf(send), send == sentinel::GotStatus::kOk);
  if (send != sentinel::GotStatus::kOk) {
    g_recvHook.Remove();
    return Refused(StatusOf(send));
  }

  // #2333: what the server states it runs. BeginMultiplayer reads the slot when it registers the callback, after this install.
  // Refused: the tac mods stay hidden on every server (the hide's gate stays closed), and the rest of the client works.
  const sentinel::GotStatus settings =
      InstallOne<LobbySettingsThunk>(g_lobbySettingsHook, LobbySettingsTarget(pins, base), kLobbySettingsRecord, lookup);
  LogHook("lobby_settings_cb", pinned::kLibR15, kLobbySettingsSymbol, StatusOf(settings), settings == sentinel::GotStatus::kOk);
  g_settingsOn = settings == sentinel::GotStatus::kOk;

  // The selected tac mod: libr15 now, the platform libraries when they load.
  {
    pthread_mutex_lock(&g_platformMu);
    const char* const status = InstallLogLib(0, logLibs[0], lookup, base);
    pthread_mutex_unlock(&g_platformMu);
    const bool ok = std::strcmp(status, "installed") == 0;
    g_logOn[0].store(ok, std::memory_order_release);
    LogHook("writelogv", pinned::kLibR15, kWriteLogVSymbol, status, ok);
  }

  // The hide's gate: Reload / Unload, both or none.
  const char* gear = "installed";
  const sentinel::GotStatus reload =
      InstallOne<ReloadThunk>(g_reloadHook, LibR15FnTarget(kReloadSymbol, pins.reloadSlot, base, pins.reloadFn), kReloadRecord, lookup);
  LogHook("reload_equipment_config", pinned::kLibR15, kReloadSymbol, StatusOf(reload), reload == sentinel::GotStatus::kOk);
  if (reload != sentinel::GotStatus::kOk) {
    gear = StatusOf(reload);
  } else {
    const sentinel::GotStatus unload =
        InstallOne<UnloadThunk>(g_unloadHook, LibR15FnTarget(kUnloadSymbol, pins.unloadSlot, base, pins.unloadFn), kUnloadRecord, lookup);
    LogHook("unload_equipment_config", pinned::kLibR15, kUnloadSymbol, StatusOf(unload), unload == sentinel::GotStatus::kOk);
    if (unload != sentinel::GotStatus::kOk) {
      g_reloadHook.Remove();
      gear = StatusOf(unload);
    }
  }
  g_listOn = std::strcmp(gear, "installed") == 0;
  g_tacListWord.store(base + pins.tacModList, std::memory_order_release);

  LoopInit();
  g_installed.store(true, std::memory_order_release);
  for (int i = 1; i < static_cast<int>(kLogLibs); ++i) EnsurePlatform(i, lookup);
  const bool observing = integration::AddModuleLoadedObserver(&OnModuleLoaded);
  if (!g_noThread) {
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, Main, nullptr) != 0) {
      sentinel::LogFields(LogLevel::kError, "combat_ec_install",
                          {{"result", "partial"}, {"reason", "thread_not_started"},
                           {"consequence", "the hooks are in but nothing is sent: the EC client is idle"}});
    }
    pthread_attr_destroy(&attr);
  }
  const bool full = g_listOn && g_logOn[0].load() && observing && g_settingsOn;
  return InstallLine(full ? "installed" : "partial", full ? "ok" : !observing ? "observer_table_full" : "hook_refused", "installed", gear, true,
                     full ? "the EC client acts on a combat map and the combat lobby; every other level keeps the game's own traffic"
                          : "the EC network client acts on a combat map and the combat lobby; a part that is not installed is named above");
}

ModuleResult Install(const Switches& switches) noexcept {
  const Pins* const pins = g_pinsOverride.load(std::memory_order_acquire);
  const LogLib* const libs = g_logLibsOverride.load(std::memory_order_acquire);
  return InstallAt(Lookup(), pins != nullptr ? *pins : kPins, libs != nullptr ? libs : kLogLib, switches);
}

void Step(std::uint64_t now100) noexcept {
  DrainLobbySettings();  // #2333
  Loop& L = g_loop;
  std::uint8_t* const page = sentinel::EcPage();
  const std::uint64_t nowMs = now100 / 10000;
  const std::uint32_t skips = Load32(page + kEquipSkips);
  if (skips != L.seenSkips) {
    L.seenSkips = skips;
    char sel[19];
    if (StepLine())
      sentinel::LogFields(LogLevel::kInfo, "combat_ec_skip",
                          {{"selected", sentinel::HexString(sel, Load64(page + kEquipSel))}, {"skipped_so_far", skips},
                           {"msg", "a tac mod that is not the selected one was used: its node skipped"}});
  }
  Use uses[4];
  Dest d{};
  bool haveDest, ans;
  int n;
  {
    LockG l;
    n = WatchUses(g_st, page, L.seenUse, static_cast<std::uint32_t>(nowMs), L.pid, now100, uses);
    if (nowMs >= L.nextPick) {
      L.nextPick = nowMs + 1000;
      g_st.PickServer();
    }
    haveDest = g_st.Server(&d);
  }
  ans = nevr_server_features::Client().Current().echoCombat;  // #2333: the server's statement, not an answer to the hello
  g_gameSrc.store(haveDest ? (static_cast<std::uint64_t>(d.addr) << 16) | d.port : 0, std::memory_order_relaxed);
  g_gameFd.store(haveDest ? d.sock : -1, std::memory_order_relaxed);  // cleared when no server is picked
  for (int i = 0; i < n; ++i) {
    if (haveDest) {
      for (int rep = 0; rep < 2; ++rep) SendPacket(d, uses[i].type, uses[i].id);  // twice, as upstream
    }
    if (StepLine()) {
      const char t[2] = {static_cast<char>(uses[i].type), 0};
      char idText[19];
      sentinel::LogFields(LogLevel::kInfo, "combat_ec_used",
                          {{"type", t}, {"id", sentinel::HexString(idText, uses[i].id)}, {"sent", haveDest ? "twice to the server" : "no (no server seen yet)"}});
    }
  }
  if (nowMs >= L.nextHello) {
    L.nextHello = nowMs + 2000;
    if (haveDest) {
      SendPacket(d, 'H', 0);
      if ((d.addr != L.logged.addr || d.port != L.logged.port) && StepLine()) {
        L.logged = d;
        char ip[24];
        const std::uint8_t* a = reinterpret_cast<const std::uint8_t*>(&d.addr);
        std::snprintf(ip, sizeof ip, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
        sentinel::LogFields(LogLevel::kInfo, "combat_ec_server",
                            {{"addr", ip}, {"port", ntohs(d.port)}, {"fd", d.sock}, {"msg", "ec: server: the game's own connection is the EC network's server"}});
      }
    }
  }
  if (g_listOn && nowMs >= L.nextList) {
    L.nextList = nowMs + 250;
    std::uint64_t count = 0;
    int r = 0;
    // one of ours selected = the list stays whole (with the hide word only; +e80 is 0 with `noequip`)
    const std::uint64_t sel = Load64(page + kEquipSel);
    const bool keep = g_hideWrite && !ans && OurSelected(sel);
    if (sel != L.selWas) {
      L.selWas = sel;
      L.selChangedMs = nowMs;  // SFD-1: a cut waits till the selection is still
    }
    {
      bool ok = true;
      const auto list = SafeRead<std::uintptr_t>(g_tacListWord.load(std::memory_order_acquire), &ok);
      g_gear.Lock();
      if (ok) r = TacModsFor(L.tacList, list, ans, g_hideWrite, &count, g_gear.gen.load(std::memory_order_relaxed), keep, HideSettled(nowMs, L.selChangedMs));
      g_gear.Unlock();
    }
    if (keep != L.keepWas) {
      L.keepWas = keep;
      char selText[19];
      if (StepLine())
        sentinel::LogFields(LogLevel::kInfo, "combat_ec_tacmods",
                            {{"selected", sentinel::HexString(selText, sel)},
                             {"msg", keep ? "ec: tac mods: an EC tac mod is selected: the list stays whole on this server"
                                          : "ec: tac mods: no EC tac mod kept: the hide rule applies again"}});
    }
    if (r != L.listWas) {
      L.listWas = r;
      if (r != 0 && StepLine()) {
        sentinel::LogFields(LogLevel::kInfo, "combat_ec_tacmods",
                            {{"server", r == 1 ? "EC" : "no EC"},
                             {"effect", !g_hideWrite ? (r == 1 ? "would be shown again (no hide word, nothing written)" : "would be hidden (no hide word, nothing written)")
                                        : r == 1 ? "shown" : r == 3 ? "shown again: one of them is selected" : "hidden"},
                             {"tac_mods", count}, {"msg", "ec: tac mods: smoke / buff / point"}});
      }
    }
  }
  if (ans != L.pageAns) {  // not under the line cap
    L.pageAns = ans;
    Store32(page + kQuestEcServer, ans ? 1 : 0);
  }
  if (ans != L.answered && StepLine()) {
    L.answered = ans;
    sentinel::LogFields(LogLevel::kInfo, "combat_ec_server_features",
                        {{"state", ans ? "stated" : "not_stated"},
                         {"msg", ans ? "ec: the server states EchoCombat" : "ec: no EchoCombat stated by this server (yet)"}});
  }
  if (g_locTokenS > 0 && nowMs >= L.nextLocToken) {
    const bool first = L.nextLocToken == 0;
    L.nextLocToken = nowMs + static_cast<std::uint64_t>(g_locTokenS * 1000);
    if (!first) {
      AddToken(page + kLocTok, now100);
      if (L.locLines < kLineCap) {
        ++L.locLines;
        sentinel::LogFields(LogLevel::kInfo, "combat_ec_loc", {{"count", Load32(page + kLocTok)}, {"msg", "ec: TEST loctoken: one LOC token added"}});
      }
    }
  }
  const std::uint32_t steps = Load32(page + kDroneBlock + kDroneSteps);
  if (steps != L.seenSteps) {
    if ((Pow2(steps) || steps < L.seenSteps) && StepLine())
      sentinel::LogFields(LogLevel::kInfo, "combat_ec_prelude", {{"steps", steps}, {"msg", "ec: 221a prelude steps"}});
    L.seenSteps = steps;
  }
  const std::uint32_t shown = Load32(page + kLocTok + 0x10);
  if (shown != L.seenLocShown) {
    L.seenLocShown = shown;
    if (L.locLines < kLineCap) {
      ++L.locLines;
      sentinel::LogFields(LogLevel::kInfo, "combat_ec_loc", {{"shown", shown}, {"msg", "ec: a translocator puck took a T token: effect shown"}});
    }
  }
  const std::uint32_t veils = Load32(page + kVeilTok + 0x10), barriers = Load32(page + kVeilTok + 0x14);
  if (veils != L.seenVeils || barriers != L.seenBarriers) {
    L.seenVeils = veils;
    L.seenBarriers = barriers;
    if (StepLine()) sentinel::LogFields(LogLevel::kInfo, "combat_ec_wall", {{"veil", veils}, {"barrier", barriers}, {"msg", "ec: wall decided"}});
  }
  if (nowMs >= L.nextSummary) {
    L.nextSummary = nowMs + 10000;
    if (L.summaries < 60) {
      ++L.summaries;
      std::uint64_t tokens, dropped, kills, answers;
      {
        LockG l;
        tokens = g_st.tokens;
        dropped = g_st.dropped;
        kills = g_st.kills;
        answers = g_st.answers;
      }
      sentinel::LogFields(LogLevel::kInfo, "combat_ec_sum",
                          {{"sendto", g_sends.load()}, {"passed", g_passed.load()}, {"consumed", g_consumed.load()}, {"A", answers},
                           {"tokens", tokens}, {"dropped", dropped}, {"K", kills}, {"server", haveDest ? "seen" : "none"}});
    }
  }
}

Stats StatsNow() noexcept {
  Stats s{};
  s.sends = g_sends.load();
  s.passed = g_passed.load();
  s.consumed = g_consumed.load();
  {
    LockG l;
    s.answers = g_st.answers;
    s.tokens = g_st.tokens;
    s.dropped = g_st.dropped;
    s.kills = g_st.kills;
    s.medi = g_st.medi;
    s.types = g_st.types;
    Dest d{};
    s.haveServer = g_st.Server(&d);
    s.serverAddr = d.addr;
    s.serverPort = d.port;
    s.serverFd = s.haveServer ? d.sock : -1;
    s.answered = nevr_server_features::Client().Current().echoCombat;
  }
  s.hideWrite = g_hideWrite;
  s.equipStore = g_equipStore;
  s.listOn = g_listOn;
  s.settingsOn = g_settingsOn;
  for (std::size_t i = 0; i < kLogLibs; ++i) s.logLib[i] = g_logOn[i].load();
  return s;
}

// ---- test support ----
void SetNoThreadForTest(bool on) noexcept { g_noThread = on; }
void SetClockForTest(std::uint64_t (*clock100)()) noexcept { g_clock = clock100; }
void ForceScopeForTest(int scope) noexcept {
  g_forceScope.store(scope, std::memory_order_release);
  g_nextScopeMs = 0;
}
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept { g_lookup.store(lookup, std::memory_order_release); }
void SetPinsForTest(const Pins* pins, const LogLib* logLibs) noexcept {
  g_pinsOverride.store(pins, std::memory_order_release);
  g_logLibsOverride.store(logLibs, std::memory_order_release);
}
std::uint32_t GearGenForTest() noexcept { return g_gear.gen.load(); }
void SetInstalledForTest(bool on) noexcept { g_installed.store(on, std::memory_order_release); }
void ResetForTest() noexcept {
  g_recvHook.Forget();
  g_sendHook.Forget();
  for (sentinel::GotHook& h : g_logHook) h.Forget();
  g_reloadHook.Forget();
  g_unloadHook.Forget();
  g_lobbySettingsHook.Forget();
  RecvThunk::Reset();
  SendThunk::Reset();
  LogVThunk<0>::Reset();
  LogVThunk<1>::Reset();
  LogVThunk<2>::Reset();
  ReloadThunk::Reset();
  UnloadThunk::Reset();
  LobbySettingsThunk::Reset();
  nevr_server_features::Client().Clear();
  pthread_mutex_lock(&g_settingsMu);
  g_settingsSize = 0;
  g_settingsBad = false;
  g_settingsSeq = g_settingsRead = 0;
  pthread_mutex_unlock(&g_settingsMu);
  {
    LockG l;
    g_st = State();
  }
  std::memset(sentinel::EcPage(), 0, sentinel::kEcPageSize);
  g_sends = 0;
  g_passed = 0;
  g_consumed = 0;
  g_killLines = 0;
  g_tokenLines = 0;
  g_selLines = 0;
  g_droneKilled = nullptr;
  for (auto& t : g_netTick) t = nullptr;
  g_gameFd = -1;
  g_gameSrc = 0;
  g_gameTid = 0;
  g_locTokenS = 0;
  g_tacListWord = 0;
  g_hideWrite = g_equipStore = g_listOn = g_settingsOn = false;
  g_installed = false;
  g_ran = false;
  for (auto& on : g_logOn) on = false;
  for (int& c : g_platformLogged) c = -1;
  g_inScope = false;
  g_game = 0;
  g_forceScope = -1;
  g_nextScopeMs = 0;
  g_lastKind = level::Kind::kNone;
  g_scopeLogged = false;
  g_lookup = nullptr;
  g_pinsOverride = nullptr;
  g_logLibsOverride = nullptr;
  g_logLibs = kLogLib;
  g_loop = Loop();
}

}  // namespace nevr_quest::combat::ec
