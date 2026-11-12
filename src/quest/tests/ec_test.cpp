// Host test of the EC client (src/quest/combat/ec.cpp, ec_core.h, ec_targets.h; #2187) against a libr15 image built in memory (the relocations
// of the six slots, the WriteLog body and its PLT stub at their pinned addresses, the tac-mod list word), the two platform libraries' images
// (libpnsovr.so, libpnsrad.so: their WriteLogV slot, body and stub), a fake libc (scripted datagrams), a fake clock and a fake CR15Game (the
// levels combat_level reads). Real GotHook, real thunks, the real handlers called THROUGH the slots as the game calls them.
//
// From the combat plugin's ec_test (#1591): the pure rules, the list hide, the wrappers, the net tick, the step, the hide and the selection keep
// their assertions. What changed, necessarily: the fixture (an ELF image and the manager's install, not a slot swap; a slot's original is what
// it held before the install, so the fakes are in the slots first), the lines (sentinel JSON records, `combat_ec_*`), the switch (`combat.ec` and
// `combat.ec_words`, not files: the plugin's files are warned about), recvfrom / sendto refused for an implausible word (the manager checks a
// libc slot for plausibility; a word inside libc that is not recvfrom is not caught, as before), WriteLogV's slot hooked in place of
// WriteLog's (ec_targets.h says why: the game's WriteLog body is what calls the hook here, through a va_list), the platform libraries pinned
// and hooked from the module-loaded observer, and the by-name lookup's own tests gone with it. New: the scope (arena: nothing consumed,
// counted or ticked; the arena lobbies; the 250 ms cadence), the refusals' tokens, the observer install, a library mapped again.
//
// Built and run by `just test-quest-combat ec`.
#include <atomic>
#include <dlfcn.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "quest/combat/combat_level.h"
#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/ec.h"
#include "quest/combat/ec_core.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/safe_read.h"
#include "quest/integration/post_load.h"
#include "quest/sentinel/ec_page.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/combat_scope_world.h"
#include "runtime/compat/server_features.h"

// The arena path's read count, through safe_read.h's test seam: every SafeRead after StartReadCount adds one.
namespace {
std::atomic<std::uint64_t> g_arenaReads{0};
[[maybe_unused]] void StartReadCount() {
  g_arenaReads.store(0);
  nevr_quest::combat::SetReadCounterForTest(&g_arenaReads);
}
[[maybe_unused]] std::uint64_t ReadCount() { return g_arenaReads.load(); }
// The arena path's level scans, through combat_level.h's process count: ScanCount is the scans since StartScanCount.
std::uint64_t g_scanBase = 0;
[[maybe_unused]] void StartScanCount() { g_scanBase = nevr_quest::combat::level::ScanCalls(); }
[[maybe_unused]] std::uint64_t ScanCount() { return nevr_quest::combat::level::ScanCalls() - g_scanBase; }
}  // namespace

using namespace nevr_quest::combat::ec;
using namespace nevr_quest::combat::ec::core;
using namespace ::nevr_ec;  // the shared rules (runtime/patch/ec_rules.h), as ec.cpp has them
namespace pinned = sentinel::pinned;
using combat_test::Image;
using sentinel::LogLevel;
using nevr_quest::combat::ModuleResult;

static int g_fail = 0;
#define CHECK(cond, ...)                                \
  do {                                                  \
    if (!(cond)) {                                      \
      ++g_fail;                                         \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
      std::printf(__VA_ARGS__);                         \
      std::printf("\n");                                \
    }                                                   \
  } while (0)

// ---- the lines ----
struct Line {
  LogLevel level;
  std::string text;
};
static std::vector<Line> g_lines;
static void Capture(LogLevel level, const char* line) { g_lines.push_back({level, line}); }
// `needle` anywhere in a line (an event is `"event":"combat_ec_x"`); level -1: any, 1 Info, 2 Warning, 3 Error (the plugin test's numbers).
static bool HasLine(const char* needle, int level = -1) {
  const LogLevel want = level == 1 ? LogLevel::kInfo : level == 2 ? LogLevel::kWarn : LogLevel::kError;
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || l.level == want)) return true;
  return false;
}
// a summary record (event combat_ec_sum) that carries `needle`
static bool HasSumLine(const char* needle) {
  for (const auto& l : g_lines)
    if (l.text.find("\"event\":\"combat_ec_sum\"") != std::string::npos && l.text.find(needle) != std::string::npos) return true;
  return false;
}

// ---- the page and the clock ----
static uint64_t g_now100 = 1000ull * 10000;  // 1000 ms
static uint64_t Clock() { return g_now100; }
static void AdvanceMs(uint64_t ms) { g_now100 += ms * 10000; }
// The combat level answer's clock (combat_scope.h) is the same fake clock, in ms.
static std::int64_t ScopeClock() noexcept { return static_cast<std::int64_t>(g_now100 / 10000); }
static uint8_t* EcPage() { return sentinel::EcPage(); }

// ---- the fakes the slots hold ----
static std::uintptr_t FakeWriteLogVRet = 0x1234;
static int g_v_level = -1;
static unsigned long long g_v_flags = 0;
static char g_v_text[512];
static int g_v_calls = 0;
static int g_v_lib = -1;
static std::uintptr_t LogVInto(int lib, int level, unsigned long long flags, const char* fmt, va_list ap) {
  ++g_v_calls;
  g_v_lib = lib;
  g_v_level = level;
  g_v_flags = flags;
  vsnprintf(g_v_text, sizeof g_v_text, fmt, ap);
  return FakeWriteLogVRet;
}
static std::uintptr_t FakeWriteLogV(int level, unsigned long long flags, const char* fmt, va_list ap) { return LogVInto(0, level, flags, fmt, ap); }
static std::uintptr_t FakeWriteLogVOvr(int level, unsigned long long flags, const char* fmt, va_list ap) { return LogVInto(1, level, flags, fmt, ap); }
static std::uintptr_t FakeWriteLogVRad(int level, unsigned long long flags, const char* fmt, va_list ap) { return LogVInto(2, level, flags, fmt, ap); }

// ---- the images ----
static Image g_image, g_ovr, g_rad;
// A libr15 image with the tick's slot row, on which the tick is installed and the scope row registers its refresh: the combat level answer can
// change, as the shipped table leaves it before this module's row.
static Image g_tickImage;
static bool TickImageLookup(const char* module, sentinel::ElfImage* out) {
  if (!g_tickImage.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_tickImage.Elf();
  return true;
}
static void StartTheScope() {
  nevr_quest::combat::tick::ResetForTest();
  nevr_quest::combat::scope::ResetForTest();
  nevr_quest::combat::scope::SetClockForTest(&ScopeClock);
  g_tickImage.Build({combat_test::scope_world::TickRow()});
  combat_test::scope_world::PutTickSlot(g_tickImage);
  CHECK(combat_test::scope_world::StartScope(&TickImageLookup), "the tick and the scope row did not start");
}
static uintptr_t g_base = 0;
static Pins g_pins = kPins;
static LogLib g_libs[kLogLibs] = {kLogLib[0], kLogLib[1], kLogLib[2]};
static uint64_t* W(uint64_t vaddr) { return reinterpret_cast<uint64_t*>(g_image.Slot(vaddr)); }
static bool Lookup(const char* module, sentinel::ElfImage* out) {
  Image* const img = std::strcmp(module, pinned::kLibR15) == 0 ? &g_image : std::strcmp(module, pinned::kPnsovr) == 0 ? &g_ovr
                     : std::strcmp(module, pinned::kPnsrad) == 0 ? &g_rad : nullptr;
  if (img == nullptr || !img->Present()) return false;
  *out = img->Elf();
  return true;
}

static int g_reload_calls = 0, g_unload_calls = 0;
static uint32_t g_gen_seen_in_reload = 0;
static void RealReload() {
  ++g_reload_calls;
  g_gen_seen_in_reload = GearGenForTest();
  // re-entry through the gate on the same thread (a Reload that reaches Unload again) must not hang
  if (g_reload_calls == 1) reinterpret_cast<void (*)()>(*W(kPins.unloadSlot))();
}
static void RealUnload() { ++g_unload_calls; }

// CR15NetGame::LobbySettingsCB(SMsg) stands in as this fake (#2333); the hook must forward every call to it unchanged.
static int g_settings_calls = 0;
static const void* g_settings_msg = nullptr;
static void RealLobbySettings(std::uintptr_t, const void* msg) {
  ++g_settings_calls;
  g_settings_msg = msg;
}
// A lobby-settings message as the game hands it to the callback: the text at +0x18, its size at +0x20 (server_features.h).
static void ServerDocument(const std::string& text) {
  static std::string keep;
  static unsigned char msg[0x40];
  keep = text;
  std::memset(msg, 0, sizeof msg);
  const char* p = keep.data();
  const std::uint64_t n = keep.size();
  std::memcpy(msg + 0x18, &p, sizeof p);
  std::memcpy(msg + 0x20, &n, sizeof n);
  reinterpret_cast<void (*)(std::uintptr_t, const void*)>(*W(kPins.lobbySettingsSlot))(0x1000, msg);
}
static void ServerStates(bool echoCombat) {
  ServerDocument(echoCombat ? R"({"nevr_server":{"features":["echocombat"]}})" : R"({"parties_unlocked":true})");
}

// the tac-mod list: { allocator, entries, count, capacity }, entries of 0x88 bytes, the name first
struct List {
  uintptr_t alloc;
  uintptr_t entries;
  uint64_t count;
  uint64_t cap;
};
alignas(16) static List g_list;
alignas(16) static char g_entries[16][0x88];
static void BuildList(int total, bool ours_last) {
  std::memset(&g_list, 0, sizeof g_list);
  std::memset(g_entries, 0, sizeof g_entries);
  for (int i = 0; i < total; ++i) std::snprintf(g_entries[i], 64, "mod%d", i);
  if (ours_last) {
    std::snprintf(g_entries[total - 3], 64, "smoke");
    std::snprintf(g_entries[total - 2], 64, "buff");
    std::snprintf(g_entries[total - 1], 64, "point");
  }
  g_list.entries = reinterpret_cast<uintptr_t>(g_entries);
  g_list.count = static_cast<uint64_t>(total);
  g_list.cap = 16;
}

static ssize_t FakeSendRet = 0;
struct Sent {
  int fd;
  uint8_t bytes[16];
  size_t len;
  uint32_t addr;
  uint16_t port;
};
static std::vector<Sent> g_sent;
static ssize_t FSend(int fd, const void* buf, size_t len, int, const sockaddr* to, socklen_t) {
  Sent s{};
  s.fd = fd;
  s.len = len;
  std::memcpy(s.bytes, buf, len < 16 ? len : 16);
  if (to != nullptr) {
    s.addr = reinterpret_cast<const sockaddr_in*>(to)->sin_addr.s_addr;
    s.port = reinterpret_cast<const sockaddr_in*>(to)->sin_port;
  }
  g_sent.push_back(s);
  return FakeSendRet != 0 ? FakeSendRet : static_cast<ssize_t>(len);
}
struct Dgram {
  std::vector<uint8_t> bytes;
  uint32_t addr;
  uint16_t port;
};
static std::vector<Dgram> g_in;
static size_t g_in_next = 0;
static int g_recv_calls = 0;
static int g_last_flags = 0;
static bool g_trunc_report = false;
static std::vector<socklen_t> g_fl_seen;
static ssize_t FRecv(int, void* buf, size_t len, int flags, sockaddr* from, socklen_t* fromlen) {
  ++g_recv_calls;
  g_last_flags = flags;
  if (g_recv_calls > 100000) {  // a wrapper that loops without end: fail at once instead of growing without bound
    std::printf("FAIL %s:%d: recvfrom called more than 100000 times by one test step (a runaway retry loop)\n", __FILE__, __LINE__);
    std::fflush(stdout);
    std::_Exit(1);
  }
  if (fromlen != nullptr && g_fl_seen.size() < 4096) g_fl_seen.push_back(*fromlen);
  if (g_in_next >= g_in.size()) {
    errno = EAGAIN;
    return -1;
  }
  const Dgram& d = g_in[(flags & MSG_PEEK) ? g_in_next : g_in_next++];
  const size_t n = d.bytes.size() < len ? d.bytes.size() : len;
  std::memcpy(buf, d.bytes.data(), n);
  if (from != nullptr && fromlen != nullptr && *fromlen >= sizeof(sockaddr_in)) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = d.addr;
    a.sin_port = d.port;
    std::memcpy(from, &a, sizeof a);
    *fromlen = sizeof a;
  }
  return static_cast<ssize_t>((flags & MSG_TRUNC) && g_trunc_report ? d.bytes.size() : n);
}
static Dgram Veil(char type, uint32_t id, uint32_t addr = 0x0a000001, uint16_t port = 6792) {
  Packet p = {kMagic, static_cast<uint8_t>(type), {0, 0, 0}, id, kMagic2};
  Dgram d;
  d.bytes.assign(reinterpret_cast<uint8_t*>(&p), reinterpret_cast<uint8_t*>(&p) + sizeof p);
  d.addr = addr;
  d.port = port;
  return d;
}
static Dgram Plain(size_t n, uint32_t addr = 0x0a000001, uint16_t port = 6792) {
  Dgram d;
  d.bytes.assign(n, 0x55);
  d.addr = addr;
  d.port = port;
  return d;
}

// One log library's words: the 25-word WriteLog body (its own `bl`) and the PLT stub (`bad_body`: one word changed; `bad_stub`: the stub's ldr).
static void PutLogWords(Image& img, const LogLib& lib, bool bad_body, bool bad_stub) {
  for (std::size_t i = 0; i < kWriteLogWords; ++i)
    img.Put<uint32_t>(lib.writeLogFn + 4 * i, i == kWriteLogBlIndex ? static_cast<uint32_t>(lib.blWord) : kWriteLogBody[i]);
  if (bad_body) img.Put<uint32_t>(lib.writeLogFn + 4 * 7, kWriteLogBody[7] ^ 1);
  for (std::size_t i = 0; i < 4; ++i) img.Put<uint32_t>(lib.stubAt + 4 * i, lib.stub[i]);
  if (bad_stub) img.Put<uint32_t>(lib.stubAt + 4, lib.stub[1] ^ 0x400);
}

// A platform library's image: its WriteLogV JUMP_SLOT holding its own WriteLogV (the fake), the body and the stub.
static void BuildPlatform(int i, unsigned bad = 0) {
  Image& img = i == 1 ? g_ovr : g_rad;
  LogLib& lib = g_libs[i];
  img.Build({{kWriteLogVSymbol, lib.slot, false, false, kLogLib[i].fn}}, lib.buildId, true, lib.module, 0x600000);
  const uintptr_t fake = reinterpret_cast<uintptr_t>(i == 1 ? &FakeWriteLogVOvr : &FakeWriteLogVRad);
  lib.fn = fake - img.Base();
  img.Put<uint64_t>(lib.slot, (bad & 1) ? 0x1234 : fake);
  PutLogWords(img, lib, (bad & 2) != 0, false);
}

// bad: which word to corrupt (1 recv implausible, 2 send implausible, 4 writelog body, 8 writelogv slot, 16 reload, 32 unload, 64 the stub,
// 128 the lobby settings callback's GLOB_DAT word)
static void BuildImage(unsigned bad = 0) {
  g_image.Build({{kRecvfromSymbol, kPins.recvfromSlot, false, false, 0},
                 {kSendtoSymbol, kPins.sendtoSlot, false, false, 0},
                 {kWriteLogVSymbol, kLogLib[0].slot, false, false, kLogLib[0].fn},
                 {kReloadSymbol, kPins.reloadSlot, false, false, kPins.reloadFn},
                 {kUnloadSymbol, kPins.unloadSlot, false, false, kPins.unloadFn},
                 {kLobbySettingsSymbol, kPins.lobbySettingsSlot, true, false, kPins.lobbySettingsFn}});
  g_base = g_image.Base();
  g_pins = kPins;
  g_pins.reloadFn = reinterpret_cast<uintptr_t>(&RealReload) - g_base;
  g_pins.unloadFn = reinterpret_cast<uintptr_t>(&RealUnload) - g_base;
  g_pins.lobbySettingsFn = reinterpret_cast<uintptr_t>(&RealLobbySettings) - g_base;
  g_libs[0] = kLogLib[0];
  g_libs[0].fn = reinterpret_cast<uintptr_t>(&FakeWriteLogV) - g_base;
  *W(kPins.recvfromSlot) = (bad & 1) ? 0x1234 : reinterpret_cast<uintptr_t>(&FRecv);
  *W(kPins.sendtoSlot) = (bad & 2) ? 0x1234 : reinterpret_cast<uintptr_t>(&FSend);
  *W(kLogLib[0].slot) = reinterpret_cast<uintptr_t>(&FakeWriteLogV) + ((bad & 8) ? 4 : 0);
  *W(kPins.reloadSlot) = reinterpret_cast<uintptr_t>(&RealReload) + ((bad & 16) ? 4 : 0);
  *W(kPins.unloadSlot) = reinterpret_cast<uintptr_t>(&RealUnload) + ((bad & 32) ? 4 : 0);
  *W(kPins.lobbySettingsSlot) = reinterpret_cast<uintptr_t>(&RealLobbySettings) + ((bad & 128) ? 4 : 0);
  PutLogWords(g_image, g_libs[0], (bad & 4) != 0, (bad & 64) != 0);
  BuildList(7, true);
  *W(kPins.tacModList) = reinterpret_cast<uintptr_t>(&g_list);
  SetPinsForTest(&g_pins, g_libs);
}

static void ResetAll(unsigned bad = 0, const char* words = "types=VPTO equip hide") {
  g_lines.clear();
  g_sent.clear();
  g_in.clear();
  g_in_next = 0;
  g_recv_calls = 0;
  g_reload_calls = g_unload_calls = 0;
  g_v_calls = 0;
  g_settings_calls = 0;
  g_settings_msg = nullptr;
  g_now100 = 1000ull * 10000;
  ResetForTest();
  StartTheScope();
  nevr_quest::integration::ClearModuleLoadedObserversForTest();
  SetNoThreadForTest(true);
  SetClockForTest(&Clock);
  SetImageLookupForTest(&Lookup);
  ForceScopeForTest(1);
  g_ovr.Drop();
  g_rad.Drop();
  for (int i = 1; i < static_cast<int>(kLogLibs); ++i) g_libs[i] = kLogLib[i];
  BuildImage(bad);
  (void)words;
}
static ModuleResult InstallWords(const char* words) {
  Switches sw;
  sw.words = words;
  const ModuleResult r = Install(sw);
  RefreshScope(g_now100 / 10000);
  return r;
}
static void InstallOk(const char* words = "types=VPTO equip hide") {
  const ModuleResult r = InstallWords(words);
  CHECK(r.installed, "install failed: %s", r.status);
}

using RecvFn = ssize_t (*)(int, void*, size_t, int, sockaddr*, socklen_t*);
using SendFn = ssize_t (*)(int, const void*, size_t, int, const sockaddr*, socklen_t);
using WriteLogVFn = std::uintptr_t (*)(int, unsigned long long, const char*, va_list);
static ssize_t GameRecv(int fd, void* buf, size_t len, int flags, sockaddr* from, socklen_t* fl) {
  auto fn = reinterpret_cast<RecvFn>(*W(kPins.recvfromSlot));
  return fn(fd, buf, len, flags, from, fl);
}
static ssize_t GameSend(int fd, const void* buf, size_t len, uint32_t addr, uint16_t port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = addr;
  a.sin_port = port;
  auto fn = reinterpret_cast<SendFn>(*W(kPins.sendtoSlot));
  return fn(fd, buf, len, 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
}
// What the game's WriteLog body does (ec_targets.h): va_start over the arguments and WriteLogV through the library's slot, with the result
// passed back.
static std::uintptr_t CallWriteLog(uint64_t* slot, int level, unsigned long long flags, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const std::uintptr_t r = reinterpret_cast<WriteLogVFn>(*slot)(level, flags, fmt, ap);
  va_end(ap);
  return r;
}
static std::uintptr_t GameWriteLog(int level, const char* fmt, const char* arg) { return CallWriteLog(W(kLogLib[0].slot), level, 7ull, fmt, arg); }

static uint32_t Server() { return 0x0a000001; }
static uint16_t SPort() { return 6792; }
// the game talks to the server: a send picks it
static void GameTalks(int fd = 7) {
  uint8_t b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  GameSend(fd, b, sizeof b, Server(), SPort());
}

static int g_tick_calls = 0;
static void NetTick() noexcept { ++g_tick_calls; }
static std::vector<uint32_t> g_kills;
static void OnKill(uint32_t i) noexcept { g_kills.push_back(i); }

// ---- the game's levels (combat_level.cpp's offsets, spelled here) ----
constexpr std::size_t kTable0 = 0x1f48, kCount0 = 0x1f78, kGamespaceOwner = 0x8, kGamespaceLevel = 0x58;
struct Gamespace {
  std::uintptr_t words[0x60 / 8 + 1] = {};
};
struct Game {
  std::uintptr_t words[0x2000 / 8] = {};
  std::uintptr_t table0[8][2] = {};
  Gamespace spaces[8];
};
static Game g_game;
static std::uintptr_t GameAddress() { return reinterpret_cast<std::uintptr_t>(g_game.words); }
static void LoadLevels(std::initializer_list<const char*> names) {
  g_game = Game{};
  int row = 0;
  for (const char* name : names) {
    Gamespace& gs = g_game.spaces[row];
    gs.words[kGamespaceOwner / 8] = GameAddress();
    gs.words[kGamespaceLevel / 8] = static_cast<std::uintptr_t>(EchoVR::CSymbol64Hash(name));
    g_game.table0[row][0] = 0x5555;
    g_game.table0[row][1] = reinterpret_cast<std::uintptr_t>(&gs);
    ++row;
  }
  g_game.words[kTable0 / 8] = reinterpret_cast<std::uintptr_t>(g_game.table0);
  g_game.words[kCount0 / 8] = static_cast<std::uintptr_t>(row);
}

static void CoreTests() {
  // the packet
  Packet p = {kMagic, 'T', {0, 0, 0}, 5, kMagic2};
  Packet out{};
  CHECK(Ours(&p, 16, &out) && out.id == 5, "a valid packet is not ours");
  CHECK(!Ours(&p, 15, &out) && !Ours(&p, 17, &out), "length not exact");
  Packet bad = p;
  bad.magic2 ^= 1;
  CHECK(!Ours(&bad, 16, &out), "second magic unchecked");
  bad = p;
  bad.magic ^= 1;
  CHECK(!Ours(&bad, 16, &out), "first magic unchecked");
  bad = p;
  bad.type = 'Z';
  CHECK(!Ours(&bad, 16, &out), "unknown type accepted");
  // #2358: every type EchoCombat v2.0.0's wire carries, match control's C L R G included (a PC server relays G and L to every client)
  for (char t : {'H', 'A', 'V', 'P', 'T', 'O', 'K', 'M', 'C', 'L', 'R', 'G'}) {
    bad = p;
    bad.type = static_cast<uint8_t>(t);
    CHECK(Ours(&bad, 16, &out), "type %c not known", t);
  }

  // tokens: expiry restarts the count
  alignas(8) uint8_t tok[0x20] = {};
  AddToken(tok, 100);
  AddToken(tok, 200);
  CHECK(*reinterpret_cast<uint32_t*>(tok) == 2, "two tokens within life: %u", *reinterpret_cast<uint32_t*>(tok));
  CHECK(*reinterpret_cast<uint64_t*>(tok + 8) == 200 + kTokenLife, "expiry");
  AddToken(tok, 200 + kTokenLife + 1);
  CHECK(*reinterpret_cast<uint32_t*>(tok) == 1, "an expired block must restart at 1: %u", *reinterpret_cast<uint32_t*>(tok));

  // the words
  CHECK(TypesFromWords("") == 4 && TypesFromWords("types=VPTO") == 15 && TypesFromWords("a types=VT b") == 5, "types parse");
  CHECK(TypesFromWords("types=VZ") == 4 && TypesFromWords("types=") == 4 && TypesFromWords("xtypes=V") == 4, "types fallback");
  CHECK(WholeWord("types=VT hide", "hide") && !WholeWord("types=VT hidey", "hide") && !WholeWord("nohide", "hide") && WholeWord("hide\n", "hide"),
        "WholeWord");
  CHECK(EquipFromWords("equip hide") && !EquipFromWords("noequip") && EquipFromWords("types=T"), "EquipFromWords");
  CHECK(LocTokenFromWords("loctoken=5") == 5 && LocTokenFromWords("loctoken=0.5") == 0 && LocTokenFromWords("loctoken=601") == 0 &&
            LocTokenFromWords("x") == 0 && LocTokenFromWords("loctoken=1") == 1 && LocTokenFromWords("loctoken=600") == 600,
        "LocTokenFromWords");

  // the selection json
  char name[24];
  CHECK(SelectedFromJson("{\"ability\":\"smoke\",\"x\":1}", name, sizeof name) == 0xe32dc7c9d4804fb5ull && !std::strcmp(name, "smoke"), "smoke symbol");
  CHECK(SelectedFromJson("{\"ability\":\"wraith\"}", name, sizeof name) == 0x41d2d02f1b2a1316ull, "wraith symbol");
  CHECK(SelectedFromJson("{\"ability\":\"nonsense\"}", name, sizeof name) == 0 && !std::strcmp(name, "nonsense"), "unknown name: no symbol but the name");
  CHECK(SelectedFromJson("{\"abilit\":\"smoke\"}", name, sizeof name) == 0 && name[0] == 0, "no key");
  CHECK(SelectedFromJson("{\"ability\":\"smoke", name, sizeof name) == 0, "unterminated");
  CHECK(SelectedFromJson("{\"ability\":\"smokee\"}", name, sizeof name) == 0, "a longer name must not match");
  CHECK(SelectedFromJson("{\"ability\":\"smok\"}", name, sizeof name) == 0, "a prefix of a name must not match (exact length)");
  CHECK(SelectedFromJson("{\"ability\":\"smoke\"}", name, 3) == 0xe32dc7c9d4804fb5ull && !std::strcmp(name, "sm"), "name cut to the buffer");
  CHECK(IsProfileFmt(kProfileFmt) && !IsProfileFmt("[NSUSER] sent profile update") && !IsProfileFmt(nullptr) && !IsProfileFmt("x"), "IsProfileFmt");
  alignas(8) uint8_t page[0x1000] = {};
  *reinterpret_cast<uint32_t*>(page + kEquipReruns) = 9;
  StoreSelected(page, 0xe32dc7c9d4804fb5ull);
  CHECK(Load64(page + kEquipSel) == 0xe32dc7c9d4804fb5ull && Load32(page + kEquipReruns) == 0, "StoreSelected");
  CHECK(OurSelected(0xe32dc7c9d4804fb5ull) && OurSelected(0xe32dc7cad6864aa4ull) && OurSelected(0xc8c33e48386618b6ull) && !OurSelected(0xc8c33e4832761fbcull) &&
            !OurSelected(0),
        "OurSelected is smoke / point / buff only");

  // the server rules
  State st;
  st.types = 15;
  st.SawSend(7, 0x0a000001, 6792);
  st.SawSend(7, 0x0a000001, 6792);
  st.SawSend(8, 0x0a000002, 53);
  CHECK(st.best == 0, "first destination is the server until the pick");
  st.SawSend(8, 0x0a000002, 53);
  st.SawSend(8, 0x0a000002, 53);
  st.PickServer();
  CHECK(st.best == 1, "busiest destination wins: %d", st.best);
  CHECK(st.dests[1].count == 1 && st.dests[0].count == 0, "counts restart after the pick");
  // #2333: an 'A' (a server of an older build) is counted and opens nothing; the server's statement is its lobby settings
  Packet a = {kMagic, 'A', {0, 0, 0}, 0, kMagic2};
  st.Handle(page, a, 10);
  CHECK(st.answers == 1 && Load32(page + kQuestEcServer) == 0, "an A was not counted, or it wrote the page's EC-server word");
  st.best = 1;
  // dedupe and token types
  Packet t = {kMagic, 'T', {0, 0, 0}, 77, kMagic2};
  CHECK(st.Handle(page, t, 5) == 0 && st.tokens == 1 && Load32(page + kLocTok) == 1, "T token");
  st.Handle(page, t, 6);
  CHECK(st.tokens == 1, "a repeat id must not add a token");
  Packet k = {kMagic, 'K', {0, 0, 0}, 0x00030009, kMagic2};
  CHECK(st.Handle(page, k, 7) == 10 && st.kills == 1, "K returns index + 1 (low 16 bits)");
  CHECK(st.Handle(page, k, 8) == 0 && st.kills == 1, "K resend");
  Packet m = {kMagic, 'M', {0, 0, 0}, 1, kMagic2};
  st.Handle(page, m, 9);
  st.Handle(page, m, 9);
  CHECK(st.medi == 1, "#2358: an M counts once per id, as EchoCombat v2.0.0 takes it (a resend does nothing): %llu", static_cast<unsigned long long>(st.medi));
  State st1;  // step 1: T only
  st1.types = 4;
  Packet v = {kMagic, 'V', {0, 0, 0}, 500, kMagic2};
  st1.Handle(page, v, 9);
  CHECK(st1.dropped == 1 && st1.tokens == 0, "a type this build does not carry is dropped");
  CHECK(!st1.Carries('V') && st1.Carries('T') && !st1.Carries('Z'), "Carries");
  // id 0 is never first (zero-filled ring)
  State st0;
  CHECK(!st0.FirstTime(0), "id 0 counts as seen");
  // the ring holds 32
  State sr;
  for (uint32_t i = 1; i <= 33; ++i) sr.FirstTime(i);
  CHECK(sr.FirstTime(1) && !sr.FirstTime(33), "ring of 32: id 1 forgotten, 33 kept");
  // 16 destinations: the lowest count is replaced and best is reset when it was the one
  State sd;
  for (int i = 0; i < 16; ++i) sd.SawSend(1, 100 + i, 1);
  sd.SawSend(1, 100, 1);
  sd.SawSend(1, 999, 1);
  bool found = false;
  for (auto& d : sd.dests) found |= d.addr == 999;
  CHECK(found && sd.best == 0, "17th destination replaces a low-count one: best %d", sd.best);

  // WatchUses
  State sw;
  sw.types = 15;
  alignas(8) uint8_t pg[0x1000] = {};
  uint32_t seen[4] = {};
  Use uses[4];
  CHECK(WatchUses(sw, pg, seen, 10, 1, 1000, uses) == 0, "nothing moved");
  *reinterpret_cast<uint32_t*>(pg + kLocUse) = 1;
  *reinterpret_cast<uint32_t*>(pg + kVeilUse) = 1;
  int n = WatchUses(sw, pg, seen, 10, 1, 1000, uses);
  CHECK(n == 2 && uses[0].type == 'V' && uses[1].type == 'T', "two uses: %d", n);
  CHECK(Load32(pg + kVeilTok) == 1 && Load32(pg + kLocTok) == 0, "a V use gives a local token, a T use does not");
  CHECK(WatchUses(sw, pg, seen, 11, 1, 1001, uses) == 0, "uses are seen once");
  State s4;
  s4.types = 4;
  uint32_t seen4[4] = {};
  *reinterpret_cast<uint32_t*>(pg + kPointUse) = 1;
  *reinterpret_cast<uint32_t*>(pg + kOcUse) = 1;
  CHECK(WatchUses(s4, pg, seen4, 12, 1, 1002, uses) == 1 && uses[0].type == 'T', "uncarried types are seen but not sent");

  // the gate: recursive, gen counts rebuilds, the looker does not count
  {
    GearGate g;
    g.Enter();
    g.Enter();
    CHECK(g.gen.load() == 2, "gen counts each Enter");
    g.Leave();
    g.Leave();
    g.Lock();
    CHECK(g.gen.load() == 2, "the looker must not bump gen");
    g.Unlock();
  }
}

static void ListTests() {
  TacList tl;
  uint64_t cnt = 0;
  BuildList(7, true);
  const uintptr_t L = reinterpret_cast<uintptr_t>(&g_list);
  CHECK(TacModsFor(tl, L, false, true, &cnt) == 2 && g_list.count == 4 && cnt == 4, "no server answer: hidden: count %llu", (unsigned long long)g_list.count);
  CHECK(TacModsFor(tl, L, false, true, &cnt) == 0, "already hidden: nothing to do");
  CHECK(TacModsFor(tl, L, true, true, &cnt) == 1 && g_list.count == 7, "server answers: shown again");
  CHECK(TacModsFor(tl, L, true, true, &cnt) == 0, "already shown");
  // write=false only tells
  CHECK(TacModsFor(tl, L, false, false, &cnt) == 2 && g_list.count == 7 && cnt == 4, "write=false must not write");
  // keepWhole: a server without EC but one of ours is selected: whole
  CHECK(TacModsFor(tl, L, false, true, &cnt, 0, true) == 0 && g_list.count == 7, "keepWhole on a whole list: nothing to do");
  CHECK(TacModsFor(tl, L, false, true, &cnt, 0, false) == 2 && g_list.count == 4, "cut");
  CHECK(TacModsFor(tl, L, false, true, &cnt, 0, true) == 3 && g_list.count == 7, "keepWhole puts a cut list back (3)");
  // lowerOk = false: a cut that is due waits; putting back does not
  CHECK(TacModsFor(tl, L, false, true, &cnt, 0, false, false) == 0 && g_list.count == 7, "a cut waits while the selection moves");
  g_list.count = 4;
  CHECK(TacModsFor(tl, L, true, true, &cnt, 0, false, false) == 1 && g_list.count == 7, "putting back does not wait");
  // not ours at the end: nothing touched
  BuildList(7, true);
  std::snprintf(g_entries[5], 64, "buffx");
  TacList t2;
  CHECK(TacModsFor(t2, L, false, true, &cnt) == 0 && g_list.count == 7, "the last three are not smoke / buff / point: untouched");
  BuildList(7, true);
  std::snprintf(g_entries[4], 64, "buff");
  std::snprintf(g_entries[5], 64, "smoke");
  TacList t3;
  CHECK(TacModsFor(t3, L, false, true, &cnt) == 0, "the order matters");
  BuildList(2, false);
  TacList t4;
  CHECK(TacModsFor(t4, L, false, true, &cnt) == 0, "fewer than 3 entries");
  BuildList(7, true);
  g_list.cap = 5000;
  TacList t5;
  CHECK(TacModsFor(t5, L, false, true, &cnt) == 0, "an implausible capacity is refused");
  BuildList(7, true);
  g_list.cap = 6;
  TacList t6;
  CHECK(TacModsFor(t6, L, false, true, &cnt) == 0, "count above capacity is refused");
  BuildList(7, true);
  g_list.entries = 0;
  TacList t7;
  CHECK(TacModsFor(t7, L, false, true, &cnt) == 0, "no entries");
  CHECK(TacModsFor(t7, 0, false, true, &cnt) == 0 && TacModsFor(t7, 0x20, false, true, &cnt) == 0, "null / unreadable list");
  // a rebuild: new entries pointer = a new list whose full count is its own; and gen forgets the old full count
  BuildList(7, true);
  TacList t8;
  TacModsFor(t8, L, false, true, &cnt);
  CHECK(g_list.count == 4 && t8.full == 7, "remembered the full count");
  alignas(16) static char other[16][0x88];
  std::memset(other, 0, sizeof other);
  for (int i = 0; i < 5; ++i) std::snprintf(other[i], 64, "n%d", i);
  std::snprintf(other[2], 64, "smoke");
  std::snprintf(other[3], 64, "buff");
  std::snprintf(other[4], 64, "point");
  g_list.entries = reinterpret_cast<uintptr_t>(other);
  g_list.count = 5;
  TacModsFor(t8, L, false, true, &cnt);
  CHECK(g_list.count == 2 && t8.full == 5, "a rebuilt list at a new address: full count restarts (%llu, full %llu)", (unsigned long long)g_list.count,
        (unsigned long long)t8.full);
  // same address rebuilt shorter: only the gen tells
  g_list.count = 3;
  TacModsFor(t8, L, false, true, &cnt, 0);
  CHECK(t8.full == 5, "without a new gen the old full count stays");
  g_list.count = 3;
  TacModsFor(t8, L, false, true, &cnt, 1);
  CHECK(t8.full == 3, "a new gen must forget the full count: full %llu", (unsigned long long)t8.full);
  // #2358: the count that moved between the read and the write is left alone (the shared write is a compare-and-swap): a reader that
  // moves the count right after reading it stands for the game writing in between
  {
    BuildList(7, true);
    struct MovingReader {
      std::uintptr_t countAt;
      bool moved;
      bool Read(std::uintptr_t a, void* out, std::size_t n) {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        if (a == countAt && !moved) {
          moved = true;
          g_list.count = 6;
        }
        return true;
      }
    } mover{L + 0x10, false};
    TacList tm;
    CHECK(::nevr_ec::TacModsFor(tm, mover, L, false, true, &cnt, 0, false, true) == 0 && g_list.count == 6,
          "a count the game moved since the read was overwritten: %llu", static_cast<unsigned long long>(g_list.count));
  }
  // HideSettled
  CHECK(!HideSettled(1999, 0) && HideSettled(2000, 0), "settle is exactly 2000 ms");
}

static void WrapperTests() {
  // sendto: counts the destination, passes arguments and result through
  ResetAll();
  InstallOk();
  uint8_t b[8] = {9, 9, 9, 9, 9, 9, 9, 9};
  FakeSendRet = 5;
  CHECK(GameSend(7, b, sizeof b, Server(), SPort()) == 5, "sendto result not passed through");
  FakeSendRet = 0;
  CHECK(g_sent.size() == 1 && g_sent[0].fd == 7 && g_sent[0].len == 8 && g_sent[0].addr == Server() && g_sent[0].port == SPort(), "sendto arguments changed");
  Stats s = StatsNow();
  CHECK(s.sends == 1 && s.haveServer && s.serverAddr == Server() && s.serverPort == SPort() && s.serverFd == 7, "the destination was not learned");
  // a non-IPv4 destination is not counted (and still sent)
  sockaddr_in6 a6{};
  a6.sin6_family = AF_INET6;
  auto sendfn = reinterpret_cast<SendFn>(*W(kPins.sendtoSlot));
  for (int i = 0; i < 5; ++i) sendfn(9, b, 8, 0, reinterpret_cast<sockaddr*>(&a6), sizeof a6);
  sendfn(9, b, 8, 0, nullptr, 0);
  s = StatsNow();
  CHECK(s.sends == 7 && g_sent.size() == 7, "v6 / null destination not passed");
  // a short sockaddr is not read
  sockaddr_in sh{};
  sh.sin_family = AF_INET;
  sh.sin_addr.s_addr = 0x01010101;
  sh.sin_port = 77;
  for (int i = 0; i < 6; ++i) sendfn(10, b, 8, 0, reinterpret_cast<sockaddr*>(&sh), sizeof sh - 4);
  Step(g_now100);
  AdvanceMs(1001);
  Step(g_now100);
  s = StatsNow();
  CHECK(s.serverAddr == Server() && s.serverFd == 7, "a v6 or truncated destination became the server (addr %x fd %d)", s.serverAddr, s.serverFd);

  // recvfrom: a foreign datagram passes untouched
  ResetAll();
  InstallOk();
  g_in.push_back(Plain(40));
  uint8_t buf[256];
  sockaddr_in from{};
  socklen_t fl = sizeof from;
  CHECK(GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl) == 40 && g_recv_calls == 1, "a foreign datagram not passed");
  CHECK(StatsNow().passed == 1 && StatsNow().consumed == 0, "passed counter");
  // a 16-byte datagram that is not an EC network packet passes (wrong magic)
  g_in.clear();
  g_in_next = 0;
  g_in.push_back(Plain(16));
  fl = sizeof from;
  CHECK(GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl) == 16, "a 16-byte foreign datagram swallowed");
  // #2358: both magics and a type the wire does not carry is the game's; a PC server's match control ('G') is ours, taken out, nothing done
  g_in.clear();
  g_in_next = 0;
  g_recv_calls = 0;
  g_in.push_back(Veil('X', 21));
  fl = sizeof from;
  CHECK(GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl) == 16 && g_recv_calls == 1, "an unknown type was taken out");
  g_in.clear();
  g_in_next = 0;
  g_recv_calls = 0;
  g_in.push_back(Veil('G', 22));
  g_in.push_back(Plain(31));
  fl = sizeof from;
  CHECK(GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl) == 31 && g_recv_calls == 2 && StatsNow().consumed == 1 &&
            StatsNow().tokens == 0,
        "a G from a PC server reached the game: calls %d", g_recv_calls);
  ResetAll();
  InstallOk();
  // ours: a first-time T is consumed; the real recvfrom is called again and the NEXT datagram is returned
  g_in.clear();
  g_in_next = 0;
  g_recv_calls = 0;
  g_in.push_back(Veil('T', 11));
  g_in.push_back(Plain(30));
  fl = sizeof from;
  CHECK(GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl) == 30 && g_recv_calls == 2, "ours not consumed / next datagram not returned: calls %d", g_recv_calls);
  CHECK(fl == sizeof from, "fromlen must be the real one of the returned datagram");
  s = StatsNow();
  CHECK(s.consumed == 1 && s.tokens == 1 && Load32(EcPage() + kLocTok) == 1, "T token not added");
  CHECK(HasLine("\"event\":\"combat_ec_token\"") && HasLine("\"type\":\"T\""), "no token record");
  // the same id again: consumed, no second token; EAGAIN passes through when nothing else is queued
  g_in.clear();
  g_in_next = 0;
  g_in.push_back(Veil('T', 11));
  errno = 0;
  fl = sizeof from;
  CHECK(GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl) == -1 && errno == EAGAIN, "EAGAIN not passed after consuming ours");
  CHECK(StatsNow().tokens == 1, "a repeated id added a token");
  // MSG_PEEK: never consumed, passed as is
  g_in.clear();
  g_in_next = 0;
  g_in.push_back(Veil('T', 12));
  g_recv_calls = 0;
  fl = sizeof from;
  CHECK(GameRecv(7, buf, sizeof buf, MSG_PEEK, reinterpret_cast<sockaddr*>(&from), &fl) == 16 && g_recv_calls == 1 && StatsNow().tokens == 1,
        "a peeked datagram was consumed");
  CHECK(g_last_flags == MSG_PEEK, "flags changed");
  // a flood of ours: one call swallows at most 256 in a row, then the next passes through to the game
  g_in.clear();
  g_in_next = 0;
  for (int i = 0; i < 400; ++i) g_in.push_back(Veil('T', 11));  // a repeat id: no new token
  g_recv_calls = 0;
  fl = sizeof from;
  CHECK(GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl) == 16 && g_recv_calls == 257 && g_in_next == 257,
        "a flood of ours pinned the call: %d real calls, %zu consumed", g_recv_calls, g_in_next);
  // D3, counted (#2358): the 257th of ours is handed to the game and counted, in the stats and in the summary (written by Step, at most every 10 s)
  CHECK(StatsNow().capped == 1, "the datagram past the per-call cap was not counted");
  AdvanceMs(10000);
  Step(g_now100);
  CHECK(HasSumLine("\"capped\":1"), "the summary does not carry the datagram the cap passed on");
  g_in.clear();
  g_in_next = 0;
  // from != null and fromlen == null: passes untouched (the game asked for the sender but gave no length)
  g_in.clear();
  g_in_next = 0;
  g_in.push_back(Veil('T', 13));
  CHECK(GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), nullptr) == 16 && StatsNow().tokens == 1, "from without fromlen was consumed");
  // from == null: ours are still recognised (own storage)
  g_in.clear();
  g_in_next = 0;
  g_in.push_back(Veil('T', 14));
  g_in.push_back(Plain(20));
  CHECK(GameRecv(7, buf, sizeof buf, 0, nullptr, nullptr) == 20 && StatsNow().tokens == 2, "from == null: ours not recognised");
  // a call that asks for the real length (MSG_TRUNC) into a buffer shorter than a packet: the 16 bytes are not in the buffer
  g_in.clear();
  g_in_next = 0;
  g_in.push_back(Veil('T', 16));
  {
    Packet full = {kMagic, 'T', {0, 0, 0}, 16, kMagic2};
    std::memcpy(buf, &full, sizeof full);  // what an over-read would see
  }
  fl = sizeof from;
  g_trunc_report = true;
  CHECK(GameRecv(7, buf, 8, MSG_TRUNC, reinterpret_cast<sockaddr*>(&from), &fl) == 16 && StatsNow().tokens == 2, "a MSG_TRUNC read into a short buffer was consumed");
  g_trunc_report = false;
  // the length the game gave is restored for every retry (a retry after the first datagram was consumed)
  g_in.clear();
  g_in_next = 0;
  g_fl_seen.clear();
  g_in.push_back(Veil('T', 17));
  g_in.push_back(Plain(10));
  sockaddr_storage big;
  socklen_t bigl = sizeof big;
  GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&big), &bigl);
  CHECK(g_fl_seen.size() == 2 && g_fl_seen[0] == sizeof big && g_fl_seen[1] == sizeof big, "the retry did not get the caller's fromlen back: %zu entries, %u", g_fl_seen.size(),
        g_fl_seen.empty() ? 0u : (unsigned)g_fl_seen.back());
  // a buffer shorter than a packet: never ours
  g_in.clear();
  g_in_next = 0;
  g_in.push_back(Veil('T', 15));
  fl = sizeof from;
  const uint64_t tokens_before_short = StatsNow().tokens;
  CHECK(GameRecv(7, buf, 8, 0, reinterpret_cast<sockaddr*>(&from), &fl) == 8 && StatsNow().tokens == tokens_before_short, "a truncated read counted as ours");
  // an error passes through and is not counted as passed
  g_in.clear();
  g_in_next = 0;
  const uint64_t passed0 = StatsNow().passed;
  fl = sizeof from;
  CHECK(GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl) == -1 && StatsNow().passed == passed0, "an error counted as a datagram");
  // types this build does not carry: dropped
  ResetAll();
  InstallOk("types=T");
  g_in.push_back(Veil('V', 21));
  g_in.push_back(Plain(10));
  fl = sizeof from;
  GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
  CHECK(StatsNow().dropped == 1 && StatsNow().tokens == 0 && Load32(EcPage() + kVeilTok) == 0, "an uncarried V was kept");
  // K: handed to the handler once (first time), not returned, and 'M' counted
  ResetAll();
  InstallOk();
  g_kills.clear();
  OnDroneKilled(&OnKill);
  g_in.push_back(Veil('K', 0x00010007));
  g_in.push_back(Veil('K', 0x00010007));
  g_in.push_back(Veil('M', 3));
  g_in.push_back(Plain(10));
  fl = sizeof from;
  CHECK(GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl) == 10, "K / M datagrams reached the game");
  CHECK(g_kills.size() == 1 && g_kills[0] == 7 && StatsNow().kills == 1 && StatsNow().medi == 1, "K not handed over once with its index");
  CHECK(HasLine("\"event\":\"combat_ec_kill\"") && HasLine("\"handler\":\"yes\""), "no K record");
  // a K without a handler is dropped and logged
  ResetAll();
  InstallOk();
  g_in.push_back(Veil('K', 0x00010008));
  g_in.push_back(Plain(10));
  fl = sizeof from;
  GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
  CHECK(HasLine("\"handler\":\"none (dropped)\""), "a K without a handler is not reported");
  // #2333: the page word follows what the server states in its lobby settings; an 'A' (an older server) opens nothing
  ResetAll();
  InstallOk();
  GameTalks();
  Step(g_now100);
  g_in.push_back(Veil('A', 0, Server(), SPort()));
  g_in.push_back(Plain(10));
  fl = sizeof from;
  GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
  AdvanceMs(5);
  Step(g_now100);
  CHECK(!StatsNow().answered && Load32(EcPage() + kQuestEcServer) == 0 && StatsNow().answers == 1, "an A opened the gate or was not counted");
  ServerStates(true);
  CHECK(g_settings_calls == 1 && g_settings_msg != nullptr, "the game's lobby settings callback was not called");
  CHECK(!HasLine("combat_ec_lobby_settings") && !nevr_server_features::Client().Current().echoCombat,
        "the document was read on the hook frame (no nlohmann on a hook's path: the EC thread reads it)");
  AdvanceMs(5);
  Step(g_now100);
  CHECK(HasLine("\"event\":\"combat_ec_lobby_settings\",\"statement\":\"stated\",\"features\":\"echocombat\""), "no lobby settings record");
  CHECK(StatsNow().answered && Load32(EcPage() + kQuestEcServer) == 1, "the server's statement not shown on the page");
  CHECK(HasLine("ec: the server states EchoCombat"), "no server features record");
  ServerStates(false);
  AdvanceMs(5);
  Step(g_now100);
  CHECK(!StatsNow().answered && Load32(EcPage() + kQuestEcServer) == 0 && HasLine("ec: no EchoCombat stated by this server"),
        "a document that states nothing kept the page word");
  ServerDocument("{not json");
  Step(g_now100);
  CHECK(HasLine("\"statement\":\"malformed\"", 2) && g_settings_calls == 3, "a malformed document not a Warning, or not forwarded");
  ServerDocument(std::string(300 * 1024, ' '));  // over the handler's 256 KiB: not copied, read as malformed, still forwarded
  Step(g_now100);
  CHECK(HasLine("\"bytes\":307200") && g_settings_calls == 4, "a document over the cap not refused, or not forwarded");
  // a session in another lobby clears it (the router's success, server_features.h ClearForLobby)
  ServerStates(true);
  Step(g_now100);
  CHECK(nevr_server_features::Client().Current().echoCombat, "setup");
  const nevr_server_features::ClientState::Lobby other = {1, 2, 3};
  bool was = false;
  CHECK(nevr_server_features::Client().ClearForLobby(other, &was) && was && !nevr_server_features::Client().Current().echoCombat,
        "a new lobby did not clear the statement");
}

static void NetTickTests() {
  // the net tick: only on the game's own socket, on the thread that got a server datagram on it
  ResetAll();
  InstallOk();
  g_tick_calls = 0;
  CHECK(OnNetTick(&NetTick), "no tick slot");
  uint8_t buf[64];
  sockaddr_in from{};
  socklen_t fl = sizeof from;
  g_in.push_back(Plain(20, Server(), SPort()));
  GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
  CHECK(g_tick_calls == 0, "a tick before any server was picked");
  GameTalks(7);
  Step(g_now100);
  g_in.clear();
  g_in_next = 0;
  g_in.push_back(Plain(20, 0x0a0000aa, 99));  // another sender on the game socket: this thread is not (yet) the game thread
  fl = sizeof from;
  GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
  CHECK(g_tick_calls == 0, "ticked on a datagram of a stranger before the server's");
  g_in.clear();
  g_in_next = 0;
  g_in.push_back(Plain(20, Server(), SPort()));
  fl = sizeof from;
  GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
  CHECK(g_tick_calls == 1, "no tick on the server's datagram on the game socket: %d", g_tick_calls);
  g_in.clear();
  g_in_next = 0;
  g_in.push_back(Plain(20, Server(), SPort()));
  fl = sizeof from;
  GameRecv(8, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);  // another fd (libevent's DNS)
  CHECK(g_tick_calls == 1, "ticked on another socket");
  // the registered ticks are called in registration order, at most four
  CHECK(OnNetTick(&NetTick) && OnNetTick(&NetTick) && OnNetTick(&NetTick), "four slots");
  CHECK(!OnNetTick(&NetTick), "a fifth tick must be refused");
  // another thread: the tid gate
  g_in.clear();
  g_in_next = 0;
  g_tick_calls = 0;
  struct Arg {
    int calls;
  } arg{0};
  struct T {
    static void* Run(void*) {
      uint8_t b[64];
      sockaddr_in f{};
      socklen_t l = sizeof f;
      g_in.clear();
      g_in_next = 0;
      g_in.push_back(Plain(20, 0x0a0000aa, 99));  // not the server: this thread never becomes the game thread
      GameRecv(7, b, sizeof b, 0, reinterpret_cast<sockaddr*>(&f), &l);
      return nullptr;
    }
  };
  pthread_t th;
  pthread_create(&th, nullptr, &T::Run, &arg);
  pthread_join(th, nullptr);
  CHECK(g_tick_calls == 0, "another thread ticked");
  // the server is cleared: no tick
  Stats s = StatsNow();
  CHECK(s.haveServer, "server");
}

// Leaving the scope and coming back (ec.cpp RefreshScope, the `if (!in)` block; #2386). The leave clears the page's +0x40 word and the step's
// copy of it, so the first step back on a server that states EchoCombat writes the word again; and it clears the game socket the net tick follows,
// so a datagram that arrives after the client is back in scope and before its step has run again does not tick.
static void LeaveTests() {
  ResetAll();
  InstallOk();
  CHECK(OnNetTick(&NetTick), "no tick slot");
  g_tick_calls = 0;
  GameTalks(7);
  ServerStates(true);
  Step(g_now100);
  CHECK(Load32(EcPage() + kQuestEcServer) == 1, "setup: the step did not set the page's +0x40 word");
  uint8_t buf[64];
  sockaddr_in from{};
  socklen_t fl = sizeof from;
  g_in.push_back(Plain(20, Server(), SPort()));
  GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
  CHECK(g_tick_calls == 1, "setup: the server's datagram on the game socket did not run the net tick: %d", g_tick_calls);
  ForceScopeForTest(0);
  CHECK(!RefreshScope(g_now100 / 10000) && Load32(EcPage() + kQuestEcServer) == 0, "setup: leaving the scope kept the page's +0x40 word");
  ForceScopeForTest(1);
  CHECK(RefreshScope(g_now100 / 10000), "setup: not back in scope");
  // back in scope, before the step: the socket of the last scope is not followed
  g_in.push_back(Plain(20, Server(), SPort()));
  fl = sizeof from;
  GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
  CHECK(g_tick_calls == 1, "back in scope and before the step, a datagram on the last scope's game socket ran the net tick: %d", g_tick_calls);
  // the first step back writes the word again: the server still states EchoCombat
  AdvanceMs(5);
  Step(g_now100);
  CHECK(Load32(EcPage() + kQuestEcServer) == 1, "back in scope on a server that states EchoCombat, the step left the page's +0x40 word at 0");
}

// The leave forgets the server pick (ec.cpp RefreshScope, the `if (!in)` block; #2390). Out of scope the sendto hook does not watch the game's
// sends, so the last scope's socket and address are not known to be the game's when the client comes back. The first step back, 3000 ms out and
// with no send by the game since, sends nothing (no hello to the last scope's server through its socket) and follows no socket for the net tick;
// the game's next send picks the server, and the hello goes to it in its 2 s slot.
// The leave keeps the counters and the carried types (#2390): only the server pick is forgotten. The fields a leave must not touch (types, tokens,
// kills, medi, answers, dropped) are set nonzero before it, and compared after it and after the first step back.
static bool KeptCounters(const Stats& now, const Stats& kept) {
  return now.types == kept.types && now.tokens == kept.tokens && now.kills == kept.kills && now.medi == kept.medi && now.answers == kept.answers &&
         now.dropped == kept.dropped;
}

static void LeavePickTests() {
  ResetAll();
  // 'V' is not carried by this install (types=PTO), so the 'V' below is counted as dropped (ec_rules.h, BasicState::Client)
  InstallOk("types=PTO");
  CHECK(OnNetTick(&NetTick), "no tick slot");
  g_tick_calls = 0;
  GameTalks(7);
  Step(g_now100);
  Stats s = StatsNow();
  CHECK(s.haveServer && s.serverFd == 7, "setup: the game's send did not pick the server (fd %d)", s.serverFd);
  // a token, a kill, an M, an A and an uncarried V from the server on the game socket, in scope, before the leave
  g_in.push_back(Veil('T', 0x00020001, Server(), SPort()));
  g_in.push_back(Veil('K', 0x00020002, Server(), SPort()));
  g_in.push_back(Veil('M', 0x00020003, Server(), SPort()));
  g_in.push_back(Veil('A', 0, Server(), SPort()));
  g_in.push_back(Veil('V', 0x00020004, Server(), SPort()));
  for (int i = 0; i < 5; ++i) {
    uint8_t rb[64];
    sockaddr_in rf{};
    socklen_t rl = sizeof rf;
    GameRecv(7, rb, sizeof rb, 0, reinterpret_cast<sockaddr*>(&rf), &rl);
  }
  const Stats kept = StatsNow();
  CHECK(kept.tokens == 1 && kept.kills == 1 && kept.medi == 1 && kept.answers == 1 && kept.dropped == 1 && kept.types != 0,
        "setup: the counters before the leave are not all set (tokens %llu, kills %llu, medi %llu, answers %llu, dropped %llu, types %x)",
        static_cast<unsigned long long>(kept.tokens), static_cast<unsigned long long>(kept.kills), static_cast<unsigned long long>(kept.medi),
        static_cast<unsigned long long>(kept.answers), static_cast<unsigned long long>(kept.dropped), kept.types);
  ForceScopeForTest(0);
  CHECK(!RefreshScope(g_now100 / 10000), "setup: still in scope");
  CHECK(HasLine("\"in_scope\":0") && HasLine("forgets the server"), "the leave's scope line does not say the server pick is forgotten");
  s = StatsNow();
  CHECK(KeptCounters(s, kept),
        "the leave changed the carried types or a counter: types %x to %x, tokens %llu to %llu, kills %llu to %llu, dropped %llu to %llu",
        kept.types, s.types, static_cast<unsigned long long>(kept.tokens), static_cast<unsigned long long>(s.tokens),
        static_cast<unsigned long long>(kept.kills), static_cast<unsigned long long>(s.kills), static_cast<unsigned long long>(kept.dropped),
        static_cast<unsigned long long>(s.dropped));
  AdvanceMs(3000);
  ForceScopeForTest(1);
  CHECK(RefreshScope(g_now100 / 10000), "setup: not back in scope");
  g_sent.clear();
  Step(g_now100);
  s = StatsNow();
  CHECK(g_sent.empty(), "the first step back, 3000 ms out and no send by the game since, sent %zu packet(s), the first '%c' on the last scope's fd %d",
        g_sent.size(), g_sent.empty() ? '-' : static_cast<char>(g_sent[0].bytes[4]), g_sent.empty() ? -1 : g_sent[0].fd);
  CHECK(!s.haveServer, "the first step back kept the last scope's server (fd %d)", s.serverFd);
  CHECK(KeptCounters(s, kept),
        "the first step back after the leave changed the carried types or a counter: types %x, tokens %llu, kills %llu, dropped %llu",
        s.types, static_cast<unsigned long long>(s.tokens), static_cast<unsigned long long>(s.kills), static_cast<unsigned long long>(s.dropped));
  uint8_t buf[64];
  sockaddr_in from{};
  socklen_t fl = sizeof from;
  g_in.push_back(Plain(20, Server(), SPort()));
  GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
  CHECK(g_tick_calls == 0, "after the first step back a datagram on the last scope's game socket ran the net tick: %d", g_tick_calls);
  // the game sends to its server on another socket: that is the server, and the hello goes to it
  uint8_t b[8] = {};
  GameSend(9, b, sizeof b, 0x0a000002, SPort());
  g_sent.clear();
  AdvanceMs(2000);
  Step(g_now100);
  s = StatsNow();
  CHECK(s.haveServer && s.serverFd == 9 && s.serverAddr == 0x0a000002, "the game's send back in scope did not pick its server (fd %d)", s.serverFd);
  CHECK(g_sent.size() == 1 && g_sent[0].fd == 9 && g_sent[0].addr == 0x0a000002 && g_sent[0].bytes[4] == 'H',
        "the hello did not go to the server the game sends to: %zu sent, the first on fd %d", g_sent.size(), g_sent.empty() ? -1 : g_sent[0].fd);
}

static void StepTests() {
  // use counters -> packets twice to the server, hello every 2 s, server line once
  ResetAll();
  InstallOk();
  Step(g_now100);  // no server yet
  uint8_t* page = EcPage();
  __atomic_store_n(reinterpret_cast<uint32_t*>(page + kLocUse), 1u, __ATOMIC_SEQ_CST);
  AdvanceMs(5);
  Step(g_now100);
  CHECK(g_sent.empty(), "a use sent with no server");
  CHECK(HasLine("\"sent\":\"no (no server seen yet)\""), "no record for a use without a server");
  GameTalks(7);
  g_sent.clear();
  __atomic_store_n(reinterpret_cast<uint32_t*>(page + kLocUse), 2u, __ATOMIC_SEQ_CST);
  AdvanceMs(5);
  Step(g_now100);
  int t_pkts = 0, h_pkts = 0;
  for (auto& s : g_sent) {
    Packet p;
    std::memcpy(&p, s.bytes, 16);
    CHECK(s.fd == 7 && s.addr == Server() && s.port == SPort() && p.magic == kMagic && p.magic2 == kMagic2, "packet fields");
    t_pkts += p.type == 'T';
    h_pkts += p.type == 'H';
  }
  CHECK(t_pkts == 2, "a use must be sent twice: %d", t_pkts);
  CHECK(Load32(page + kLocTok) == 0, "a T use made a local token");
  CHECK(h_pkts == 0, "the first hello waits for its 2 s slot (the slot is armed by the first pass): %d", h_pkts);
  g_sent.clear();
  AdvanceMs(2000);
  Step(g_now100);
  CHECK(g_sent.size() == 1 && g_sent[0].bytes[4] == 'H' && HasLine("\"event\":\"combat_ec_server\"") && HasLine("\"addr\":\"1.0.0.10\""), "hello + server line");
  g_sent.clear();
  for (int i = 0; i < 19; ++i) {
    AdvanceMs(100);
    Step(g_now100);
  }
  CHECK(g_sent.empty(), "hello sent more than every 2 s: %zu in 1.9 s", g_sent.size());
  AdvanceMs(100);
  Step(g_now100);
  CHECK(g_sent.size() == 1, "no hello after 2 s: %zu", g_sent.size());
  // a V use gives a local token and sends twice
  g_sent.clear();
  __atomic_store_n(reinterpret_cast<uint32_t*>(page + kVeilUse), 1u, __ATOMIC_SEQ_CST);
  AdvanceMs(5);
  Step(g_now100);
  CHECK(Load32(page + kVeilTok) == 1, "a V use must make a local token");
  // the summary and the cap
  for (int i = 0; i < 3; ++i) {
    AdvanceMs(10000);
    Step(g_now100);
  }
  CHECK(HasLine("\"event\":\"combat_ec_sum\""), "no summary");
  // wall decided / shown / 221a steps
  __atomic_store_n(reinterpret_cast<uint32_t*>(page + kVeilTok + 0x10), 2u, __ATOMIC_SEQ_CST);
  __atomic_store_n(reinterpret_cast<uint32_t*>(page + kLocTok + 0x10), 3u, __ATOMIC_SEQ_CST);
  __atomic_store_n(reinterpret_cast<uint32_t*>(page + kDroneBlock + kDroneSteps), 4u, __ATOMIC_SEQ_CST);
  AdvanceMs(5);
  Step(g_now100);
  CHECK(HasLine("ec: wall decided") && HasLine("ec: a translocator puck took a T token") && HasLine("ec: 221a prelude steps"), "page counters not reported");
  __atomic_store_n(reinterpret_cast<uint32_t*>(page + kDroneBlock + kDroneSteps), 5u, __ATOMIC_SEQ_CST);
  g_lines.clear();
  AdvanceMs(5);
  Step(g_now100);
  CHECK(!HasLine("221a prelude steps"), "a non power of two step count was reported");
  __atomic_store_n(reinterpret_cast<uint32_t*>(page + kEquipSkips), 1u, __ATOMIC_SEQ_CST);
  AdvanceMs(5);
  Step(g_now100);
  CHECK(HasLine("a tac mod that is not the selected one was used"), "equip skip not reported");

  // loctoken TEST word: the first pass arms, then one LOC token every <s> seconds
  ResetAll();
  InstallOk("types=T loctoken=2");
  Step(g_now100);
  CHECK(Load32(EcPage() + kLocTok) == 0, "a loc token on the arming pass");
  AdvanceMs(1990);
  Step(g_now100);
  CHECK(Load32(EcPage() + kLocTok) == 0, "loc token early");
  AdvanceMs(20);
  Step(g_now100);
  CHECK(Load32(EcPage() + kLocTok) == 1 && HasLine("ec: TEST loctoken"), "loc token not added after the period");
  // 17th destination and the server pick once a second: the busiest destination becomes the server
  ResetAll();
  InstallOk();
  uint8_t b[8] = {};
  GameSend(7, b, 8, 0x0a000002, 53);
  Step(g_now100);
  for (int i = 0; i < 5; ++i) GameSend(8, b, 8, 0x0a000003, 6792);
  AdvanceMs(1001);
  Step(g_now100);
  Stats s = StatsNow();
  CHECK(s.serverAddr == 0x0a000003 && s.serverFd == 8, "the busiest destination of the last second is not the server");
}

static void HideTests() {
  // hide with the word: on a server that states no EC the list is cut (once the selection is still), and put back when one states it
  ResetAll();
  InstallOk("types=VPTO equip hide");
  GameTalks();
  Step(g_now100);
  CHECK(g_list.count == 7, "cut before the settle time: %llu", (unsigned long long)g_list.count);
  AdvanceMs(2000);
  Step(g_now100);
  CHECK(g_list.count == 4, "no hide after the selection settled: %llu", (unsigned long long)g_list.count);
  CHECK(HasLine("ec: tac mods: smoke / buff / point") && HasLine("\"effect\":\"hidden\""), "no hide record");
  // the server states EchoCombat (#2333): shown again
  ServerStates(true);
  AdvanceMs(300);
  Step(g_now100);
  CHECK(g_list.count == 7 && HasLine("\"effect\":\"shown\""), "the list was not put back when the server stated EC: %llu", (unsigned long long)g_list.count);
  // the server has EC: one of ours selected is nothing to keep (the list is whole anyway), and is not reported as kept
  *reinterpret_cast<uint64_t*>(EcPage() + kEquipSel) = 0xe32dc7c9d4804fb5ull;
  AdvanceMs(300);
  Step(g_now100);
  CHECK(g_list.count == 7 && !HasLine("an EC tac mod is selected"), "keep-whole was reported while the server states EC");
  // the next server states nothing: hidden again; but with one of ours selected the list stays whole (keepWhole)
  ServerStates(false);
  AdvanceMs(300);
  Step(g_now100);
  AdvanceMs(300);
  Step(g_now100);
  AdvanceMs(2300);
  Step(g_now100);
  CHECK(g_list.count == 7 && HasLine("an EC tac mod is selected"), "the list was cut with one of ours selected: %llu", (unsigned long long)g_list.count);
  // deselect: the hide applies again after the settle
  *reinterpret_cast<uint64_t*>(EcPage() + kEquipSel) = 0;
  AdvanceMs(300);
  Step(g_now100);
  CHECK(g_list.count == 7, "cut at once after deselecting (the settle is 2 s)");
  AdvanceMs(2300);
  Step(g_now100);
  CHECK(g_list.count == 4 && HasLine("no EC tac mod kept"), "not cut after the settle: %llu", (unsigned long long)g_list.count);
  // without the hide word: logged only, nothing written
  ResetAll();
  InstallOk("types=VT equip");
  GameTalks();
  Step(g_now100);
  AdvanceMs(2500);
  Step(g_now100);
  CHECK(g_list.count == 7 && HasLine("would be hidden (no hide word, nothing written)"), "the count was written without the hide word");
  *reinterpret_cast<uint64_t*>(EcPage() + kEquipSel) = 0xe32dc7c9d4804fb5ull;
  AdvanceMs(300);
  Step(g_now100);
  CHECK(!HasLine("an EC tac mod is selected"), "keep-whole applied without the hide word");
  // the gate: the list pointer being read only under the gate; the Reload wrapper holds it and counts a rebuild
  ResetAll();
  InstallOk();
  const uint32_t g0 = GearGenForTest();
  reinterpret_cast<void (*)()>(*W(kPins.reloadSlot))();
  CHECK(g_reload_calls == 1 && g_unload_calls == 1, "the real Reload / Unload were not called through the wrappers: %d %d", g_reload_calls, g_unload_calls);
  CHECK(GearGenForTest() == g0 + 2 && g_gen_seen_in_reload == g0 + 1, "gen must count each rebuild / free (and Reload re-entering Unload must not hang)");
  // a rebuilt list: after Reload the full count restarts (gen), so a shorter rebuilt list is not mistaken
  ResetAll();
  InstallOk();
  GameTalks();
  Step(g_now100);
  AdvanceMs(2300);
  Step(g_now100);
  CHECK(g_list.count == 4, "setup");
  BuildList(5, true);  // same entries address, a shorter list (5 with ours last)
  reinterpret_cast<void (*)()>(*W(kPins.reloadSlot))();
  AdvanceMs(300);
  Step(g_now100);
  CHECK(g_list.count == 2, "a rebuilt list at the same address kept the old full count: %llu", (unsigned long long)g_list.count);
  // the list pointer unreadable: nothing happens
  ResetAll();
  InstallOk();
  *W(kPins.tacModList) = 0x20;
  GameTalks();
  Step(g_now100);
  AdvanceMs(2500);
  Step(g_now100);
  CHECK(g_list.count == 7, "a bad list pointer was followed");
}

static void SelectionTests() {
  // the selected tac mod: the profile update line is seen through the real WriteLog slot; the call goes on to WriteLogV with the
  // same arguments and its result
  ResetAll();
  InstallOk();
  FakeWriteLogVRet = 0xabcd;
  const std::uintptr_t r = GameWriteLog(2, "[NSUSER] sent profile update: %s", "{\"ability\":\"buff\",\"a\":1}");
  CHECK(r == 0xabcd, "the WriteLogV result was not passed through");
  CHECK(g_v_calls == 1 && g_v_level == 2 && g_v_flags == 7 && std::string(g_v_text) == "[NSUSER] sent profile update: {\"ability\":\"buff\",\"a\":1}",
        "WriteLogV not called with the same arguments: '%s'", g_v_text);
  CHECK(Load64(EcPage() + kEquipSel) == 0xc8c33e48386618b6ull, "the selected tac mod was not stored");
  CHECK(HasLine("\"event\":\"combat_ec_sel\"") && HasLine("\"name\":\"buff\"") && HasLine("stored at +e80"), "no sel record");
  // another format: passed on, nothing stored, no scan
  *reinterpret_cast<uint64_t*>(EcPage() + kEquipSel) = 0;
  GameWriteLog(1, "other %s", "{\"ability\":\"smoke\"}");
  CHECK(Load64(EcPage() + kEquipSel) == 0 && g_v_calls == 2, "another format was scanned");
  // an unknown name: logged, nothing stored
  GameWriteLog(2, "[NSUSER] sent profile update: %s", "{\"ability\":\"zzz\"}");
  CHECK(Load64(EcPage() + kEquipSel) == 0 && HasLine("not a known tac mod, nothing stored"), "an unknown name stored");
  // a profile update without a tac mod: nothing
  g_lines.clear();
  GameWriteLog(2, "[NSUSER] sent profile update: %s", "{\"name\":\"x\"}");
  CHECK(!HasLine("\"event\":\"combat_ec_sel\""), "a profile update without an ability was reported");
  // noequip: logged only
  ResetAll();
  InstallOk("types=T noequip");
  GameWriteLog(2, "[NSUSER] sent profile update: %s", "{\"ability\":\"smoke\"}");
  CHECK(Load64(EcPage() + kEquipSel) == 0 && HasLine("NOT stored (noequip word)"), "noequip stored the selection");
  // the same line with the format a different length: not ours
  ResetAll();
  InstallOk();
  GameWriteLog(2, "[NSUSER] sent profile update: %s ", "{\"ability\":\"smoke\"}");
  CHECK(Load64(EcPage() + kEquipSel) == 0, "a longer format matched");
  // re-run counter cleared with the store
  *reinterpret_cast<uint32_t*>(EcPage() + kEquipReruns) = 5;
  GameWriteLog(2, "[NSUSER] sent profile update: %s", "{\"ability\":\"smoke\"}");
  CHECK(Load32(EcPage() + kEquipReruns) == 0, "re-runs not cleared");
}

// ===================== install =====================
static void InstallTests() {
  // 1. install: all six libr15 slots hold the thunks, each reported, the words applied
  ResetAll();
  const uint64_t recv0 = *W(kPins.recvfromSlot), send0 = *W(kPins.sendtoSlot), logv0 = *W(kLogLib[0].slot);
  InstallOk();
  CHECK(*W(kPins.recvfromSlot) != recv0 && *W(kPins.sendtoSlot) != send0, "recvfrom / sendto not hooked");
  CHECK(*W(kLogLib[0].slot) != logv0, "the WriteLogV slot not hooked");
  CHECK(*W(kPins.reloadSlot) != reinterpret_cast<uintptr_t>(&RealReload) && *W(kPins.unloadSlot) != reinterpret_cast<uintptr_t>(&RealUnload), "gear gate slots not hooked");
  CHECK(*W(kPins.lobbySettingsSlot) != reinterpret_cast<uintptr_t>(&RealLobbySettings) && HasLine("\"hook\":\"lobby_settings_cb\"", 1) &&
            StatsNow().settingsOn,
        "the lobby settings callback's GLOB_DAT word not hooked (#2333)");
  CHECK(HasLine("\"hook\":\"recvfrom\"", 1) && HasLine("\"hook\":\"sendto\"", 1) && HasLine("\"hook\":\"writelogv\"", 1) &&
            HasLine("\"hook\":\"reload_equipment_config\"", 1) && HasLine("\"hook\":\"unload_equipment_config\"", 1),
        "no per-hook record");
  CHECK(HasLine("\"event\":\"combat_ec_install\",\"result\":\"installed\"", 1), "no install line");
  Stats s = StatsNow();
  CHECK(s.listOn && s.hideWrite && s.equipStore && s.types == 15 && s.logLib[0], "words not applied: list %d hide %d equip %d types %u", s.listOn, s.hideWrite, s.equipStore, s.types);
  CHECK(HasLine("\"event\":\"combat_ec_page\"") && (HasLine("\"state\":\"visible\"") || HasLine("\"state\":\"invisible\"") || HasLine("\"state\":\"other\"")), "no page line");
  CHECK(Installed() && nevr_quest::combat::tick::Count() == 1, "the client registered a tick callback (the one callback is the scope row's refresh)");
  CHECK(std::string(InstallWords("").status) == "already_ran", "a second install must refuse");

  // 1b. #2333: the lobby settings callback's word holds another function: partial and loud, that word untouched, the rest of the client
  // installed, and no server's statement is read (the hide's gate stays closed)
  ResetAll(128);
  ModuleResult rs = InstallWords("types=VPTO equip hide");
  CHECK(rs.installed && std::string(rs.status) == "partial" && !StatsNow().settingsOn, "a refused lobby settings word: %s", rs.status);
  CHECK(HasLine("\"hook\":\"lobby_settings_cb\"", 3) && HasLine("\"event\":\"combat_ec_install\",\"result\":\"partial\""),
        "a refused lobby settings word not loud");
  CHECK(*W(kPins.lobbySettingsSlot) == reinterpret_cast<uintptr_t>(&RealLobbySettings) + 4, "a refused word was written");
  CHECK(*W(kPins.recvfromSlot) != reinterpret_cast<uintptr_t>(&FRecv), "the net pair not hooked when only the lobby settings word was refused");

  // 2. recvfrom's slot holds no function: refused, loud, nothing hooked, page zero
  ResetAll(1);
  ModuleResult r = InstallWords("types=T");
  CHECK(!r.installed && std::string(r.status) == "original_implausible", "recvfrom slot with no function: %s", r.status);
  CHECK(HasLine("\"event\":\"combat_ec_install\",\"result\":\"refused\"", 3), "no Error for a refused net hook");
  CHECK(*W(kPins.sendtoSlot) == reinterpret_cast<uintptr_t>(&FSend), "sendto hooked although recvfrom was refused");
  CHECK(*W(kLogLib[0].slot) == reinterpret_cast<uintptr_t>(&FakeWriteLogV), "WriteLogV hooked although the net hook was refused");
  CHECK(!Installed(), "a refused net pair reads as installed");

  // 3. sendto refused after recvfrom was hooked: recvfrom is put back (both or none)
  ResetAll(2);
  r = InstallWords("types=T");
  CHECK(!r.installed && std::string(r.status) == "original_implausible", "sendto slot with no function: %s", r.status);
  CHECK(*W(kPins.recvfromSlot) == reinterpret_cast<uintptr_t>(&FRecv), "recvfrom not put back after the sendto refusal");

  // 4. WriteLog's body unknown: the selected-tac-mod hook refuses (loud), the net pair stays
  ResetAll(4);
  r = InstallWords("types=T equip");
  CHECK(r.installed && std::string(r.status) == "partial", "the net pair must stand when only WriteLogV is refused: %s", r.status);
  CHECK(*W(kLogLib[0].slot) == reinterpret_cast<uintptr_t>(&FakeWriteLogV) && HasLine("writelog_body_not_known", 3), "WriteLogV hooked over an unknown body / not loud");
  // the stub that calls through the slot is not the pinned one
  ResetAll(64);
  r = InstallWords("types=T equip");
  CHECK(r.installed && *W(kLogLib[0].slot) == reinterpret_cast<uintptr_t>(&FakeWriteLogV) && HasLine("writelogv_stub_not_known", 3), "the stub not checked");
  // WriteLogV's slot holding another function
  ResetAll(8);
  r = InstallWords("types=T equip");
  CHECK(r.installed && HasLine("\"status\":\"original_mismatch\"", 3) && !StatsNow().logLib[0], "WriteLogV slot with another word accepted");

  // 5. gear gate: both or none
  ResetAll(32);
  const uint64_t reload0 = *W(kPins.reloadSlot);
  r = InstallWords("types=T hide");
  CHECK(r.installed && !StatsNow().listOn, "the hide must stay off when a gate word cannot be hooked");
  CHECK(*W(kPins.reloadSlot) == reload0, "Reload's word not put back after the Unload refusal");
  CHECK(HasLine("\"gear_gate\":\"original_mismatch\""), "the install line does not name the gate's refusal");
  ResetAll(16);
  r = InstallWords("types=T hide");
  CHECK(r.installed && !StatsNow().listOn && *W(kPins.unloadSlot) == reinterpret_cast<uintptr_t>(&RealUnload), "Reload refused: Unload must stay untouched");

  // 6. the refusals before any slot: no libr15, another build, a combat level answer that cannot change
  ResetAll();
  g_image.Drop();
  CHECK(std::string(InstallWords("").status) == "module_not_loaded", "no libr15");
  ResetAll();
  g_image.Build({{kRecvfromSymbol, kPins.recvfromSlot, false, false, 0}}, "0123456789012345678901234567890123456789");
  CHECK(std::string(InstallWords("").status) == "build_id_mismatch" && !HasLine("\"event\":\"combat_ec_hook\""), "another build: refused before any hook");
  ResetAll();
  nevr_quest::combat::tick::ResetForTest();  // the tick is not installed, so the scope row registered no refresh
  nevr_quest::combat::scope::ResetForTest();
  g_lines.clear();
  const uint64_t recvBefore = *W(kPins.recvfromSlot);
  CHECK(std::string(InstallWords("").status) == "scope_not_live" && *W(kPins.recvfromSlot) == recvBefore && HasLine("\"event\":\"combat_ec_words\"") &&
            HasLine("\"event\":\"combat_ec_install\",\"result\":\"refused\"", 3),
        "a combat level answer that cannot change must refuse before any slot, after the words line");
  ResetAll();
  nevr_quest::combat::tick::ResetForTest();
  nevr_quest::combat::scope::ResetForTest();
  g_image.Drop();  // no libr15 either: the scope is asked first
  CHECK(std::string(InstallWords("").status) == "scope_not_live", "a dead scope is asked after the image");
}

// ===================== the platform libraries =====================
static void PlatformTests() {
  // loaded before the install: hooked at the install
  ResetAll();
  BuildPlatform(1);
  InstallOk();
  CHECK(StatsNow().logLib[1] && !StatsNow().logLib[2], "libpnsovr loaded at install: not hooked (or libpnsrad, not loaded, hooked)");
  CHECK(HasLine("\"event\":\"combat_ec_platform\",\"lib\":\"libpnsovr\",\"status\":\"installed\"", 1), "no platform line");
  CHECK(!HasLine("\"lib\":\"libpnsrad\""), "a library that is not loaded yet was reported");
  // the thunk works through the hooked word: the profile line of that library is seen and forwarded with its arguments
  g_v_calls = 0;
  const std::uintptr_t ret = CallWriteLog(reinterpret_cast<uint64_t*>(g_ovr.Slot(g_libs[1].slot)), 2, 0ull, "[NSUSER] sent profile update: %s",
                                          "{\"ability\":\"point\"}");
  CHECK(ret == FakeWriteLogVRet && g_v_calls == 1 && g_v_lib == 1 && Load64(EcPage() + kEquipSel) == 0xe32dc7cad6864aa4ull,
        "the platform library's WriteLogV did not forward / store (lib %d calls %d)", g_v_lib, g_v_calls);
  CHECK(HasLine("\"lib\":\"libpnsovr\"") && HasLine("\"event\":\"combat_ec_sel\""), "no sel record for the platform library");
  // loaded later: the observer hooks it when that library is reported, and not at another library's load (each of the game's ~480 script
  // library loads would look the two up)
  BuildPlatform(2);
  nevr_quest::integration::NotifyModuleLoaded("/data/app/x/lib/arm64/lib0123456789abcdef.so", &g_rad);
  CHECK(!StatsNow().logLib[2], "another library's load hooked libpnsrad");
  nevr_quest::integration::NotifyModuleLoaded("/data/app/x/lib/arm64/libpnsrad.so", &g_rad);
  CHECK(StatsNow().logLib[2] && HasLine("\"lib\":\"libpnsrad\",\"status\":\"installed\""), "the observer did not hook libpnsrad");
  // another library's load changes nothing; a second report of the same mapping hooks nothing again and logs nothing
  const std::size_t lines = g_lines.size();
  nevr_quest::integration::NotifyModuleLoaded("libfoo.so", &g_rad);
  nevr_quest::integration::NotifyModuleLoaded("libpnsrad.so", &g_rad);
  CHECK(g_lines.size() == lines, "a repeated report logged again");
  // the library mapped again at another address: hooked again
  g_rad.Drop();
  BuildPlatform(2);
  CHECK(*reinterpret_cast<uint64_t*>(g_rad.Slot(g_libs[2].slot)) == reinterpret_cast<uintptr_t>(&FakeWriteLogVRad), "setup");
  nevr_quest::integration::NotifyModuleLoaded("libpnsrad.so", &g_rad);
  CHECK(*reinterpret_cast<uint64_t*>(g_rad.Slot(g_libs[2].slot)) != reinterpret_cast<uintptr_t>(&FakeWriteLogVRad), "a library mapped again was not hooked again");
  // a platform slot bound to libr15's WriteLogV (the loader may bind either) is accepted
  ResetAll();
  InstallOk();
  BuildPlatform(1);
  g_ovr.Put<uint64_t>(g_libs[1].slot, reinterpret_cast<uintptr_t>(&FakeWriteLogV));
  nevr_quest::integration::NotifyModuleLoaded("libpnsovr.so", &g_ovr);
  CHECK(StatsNow().logLib[1], "a slot that holds libr15's WriteLogV was refused");
  // refusals: loud, the word untouched
  struct Case {
    unsigned bad;
    const char* token;
  } cases[] = {{1, "slot_not_writelogv"}, {2, "writelog_body_not_known"}};
  for (const Case& c : cases) {
    ResetAll();
    InstallOk();
    BuildPlatform(1, c.bad);
    const uint64_t before = *reinterpret_cast<uint64_t*>(g_ovr.Slot(g_libs[1].slot));
    nevr_quest::integration::NotifyModuleLoaded("libpnsovr.so", &g_ovr);
    CHECK(!StatsNow().logLib[1] && HasLine(c.token, 2) && *reinterpret_cast<uint64_t*>(g_ovr.Slot(g_libs[1].slot)) == before, "%s: not refused loud", c.token);
  }
  // the observer is in the table only while ec is installed: before the install a report does nothing
  ResetAll();
  BuildPlatform(1);
  OnModuleLoaded("libpnsovr.so", &g_ovr);
  CHECK(!StatsNow().logLib[1] && !HasLine("\"event\":\"combat_ec_platform\""), "a platform library was looked at before ec was installed");
}

// ===================== the keys =====================
static void ConfigTests() {
  // combat.ec false: nothing hooked, the page line is still written, the install line says off
  ResetAll();
  Switches off;
  off.on = false;
  const ModuleResult r = Install(off);
  CHECK(!r.installed && std::string(r.status) == "off" && *W(kPins.recvfromSlot) == reinterpret_cast<uintptr_t>(&FRecv), "combat.ec false installed something");
  CHECK(HasLine("\"event\":\"combat_ec_page\"") && HasLine("\"result\":\"off\""), "no page or off line");
  // no words: the default words; given words: those
  ResetAll();
  Install(Switches{});
  CHECK(StatsNow().types == 15 && StatsNow().equipStore && StatsNow().hideWrite && HasLine("\"source\":\"default\""), "the default words are not types=VPTO equip hide");
  ResetAll();
  InstallOk("types=V noequip");
  CHECK(StatsNow().types == 1 && !StatsNow().equipStore && !StatsNow().hideWrite && HasLine("\"source\":\"config\""), "the config's words not applied");
  // the plugin's switch files are not read: one found is a Warning naming the key, and changes nothing
  char tmpl[] = "/tmp/nevr-ec-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) std::exit(2);
  const std::string d = tmpl;
  for (const char* f : {"ec.off", "ec.on", "ec_preset1.on"}) {
    FILE* fp = std::fopen((d + "/" + f).c_str(), "w");
    if (fp != nullptr) std::fclose(fp);
  }
  nevr_quest::combat::legacy::SetDirectoryForTest(tmpl);
  ResetAll();
  InstallOk();
  CHECK(HasLine("\"event\":\"combat_ec_legacy_switch\",\"file\":\"ec.off\",\"replaced_by\":\"combat.ec: false\"", 2) &&
            HasLine("\"file\":\"ec.on\",\"replaced_by\":\"combat.ec_words\"", 2) && HasLine("\"file\":\"ec_preset1.on\"", 2),
        "the legacy files were not warned about");
  CHECK(Installed(), "ec.off still switched ec off");
  for (const char* f : {"ec.off", "ec.on", "ec_preset1.on"}) unlink((d + "/" + f).c_str());
  rmdir(tmpl);
  nevr_quest::combat::legacy::SetDirectoryForTest(nullptr);
}

// ===================== arena (the scope) =====================
// The combat level answer looks at the game (the scope row's refresh, on the game thread), then the ec thread asks it (RefreshScope).
static bool LookAndAsk() {
  nevr_quest::combat::scope::Refresh(GameAddress());
  return RefreshScope(g_now100 / 10000);
}
static void ArenaTests() {
  // An Echo Arena level: every datagram goes to the game as libc gave it, nothing is counted, no net tick, nothing is consumed
  ResetAll();
  ForceScopeForTest(-1);
  InstallOk();
  LoadLevels({"mpl_arena_a"});
  AdvanceMs(300);
  CHECK(!LookAndAsk() && !InCombatScope(), "an Echo Arena level is in scope");
  CHECK(HasLine("\"in_scope\":0,\"level_class\":\"echo_arena\""), "no scope line for arena");
  OnNetTick(&NetTick);
  g_tick_calls = 0;
  StartReadCount();  // the arena path from here: no read of game memory, no scan of the levels (the count, not only what the game gets)
  StartScanCount();
  GameTalks();
  CHECK(StatsNow().sends == 0 && !StatsNow().haveServer, "a send in arena was counted");
  uint8_t buf[64];
  sockaddr_in from{};
  socklen_t fl = sizeof from;
  g_in.push_back(Veil('T', 99));
  CHECK(GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl) == 16 && g_recv_calls == 1, "an EC datagram in arena was consumed");
  CHECK(StatsNow().tokens == 0 && StatsNow().passed == 0 && StatsNow().consumed == 0 && g_tick_calls == 0, "arena: counted, tokened or ticked");
  CHECK(ReadCount() == 0 && ScanCount() == 0, "arena: %llu reads and %llu scans on the arena path",
        static_cast<unsigned long long>(ReadCount()), static_cast<unsigned long long>(ScanCount()));
  // the arena lobbies are no combat level either
  for (const char* lobby : {"mpl_lobby_b", "mpl_lobby_b2", "mpl_lobby_b_arena", "mpl_lobby_b_social"}) {
    LoadLevels({lobby});
    AdvanceMs(300);
    CHECK(!LookAndAsk(), "%s is in scope", lobby);
  }
  // the combat lobby and a combat map are; the level is read at most every 250 ms
  LoadLevels({"mpl_lobby_b_combat"});
  AdvanceMs(300);
  CHECK(LookAndAsk() && InCombatScope(), "the combat lobby is not in scope");
  // the ec thread asks at most every 250 ms, whatever the answer's own looks: a look made 200 ms before this thread's ask is read, and a newer one
  // (an Echo Arena level) is not read until 250 ms after that ask
  AdvanceMs(300);
  nevr_quest::combat::scope::Refresh(GameAddress());  // a look: the combat lobby
  AdvanceMs(200);
  CHECK(RefreshScope(g_now100 / 10000), "the combat lobby is not in scope");
  LoadLevels({"mpl_arena_a"});
  AdvanceMs(100);
  CHECK(LookAndAsk(), "the answer was read again 100 ms after the last ask (the arena look is newer than it)");
  AdvanceMs(150);
  CHECK(!LookAndAsk(), "the answer was not read again 250 ms after the last ask");
  LoadLevels({"mpl_combat_dyson"});
  AdvanceMs(250);
  CHECK(LookAndAsk(), "a combat map is not in scope");
  GameTalks();
  CHECK(StatsNow().sends == 1 && StatsNow().haveServer, "in scope a send is not counted");
  // leaving: the page's answer word is cleared and no server is kept for the net tick
  Store32(EcPage() + kQuestEcServer, 1);
  LoadLevels({"mpl_arena_a"});
  AdvanceMs(250);
  CHECK(!LookAndAsk() && Load32(EcPage() + kQuestEcServer) == 0, "leaving the scope kept the answer word");
  // the tick stops (nothing refreshes the answer): a combat answer older than 1 s is not believed, so the client leaves the scope (this thread
  // keeps asking while the game thread is stalled), and the line names the class `stale`
  LoadLevels({"mpl_combat_dyson"});
  AdvanceMs(250);
  CHECK(LookAndAsk(), "a combat map is not in scope");
  // the step of this in-scope look sets the page's answer word (the server states EchoCombat) and the net tick runs on the game's socket (a datagram
  // that is not ours, from the server): the stale leave below must clear the word and stop the tick. The game sends first: the leave above forgot
  // the server pick (#2390), so this scope's server is the one the game sends to in it.
  GameTalks();
  ServerStates(true);
  Step(g_now100);
  CHECK(Load32(EcPage() + kQuestEcServer) == 1, "setup: the step did not set the answer word");
  g_in.push_back(Plain(40));
  int ticks = g_tick_calls;
  uint8_t gb[64];
  sockaddr_in gfrom{};
  socklen_t gfl = sizeof gfrom;
  GameRecv(7, gb, sizeof gb, 0, reinterpret_cast<sockaddr*>(&gfrom), &gfl);
  CHECK(g_tick_calls == ticks + 1, "setup: a game datagram in scope did not run the net tick");
  AdvanceMs(1001);
  CHECK(!RefreshScope(g_now100 / 10000) && HasLine("\"in_scope\":0,\"level_class\":\"stale\""), "a stale combat answer kept the scope");
  CHECK(Load32(EcPage() + kQuestEcServer) == 0, "the stale leave kept the answer word");
  g_in.push_back(Plain(40));
  ticks = g_tick_calls;
  GameRecv(7, gb, sizeof gb, 0, reinterpret_cast<sockaddr*>(&gfrom), &gfl);
  CHECK(g_tick_calls == ticks, "after the stale leave a game datagram still ran the net tick");
  // a transition: an Echo Arena level still loaded beside a combat map wins
  LoadLevels({"mpl_combat_dyson", "mpl_arena_a"});
  AdvanceMs(250);
  CHECK(!LookAndAsk(), "a transition holding an Echo Arena level is in scope");
  // no look yet: out of scope
  ResetAll();
  ForceScopeForTest(-1);
  InstallOk();
  CHECK(!InCombatScope() && HasLine("\"level_class\":\"no_look\""), "no look reads as in scope");
}

// ===================== the scope's one look per decision, and the line that names it (ec.cpp RefreshScope) =====================
// RefreshScope reads the combat level answer once per decision, so `in`, the class and the level on the `combat_ec_scope` line are of one look.
// The read seam fires at each read of the answer; the second read of a decision is the one a second reader would make, so the seam writes a new
// look there. A decision that reads twice can then name an Echo Arena look as its class or level, or decide `in` from another look than the line.
static int g_oneLookReads = 0;
static void WriteArenaOnSecondRead() {
  if (++g_oneLookReads != 2) return;
  LoadLevels({"mpl_arena_a"});
  AdvanceMs(300);
  nevr_quest::combat::scope::Refresh(GameAddress());
}
static std::string ScopeLine(bool in, const char* cls, std::uint64_t level) {
  char hex[19];
  return std::string("\"in_scope\":") + (in ? "1" : "0") + ",\"level_class\":\"" + cls + "\",\"level\":\"" + sentinel::HexString(hex, level) + "\"";
}
static void ScopeLineCase(const char* level, bool in, const char* cls, std::uint64_t expectLevel, const char* what) {
  ResetAll();
  ForceScopeForTest(-1);
  InstallOk();
  LoadLevels({level});
  AdvanceMs(300);
  const bool got = LookAndAsk();
  const std::string want = ScopeLine(in, cls, expectLevel);
  CHECK(got == in, "%s: in scope is %d", what, got ? 1 : 0);
  CHECK(HasLine(want.c_str()), "%s: the scope line is not %s", what, want.c_str());
}
static void OneLookTests() {
  // a test's forced scope names the class of the in-scope answer and no level (the look is none)
  ResetAll();
  CHECK(RefreshScope(g_now100 / 10000) && HasLine(ScopeLine(true, "combat_or_lobby", 0).c_str()), "forced in scope: the scope line is not combat_or_lobby");
  // the scope line's class and level are those of the look that decides, for each class
  ScopeLineCase("mpl_combat_dyson", true, "combat_or_lobby", EchoVR::CSymbol64Hash("mpl_combat_dyson"), "a combat map");
  ScopeLineCase("mpl_lobby_b_combat", true, "combat_or_lobby", EchoVR::CSymbol64Hash("mpl_lobby_b_combat"), "the combat lobby");
  ScopeLineCase("mpl_lobby_b", false, "other", 0, "the arena lobby mpl_lobby_b");
  // one look per decision: the answer is read once, so a look written between two reads is not named in the line
  ResetAll();
  ForceScopeForTest(-1);
  InstallOk();
  LoadLevels({"mpl_arena_a"});
  AdvanceMs(300);
  nevr_quest::combat::scope::Refresh(GameAddress());
  CHECK(!RefreshScope(g_now100 / 10000), "setup: an Echo Arena level is in scope");
  LoadLevels({"mpl_combat_dyson"});
  AdvanceMs(300);
  nevr_quest::combat::scope::Refresh(GameAddress());
  g_oneLookReads = 0;
  nevr_quest::combat::scope::SetLookReadSeamForTest(&WriteArenaOnSecondRead);
  const bool in = RefreshScope(g_now100 / 10000);
  nevr_quest::combat::scope::SetLookReadSeamForTest(nullptr);
  CHECK(g_oneLookReads == 1, "the decision read the combat level answer %d times: a second read can see a second look", g_oneLookReads);
  CHECK(in, "the decision made on the look it read is not in scope");
  CHECK(HasLine(ScopeLine(true, "combat_or_lobby", EchoVR::CSymbol64Hash("mpl_combat_dyson")).c_str()),
        "the scope line does not name the look that decided (a second read named another look)");
}

static void PageTests() {
  ResetAll();
  uint8_t* p = EcPage();
  CHECK((reinterpret_cast<uintptr_t>(p) & 0xfff) == 0, "page not 4096-aligned");
  bool zero = true;
  for (int i = 0; i < 4096; ++i) zero &= p[i] == 0;
  CHECK(zero, "page not zeroed");
  CHECK(dlsym(RTLD_DEFAULT, "re33_ec_page") == p, "the exported symbol is not the page the sentinel writes");
  bool reopened = false;
  const void* seen = nullptr;
  const sentinel::EcPageState st = sentinel::PublishEcPage(&reopened, &seen);
  CHECK(st == sentinel::EcPageState::kVisible && seen == p, "the test executable exports the page (-rdynamic): state %d", static_cast<int>(st));
  InstallOk();
  CHECK(HasLine("ec: page published", 1), "no Info for a published page");
}

int main() {
  sentinel::SetLogSink(&Capture);
  CoreTests();
  ListTests();
  InstallTests();
  WrapperTests();
  NetTickTests();
  LeaveTests();
  LeavePickTests();
  StepTests();
  HideTests();
  SelectionTests();
  PlatformTests();
  ConfigTests();
  ArenaTests();
  OneLookTests();
  PageTests();
  sentinel::SetLogSink(nullptr);
  if (g_fail == 0) std::printf("ec_test: all checks passed\n");
  return g_fail == 0 ? 0 : 1;
}
