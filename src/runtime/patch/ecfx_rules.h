#pragma once
// NEUTRON's effects drain, for both builds (#2349): the queue a NEUTRON wall's node writes its effects to instead of running them inside
// its own node, and the rules that decide which queued effect runs, which is freed unrun and when. This header is the whole rule: the
// queue's shape and kinds, the stale rule, one entry per wall node per drain with its lowest kind first, the stun boom left to the server,
// one sound per wall per 3000 ms (`Sounds::Again`) and the drain itself (`Drain::Plan`). The game's reads it needs come in through `Env`:
// the read of a wall from its node (`wallOf`) and, where the caller knows them, the exact entry for each kind (`entries`). The callers keep
// their own clock, their own cadence, their own call of an entry and their own log lines: runtime/combat/dronefix.cpp on PC (WallFxDue,
// RunWallFx, every follow update with DRONE on, on a client) and quest/combat/ecfx.cpp on Quest (TickOnce, on the EC client's net tick, at
// most every 8 ms, after `dladdr` names the entry). The PC GTest (runtime/tests/test_ecfx_rules.cpp) and the Quest host test
// (quest/tests/ecfx_test.cpp) both compile this one definition.
//
// Origin. EchoCombat's `plugin/dronefix.cpp` (he_is_the_cat, heisthecat31/EchoCombat), as released in v2.0.0 (nevr-pack 4d1f0024, whose
// dronefix.cpp is c3ff4b54's; "EC" lines below are that file): the queue EC 385-391, WallOf EC 393-395, SoundAgain EC 397-405, WallFxDue
// EC 420-455. PC ports c3ff4b54 (runtime/combat/dronefix.cpp); the Quest ported c14c3e0a, whose block is the same text, through the
// reference build's `runtime/native/afkxr/afk_ecfx_core.h` (kotorvr's q15t-upstream: the acquire/release accesses, the counts, the exact
// entries and the guards are its lines). Both builds hold these forms (#2349):
//   - Every gameplay rule is EC's: stale after more than 500 ms, an in-flight slot left alone, one entry per node, the lowest kind first,
//     the boom freed unrun, `word & ~0xf` the entry, 3000 ms and 8 walls for the sounds, a slot freed word first, then its node.
//   - The order is EC's (D5): each entry runs when the drain reaches its slot, before the next slot is read, and its slot is freed after
//     the entry returns. So an effect queued by a running entry, into a later slot, for a node this drain has not reached, runs this drain.
//   - The exact-entry check is an `Env` input (D1): with `entries` the entry must be the one for its kind (the Quest's, from dlsym); with
//     none (PC, which resolves no entry address, and EC) the check is off.
//   - Bad input is freed and not run on both builds, whatever EC does: an entry of 0, a word with bit 2 or 3 set (D2: the defer stub writes
//     a 16-aligned entry and the kind), a sound whose wall is 0, which is `WallOf`'s read-failure value on both builds (D3), and an empty
//     remembered sound slot matches no wall (D4). EC runs the first two and the third inside `__try`, and its empty slots match wall 0.
//
// The queue, both builds: 6 slots of { u64 node, u64 word } at PC rva 0x2104DA0 (Quest: re33_ec_page + 0xda0); word = entry | kind, kind
// = word & 3 (0 charge, 1 burst, 2 boom, 3 sound). The writer claims a slot whose node is 0, then stores the word: a node with a word of 0
// is a slot still being written.
#include <stdint.h>

namespace nevr_ecfx {

constexpr int kSlots = 6;
constexpr int kCharge = 0, kBurst = 1, kBoom = 2, kSound = 3;  // word & 3
constexpr uint64_t kStaleMs = 500, kSoundMs = 3000;
constexpr int kSoundWalls = 8;

// The node, then the word, each loaded with acquire; a slot is freed word first, then node, each with release (the writer claims only a
// slot whose node is 0).
inline uint64_t Ctx(uint64_t* q, int i) { return __atomic_load_n(q + 2 * i, __ATOMIC_ACQUIRE); }
inline uint64_t Word(uint64_t* q, int i) { return __atomic_load_n(q + 2 * i + 1, __ATOMIC_ACQUIRE); }
inline void Free(uint64_t* q, int i) {
  __atomic_store_n(q + 2 * i + 1, uint64_t{0}, __ATOMIC_RELEASE);
  __atomic_store_n(q + 2 * i, uint64_t{0}, __ATOMIC_RELEASE);
}

// EC SoundAgain: true when this wall's sound played less than kSoundMs ago. Else the wall is remembered in place of the oldest of the
// kSoundWalls and the answer is false. A slot never written (at 0) matches no wall (D4).
struct Sounds {
  uint64_t wall[kSoundWalls] = {}, at[kSoundWalls] = {};
  bool Again(uint64_t w, uint64_t nowMs) {
    int oldest = 0;
    for (int i = 0; i < kSoundWalls; ++i) {
      if (wall[i] == w && at[i] && nowMs - at[i] < kSoundMs) return true;
      if (at[i] < at[oldest]) oldest = i;
    }
    wall[oldest] = w;
    at[oldest] = nowMs;
    return false;
  }
};

// entry[kind] = the exact address of the entry for that kind; 0 = unknown, and that kind never runs.
struct Entries {
  uint64_t entry[4] = {};
};

struct Env {
  const Entries* entries = nullptr;              // nullptr: the exact-entry check is off (D1)
  uint64_t (*wallOf)(uint64_t ctx) = nullptr;   // [[ctx]+0x68]; 0 = unreadable (nullptr: every sound is refused)
};

struct Run {
  uint64_t ctx, fn;
  int kind, slot;
};
// sound counts every sound freed unrun, a repeat or an unreadable wall; nowall counts the unreadable walls among them.
struct Counts {
  int run = 0, boom = 0, sound = 0, nowall = 0, refused = 0, stale = 0, inflight = 0;
};

// One drain (EC WallFxDue). Stale (no drain for more than kStaleMs; the first drain never is): every complete slot is freed, nothing runs.
// Else, slot by slot: a node with no word is in flight and left alone; a node that already had its entry this drain is passed over; else
// the node's lowest kind among its complete slots is taken, and it is freed unrun when it is the boom, bad input (D2, D3) or a repeated
// sound, else `run(const Run&)` is called at once and the slot is freed after it returns. Returns the number of entries run.
struct Drain {
  uint64_t last = 0;
  Sounds sounds;
  template <class RunFn>
  int Plan(uint64_t* q, const Env& env, uint64_t nowMs, RunFn&& run, Counts* c) {
    const bool stale = last && nowMs - last > kStaleMs;
    last = nowMs;
    int n = 0;
    uint64_t ran[kSlots];
    int nran = 0;
    for (int i = 0; i < kSlots; ++i) {
      const uint64_t ctx = Ctx(q, i);
      if (!ctx) continue;
      const uint64_t w = Word(q, i);
      if (!w) {
        ++c->inflight;
        continue;
      }
      if (stale) {
        Free(q, i);
        ++c->stale;
        continue;
      }
      bool done = false;
      for (int k = 0; k < nran; ++k) done |= ran[k] == ctx;
      if (done) continue;  // this node already had its entry this drain
      int best = i;
      for (int j = i + 1; j < kSlots; ++j) {
        if (Ctx(q, j) != ctx) continue;
        const uint64_t wj = Word(q, j);
        if (wj && (wj & 3) < (Word(q, best) & 3)) best = j;
      }
      const uint64_t wb = Word(q, best);
      const int kind = static_cast<int>(wb & 3);
      const uint64_t fn = wb & ~uint64_t{0xf};
      ran[nran++] = ctx;
      if (kind == kBoom) {
        Free(q, best);
        ++c->boom;
        continue;
      }
      if (!fn || (wb & 0xc) || (env.entries && fn != env.entries->entry[kind])) {
        Free(q, best);
        ++c->refused;
        continue;
      }
      if (kind == kSound) {
        const uint64_t wall = env.wallOf ? env.wallOf(ctx) : 0;
        if (!wall || sounds.Again(wall, nowMs)) {
          Free(q, best);
          ++c->sound;
          if (!wall) ++c->nowall;
          continue;
        }
      }
      const Run r{ctx, fn, kind, best};
      run(r);
      Free(q, best);
      ++c->run;
      ++n;
    }
    return n;
  }
};

}  // namespace nevr_ecfx
