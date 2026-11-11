/* The Quest's half of the EC client (ec.h, docs/quest/combat/ec.md) that is not shared with PC: the page words only the Quest uses, the
 * switch words, the gear gate, and the reads of game memory the shared tac-mod hide makes (through SafeRead). The EC network rules (the
 * packet and its types, the receive test and its cap, the tokens, the dedupe ring, the server pick, the client branch, the use watcher, the
 * tac-mod parse and the list hide) are runtime/patch/ec_rules.h, which PC's runtime/combat/veilnet.cpp compiles too (#2358). The words and
 * the gate are the reference build's `afk_ec_core.h` (kotorvr's q15t-upstream). Deliberate differences from it:
 *   - no MD5 / libr15 file identity: the install checks the pinned build ID;
 *   - every read of game memory goes through SafeRead (combat/safe_read.h);
 *   - the gear gate is a pthread recursive mutex with a static initializer (std::recursive_mutex has a non-constexpr constructor in libc++,
 *     which the static-init check refuses as a namespace-scope object).
 *
 * The page stands for echovr.exe RVAs [0x2104000, 0x2105000): the EC script libs take `re33_ec_page - 0x2104000` as the image base, so every
 * PC offset keeps its meaning. Clock = CLOCK_MONOTONIC / 100 ns (the clock the lifted bodies read).
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "quest/combat/safe_read.h"
#include "runtime/patch/ec_rules.h"

namespace nevr_quest::combat::ec::core {

using Dest = ::nevr_ec::BasicDest<int>;
using State = ::nevr_ec::BasicState<int>;

// QUEST-ONLY word: u32, 1 while the CURRENT server states EchoCombat in its lobby settings (#2333, runtime/compat/server_features.h), else 0
constexpr std::uint32_t kQuestEcServer = 0x040;
// the drone block: +18 u32 = node steps the 221a prelude saw
constexpr std::uint32_t kDroneBlock = 0xd60, kDroneSteps = 0x18;
// the EQUIP block's +10 u32 = guarded nodes skipped (+14 is the server flag, never set by a client)
constexpr std::uint32_t kEquipSkips = 0xe90;

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

// The shared hide's reads of the list, through SafeRead.
struct SafeReader {
  bool Read(std::uintptr_t address, void* out, std::size_t length) const { return SafeRead(address, out, length); }
};
// The shared tac-mod hide (ec_rules.h TacModsFor) with the Quest's reads. `list`: the address of the list object (the value of the word at
// libr15 + kTacModList). write = false: only tells what it would do. settled = false: a cut that is due waits (0 = nothing done).
// Returns 0: nothing to do, 1: shown, 2: hidden, 3: kept whole (keepWhole: one of ours is the player's selection).
inline int TacModsFor(::nevr_ec::TacList& st, std::uintptr_t list, bool serverHasIt, bool write, std::uint64_t* nowCount,
                      std::uint32_t gen = 0, bool keepWhole = false, bool settled = true) {
  SafeReader reader;
  return ::nevr_ec::TacModsFor(st, reader, list, serverHasIt, write, nowCount, gen, keepWhole, settled);
}

}  // namespace nevr_quest::combat::ec::core
