// The platform-free half of the `ecfx` module (docs/quest/q15t/modules/ec.md): EC 1.1.7's NEUTRON effects deferral over the queue on
// re33_ec_page. A port of the source build's ecfx_core (upstream WallFxDue / SoundAgain).
// Nothing here calls the game; the caller runs the planned entries.
#pragma once

#include <cstdint>

namespace q15t {
namespace ecfx {

// page offsets (PC rva 0x2104000 base): veil_fx FX_QUEUE_RVA 0x2104DA0 (6 x { u64 ctx, u64 word }), FX_DEFER_RVA 0x2104E20
constexpr std::uint32_t kQueueOff = 0xda0, kGateOff = 0xe20;
constexpr int kSlots = 6;
constexpr int kCharge = 0, kBurst = 1, kBoom = 2, kSound = 3;  // word & 3
constexpr std::uint64_t kStaleMs = 500, kSoundMs = 3000, kEveryMs = 8;

// Q-1: ctx then word loaded with ACQUIRE (the lib claims ctx by CAS, then stores word with RELEASE: word != 0 = the slot is
// complete); a slot is freed word first, then ctx, both RELEASE (the lib claims only a slot whose ctx is 0).
inline std::uint64_t Ctx(std::uint64_t* q, int i) { return __atomic_load_n(q + 2 * i, __ATOMIC_ACQUIRE); }
inline std::uint64_t Word(std::uint64_t* q, int i) { return __atomic_load_n(q + 2 * i + 1, __ATOMIC_ACQUIRE); }
inline void Free(std::uint64_t* q, int i) {
  __atomic_store_n(q + 2 * i + 1, 0, __ATOMIC_RELEASE);
  __atomic_store_n(q + 2 * i, 0, __ATOMIC_RELEASE);
}

// df SoundAgain: one sound per wall per kSoundMs; 8 walls remembered, the oldest replaced
struct Sounds {
  std::uint64_t wall[8] = {}, at[8] = {};
  bool Again(std::uint64_t w, std::uint64_t nowMs) {
    int oldest = 0;
    for (int i = 0; i < 8; ++i) {
      if (wall[i] == w && at[i] && nowMs - at[i] < kSoundMs) return true;
      if (at[i] < at[oldest]) oldest = i;
    }
    wall[oldest] = w;
    at[oldest] = nowMs;
    return false;
  }
};

// entry[kind] = the exact address of the lib's entry for that kind (0 = unknown: that kind is never run). Q-2: the pointer is
// word & ~0xf; kind 2 (the stun BOOM) is the server's alone: freed, never run on a client.
struct Entries {
  std::uint64_t entry[4] = {};
};

struct Run {
  std::uint64_t ctx, fn;
  int kind, slot;
};
struct Counts {
  int run = 0, boom = 0, sound = 0, refused = 0, stale = 0, inflight = 0;
};

// One drain (df WallFxDue): stale (no drain for more than kStaleMs) = every complete slot freed, nothing run. Else one entry per
// ctx, the lowest kind first; BOOM / a repeated sound / an entry that is not the lib's exact one for its kind = freed without a
// run; ctx != 0 && word == 0 = in flight (left alone). The planned runs are NOT freed here: the caller frees each slot after its
// entry returned (Q-1). wallOf(ctx) = [[ctx]+0x68] (0 = unreadable: sound refused).
struct Drain {
  std::uint64_t last = 0;
  Sounds sounds;
  int Plan(std::uint64_t* q, const Entries& e, std::uint64_t nowMs, std::uint64_t (*wallOf)(std::uint64_t ctx), Run* out, Counts* c) {
    const bool stale = last && nowMs - last > kStaleMs;
    last = nowMs;
    int n = 0;
    std::uint64_t ran[kSlots];
    int nran = 0;
    for (int i = 0; i < kSlots; ++i) {
      const std::uint64_t ctx = Ctx(q, i);
      if (!ctx) continue;
      const std::uint64_t w = Word(q, i);
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
      if (done) continue;  // this ctx already has its entry this drain
      int best = i;
      for (int j = i + 1; j < kSlots; ++j) {
        if (Ctx(q, j) != ctx) continue;
        const std::uint64_t wj = Word(q, j);
        if (wj && (wj & 3) < (Word(q, best) & 3)) best = j;
      }
      const std::uint64_t wb = Word(q, best);
      const int kind = static_cast<int>(wb & 3);
      const std::uint64_t fn = wb & ~static_cast<std::uint64_t>(0xf);
      ran[nran++] = ctx;
      if (kind == kBoom) {
        Free(q, best);
        ++c->boom;
        continue;
      }
      if (!fn || fn != e.entry[kind] || (wb & 0xc)) {
        Free(q, best);
        ++c->refused;
        continue;
      }
      if (kind == kSound) {
        const std::uint64_t wall = wallOf ? wallOf(ctx) : 0;
        if (!wall || sounds.Again(wall, nowMs)) {
          Free(q, best);
          ++c->sound;
          continue;
        }
      }
      out[n++] = {ctx, fn, kind, best};
      ++c->run;
    }
    return n;
  }
};

}  // namespace ecfx
}  // namespace q15t
