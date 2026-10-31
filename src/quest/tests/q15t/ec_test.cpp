// Host test of the `ec` module (src/quest/plugins/q15t/src/ec.cpp, src/quest/plugins/q15t/src/ec_core.h) against a fake libr15 image (an anonymous mapping holding the GOT words,
// the WriteLog body and the tac-mod list pointer at the pinned vaddrs), a fake libc (scripted datagrams) and a fake clock. Real
// SafeRead, real SwapWord, real InstallImportHook, real wrappers called THROUGH the swapped slot words as the game would.
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ec.h"
#include "ec_core.h"
#include "nevr/plugin_interface.h"
#include "plugin_log.h"

using namespace q15t;
using namespace q15t::ecc;

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

struct Line {
  uint32_t level;
  std::string text;
};
static std::vector<Line> g_lines;
static void FakeLog(const char* id, uint32_t level, const char* msg, const NevrLogField* f, uint32_t n) {
  std::string t = std::string("plugin=") + id + " " + msg;
  for (uint32_t i = 0; i < n; ++i) t += std::string(" ") + f[i].key + "=" + f[i].value;
  g_lines.push_back({level, t});
}
static bool HasLine(const char* needle, int level = -1) {
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || (int)l.level == level)) return true;
  return false;
}

// ---- the page and the clock ----
static uint64_t g_now100 = 1000ull * 10000;  // 1000 ms
static uint64_t Clock() { return g_now100; }
static void AdvanceMs(uint64_t ms) { g_now100 += ms * 10000; }

// ---- the fake libr15 image ----
constexpr uintptr_t kImgLo = 0xf88000, kImgHi = 0x37b0000;
static uint8_t* g_img = nullptr;
static uintptr_t g_base = 0;
static uint64_t* W(uintptr_t vaddr) { return reinterpret_cast<uint64_t*>(g_img + (vaddr - kImgLo)); }

static std::uintptr_t FakeWriteLogVRet = 0x1234;
static int g_v_level = -1;
static unsigned long long g_v_flags = 0;
static char g_v_text[512];
static int g_v_calls = 0;
static std::uintptr_t FakeWriteLogV(int level, unsigned long long flags, const char* fmt, va_list ap) {
  ++g_v_calls;
  g_v_level = level;
  g_v_flags = flags;
  vsnprintf(g_v_text, sizeof g_v_text, fmt, ap);
  return FakeWriteLogVRet;
}
static int g_reload_calls = 0, g_unload_calls = 0;
static uint32_t g_gen_seen_in_reload = 0;
static void RealReload() {
  ++g_reload_calls;
  g_gen_seen_in_reload = EcGearGenForTest();
  // re-entry through the gate on the same thread (a Reload that reaches Unload again) must not hang
  if (g_reload_calls == 1) reinterpret_cast<void (*)()>(*W(kEcSlotUnloadEquip))();
}
static void RealUnload() { ++g_unload_calls; }

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

static void DropImage() {
  if (g_img != nullptr) munmap(g_img, kImgHi - kImgLo);
  g_img = nullptr;
}
// bad: which slot value to corrupt (a bitmask of 1 recv, 2 send, 4 writelog body, 8 writelogv slot, 16 reload, 32 unload, 64 writelog slot)
static void BuildImage(unsigned bad = 0) {
  DropImage();
  void* m = mmap(nullptr, kImgHi - kImgLo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (m == MAP_FAILED) {
    std::printf("mmap failed\n");
    std::exit(2);
  }
  g_img = static_cast<uint8_t*>(m);
  g_base = reinterpret_cast<uintptr_t>(g_img) - kImgLo;
  *W(kEcSlotRecvfrom) = reinterpret_cast<uintptr_t>(dlsym(RTLD_DEFAULT, "recvfrom")) + ((bad & 1) ? 8 : 0);
  *W(kEcSlotSendto) = reinterpret_cast<uintptr_t>(dlsym(RTLD_DEFAULT, "sendto")) + ((bad & 2) ? 8 : 0);
  *W(kEcSlotWriteLog) = g_base + kEcFnWriteLog + ((bad & 64) ? 4 : 0);
  *W(kEcSlotWriteLogV) = g_base + kEcFnWriteLogV + ((bad & 8) ? 4 : 0);
  *W(kEcSlotReloadEquip) = g_base + kEcFnReloadEquip + ((bad & 16) ? 4 : 0);
  *W(kEcSlotUnloadEquip) = g_base + kEcFnUnloadEquip + ((bad & 32) ? 4 : 0);
  uint32_t* body = reinterpret_cast<uint32_t*>(W(kEcFnWriteLog));
  for (int i = 0; i < 25; ++i) body[i] = kWriteLogBody[i];
  body[21] = 0x97fd7a47;  // the real bl word (disassembly 1088c74: 477afd97 little-endian): any target
  if (bad & 4) body[7] ^= 1;
  BuildList(7, true);
  *W(kEcTacModList) = reinterpret_cast<uintptr_t>(&g_list);
}

static void ResetAll(unsigned bad = 0, const char* words = "types=VPTO equip hide") {
  g_lines.clear();
  g_sent.clear();
  g_in.clear();
  g_in_next = 0;
  g_recv_calls = 0;
  g_reload_calls = g_unload_calls = 0;
  g_v_calls = 0;
  g_now100 = 1000ull * 10000;
  EcResetForTest();
  SetEcNoThreadForTest(true);
  SetEcClockForTest(&Clock);
  SetEcDirForTest("");
  BuildImage(bad);
  (void)words;
}
static void InstallOk(const char* words = "types=VPTO equip hide") {
  const InstallResult r = InstallEcWithWords(g_base, words);
  CHECK(r.status == SwapStatus::kOk, "install failed: %s %s", StatusName(r.status), r.reason);
  SetEcLibcForTest(&FRecv, &FSend);
  SetEcLogVForTest(0, reinterpret_cast<uintptr_t>(&FakeWriteLogV));
  SetEcGearRealForTest(0, &RealReload);
  SetEcGearRealForTest(1, &RealUnload);
}

static ssize_t GameRecv(int fd, void* buf, size_t len, int flags, sockaddr* from, socklen_t* fl) {
  auto fn = reinterpret_cast<EcRecvfromFn>(*W(kEcSlotRecvfrom));
  return fn(fd, buf, len, flags, from, fl);
}
static ssize_t GameSend(int fd, const void* buf, size_t len, uint32_t addr, uint16_t port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = addr;
  a.sin_port = port;
  auto fn = reinterpret_cast<EcSendtoFn>(*W(kEcSlotSendto));
  return fn(fd, buf, len, 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
}
typedef std::uintptr_t (*WriteLogFn)(int, unsigned long long, const char*, ...);
static std::uintptr_t GameWriteLog(int level, const char* fmt, const char* arg) {
  return reinterpret_cast<WriteLogFn>(*W(kEcSlotWriteLog))(level, 7ull, fmt, arg);
}

static uint32_t Server() { return 0x0a000001; }
static uint16_t SPort() { return 6792; }
// the game talks to the server: a send picks it
static void GameTalks(int fd = 7) {
  uint8_t b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  GameSend(fd, b, sizeof b, Server(), SPort());
}

static int g_tick_calls = 0;
static void NetTick() { ++g_tick_calls; }
static std::vector<uint32_t> g_kills;
static void OnKill(uint32_t i) { g_kills.push_back(i); }

// ===================== pure rules (ec_core.h) =====================
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
  for (char t : {'H', 'A', 'V', 'P', 'T', 'O', 'K', 'M'}) {
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
  // 'A' only from the current server
  Packet a = {kMagic, 'A', {0, 0, 0}, 0, kMagic2};
  st.Handle(page, a, 0x0a000001, 6792, 10, 1000);
  CHECK(!st.Answered(1000) && st.answers == 0, "an A from a non-server counted");
  st.Handle(page, a, 0x0a000002, 53, 10, 1000);
  CHECK(st.Answered(1000) && st.answers == 1, "an A from the server not counted");
  CHECK(st.Answered(1000 + kServerTimeoutMs - 1) && !st.Answered(1000 + kServerTimeoutMs), "answer timeout is exactly 6000 ms");
  st.best = 0;  // moved away from the server that answered
  CHECK(!st.Answered(1001), "an answer of a server we moved away from counts");
  {  // same port, another address: the answer was the old server's
    const Dest keep0 = st.dests[0];
    st.dests[0].addr = 0x0a000009;
    st.dests[0].port = 53;
    st.best = 0;
    CHECK(!st.Answered(1001), "an answer from another address counts for the new server");
    st.dests[0] = keep0;
  }
  st.best = 1;
  // dedupe and token types
  Packet t = {kMagic, 'T', {0, 0, 0}, 77, kMagic2};
  CHECK(st.Handle(page, t, 1, 2, 5, 5) == 0 && st.tokens == 1 && Load32(page + kLocTok) == 1, "T token");
  st.Handle(page, t, 1, 2, 6, 6);
  CHECK(st.tokens == 1, "a repeat id must not add a token");
  Packet k = {kMagic, 'K', {0, 0, 0}, 0x00030009, kMagic2};
  CHECK(st.Handle(page, k, 1, 2, 7, 7) == 10 && st.kills == 1, "K returns index + 1 (low 16 bits)");
  CHECK(st.Handle(page, k, 1, 2, 8, 8) == 0 && st.kills == 1, "K resend");
  Packet m = {kMagic, 'M', {0, 0, 0}, 1, kMagic2};
  st.Handle(page, m, 1, 2, 9, 9);
  st.Handle(page, m, 1, 2, 9, 9);
  CHECK(st.medi == 2, "every M counts, resends too");
  State st1;  // step 1: T only
  st1.types = 4;
  Packet v = {kMagic, 'V', {0, 0, 0}, 500, kMagic2};
  st1.Handle(page, v, 1, 2, 9, 9);
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

  // the WriteLog body words: the disassembly 0x1088c20..0x1088c80, little-endian bytes as read (docs/quest/q15t/modules/ec.md section 3)
  static const char* const rv[25] = {"ff0304d1", "e8048092", "e9030091", "ea230291", "08f0dff2", "eb030491", "29010291", "4aa10091", "e8fb0ea9",
                                      "eaa70da9", "eb6b00f9", "e00700ad", "e18346ad", "e39308a9", "e3c30291", "e59b09a9", "e75700f9", "e20f01ad",
                                      "e41702ad", "e61f03ad", "e18305ad", "477afd97", "fe7b40f9", "ff030491", "c0035fd6"};
  uint32_t w[25];
  for (int i = 0; i < 25; ++i) {
    uint32_t v32 = 0;
    for (int b = 0; b < 4; ++b) v32 |= static_cast<uint32_t>(std::strtoul(std::string(rv[i]).substr(b * 2, 2).c_str(), nullptr, 16)) << (8 * b);
    w[i] = v32;
  }
  CHECK(IsWriteLogWords(w), "the words the disassembly shows are not the body this port checks");
  for (int i = 0; i < 25; ++i) {
    uint32_t keep = w[i];
    w[i] ^= 0x100;
    if (i == 21) w[i] = 0x14000001;  // a `b`, not a `bl`
    CHECK(!IsWriteLogWords(w), "word %d may change", i);
    w[i] = keep;
  }
  w[21] = 0x94000123;
  CHECK(IsWriteLogWords(w), "any bl target");
  CHECK(!IsWriteLogBody(0x10), "unreadable address");

  // the dynamic-table lookup: a synthetic ELF image (base + offsets), R_AARCH64_JUMP_SLOT = 1026
  {
    alignas(16) static uint8_t img[0x2000];
    std::memset(img, 0, sizeof img);
    const uintptr_t base = reinterpret_cast<uintptr_t>(img);
    // strtab at 0x1000, symtab at 0x800, rela at 0x400, dyn at 0x100
    const char names[] = "\0other\0_ZN10NRadEngine9NWriteLog8WriteLogENS0_9ELogLevelEyPKcz\0third";
    std::memcpy(img + 0x1000, names, sizeof names);
    ElfSym* sy = reinterpret_cast<ElfSym*>(img + 0x800);
    sy[1].name = 1;  // other
    sy[2].name = 7;  // the WriteLog name
    sy[3].name = 7 + 57;
    ElfRela* re = reinterpret_cast<ElfRela*>(img + 0x400);
    re[0] = {0x1800, (1ull << 32) | 1026, 0};
    re[1] = {0x1808, (2ull << 32) | 257, 0};  // an ABS64, same symbol: not a jump slot
    re[2] = {0x1810, (2ull << 32) | 1026, 0};
    ElfDyn* dy = reinterpret_cast<ElfDyn*>(img + 0x100);
    int i = 0;
    dy[i++] = {23, 0x400};
    dy[i++] = {2, 3 * sizeof(ElfRela)};
    dy[i++] = {20, 7};
    dy[i++] = {6, 0x800};
    dy[i++] = {5, 0x1000};
    dy[i++] = {0, 0};
    const uintptr_t dynp = reinterpret_cast<uintptr_t>(img + 0x100);
    const char* wl = "_ZN10NRadEngine9NWriteLog8WriteLogENS0_9ELogLevelEyPKcz";
    CHECK(JumpSlot(base, dynp, wl) == base + 0x1810, "jump slot found by name (skips the non-JUMP_SLOT reloc)");
    CHECK(JumpSlot(base, dynp, "other") == base + 0x1800, "first slot by name");
    CHECK(JumpSlot(base, dynp, "missing") == 0, "no such name");
    dy[2].val = 8;  // DT_PLTREL = REL not RELA
    CHECK(JumpSlot(base, dynp, "other") == 0, "REL tables refused");
    dy[2].val = 7;
    CHECK(JumpSlot(base, 0, "other") == 0, "null dyn");
    // a name that ends at an unreadable page edge is still read
    void* two = mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(static_cast<char*>(two) + 4096, 4096);
    char* end = static_cast<char*>(two) + 4096 - 6;
    std::memcpy(end, "abcde", 6);
    char got[32];
    CHECK(ReadCStr(reinterpret_cast<uintptr_t>(end), got, sizeof got) && !std::strcmp(got, "abcde"), "string at a page edge");
    munmap(two, 4096);
    CHECK(!ReadCStr(reinterpret_cast<uintptr_t>(end), got, sizeof got), "unmapped string");
  }

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

// ===================== the tac-mod list =====================
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
  // the count that moved between the read and the write is left alone: simulate with a CAS expectation mismatch
  // (TacModsFor reads then compare-exchanges; here the count is changed by a second look inside the same call: not reproducible
  // single-threaded, so the compare-exchange is covered by the mutant that replaces it with a plain store)
  // HideSettled
  CHECK(!HideSettled(1999, 0) && HideSettled(2000, 0), "settle is exactly 2000 ms");
}

// ===================== install, wrappers, step =====================
static void InstallTests() {
  // 1. install: all slots swapped, each reported
  ResetAll();
  const uintptr_t recv0 = *W(kEcSlotRecvfrom), send0 = *W(kEcSlotSendto);
  InstallOk();
  CHECK(*W(kEcSlotRecvfrom) != recv0 && *W(kEcSlotSendto) != send0, "recvfrom / sendto not swapped");
  CHECK(*W(kEcSlotWriteLog) == EcWriteLogWrapForTest(0), "WriteLog slot not the wrapper");
  CHECK(*W(kEcSlotReloadEquip) == EcGearWrapForTest(0) && *W(kEcSlotUnloadEquip) == EcGearWrapForTest(1), "gear gate slots not swapped");
  CHECK(HasLine("hook=recvfrom status=ok") && HasLine("hook=sendto status=ok"), "no per-hook record");
  CHECK(HasLine("ec: on: recvfrom / sendto swapped", 1), "no install line");
  EcStats s = EcStatsNow();
  CHECK(s.list_on && s.hide_write && s.equip_store && s.types == 15, "words not applied: list %d hide %d equip %d types %u", s.list_on, s.hide_write, s.equip_store, s.types);
  CHECK(HasLine("ec: page") && (HasLine("state=visible") || HasLine("state=invisible") || HasLine("state=other")), "no page line");
  CHECK(InstallEcWithWords(g_base, "").reason == std::string("already_installed"), "a second install must refuse");

  // 2. a slot with an unexpected original: refused, loud, nothing swapped, no thread, page zero
  ResetAll(1);
  InstallResult r = InstallEcWithWords(g_base, "types=T");
  CHECK(r.status == SwapStatus::kUnexpectedOriginal, "recvfrom slot with another word: %s", StatusName(r.status));
  CHECK(HasLine("ec: refused: no net hook", 3), "no ERROR for a refused net hook");
  CHECK(*W(kEcSlotSendto) == reinterpret_cast<uintptr_t>(dlsym(RTLD_DEFAULT, "sendto")), "sendto swapped although recvfrom was refused");
  CHECK(*W(kEcSlotWriteLog) == g_base + kEcFnWriteLog, "WriteLog swapped although the net hook was refused");

  // 3. sendto refused after recvfrom was swapped: recvfrom is put back (both or none)
  ResetAll(2);
  const uintptr_t recv1 = *W(kEcSlotRecvfrom);
  r = InstallEcWithWords(g_base, "types=T");
  CHECK(r.status == SwapStatus::kUnexpectedOriginal, "sendto slot with another word: %s", StatusName(r.status));
  CHECK(*W(kEcSlotRecvfrom) == recv1, "recvfrom not put back after the sendto refusal");

  // 4. WriteLog body unknown: the selected-tac-mod arm refuses (loud), the net pair stays
  ResetAll(4);
  r = InstallEcWithWords(g_base, "types=T equip");
  CHECK(r.status == SwapStatus::kOk, "net pair must stand when only WriteLog is refused");
  CHECK(*W(kEcSlotWriteLog) == g_base + kEcFnWriteLog && HasLine("writelog_body_not_known", 3), "WriteLog swapped over an unknown body / not loud");
  // WriteLogV slot not WriteLogV
  ResetAll(8);
  r = InstallEcWithWords(g_base, "types=T equip");
  CHECK(r.status == SwapStatus::kOk && *W(kEcSlotWriteLog) == g_base + kEcFnWriteLog && HasLine("writelogv_slot_not_writelogv", 3), "WriteLogV owner not checked");
  // WriteLog slot holding another function
  ResetAll(64);
  r = InstallEcWithWords(g_base, "types=T equip");
  CHECK(r.status == SwapStatus::kOk && HasLine("status=unexpected_original", 3), "WriteLog slot with another word accepted");

  // 5. gear gate: both or none
  ResetAll(32);
  const uintptr_t reload0 = *W(kEcSlotReloadEquip);
  r = InstallEcWithWords(g_base, "types=T hide");
  CHECK(r.status == SwapStatus::kOk && !EcStatsNow().list_on, "the hide must stay off when a gate word cannot be swapped");
  CHECK(*W(kEcSlotReloadEquip) == reload0, "Reload word not put back after the Unload refusal");
  CHECK(HasLine("gear gate not installed", 2), "no WARNING for the missing gate");
  ResetAll(16);
  r = InstallEcWithWords(g_base, "types=T hide");
  CHECK(r.status == SwapStatus::kOk && !EcStatsNow().list_on && *W(kEcSlotUnloadEquip) == g_base + kEcFnUnloadEquip, "Reload refused: Unload must stay untouched");
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
  EcStats s = EcStatsNow();
  CHECK(s.sends == 1 && s.have_server && s.server_addr == Server() && s.server_port == SPort() && s.server_fd == 7, "the destination was not learned");
  // a non-IPv4 destination is not counted (and still sent)
  sockaddr_in6 a6{};
  a6.sin6_family = AF_INET6;
  auto sendfn = reinterpret_cast<EcSendtoFn>(*W(kEcSlotSendto));
  for (int i = 0; i < 5; ++i) sendfn(9, b, 8, 0, reinterpret_cast<sockaddr*>(&a6), sizeof a6);
  sendfn(9, b, 8, 0, nullptr, 0);
  s = EcStatsNow();
  CHECK(s.sends == 7 && g_sent.size() == 7, "v6 / null destination not passed");
  // a short sockaddr is not read
  sockaddr_in sh{};
  sh.sin_family = AF_INET;
  sh.sin_addr.s_addr = 0x01010101;
  sh.sin_port = 77;
  for (int i = 0; i < 6; ++i) sendfn(10, b, 8, 0, reinterpret_cast<sockaddr*>(&sh), sizeof sh - 4);
  EcStep(g_now100);
  AdvanceMs(1001);
  EcStep(g_now100);
  s = EcStatsNow();
  CHECK(s.server_addr == Server() && s.server_fd == 7, "a v6 or truncated destination became the server (addr %x fd %d)", s.server_addr, s.server_fd);

  // recvfrom: a foreign datagram passes untouched
  ResetAll();
  InstallOk();
  g_in.push_back(Plain(40));
  uint8_t buf[256];
  sockaddr_in from{};
  socklen_t fl = sizeof from;
  CHECK(GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl) == 40 && g_recv_calls == 1, "a foreign datagram not passed");
  CHECK(EcStatsNow().passed == 1 && EcStatsNow().consumed == 0, "passed counter");
  // a 16-byte datagram that is not an EC network packet passes (wrong magic)
  g_in.clear();
  g_in_next = 0;
  g_in.push_back(Plain(16));
  fl = sizeof from;
  CHECK(GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl) == 16, "a 16-byte foreign datagram swallowed");
  // ours: a first-time T is consumed; the real recvfrom is called again and the NEXT datagram is returned
  g_in.clear();
  g_in_next = 0;
  g_recv_calls = 0;
  g_in.push_back(Veil('T', 11));
  g_in.push_back(Plain(30));
  fl = sizeof from;
  CHECK(GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl) == 30 && g_recv_calls == 2, "ours not consumed / next datagram not returned: calls %d", g_recv_calls);
  CHECK(fl == sizeof from, "fromlen must be the real one of the returned datagram");
  s = EcStatsNow();
  CHECK(s.consumed == 1 && s.tokens == 1 && Load32(EcPage() + kLocTok) == 1, "T token not added");
  CHECK(HasLine("ec: token received") && HasLine("type=T"), "no token record");
  // the same id again: consumed, no second token; EAGAIN passes through when nothing else is queued
  g_in.clear();
  g_in_next = 0;
  g_in.push_back(Veil('T', 11));
  errno = 0;
  fl = sizeof from;
  CHECK(GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl) == -1 && errno == EAGAIN, "EAGAIN not passed after consuming ours");
  CHECK(EcStatsNow().tokens == 1, "a repeated id added a token");
  // MSG_PEEK: never consumed, passed as is
  g_in.clear();
  g_in_next = 0;
  g_in.push_back(Veil('T', 12));
  g_recv_calls = 0;
  fl = sizeof from;
  CHECK(GameRecv(7, buf, sizeof buf, MSG_PEEK, reinterpret_cast<sockaddr*>(&from), &fl) == 16 && g_recv_calls == 1 && EcStatsNow().tokens == 1,
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
  g_in.clear();
  g_in_next = 0;
  // from != null and fromlen == null: passes untouched (the game asked for the sender but gave no length)
  g_in.clear();
  g_in_next = 0;
  g_in.push_back(Veil('T', 13));
  CHECK(GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), nullptr) == 16 && EcStatsNow().tokens == 1, "from without fromlen was consumed");
  // from == null: ours are still recognised (own storage)
  g_in.clear();
  g_in_next = 0;
  g_in.push_back(Veil('T', 14));
  g_in.push_back(Plain(20));
  CHECK(GameRecv(7, buf, sizeof buf, 0, nullptr, nullptr) == 20 && EcStatsNow().tokens == 2, "from == null: ours not recognised");
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
  CHECK(GameRecv(7, buf, 8, MSG_TRUNC, reinterpret_cast<sockaddr*>(&from), &fl) == 16 && EcStatsNow().tokens == 2, "a MSG_TRUNC read into a short buffer was consumed");
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
  const uint64_t tokens_before_short = EcStatsNow().tokens;
  CHECK(GameRecv(7, buf, 8, 0, reinterpret_cast<sockaddr*>(&from), &fl) == 8 && EcStatsNow().tokens == tokens_before_short, "a truncated read counted as ours");
  // an error passes through and is not counted as passed
  g_in.clear();
  g_in_next = 0;
  const uint64_t passed0 = EcStatsNow().passed;
  fl = sizeof from;
  CHECK(GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl) == -1 && EcStatsNow().passed == passed0, "an error counted as a datagram");
  // types this build does not carry: dropped
  ResetAll();
  InstallOk("types=T");
  g_in.push_back(Veil('V', 21));
  g_in.push_back(Plain(10));
  fl = sizeof from;
  GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
  CHECK(EcStatsNow().dropped == 1 && EcStatsNow().tokens == 0 && Load32(EcPage() + kVeilTok) == 0, "an uncarried V was kept");
  // K: handed to the handler once (first time), not returned, and 'M' counted
  ResetAll();
  InstallOk();
  g_kills.clear();
  EcOnDroneKilled(&OnKill);
  g_in.push_back(Veil('K', 0x00010007));
  g_in.push_back(Veil('K', 0x00010007));
  g_in.push_back(Veil('M', 3));
  g_in.push_back(Plain(10));
  fl = sizeof from;
  CHECK(GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl) == 10, "K / M datagrams reached the game");
  CHECK(g_kills.size() == 1 && g_kills[0] == 7 && EcStatsNow().kills == 1 && EcStatsNow().medi == 1, "K not handed over once with its index");
  CHECK(HasLine("ec: K drone killed on the server") && HasLine("handler=yes"), "no K record");
  // a K without a handler is dropped and logged
  ResetAll();
  InstallOk();
  g_in.push_back(Veil('K', 0x00010008));
  g_in.push_back(Plain(10));
  fl = sizeof from;
  GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
  CHECK(HasLine("handler=none (dropped)"), "a K without a handler is not reported");
  // 'A' only from the server: sets the page word at the step
  ResetAll();
  InstallOk();
  GameTalks();
  EcStep(g_now100);
  g_in.push_back(Veil('A', 0, 0x0a000009, 5));
  g_in.push_back(Plain(10));
  fl = sizeof from;
  GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
  EcStep(g_now100);
  CHECK(!EcStatsNow().answered && Load32(EcPage() + kQuestEcServer) == 0, "an A from a stranger counted");
  g_in.clear();
  g_in_next = 0;
  g_in.push_back(Veil('A', 0, Server(), SPort()));
  g_in.push_back(Plain(10));
  fl = sizeof from;
  GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
  AdvanceMs(5);
  EcStep(g_now100);
  CHECK(EcStatsNow().answered && Load32(EcPage() + kQuestEcServer) == 1, "an A from the server not shown on the page");
  CHECK(HasLine("ec: answer seen"), "no answer record");
  AdvanceMs(6100);
  EcStep(g_now100);
  CHECK(!EcStatsNow().answered && Load32(EcPage() + kQuestEcServer) == 0 && HasLine("ec: answer lost"), "the answer did not time out on the page");
}

static void NetTickTests() {
  // the net tick: only on the game's own socket, on the thread that got a server datagram on it
  ResetAll();
  InstallOk();
  g_tick_calls = 0;
  CHECK(EcOnNetTick(&NetTick), "no tick slot");
  uint8_t buf[64];
  sockaddr_in from{};
  socklen_t fl = sizeof from;
  g_in.push_back(Plain(20, Server(), SPort()));
  GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
  CHECK(g_tick_calls == 0, "a tick before any server was picked");
  GameTalks(7);
  EcStep(g_now100);
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
  CHECK(EcOnNetTick(&NetTick) && EcOnNetTick(&NetTick) && EcOnNetTick(&NetTick), "four slots");
  CHECK(!EcOnNetTick(&NetTick), "a fifth tick must be refused");
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
  EcStats s = EcStatsNow();
  CHECK(s.have_server, "server");
}

static void StepTests() {
  // use counters -> packets twice to the server, hello every 2 s, server line once
  ResetAll();
  InstallOk();
  EcStep(g_now100);  // no server yet
  uint8_t* page = EcPage();
  __atomic_store_n(reinterpret_cast<uint32_t*>(page + kLocUse), 1u, __ATOMIC_SEQ_CST);
  AdvanceMs(5);
  EcStep(g_now100);
  CHECK(g_sent.empty(), "a use sent with no server");
  CHECK(HasLine("sent=no (no server seen yet)"), "no record for a use without a server");
  GameTalks(7);
  g_sent.clear();
  __atomic_store_n(reinterpret_cast<uint32_t*>(page + kLocUse), 2u, __ATOMIC_SEQ_CST);
  AdvanceMs(5);
  EcStep(g_now100);
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
  EcStep(g_now100);
  CHECK(g_sent.size() == 1 && g_sent[0].bytes[4] == 'H' && HasLine("ec: server (the game's own connection)") && HasLine("addr=1.0.0.10"), "hello + server line");
  g_sent.clear();
  for (int i = 0; i < 19; ++i) {
    AdvanceMs(100);
    EcStep(g_now100);
  }
  CHECK(g_sent.empty(), "hello sent more than every 2 s: %zu in 1.9 s", g_sent.size());
  AdvanceMs(100);
  EcStep(g_now100);
  CHECK(g_sent.size() == 1, "no hello after 2 s: %zu", g_sent.size());
  // a V use gives a local token and sends twice
  g_sent.clear();
  __atomic_store_n(reinterpret_cast<uint32_t*>(page + kVeilUse), 1u, __ATOMIC_SEQ_CST);
  AdvanceMs(5);
  EcStep(g_now100);
  CHECK(Load32(page + kVeilTok) == 1, "a V use must make a local token");
  // the summary and the cap
  for (int i = 0; i < 3; ++i) {
    AdvanceMs(10000);
    EcStep(g_now100);
  }
  CHECK(HasLine("ec: sum"), "no summary");
  // wall decided / shown / 221a steps
  __atomic_store_n(reinterpret_cast<uint32_t*>(page + kVeilTok + 0x10), 2u, __ATOMIC_SEQ_CST);
  __atomic_store_n(reinterpret_cast<uint32_t*>(page + kLocTok + 0x10), 3u, __ATOMIC_SEQ_CST);
  __atomic_store_n(reinterpret_cast<uint32_t*>(page + kDroneBlock + kDroneSteps), 4u, __ATOMIC_SEQ_CST);
  AdvanceMs(5);
  EcStep(g_now100);
  CHECK(HasLine("ec: wall decided") && HasLine("ec: a translocator puck took a T token") && HasLine("ec: 221a prelude steps"), "page counters not reported");
  __atomic_store_n(reinterpret_cast<uint32_t*>(page + kDroneBlock + kDroneSteps), 5u, __ATOMIC_SEQ_CST);
  g_lines.clear();
  AdvanceMs(5);
  EcStep(g_now100);
  CHECK(!HasLine("221a prelude steps"), "a non power of two step count was reported");
  __atomic_store_n(reinterpret_cast<uint32_t*>(page + kEquipSkips), 1u, __ATOMIC_SEQ_CST);
  AdvanceMs(5);
  EcStep(g_now100);
  CHECK(HasLine("ec: a tac mod that is not the selected one was used"), "equip skip not reported");

  // loctoken TEST word: the first pass arms, then one LOC token every <s> seconds
  ResetAll();
  InstallOk("types=T loctoken=2");
  EcStep(g_now100);
  CHECK(Load32(EcPage() + kLocTok) == 0, "a loc token on the arming pass");
  AdvanceMs(1990);
  EcStep(g_now100);
  CHECK(Load32(EcPage() + kLocTok) == 0, "loc token early");
  AdvanceMs(20);
  EcStep(g_now100);
  CHECK(Load32(EcPage() + kLocTok) == 1 && HasLine("ec: TEST loctoken"), "loc token not added after the period");
  // 17th destination and the server pick once a second: the busiest destination becomes the server
  ResetAll();
  InstallOk();
  uint8_t b[8] = {};
  GameSend(7, b, 8, 0x0a000002, 53);
  EcStep(g_now100);
  for (int i = 0; i < 5; ++i) GameSend(8, b, 8, 0x0a000003, 6792);
  AdvanceMs(1001);
  EcStep(g_now100);
  EcStats s = EcStatsNow();
  CHECK(s.server_addr == 0x0a000003 && s.server_fd == 8, "the busiest destination of the last second is not the server");
}

static void HideTests() {
  // hide with the word: after the answer is lost the list is cut (once the selection is still), with an answer put back
  ResetAll();
  InstallOk("types=VPTO equip hide");
  GameTalks();
  EcStep(g_now100);
  CHECK(g_list.count == 7, "cut before the settle time: %llu", (unsigned long long)g_list.count);
  AdvanceMs(2000);
  EcStep(g_now100);
  CHECK(g_list.count == 4, "no hide after the selection settled: %llu", (unsigned long long)g_list.count);
  CHECK(HasLine("ec: tac mods: smoke / buff / point") && HasLine("effect=hidden"), "no hide record");
  // an answer: shown again
  uint8_t buf[64];
  sockaddr_in from{};
  socklen_t fl = sizeof from;
  g_in.push_back(Veil('A', 0, Server(), SPort()));
  g_in.push_back(Plain(10));
  GameRecv(7, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
  AdvanceMs(300);
  EcStep(g_now100);
  CHECK(g_list.count == 7 && HasLine("effect=shown"), "the list was not put back on an answer: %llu", (unsigned long long)g_list.count);
  // the server has EC: one of ours selected is nothing to keep (the list is whole anyway), and is not reported as kept
  *reinterpret_cast<uint64_t*>(EcPage() + kEquipSel) = 0xe32dc7c9d4804fb5ull;
  AdvanceMs(300);
  EcStep(g_now100);
  CHECK(g_list.count == 7 && !HasLine("an EC tac mod is selected"), "keep-whole was reported while the server answers");
  // the answer is lost: hidden again; but with one of ours selected the list stays whole (keepWhole)
  AdvanceMs(7000);
  EcStep(g_now100);
  AdvanceMs(300);
  EcStep(g_now100);
  AdvanceMs(2300);
  EcStep(g_now100);
  CHECK(g_list.count == 7 && HasLine("an EC tac mod is selected"), "the list was cut with one of ours selected: %llu", (unsigned long long)g_list.count);
  // deselect: the hide applies again after the settle
  *reinterpret_cast<uint64_t*>(EcPage() + kEquipSel) = 0;
  AdvanceMs(300);
  EcStep(g_now100);
  CHECK(g_list.count == 7, "cut at once after deselecting (the settle is 2 s)");
  AdvanceMs(2300);
  EcStep(g_now100);
  CHECK(g_list.count == 4 && HasLine("no EC tac mod kept"), "not cut after the settle: %llu", (unsigned long long)g_list.count);
  // without the hide word: logged only, nothing written
  ResetAll();
  InstallOk("types=VT equip");
  GameTalks();
  EcStep(g_now100);
  AdvanceMs(2500);
  EcStep(g_now100);
  CHECK(g_list.count == 7 && HasLine("would be hidden (no hide word, nothing written)"), "the count was written without the hide word");
  *reinterpret_cast<uint64_t*>(EcPage() + kEquipSel) = 0xe32dc7c9d4804fb5ull;
  AdvanceMs(300);
  EcStep(g_now100);
  CHECK(!HasLine("an EC tac mod is selected"), "keep-whole applied without the hide word");
  // the gate: the list pointer being read only under the gate; the Reload wrapper holds it and counts a rebuild
  ResetAll();
  InstallOk();
  const uint32_t g0 = EcGearGenForTest();
  reinterpret_cast<void (*)()>(*W(kEcSlotReloadEquip))();
  CHECK(g_reload_calls == 1 && g_unload_calls == 1, "the real Reload / Unload were not called through the wrappers: %d %d", g_reload_calls, g_unload_calls);
  CHECK(EcGearGenForTest() == g0 + 2 && g_gen_seen_in_reload == g0 + 1, "gen must count each rebuild / free (and Reload re-entering Unload must not hang)");
  // a rebuilt list: after Reload the full count restarts (gen), so a shorter rebuilt list is not mistaken
  ResetAll();
  InstallOk();
  GameTalks();
  EcStep(g_now100);
  AdvanceMs(2300);
  EcStep(g_now100);
  CHECK(g_list.count == 4, "setup");
  BuildList(5, true);  // same entries address, a shorter list (5 with ours last)
  reinterpret_cast<void (*)()>(*W(kEcSlotReloadEquip))();
  AdvanceMs(300);
  EcStep(g_now100);
  CHECK(g_list.count == 2, "a rebuilt list at the same address kept the old full count: %llu", (unsigned long long)g_list.count);
  // the list pointer unreadable: nothing happens
  ResetAll();
  InstallOk();
  *W(kEcTacModList) = 0x20;
  GameTalks();
  EcStep(g_now100);
  AdvanceMs(2500);
  EcStep(g_now100);
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
  CHECK(HasLine("ec: sel") && HasLine("name=buff") && HasLine("result=stored at +e80"), "no sel record");
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
  CHECK(!HasLine("ec: sel"), "a profile update without an ability was reported");
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

// ---- the platform libs by name (fakes for the lib lookup and dladdr; a synthetic dynamic table) ----
alignas(16) static uint8_t g_lib_img[0x2000];
alignas(16) static uint32_t g_fake_wl_body[25];
static uintptr_t g_fake_vfn = 0;
static bool g_lib_loaded = false;
static bool g_named_ok = true;
static uintptr_t g_slot_word_addr = 0;
static bool FakeFind(const char* tail, uintptr_t addr, EcLibInfo* out) {
  if (tail != nullptr) {
    if (std::strcmp(tail, "/libpnsovr.so") != 0 || !g_lib_loaded) return false;
  } else if (addr != reinterpret_cast<uintptr_t>(g_fake_wl_body)) {
    return false;
  }
  out->base = reinterpret_cast<uintptr_t>(g_lib_img);
  out->dyn = reinterpret_cast<uintptr_t>(g_lib_img + 0x100);
  return true;
}
static bool FakeNamed(uintptr_t fn, const char* sym) {
  if (!g_named_ok) return false;
  if (!std::strcmp(sym, "_ZN10NRadEngine9NWriteLog8WriteLogENS0_9ELogLevelEyPKcz")) return fn == reinterpret_cast<uintptr_t>(g_fake_wl_body);
  if (!std::strcmp(sym, "_ZN10NRadEngine9NWriteLog9WriteLogVENS0_9ELogLevelEyPKcSt9__va_list")) return fn == g_fake_vfn;
  return false;
}
static void BuildLib(bool with_slot = true, bool with_vslot = true) {
  std::memset(g_lib_img, 0, sizeof g_lib_img);
  for (int i = 0; i < 25; ++i) g_fake_wl_body[i] = kWriteLogBody[i];
  g_fake_wl_body[21] = 0x94000010;
  g_fake_vfn = reinterpret_cast<uintptr_t>(&FakeWriteLogV);
  // strtab 0x1000: \0 WriteLog \0 WriteLogV \0
  const char* wl = "_ZN10NRadEngine9NWriteLog8WriteLogENS0_9ELogLevelEyPKcz";
  const char* wv = "_ZN10NRadEngine9NWriteLog9WriteLogVENS0_9ELogLevelEyPKcSt9__va_list";
  size_t o1 = 1, o2 = 1 + std::strlen(wl) + 1;
  std::memcpy(g_lib_img + 0x1000 + o1, wl, std::strlen(wl) + 1);
  std::memcpy(g_lib_img + 0x1000 + o2, wv, std::strlen(wv) + 1);
  ElfSym* sy = reinterpret_cast<ElfSym*>(g_lib_img + 0x800);
  sy[1].name = static_cast<uint32_t>(o1);
  sy[2].name = static_cast<uint32_t>(o2);
  ElfRela* re = reinterpret_cast<ElfRela*>(g_lib_img + 0x400);
  int n = 0;
  if (with_slot) re[n++] = {0x1800, (1ull << 32) | 1026, 0};
  if (with_vslot) re[n++] = {0x1808, (2ull << 32) | 1026, 0};
  ElfDyn* dy = reinterpret_cast<ElfDyn*>(g_lib_img + 0x100);
  int i = 0;
  dy[i++] = {23, 0x400};
  dy[i++] = {2, static_cast<uint64_t>(n) * sizeof(ElfRela)};
  dy[i++] = {20, 7};
  dy[i++] = {6, 0x800};
  dy[i++] = {5, 0x1000};
  dy[i++] = {0, 0};
  g_slot_word_addr = reinterpret_cast<uintptr_t>(g_lib_img + 0x1800);
  *reinterpret_cast<uintptr_t*>(g_slot_word_addr) = reinterpret_cast<uintptr_t>(g_fake_wl_body);
  *reinterpret_cast<uintptr_t*>(g_lib_img + 0x1808) = g_fake_vfn;
  g_lib_loaded = true;
  g_named_ok = true;
}
static void PlatformLibTests() {
  EcSelEnv env;
  env.find_lib = &FakeFind;
  env.named_as = &FakeNamed;
  ResetAll();
  InstallOk();
  BuildLib();
  g_lib_loaded = false;
  SetEcSelEnvForTest(env);
  EcStep(g_now100);
  CHECK(!EcSelTriedForTest(1) && !EcSelTriedForTest(2), "a lib that is not loaded was counted as tried");
  CHECK(*reinterpret_cast<uintptr_t*>(g_slot_word_addr) == reinterpret_cast<uintptr_t>(g_fake_wl_body), "slot touched before the lib loaded");
  g_lib_loaded = true;
  AdvanceMs(500);
  EcStep(g_now100);
  CHECK(!EcSelTriedForTest(1), "retried before a second passed");
  AdvanceMs(600);
  EcStep(g_now100);
  CHECK(EcSelTriedForTest(1) && !EcSelTriedForTest(2), "libpnsovr tried, libpnsrad (not loaded) not");
  CHECK(*reinterpret_cast<uintptr_t*>(g_slot_word_addr) == EcWriteLogWrapForTest(1), "the platform lib's WriteLog word not swapped");
  CHECK(HasLine("ec: sel watching") && HasLine("lib=libpnsovr.so"), "no sel watching record");
  // the wrapper works through the swapped word
  auto fn = reinterpret_cast<WriteLogFn>(*reinterpret_cast<uintptr_t*>(g_slot_word_addr));
  fn(2, 3ull, "[NSUSER] sent profile update: %s", "{\"ability\":\"point\"}");
  CHECK(g_v_calls == 1 && Load64(EcPage() + kEquipSel) == 0xe32dc7cad6864aa4ull, "the platform lib wrapper did not forward / store");
  const int calls_before = g_v_calls;
  AdvanceMs(5000);
  EcStep(g_now100);
  CHECK(g_v_calls == calls_before && *reinterpret_cast<uintptr_t*>(g_slot_word_addr) == EcWriteLogWrapForTest(1), "a tried lib must not be swapped again");
  CHECK(!HasLine("ec: sel refused"), "a lib that was swapped was looked at again (its word now holds our wrapper)");
  // refusals: a lib is tried once, loud, its word untouched
  struct Case {
    const char* what;
    bool slot, vslot, named, body_ok;
  } cases[] = {{"no WriteLog jump slot", false, true, true, true},
               {"the word is not a WriteLog of the known body", true, true, false, true},
               {"the word is not a WriteLog of the known body", true, true, true, false},
               {"the owner of its WriteLog has no WriteLogV word", true, false, true, true}};
  for (const auto& c : cases) {
    ResetAll();
    InstallOk();
    BuildLib(c.slot, c.vslot);
    if (!c.body_ok) g_fake_wl_body[7] ^= 1;
    g_named_ok = c.named;
    SetEcSelEnvForTest(env);
    EcStep(g_now100);
    CHECK(EcSelTriedForTest(1), "%s: a refused lib must count as tried", c.what);
    CHECK(HasLine("ec: sel refused", 2) && HasLine(c.what), "%s: no WARNING", c.what);
    if (c.slot) CHECK(*reinterpret_cast<uintptr_t*>(g_slot_word_addr) == reinterpret_cast<uintptr_t>(g_fake_wl_body), "%s: the word was touched", c.what);
  }
  // the owner's WriteLogV word does not hold a WriteLogV
  ResetAll();
  InstallOk();
  BuildLib();
  *reinterpret_cast<uintptr_t*>(g_lib_img + 0x1808) = 0x1234;
  SetEcSelEnvForTest(env);
  EcStep(g_now100);
  CHECK(HasLine("ec: sel refused", 2) && *reinterpret_cast<uintptr_t*>(g_slot_word_addr) == reinterpret_cast<uintptr_t>(g_fake_wl_body), "WriteLogV word unchecked");
  SetEcSelEnvForTest(EcSelEnv());
}

static void SwitchTests() {
  // no switch file: off, the page line is still logged, nothing installed
  char tmpl[] = "/tmp/q15t-ec-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) std::exit(2);
  ResetAll();
  SetEcDirForTest(tmpl);
  InstallResult r = InstallEc(g_base);
#ifdef Q15T_EC_DEFAULT_WORDS
  // a build with default words: a fresh install with NO switch file is on, with the build's words
  CHECK(r.status == SwapStatus::kOk && EcStatsNow().types == 15 && EcStatsNow().hide_write && EcStatsNow().equip_store,
        "default words, no file: must be on with types 15 (VPTO): status %s types %u", StatusName(r.status), EcStatsNow().types);
  CHECK(HasLine("switch=build default") && !HasLine("ec: off"), "default words: no 'build default' record, or an 'ec: off' line");
  CHECK(*W(kEcSlotRecvfrom) != reinterpret_cast<uintptr_t>(dlsym(RTLD_DEFAULT, "recvfrom")), "default words: recvfrom was not swapped");
#else
  CHECK(std::string(r.reason) == "off" && *W(kEcSlotRecvfrom) == reinterpret_cast<uintptr_t>(dlsym(RTLD_DEFAULT, "recvfrom")), "no switch: must install nothing");
  CHECK(HasLine("ec: off") && HasLine("ec: page"), "no off / page record");
#endif
  // ec_preset1.on = the preset
  const std::string d = tmpl;
  FILE* f = std::fopen((d + "/ec_preset1.on").c_str(), "w");
  std::fclose(f);
  ResetAll();
  SetEcDirForTest(tmpl);
  r = InstallEc(g_base);
  CHECK(r.status == SwapStatus::kOk && EcStatsNow().types == 5 && EcStatsNow().hide_write && EcStatsNow().equip_store, "ec_preset1.on preset: types %u", EcStatsNow().types);
  CHECK(HasLine("switch=ec_preset1.on"), "no switch record");
  // ec_preset2.on / ec_preset3.on
  unlink((d + "/ec_preset1.on").c_str());
  f = std::fopen((d + "/ec_preset2.on").c_str(), "w");
  std::fclose(f);
  ResetAll();
  SetEcDirForTest(tmpl);
  InstallEc(g_base);
  CHECK(EcStatsNow().types == 7, "ec_preset2 preset: %u", EcStatsNow().types);
  unlink((d + "/ec_preset2.on").c_str());
  f = std::fopen((d + "/ec_preset3.on").c_str(), "w");
  std::fclose(f);
  ResetAll();
  SetEcDirForTest(tmpl);
  InstallEc(g_base);
  CHECK(EcStatsNow().types == 15, "ec_preset3 preset: %u", EcStatsNow().types);
  unlink((d + "/ec_preset3.on").c_str());
  // ec.on wins and carries its own words (first line)
  f = std::fopen((d + "/ec.on").c_str(), "w");
  std::fputs("types=V noequip\nhide\n", f);
  std::fclose(f);
  f = std::fopen((d + "/ec_preset1.on").c_str(), "w");
  std::fclose(f);
  ResetAll();
  SetEcDirForTest(tmpl);
  InstallEc(g_base);
  EcStats s = EcStatsNow();
  CHECK(s.types == 1 && !s.equip_store && !s.hide_write, "ec.on must win and only its first line counts: types %u equip %d hide %d", s.types, s.equip_store, s.hide_write);
  CHECK(EcDataFileExists("ec.on") && !EcDataFileExists("nothere"), "EcDataFileExists");
  // ec.off is a kill file: it beats ec.on, the presets and the build's default words, and installs nothing
  f = std::fopen((d + "/ec.off").c_str(), "w");
  std::fclose(f);
  ResetAll();
  SetEcDirForTest(tmpl);
  r = InstallEc(g_base);
  CHECK(std::string(r.reason) == "off" && *W(kEcSlotRecvfrom) == reinterpret_cast<uintptr_t>(dlsym(RTLD_DEFAULT, "recvfrom")) &&
            *W(kEcSlotSendto) == reinterpret_cast<uintptr_t>(dlsym(RTLD_DEFAULT, "sendto")),
        "ec.off with ec.on and a preset beside it: must install nothing: reason %s", r.reason);
  CHECK(HasLine("kill_file=ec.off") && HasLine("ec: off") && !HasLine("ec: on"), "ec.off: no 'off' record naming the kill file, or an 'ec: on' line");
  unlink((d + "/ec.off").c_str());
  unlink((d + "/ec.on").c_str());
  unlink((d + "/ec_preset1.on").c_str());
  rmdir(tmpl);
  // a data directory that does not exist: off, or the build's default words when it has them
  ResetAll();
  SetEcDirForTest("/nonexistent/q15t");
#ifdef Q15T_EC_DEFAULT_WORDS
  CHECK(InstallEc(g_base).status == SwapStatus::kOk, "default words: a missing data directory must still be on");
#else
  CHECK(std::string(InstallEc(g_base).reason) == "off", "a missing data directory must read as off");
#endif
}

static void PageTests() {
  ResetAll();
  uint8_t* p = EcPage();
  CHECK((reinterpret_cast<uintptr_t>(p) & 0xfff) == 0, "page not 4096-aligned");
  bool zero = true;
  for (int i = 0; i < 4096; ++i) zero &= p[i] == 0;
  CHECK(zero, "page not zeroed");
  CHECK(dlsym(RTLD_DEFAULT, "re33_ec_page") == p || dlsym(RTLD_DEFAULT, "re33_ec_page") == nullptr, "symbol resolves to the page");
  const EcPageState st = PublishEcPage();
  CHECK(st == EcPageState::kVisible, "the test executable exports the page (-rdynamic): state %d", (int)st);
  CHECK(HasLine("ec: page published", 1), "no INFO for a published page");
}

int main() {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  SetLogContext(&ctx);
  CoreTests();
  ListTests();
  InstallTests();
  WrapperTests();
  NetTickTests();
  StepTests();
  HideTests();
  SelectionTests();
  PlatformLibTests();
  SwitchTests();
  PageTests();
  DropImage();
  if (g_fail == 0) std::printf("ec_test: all checks passed\n");
  return g_fail == 0 ? 0 : 1;
}
