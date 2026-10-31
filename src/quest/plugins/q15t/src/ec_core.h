// The platform-free half of the `ec` module (docs/quest/q15t/modules/ec.md): the EC spare page layout, the EC network packet, the
// token / dedupe / server-pick rules, the tac-mod selection watcher, the tac-mod list hide and the ELF jump-slot lookup.
// A port of the source build's ec_core (<source build>/the source build's ec_core, rules from
// EC upstream 1.1.7). Differences, all deliberate:
//   - no MD5 / libr15 file identity: the plugin's InitEx already gates on the pinned build id;
//   - every read of game memory goes through SafeRead (the source dereferenced the game's lists and a lib's dynamic table
//     directly); the one write into game memory, the tac-mod count, is a compare-and-swap against the count that was read;
//   - the gear gate is a pthread recursive mutex with a static initializer (std::recursive_mutex has a non-constexpr
//     constructor in libc++, which the repack's static-init check would refuse as a namespace-scope object).
//
// The page stands for echovr.exe RVAs [0x2104000, 0x2105000): the EC script libs take `re33_ec_page - 0x2104000` as the image
// base, so every PC offset keeps its meaning. Clock = CLOCK_MONOTONIC / 100 ns (the clock the lifted bodies read).
#pragma once

#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "safe_read.h"

namespace q15t {
namespace ecc {

constexpr std::uint32_t kPageRva = 0x2104000, kPageSize = 0x1000;
// token blocks (+00 u32 count, +08 u64 expiry, +10 u32 shown) and use counters (u32), page offsets
constexpr std::uint32_t kOcUse = 0xca4, kOcTok = 0xca8, kVeilTok = 0xcc0, kPointTok = 0xce0, kLocTok = 0xd20, kLocUse = 0xd40;
constexpr std::uint32_t kVeilUse = 0xf00, kPointUse = 0xf80;
// the drone block: +18 u32 = node steps the 221a prelude saw
constexpr std::uint32_t kDroneBlock = 0xd60, kDroneSteps = 0x18;
// QUEST-ONLY word: u32, 1 while the CURRENT server answered the EC network within kServerTimeoutMs, else 0
constexpr std::uint32_t kQuestEcServer = 0x040;
// the EQUIP block: +00 u64 = the selected tac mod's symbol (0 = none known), +10 u32 = guarded nodes skipped, +14 server flag
// (never set by a client), +20 u32 = the controller's re-run counter
constexpr std::uint32_t kEquipSel = 0xe80, kEquipSkips = 0xe90, kEquipReruns = 0xea0;
constexpr std::uint64_t kTokenLife = 15000000;                  // 1.5 s in 100 ns
constexpr std::uint32_t kMagic = 0x4c564345, kMagic2 = 0x4c494556;  // "ECVL" / "VEIL"
constexpr std::uint64_t kServerTimeoutMs = 6000;

struct Packet {  // 16 bytes, little-endian on both sides
  std::uint32_t magic;
  std::uint8_t type;  // 'H' hello, 'A' answer, 'V' veil, 'P' point, 'T' teleport, 'O' overcharge, 'K' drone killed, 'M' medigun
  std::uint8_t pad[3];
  std::uint32_t id;
  std::uint32_t magic2;
};
static_assert(sizeof(Packet) == 16, "EC network packet");

inline bool Carried(std::uint8_t t) { return t == 'V' || t == 'P' || t == 'T' || t == 'O'; }
inline bool Known(std::uint8_t t) { return Carried(t) || t == 'H' || t == 'A' || t == 'K' || t == 'M'; }
inline std::uint32_t TokenOf(std::uint8_t t) { return t == 'P' ? kPointTok : t == 'T' ? kLocTok : t == 'O' ? kOcTok : kVeilTok; }
inline std::uint32_t UseOf(int k) { return k == 3 ? kOcUse : k == 2 ? kLocUse : k ? kPointUse : kVeilUse; }  // k over "VPTO"

inline void Store32(std::uint8_t* p, std::uint32_t v) { __atomic_store_n(reinterpret_cast<std::uint32_t*>(p), v, __ATOMIC_SEQ_CST); }
inline std::uint32_t Load32(const std::uint8_t* p) { return __atomic_load_n(reinterpret_cast<const std::uint32_t*>(p), __ATOMIC_SEQ_CST); }
inline std::uint64_t Load64(const std::uint8_t* p) { return __atomic_load_n(reinterpret_cast<const std::uint64_t*>(p), __ATOMIC_SEQ_CST); }

// a token block whose life is over starts again at 0; then one more token, good for kTokenLife
inline void AddToken(std::uint8_t* tok, std::uint64_t now100) {
  auto* count = reinterpret_cast<std::uint32_t*>(tok);
  auto* expiry = reinterpret_cast<std::uint64_t*>(tok + 8);
  if (now100 >= __atomic_load_n(expiry, __ATOMIC_SEQ_CST)) __atomic_store_n(count, 0u, __ATOMIC_SEQ_CST);
  __atomic_add_fetch(count, 1u, __ATOMIC_SEQ_CST);
  __atomic_store_n(expiry, now100 + kTokenLife, __ATOMIC_SEQ_CST);
}

struct Dest {
  int sock;
  std::uint32_t addr;
  std::uint16_t port;
  int count;
};  // addr / port as on the wire (network order)

// Everything below is called with the owner's lock held; the page writes are atomic by themselves.
struct State {
  std::uint32_t seen[32] = {};  // the last ids; zero-filled, so id 0 is never "first" (as upstream)
  unsigned seenNext = 0;
  Dest dests[16] = {};
  int best = -1;
  std::uint64_t lastAnswerMs = 0;
  std::uint32_t answerAddr = 0;  // who sent that 'A' (network order, as Dest)
  std::uint16_t answerPort = 0;
  std::uint32_t types = 0;  // bit k of "VPTO": this build carries that type
  std::uint64_t dropped = 0, tokens = 0, kills = 0, answers = 0, medi = 0;

  bool Carries(std::uint8_t t) const {
    const int k = t == 'V' ? 0 : t == 'P' ? 1 : t == 'T' ? 2 : t == 'O' ? 3 : -1;
    return k >= 0 && (types >> k & 1);
  }
  bool FirstTime(std::uint32_t id) {
    bool was = false;
    for (std::uint32_t s : seen) was |= s == id;
    if (!was) seen[seenNext++ % 32] = id;
    return !was;
  }
  void SawSend(int sock, std::uint32_t addr, std::uint16_t port) {
    int i = 0, freeSlot = -1, low = 0;
    for (; i < 16; ++i) {
      if (dests[i].count && dests[i].sock == sock && dests[i].addr == addr && dests[i].port == port) break;
      if (!dests[i].count && freeSlot < 0) freeSlot = i;
      if (dests[i].count < dests[low].count) low = i;
    }
    if (i == 16) {
      i = freeSlot >= 0 ? freeSlot : low;
      if (i == best) best = -1;
      dests[i] = Dest{sock, addr, port, 0};
    }
    ++dests[i].count;
    if (best < 0) best = i;
  }
  void PickServer() {  // once a second, the busiest destination of the last second
    int b = -1;
    for (int i = 0; i < 16; ++i)
      if (dests[i].count && (b < 0 || dests[i].count > dests[b].count)) b = i;
    if (b >= 0) best = b;
    for (int i = 0; i < 16; ++i) dests[i].count = i == best ? 1 : 0;
  }
  // The CURRENT server answered within the timeout: an answer of a server we have moved away from does not count.
  bool Answered(std::uint64_t nowMs) const {
    return best >= 0 && lastAnswerMs && nowMs - lastAnswerMs < kServerTimeoutMs && dests[best].addr == answerAddr &&
           dests[best].port == answerPort;
  }
  bool Server(Dest* out) const {
    if (best < 0) return false;
    *out = dests[best];
    return true;
  }
  // the client branch, for a datagram that is ours (16 bytes, both magics, a known type). The sender rule is
  // upstream's: 'A' counts only from the current server; 'K' and the carried types are taken from any sender.
  // Returns the drone index + 1 for a first-time 'K', else 0.
  std::uint32_t Handle(std::uint8_t* page, const Packet& p, std::uint32_t addr, std::uint16_t port, std::uint64_t now100,
                       std::uint64_t nowMs) {
    if (p.type == 'A') {
      Dest d;
      if (Server(&d) && d.addr == addr && d.port == port) {
        lastAnswerMs = nowMs;
        answerAddr = addr;
        answerPort = port;
        ++answers;
      }
    } else if (p.type == 'K' && FirstTime(p.id)) {
      ++kills;
      return (p.id & 0xffff) + 1;
    } else if (Carried(p.type) && FirstTime(p.id)) {
      if (Carries(p.type)) {
        AddToken(page + TokenOf(p.type), now100);
        ++tokens;
      } else {
        ++dropped;
      }
    } else if (p.type == 'M') {
      ++medi;  // every one, resends too: nothing done
    }
    return 0;
  }
};

// The words of the switch file (one line, space separated). A whole word: preceded by start / space, followed by end / space / EOL.
inline bool WholeWord(const char* words, const char* word) {
  const std::size_t n = std::strlen(word);
  for (const char* w = std::strstr(words, word); w; w = std::strstr(w + 1, word)) {
    const char end = w[n];
    if ((w == words || w[-1] == ' ') && (end == 0 || end == ' ' || end == '\n' || end == '\r')) return true;
  }
  return false;
}

// The carried-type mask: the word `types=<letters of VPTO>`; no word, an empty list or any other letter = 'T' only.
inline std::uint32_t TypesFromWords(const char* words) {
  const std::uint32_t step1 = 1u << 2;
  const char* w = std::strstr(words, "types=");
  if (!w || (w != words && w[-1] != ' ')) return step1;
  std::uint32_t m = 0;
  for (w += 6; *w && *w != ' ' && *w != '\n' && *w != '\r'; ++w) {
    const int k = *w == 'V' ? 0 : *w == 'P' ? 1 : *w == 'T' ? 2 : *w == 'O' ? 3 : -1;
    if (k < 0) return step1;
    m |= 1u << k;
  }
  return m ? m : step1;
}
// The selection is stored by default; the whole word `noequip` = seen and logged only.
inline bool EquipFromWords(const char* words) { return !WholeWord(words, "noequip"); }
// `loctoken=<s>`: seconds between test LOC tokens; outside [1, 600] or absent = 0 (off).
inline double LocTokenFromWords(const char* words) {
  const char* w = std::strstr(words, "loctoken=");
  if (!w) return 0;
  const double s = std::atof(w + 9);
  return s >= 1 && s <= 600 ? s : 0;
}

// The selected tac mod. The client logs "[NSUSER] sent profile update: <compact json>" each time it sends profile
// changes; a changed tac mod is in it as "ability":"<name>". The seven names and their symbols are upstream's.
constexpr char kProfileFmt[] = "[NSUSER] sent profile update: %s";
inline std::uint64_t AbilitySymbol(const char* name, std::size_t n) {
  static const struct {
    const char* name;
    std::uint64_t sym;
  } k[] = {{"smoke", 0xe32dc7c9d4804fb5ull}, {"point", 0xe32dc7cad6864aa4ull},  {"buff", 0xc8c33e48386618b6ull},
           {"heal", 0xc8c33e4832761fbcull},  {"shield", 0x41d2d43513260b1aull}, {"sensor", 0x41d2d4381430080cull},
           {"wraith", 0x41d2d02f1b2a1316ull}};
  for (const auto& e : k)
    if (std::strlen(e.name) == n && std::memcmp(e.name, name, n) == 0) return e.sym;
  return 0;
}
inline bool IsProfileFmt(const char* fmt) { return fmt && fmt[0] == '[' && fmt[1] == 'N' && std::strcmp(fmt, kProfileFmt) == 0; }
// nameOut (may be null): the name as sent, cut to nameLen - 1, also when it is not one of the seven
inline std::uint64_t SelectedFromJson(const char* json, char* nameOut = nullptr, std::size_t nameLen = 0) {
  if (nameOut && nameLen) nameOut[0] = 0;
  const char* a = json ? std::strstr(json, "\"ability\":\"") : nullptr;
  if (!a) return 0;
  a += 11;
  const char* e = std::strchr(a, '"');
  if (!e) return 0;
  const std::size_t n = static_cast<std::size_t>(e - a);
  if (nameOut && nameLen) {
    const std::size_t c = n < nameLen - 1 ? n : nameLen - 1;
    std::memcpy(nameOut, a, c);
    nameOut[c] = 0;
  }
  return AbilitySymbol(a, n);
}
// re-runs allowed again, then the symbol; each store atomic (the lifted guard loads +0xe80 once per node)
inline void StoreSelected(std::uint8_t* page, std::uint64_t sym) {
  Store32(page + kEquipReruns, 0);
  __atomic_store_n(reinterpret_cast<std::uint64_t*>(page + kEquipSel), sym, __ATOMIC_SEQ_CST);
}

// A loaded ELF64 lib's JUMP_SLOT word for one imported / interposable symbol, found by NAME in its own dynamic table (so no
// per-build offset): `dyn` = its PT_DYNAMIC in memory, `base` = its load bias (bionic leaves the d_ptr values unrelocated).
// Every read is a SafeRead. 0: no DT_JMPREL of RELA entries, or no R_AARCH64_JUMP_SLOT with that name.
struct ElfDyn {
  std::int64_t tag;
  std::uint64_t val;
};
struct ElfRela {
  std::uint64_t offset, info;
  std::int64_t addend;
};
struct ElfSym {
  std::uint32_t name;
  std::uint8_t info, other;
  std::uint16_t shndx;
  std::uint64_t value, size;
};
// a NUL-terminated string at `addr`, at most n-1 bytes: reads in shrinking pieces so a string that ends at a page edge is read
inline bool ReadCStr(std::uintptr_t addr, char* out, std::size_t n) {
  std::size_t got = 0;
  while (got + 1 < n) {
    std::size_t want = n - 1 - got < 64 ? n - 1 - got : 64;
    while (want > 0 && !SafeRead(addr + got, out + got, want)) want >>= 1;
    if (want == 0) return false;
    for (std::size_t i = 0; i < want; ++i)
      if (out[got + i] == 0) return true;
    got += want;
  }
  out[n - 1] = 0;
  return true;
}
inline std::uintptr_t JumpSlot(std::uintptr_t base, std::uintptr_t dyn, const char* sym) {
  std::uint64_t jmprel = 0, pltrelsz = 0, pltrel = 0, symtab = 0, strtab = 0;
  bool ended = false;
  for (int i = 0; i < 512 && dyn != 0; ++i) {
    ElfDyn d{};
    if (!SafeRead(dyn + static_cast<std::uintptr_t>(i) * sizeof d, &d, sizeof d)) return 0;
    if (d.tag == 0) {
      ended = true;
      break;
    }
    if (d.tag == 23) jmprel = d.val;         // DT_JMPREL
    else if (d.tag == 2) pltrelsz = d.val;   // DT_PLTRELSZ
    else if (d.tag == 20) pltrel = d.val;    // DT_PLTREL
    else if (d.tag == 6) symtab = d.val;     // DT_SYMTAB
    else if (d.tag == 5) strtab = d.val;     // DT_STRTAB
  }
  if (!ended || !jmprel || !symtab || !strtab || pltrel != 7) return 0;  // 7 = DT_RELA
  const std::uint64_t count = pltrelsz / sizeof(ElfRela);
  constexpr std::uint64_t kChunk = 64;
  for (std::uint64_t at = 0; at < count; at += kChunk) {
    ElfRela r[kChunk];
    const std::uint64_t n = count - at < kChunk ? count - at : kChunk;
    if (!SafeRead(base + jmprel + at * sizeof(ElfRela), r, n * sizeof(ElfRela))) return 0;
    for (std::uint64_t i = 0; i < n; ++i) {
      if ((r[i].info & 0xffffffffu) != 1026) continue;  // R_AARCH64_JUMP_SLOT
      ElfSym st{};
      if (!SafeRead(base + symtab + (r[i].info >> 32) * sizeof st, &st, sizeof st)) continue;
      char name[160];
      if (!ReadCStr(base + strtab + st.name, name, sizeof name)) continue;
      if (std::strcmp(name, sym) == 0) return base + static_cast<std::uintptr_t>(r[i].offset);
    }
  }
  return 0;
}

// NRadEngine::NWriteLog::WriteLog(ELogLevel, unsigned long long, char const*, ...) of the pinned build: 25 instructions that
// build a va_list and call WriteLogV(level, flags, fmt, va_list), nothing else. the disassembly 0x1088c20..0x1088c80 reads
// exactly these words (docs/quest/q15t/modules/ec.md section 3); word 21 is the `bl WriteLogV@plt`, any target.
constexpr std::uint32_t kWriteLogBody[25] = {0xd10403ff, 0x928004e8, 0x910003e9, 0x910223ea, 0xf2dff008, 0x910403eb, 0x91020129,
                                             0x9100a14a, 0xa90efbe8, 0xa90da7ea, 0xf9006beb, 0xad0007e0, 0xad4683e1, 0xa90893e3,
                                             0x9102c3e3, 0xa9099be5, 0xf90057e7, 0xad010fe2, 0xad0217e4, 0xad031fe6, 0xad0583e1,
                                             0x94000000 /* bl, any target */, 0xf9407bfe, 0x910403ff, 0xd65f03c0};
inline bool IsWriteLogWords(const std::uint32_t w[25]) {
  for (int i = 0; i < 25; ++i)
    if (i == 21 ? (w[i] & 0xfc000000u) != 0x94000000u : w[i] != kWriteLogBody[i]) return false;
  return true;
}
inline bool IsWriteLogBody(std::uintptr_t fn) {
  std::uint32_t w[25];
  return SafeRead(fn, w, sizeof w) && IsWriteLogWords(w);
}

// The tac-mod list hide. The gear registry's tac-mod list is { allocator, entries, u64 count, u64 capacity },
// entries of 0x88 bytes with the name (char[64]) first; EC's three are the table's last three, in this order. On a
// server without EC the count is lowered by 3 (the entries stay), with it the full count is put back. A rebuilt list
// has a new entries pointer and its full count again. Nothing is touched unless the last three are ours. write = false: only
// tells what it would do. Returns 0: nothing to do, 1: shown, 2: hidden, 3: kept whole.
// keepWhole: one of the three is the player's selection, so on a server without EC the list stays
// whole (cut short, the selection would point past its end); 3 = a cut list was put back for that reason.
constexpr std::uint32_t kTacEntry = 0x88;
constexpr char kNewTacMods[3][8] = {"smoke", "buff", "point"};
struct TacList {
  std::uintptr_t entries = 0;
  std::uint64_t full = 0;
  std::uint32_t gen = 0;
};
// The gate round the game's own rebuild / free of the gear tables: the wrappers of ReloadEquipmentConfig /
// UnloadEquipmentConfig hold it across the real call, the looker holds it across its look. Recursive: a Reload that reaches
// Reload or Unload again through a PLT stub on the same thread must not hang itself. gen counts the rebuilds / frees: a looker
// that sees a new gen forgets what it knew of the list.
struct GearGate {
  pthread_mutex_t mu = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
  std::atomic<std::uint32_t> gen{0};
  void Enter() {
    pthread_mutex_lock(&mu);
    gen.fetch_add(1, std::memory_order_relaxed);
  }
  void Leave() { pthread_mutex_unlock(&mu); }
  void Lock() { pthread_mutex_lock(&mu); }    // the looker: no rebuild counted
  void Unlock() { pthread_mutex_unlock(&mu); }
};
inline bool OursAtEnd(std::uintptr_t entries, std::uint64_t full) {
  for (int k = 0; k < 3; ++k) {
    char name[64];
    if (!SafeRead(entries + (full - 3 + static_cast<std::uint64_t>(k)) * kTacEntry, name, sizeof name)) return false;
    if (std::strncmp(name, kNewTacMods[k], 64) != 0) return false;
  }
  return true;
}
// the selected tac mod (EQUIP +0xe80) is smoke / point / buff
inline bool OurSelected(std::uint64_t sym) {
  return sym == 0xe32dc7c9d4804fb5ull || sym == 0xe32dc7cad6864aa4ull || sym == 0xc8c33e48386618b6ull;
}
// SFD-1: the list is only cut short once the selection has been still for kHideSettleMs; putting it back stays at once.
constexpr std::uint64_t kHideSettleMs = 2000;
inline bool HideSettled(std::uint64_t nowMs, std::uint64_t selChangedMs) { return nowMs - selChangedMs >= kHideSettleMs; }
// `list`: the address of the list object (the value of the word at libr15 + kTacModList). lowerOk = false: a cut that is due
// waits (0 = nothing done).
inline int TacModsFor(TacList& st, std::uintptr_t list, bool serverHasIt, bool write, std::uint64_t* nowCount, std::uint32_t gen = 0,
                      bool keepWhole = false, bool lowerOk = true) {
  if (gen != st.gen) {  // rebuilt or freed since the last look
    st.entries = 0;
    st.full = 0;
    st.gen = gen;
  }
  if (!list) return 0;
  bool ok = true;
  const auto entries = Rd<std::uintptr_t>(list + 0x08, &ok);
  const auto count_now = Rd<std::uint64_t>(list + 0x10, &ok);
  const auto capacity = Rd<std::uint64_t>(list + 0x18, &ok);
  if (!ok || !entries) return 0;
  if (entries != st.entries) {  // a new list: its full count is the most it has had
    st.entries = entries;
    st.full = 0;
  }
  if (count_now > st.full) st.full = count_now;
  const std::uint64_t full = st.full;
  if (full < 3 || full > capacity || capacity > 4096 || !OursAtEnd(entries, full)) return 0;
  const std::uint64_t want = serverHasIt || keepWhole ? full : full - 3;
  if (count_now == want) return 0;
  if (want < count_now && !lowerOk) return 0;
  if (write) {
    std::uint64_t expect = count_now;  // a count that moved since the read is left alone: the next look decides again
    if (!__atomic_compare_exchange_n(reinterpret_cast<std::uint64_t*>(list + 0x10), &expect, want, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
      return 0;
  }
  *nowCount = want;
  return serverHasIt ? 1 : keepWhole ? 3 : 2;
}

// Is this datagram ours? Exactly 16 bytes, both magics, a known type (upstream takes any type).
inline bool Ours(const void* buf, long n, Packet* out) {
  if (n != static_cast<long>(sizeof(Packet))) return false;
  std::memcpy(out, buf, sizeof(Packet));
  return out->magic == kMagic && out->magic2 == kMagic2 && Known(out->type);
}

// One pass over the four use counters. seenUse[4] = the values of the last pass. For each counter that moved and
// whose type this build carries: an id, remembered; a local token unless it is 'T' (the teleporting machine shows its own); the
// packet to send twice. Returns how many (type, id) pairs were written to out (at most 4).
struct Use {
  std::uint8_t type;
  std::uint32_t id;
};
inline int WatchUses(State& st, std::uint8_t* page, std::uint32_t seenUse[4], std::uint32_t tickMs, std::uint32_t pid, std::uint64_t now100,
                     Use out[4]) {
  int n = 0;
  for (int k = 0; k < 4; ++k) {
    const std::uint8_t type = static_cast<std::uint8_t>("VPTO"[k]);
    const std::uint32_t u = Load32(page + UseOf(k));
    if (u == seenUse[k]) continue;
    seenUse[k] = u;
    if (!st.Carries(type)) continue;
    const std::uint32_t id = tickMs * 2654435761u ^ pid << 16 ^ u ^ static_cast<std::uint32_t>(k);
    st.FirstTime(id);
    if (type != 'T') {
      AddToken(page + TokenOf(type), now100);
      ++st.tokens;
    }
    out[n++] = Use{type, id};
  }
  return n;
}

}  // namespace ecc
}  // namespace q15t
