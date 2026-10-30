// The platform-free half of the `decals` module (docs/quest/q15t/modules/decals.md): the table file, the level gate, the vertex
// fill and the per-system state machine. Nothing here calls the game: the caller passes the five operations (`Ops`), so the host
// tests drive every rule with stand-ins and the plugin passes the real draw calls.
//
// decals.bin (little endian), as the bake writes it:
//   char magic[4] = "ADC1"; u32 groups
//   per group: u64 level (the CSymbol64 a game space reports, e.g. mpl_combat_dyson), u64 material (the CSymbol64 of the shader
//              set / material pair the dynamic mesh loads), u32 quads, u32 flags (low byte: the blend written into the node's
//              render state; 0 = additive), then quads x 4 corners x {f32 x, y, z, u, v}: world space, corners (-,-) (+,-) (+,+) (-,+).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "plugin_log.h"

namespace q15t {
namespace decals {

constexpr std::uint32_t kMaxGroups = 256;       // sanity caps on the file: a table above them is refused whole
constexpr std::uint32_t kMaxFileQuads = 4096;   // per group
constexpr std::size_t kMaxFileBytes = 32u << 20;
constexpr std::uint32_t kMaxNodeQuads = 160;    // one dynamic mesh node holds this many quads: a longer group draws its first 160
constexpr int kMaxNodes = 24;
constexpr int kMaxSystems = 8;
constexpr int kMaxTries = 3;                    // node creation attempts per system
constexpr std::int64_t kRetryMs = 5000;
constexpr std::int64_t kWindowMs = 10000;

constexpr std::size_t kFileHeaderBytes = 8, kGroupHeaderBytes = 24, kQuadFloats = 20, kQuadBytes = 80;

// CSymbol64 of the level names the table refers to (checked against the SDK's hash in tests/q15t/decals_test.cpp).
constexpr std::uint64_t kLevelWarRoom = 0x08a1af9e108def0bull;  // mpl_combat_war_room
constexpr std::uint64_t kCombatLevels[4] = {0x43e2da7914642604ull, 0x42670f2bed45703cull, 0xdf5ca7b7dfa383d4ull,
                                            0x43e2da7a0c623a19ull};  // mpl_combat_dyson, _combustion, _fission, _gauss

// One vertex of a dynamic mesh (SDynamicMeshVtx, 44 bytes: position, normal, tangent, uv, two colours).
struct Vtx {
  float p[3];
  std::uint16_t n[4];
  std::uint16_t t[4];
  float uv[2];
  std::uint32_t c0, c1;
};
static_assert(sizeof(Vtx) == 44, "SDynamicMeshVtx");

struct Group {
  std::uint64_t level = 0, mat = 0;
  std::uint32_t quads = 0;  // in the file
  std::uint32_t n = 0;      // drawn: min(quads, kMaxNodeQuads)
  std::uint32_t blend = 1;  // the render-state blend (BlendOf)
  const float* v = nullptr; // n x 4 x {x, y, z, u, v} (into the table's storage)
};

struct Table {
  Group* groups = nullptr;
  float* floats = nullptr;
  std::uint32_t count = 0, total_quads = 0, truncated_groups = 0;
  std::size_t trailing_bytes = 0;
};

enum class LoadResult {
  kOk,
  kTooShort,
  kBadMagic,
  kTooManyGroups,
  kGroupHeaderCut,   // a group's header runs past the end of the file
  kTooManyQuads,
  kGroupDataCut,     // a group's quads run past the end of the file
  kNotFinite,        // a coordinate or a uv is NaN or infinite
  kNoMemory,
};

inline const char* LoadResultName(LoadResult r) {
  switch (r) {
    case LoadResult::kOk: return "ok";
    case LoadResult::kTooShort: return "too_short";
    case LoadResult::kBadMagic: return "bad_magic";
    case LoadResult::kTooManyGroups: return "too_many_groups";
    case LoadResult::kGroupHeaderCut: return "group_header_cut";
    case LoadResult::kTooManyQuads: return "too_many_quads";
    case LoadResult::kGroupDataCut: return "group_data_cut";
    case LoadResult::kNotFinite: return "not_finite";
    case LoadResult::kNoMemory: return "no_memory";
  }
  return "unknown";
}

inline void FreeTable(Table* t) {
  delete[] t->groups;
  delete[] t->floats;
  *t = Table();
}

// The blend a group asks for: its flags' low byte when that is one of the render states the decals may use (1 ONE/ONE,
// 6 ONE/INV_DEST_COLOR, 7 SRC_ALPHA/INV_SRC_ALPHA, 8 SRC_ALPHA/ONE, 11 ONE/INV_SRC_ALPHA), else 1.
inline std::uint32_t BlendOf(std::uint32_t flags) {
  const std::uint32_t b = flags & 0xffu;
  return (b == 1 || b == 6 || b == 7 || b == 8 || b == 11) ? b : 1u;
}

// Mirrors the uv inside one quad of 4 x {x, y, z, u, v}: u swaps corners 0<->1 and 3<->2, v swaps 0<->3 and 1<->2.
inline void FlipQuadUv(float* quad, bool flip_u, bool flip_v) {
  auto swap_uv = [quad](int i, int j) {
    for (int k = 3; k < 5; ++k) {
      const float t = quad[5 * i + k];
      quad[5 * i + k] = quad[5 * j + k];
      quad[5 * j + k] = t;
    }
  };
  if (flip_u) {
    swap_uv(0, 1);
    swap_uv(3, 2);
  }
  if (flip_v) {
    swap_uv(0, 3);
    swap_uv(1, 2);
  }
}

inline bool FiniteBits(const std::uint8_t* p) {
  std::uint32_t bits;
  std::memcpy(&bits, p, sizeof bits);
  return (bits & 0x7f800000u) != 0x7f800000u;
}

// Every group is checked against the file's size before anything is kept; a table that fails any check is refused whole and `out`
// stays empty. On success `out` owns its storage (FreeTable).
inline LoadResult ParseTable(const std::uint8_t* b, std::size_t len, bool flip_u, bool flip_v, Table* out) {
  *out = Table();
  if (b == nullptr || len < kFileHeaderBytes) return LoadResult::kTooShort;
  if (std::memcmp(b, "ADC1", 4) != 0) return LoadResult::kBadMagic;
  std::uint32_t groups;
  std::memcpy(&groups, b + 4, 4);
  if (groups > kMaxGroups) return LoadResult::kTooManyGroups;
  std::size_t o = kFileHeaderBytes, floats = 0;
  for (std::uint32_t g = 0; g < groups; ++g) {
    if (len - o < kGroupHeaderBytes) return LoadResult::kGroupHeaderCut;
    std::uint32_t q;
    std::memcpy(&q, b + o + 16, 4);
    if (q > kMaxFileQuads) return LoadResult::kTooManyQuads;
    if ((len - o - kGroupHeaderBytes) / kQuadBytes < q) return LoadResult::kGroupDataCut;
    floats += static_cast<std::size_t>(q) * kQuadFloats;
    o += kGroupHeaderBytes + static_cast<std::size_t>(q) * kQuadBytes;
  }
  Table t;
  t.groups = new (std::nothrow) Group[groups == 0 ? 1 : groups];
  t.floats = new (std::nothrow) float[floats == 0 ? 1 : floats];
  if (t.groups == nullptr || t.floats == nullptr) {
    FreeTable(&t);
    return LoadResult::kNoMemory;
  }
  std::size_t at = kFileHeaderBytes, fi = 0;
  for (std::uint32_t g = 0; g < groups; ++g) {
    Group d;
    std::uint32_t q, fl;
    std::memcpy(&d.level, b + at, 8);
    std::memcpy(&d.mat, b + at + 8, 8);
    std::memcpy(&q, b + at + 16, 4);
    std::memcpy(&fl, b + at + 20, 4);
    at += kGroupHeaderBytes;
    for (std::size_t k = 0; k < static_cast<std::size_t>(q) * kQuadFloats; ++k) {
      if (!FiniteBits(b + at + 4 * k)) {
        FreeTable(&t);
        return LoadResult::kNotFinite;
      }
    }
    float* dst = t.floats + fi;
    std::memcpy(dst, b + at, static_cast<std::size_t>(q) * kQuadBytes);
    at += static_cast<std::size_t>(q) * kQuadBytes;
    for (std::uint32_t k = 0; k < q; ++k) FlipQuadUv(dst + kQuadFloats * k, flip_u, flip_v);
    d.quads = q;
    d.n = q > kMaxNodeQuads ? kMaxNodeQuads : q;
    d.blend = BlendOf(fl);
    d.v = dst;
    fi += static_cast<std::size_t>(q) * kQuadFloats;
    if (q > kMaxNodeQuads) ++t.truncated_groups;
    t.total_quads += q;
    t.groups[t.count++] = d;
  }
  t.trailing_bytes = len - at;
  *out = t;
  return LoadResult::kOk;
}

inline bool IsCombatLevel(std::uint64_t l) {
  for (std::uint64_t c : kCombatLevels)
    if (c == l) return true;
  return false;
}

// A group is drawn in the game space of its own level; the war room's (the drop ships') also in a combat arena's, because it is not
// known that the ships load as a game space of their own.
inline bool Applies(std::uint64_t group_level, std::uint64_t level) {
  return group_level == level || (group_level == kLevelWarRoom && IsCombatLevel(level));
}

// Both windings of every quad (the donor material is single-sided and the decal's facing is not known, so exactly one of the two
// survives culling from any side): 12 indices a quad, `idx` holds 12 x kMaxNodeQuads.
inline void FillIndices(std::uint16_t* idx) {
  for (std::uint32_t k = 0; k < kMaxNodeQuads; ++k) {
    const std::uint16_t b = static_cast<std::uint16_t>(4 * k);
    std::uint16_t* t = idx + 12 * k;
    t[0] = b; t[1] = static_cast<std::uint16_t>(b + 1); t[2] = static_cast<std::uint16_t>(b + 2);
    t[3] = b; t[4] = static_cast<std::uint16_t>(b + 2); t[5] = static_cast<std::uint16_t>(b + 3);
    t[6] = b; t[7] = static_cast<std::uint16_t>(b + 2); t[8] = static_cast<std::uint16_t>(b + 1);
    t[9] = b; t[10] = static_cast<std::uint16_t>(b + 3); t[11] = static_cast<std::uint16_t>(b + 2);
  }
}

// The vertices of one group: position, uv, both colours white (the pixel shader ignores them), normal and tangent zero. 4 x g.n.
inline void FillVertices(const Group& g, Vtx* out) {
  for (std::uint32_t k = 0; k < 4 * g.n; ++k) {
    const float* c = g.v + 5 * k;
    out[k] = Vtx{};
    out[k].p[0] = c[0];
    out[k].p[1] = c[1];
    out[k].p[2] = c[2];
    out[k].uv[0] = c[3];
    out[k].uv[1] = c[4];
    out[k].c0 = out[k].c1 = 0xffffffffu;
  }
}

// ---- the state machine --------------------------------------------------------------------------------------------------

struct Ops {
  void* user = nullptr;
  // The level of the game space of bullet system `sys`; false while it has none.
  bool (*level_of)(void* user, std::uintptr_t sys, std::uint64_t* level) = nullptr;
  // A node of `g` on `sys`'s allocator, 0 on failure; *rc is the engine's answer (0 = made), *tried the node's address when one was
  // allocated (0 otherwise), *blended whether the group's blend was written into its render state.
  std::uintptr_t (*create)(void* user, std::uintptr_t sys, const Group& g, int* rc, std::uintptr_t* tried, bool* blended) = nullptr;
  void (*destroy)(void* user, std::uintptr_t sys, std::uintptr_t node) = nullptr;
  // Writes the group's vertices into the node, sets its drawn index count and shows it; false when the vertices cannot be mapped.
  bool (*draw)(void* user, std::uintptr_t node, const Group& g) = nullptr;
  void (*hide)(void* user, std::uintptr_t node) = nullptr;
};

struct Node {
  std::uintptr_t node = 0, sys = 0;
  int group = 0;
};

struct SysRec {
  std::uintptr_t sys = 0;
  std::uint64_t level = 0;
  int tries = 0;
  std::int64_t next = 0;
  bool done = false;
};

struct Counters {
  std::uint64_t systems_seen = 0, nodes_made = 0, nodes_failed = 0, nodes_destroyed = 0, level_changes = 0, ticks = 0, drawn_quads = 0;
};

inline const char* Hex(char* buf, std::size_t n, std::uint64_t v) {
  std::snprintf(buf, n, "0x%llx", static_cast<unsigned long long>(v));
  return buf;
}
inline const char* Dec(char* buf, std::size_t n, long long v) {
  std::snprintf(buf, n, "%lld", v);
  return buf;
}

struct Host {
  const Table* table = nullptr;
  Ops ops;
  Node nodes[kMaxNodes];
  int node_n = 0;
  SysRec systems[kMaxSystems];
  int sys_n = 0;
  int war_room_systems = 0;     // game spaces whose level is the war room that are loaded: each draws its own ships
  bool told_full = false;
  std::uint32_t frames = 0, win_quads = 0;
  std::int64_t win_at = 0;
  Counters counters;

  int DropNodes(std::uintptr_t sys) {
    int kept = 0, gone = 0;
    for (int i = 0; i < node_n; ++i) {
      if (nodes[i].sys == sys) {
        ops.destroy(ops.user, sys, nodes[i].node);
        ++gone;
      } else {
        nodes[kept++] = nodes[i];
      }
    }
    node_n = kept;
    counters.nodes_destroyed += static_cast<std::uint64_t>(gone);
    return gone;
  }

  SysRec* Find(std::uintptr_t sys) {
    for (int i = 0; i < sys_n; ++i)
      if (systems[i].sys == sys) return &systems[i];
    return nullptr;
  }

  void Forget(SysRec* s) {
    if (s->level == kLevelWarRoom) --war_room_systems;
    *s = systems[--sys_n];
  }

  // The update of bullet system `sys` (the game-space update thread of that system); the caller holds the module's lock.
  void Tick(std::uintptr_t sys, std::int64_t now_ms) {
    if (table == nullptr || table->count == 0) return;
    std::uint64_t level = 0;
    if (!ops.level_of(ops.user, sys, &level)) return;
    ++counters.ticks;
    SysRec* ds = Find(sys);
    if (ds != nullptr && ds->level != level) {  // the system outlived a level change: its nodes go, and it is gated again
      const int gone = DropNodes(sys);
      ++counters.level_changes;
      char s[24], a[24], b[24], n[24];
      const NevrLogField f[] = {{"system", Hex(s, sizeof s, sys)}, {"level_from", Hex(a, sizeof a, ds->level)},
                                {"level_to", Hex(b, sizeof b, level)}, {"nodes_destroyed", Dec(n, sizeof n, gone)}};
      Log(NEVR_LOG_INFO, "decals: level changed under a bullet system, its nodes are destroyed", f, 4);
      Forget(ds);
      ds = nullptr;
    }
    if (ds == nullptr) {
      if (sys_n >= kMaxSystems) {
        if (!told_full) {
          told_full = true;
          char s[24], m[24];
          const NevrLogField f[] = {{"system", Hex(s, sizeof s, sys)}, {"max", Dec(m, sizeof m, kMaxSystems)}};
          Log(NEVR_LOG_WARNING, "decals: more bullet systems than the table keeps, this one draws no decals", f, 2);
        }
        return;
      }
      ds = &systems[sys_n++];
      *ds = SysRec();
      ds->sys = sys;
      ds->level = level;
      ++counters.systems_seen;
      if (level == kLevelWarRoom) ++war_room_systems;
      int ng = 0;
      std::uint32_t nq = 0;
      for (std::uint32_t i = 0; i < table->count; ++i)
        if (Applies(table->groups[i].level, level)) {
          ++ng;
          nq += table->groups[i].n;
        }
      char s[24], l[24], g[24], q[24];
      const NevrLogField f[] = {{"system", Hex(s, sizeof s, sys)}, {"level", Hex(l, sizeof l, level)},
                                {"groups", Dec(g, sizeof g, ng)}, {"quads", Dec(q, sizeof q, nq)}};
      Log(NEVR_LOG_INFO, "decals: game space", f, 4);
    }
    if (!ds->done && ds->tries < kMaxTries && now_ms >= ds->next) {
      ++ds->tries;
      ds->next = now_ms + kRetryMs;
      bool all = true;
      for (int gi = 0; gi < static_cast<int>(table->count); ++gi) {
        const Group& g = table->groups[gi];
        if (!Applies(g.level, level)) continue;
        bool have = false;
        for (int i = 0; i < node_n; ++i)
          if (nodes[i].sys == sys && nodes[i].group == gi) have = true;
        if (have) continue;
        if (node_n >= kMaxNodes) {
          all = false;
          break;
        }
        int rc = -1;
        std::uintptr_t tried = 0;
        bool blended = false;
        const std::uintptr_t node = ops.create(ops.user, sys, g, &rc, &tried, &blended);
        if (node != 0) {
          nodes[node_n].node = node;
          nodes[node_n].sys = sys;
          nodes[node_n].group = gi;
          ++node_n;
          ++counters.nodes_made;
        } else {
          ++counters.nodes_failed;
          all = false;
        }
      }
      ds->done = all || ds->tries >= kMaxTries;
    }
    for (int i = 0; i < node_n; ++i) {
      const Node& dn = nodes[i];
      if (dn.sys != sys) continue;
      const Group& g = table->groups[dn.group];
      if (g.level == kLevelWarRoom && level != kLevelWarRoom && war_room_systems > 0) {  // the ships draw their own
        ops.hide(ops.user, dn.node);
        continue;
      }
      if (ops.draw(ops.user, dn.node, g)) {
        win_quads += g.n;
        counters.drawn_quads += g.n;
      }
    }
    ++frames;
    if (now_ms - win_at >= kWindowMs) {
      if (win_at != 0) {
        char fr[24], nn[24], q[24];
        const NevrLogField f[] = {{"frames", Dec(fr, sizeof fr, frames)}, {"nodes", Dec(nn, sizeof nn, node_n)},
                                  {"quads", Dec(q, sizeof q, win_quads)}};
        Log(NEVR_LOG_INFO, "decals: window", f, 3);
      }
      win_at = now_ms;
      frames = win_quads = 0;
    }
  }

  // Bullet system `sys` is shutting down: its nodes go first, then the record of it.
  void Shutdown(std::uintptr_t sys) {
    if (table == nullptr) return;
    const int gone = DropNodes(sys);
    for (int i = 0; i < sys_n; ++i) {
      if (systems[i].sys == sys) {
        Forget(&systems[i]);
        break;
      }
    }
    if (gone != 0) {
      char s[24], n[24];
      const NevrLogField f[] = {{"system", Hex(s, sizeof s, sys)}, {"nodes_destroyed", Dec(n, sizeof n, gone)}};
      Log(NEVR_LOG_INFO, "decals: bullet system shutdown, its nodes are destroyed", f, 2);
    }
  }
};

}  // namespace decals
}  // namespace q15t
