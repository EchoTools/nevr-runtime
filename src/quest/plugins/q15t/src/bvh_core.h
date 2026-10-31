// Ray queries against the game's own static BVH (CBVHResource / SBVHTreeData), for bvh.cpp and the host tests. Pure C++:
// no game memory is read here, no Android, no allocation. (Port of the source build's bvh_core; the differences are
// listed in docs/quest/q15t/modules/bvh.md section 6.)
//
// Format: Embree 1.0 BVH2<Triangle4> as the game ships it. Every offset below is checked against the disassembly: libr15
// (CMemBlock / SBVHTreeData / CStaticRaycastCS) and echovr.exe (the Embree traversal FUN_14135f9a0 and the tree
// setup FUN_14070f6e0); see docs/quest/q15t/modules/bvh.md section 3.
//   SBVHTreeData (0x50 bytes): CMemBlock triangles {ptr +0x00, size +0x08}, CMemBlock nodes {ptr +0x20, size +0x28},
//     u32 root reference +0x40. Sizes are in bytes.
//   Triangle4 (0xe0 bytes): v0, e1, e2, Ng as x[4] y[4] z[4] each, then id0[4], id1[4].
//     v1 = v0 - e1, v2 = v0 + e2, Ng = e1 x e2 (not normalised). An empty lane has id0 = -1.
//   Node (0x40 bytes): x/y/z[4] = {left.lo, right.lo, left.hi, right.hi}, child[2], pad[2].
//     child >= 0: inner node at byte offset child * 8 (index child >> 3). child < 0: leaf, first Triangle4 index
//     (child & 0x7fffffff) >> 5, count child & 31 (0 = empty). The root is the reference in the header (0 = node 0).
#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

namespace q15t {
namespace bvh {

struct Node {
  float x[4], y[4], z[4];
  int32_t child[2];
  int32_t pad[2];
};
struct Tri4 {
  float v0[3][4], e1[3][4], e2[3][4], ng[3][4];
  int32_t id0[4], id1[4];
};
static_assert(sizeof(Node) == 0x40, "Node");
static_assert(sizeof(Tri4) == 0xe0, "Tri4");

constexpr uint32_t kHeaderSize = 0x50;       // SBVHTreeData
constexpr uint64_t kMaxTri4 = 0x3ffffff;     // a leaf's first-record field is 26 bits
constexpr uint64_t kMaxNodes = 0xfffffff;    // an inner reference is child >> 3 in 31 bits
constexpr int kStackSize = 128;              // traversal stack entries (a deeper tree answers "miss")

struct Tree {
  const Tri4* tris;
  uint32_t ntris;  // Triangle4 records
  const Node* nodes;
  uint32_t nnodes;
  int32_t root;  // header +0x40: 0 = node 0
};

// A parsed SBVHTreeData header: the two blocks' base addresses and byte sizes.
struct Blocks {
  uint64_t tri_ptr, tri_bytes, node_ptr, node_bytes;
  uint32_t root;
};

// `hdr`: the 0x50 header bytes (copied from game memory by the caller); `hdr_addr`: where they came from. A block whose
// pointer is 0 follows the header in the resource (triangles, then nodes). False (and why) for a resource without
// geometry or one this code does not understand; the caller then treats every ray as a miss, as the stock Quest build does.
inline bool Parse(const uint8_t* hdr, uint64_t hdr_addr, Tree* tree, Blocks* blocks, const char** why) {
  uint64_t p0, s0, p1, s1;
  uint32_t root;
  memcpy(&p0, hdr + 0x00, 8);
  memcpy(&s0, hdr + 0x08, 8);
  memcpy(&p1, hdr + 0x20, 8);
  memcpy(&s1, hdr + 0x28, 8);
  memcpy(&root, hdr + 0x40, 4);
  if (s0 == 0 || s1 == 0) {
    *why = "empty";
    return false;
  }
  if (s0 % sizeof(Tri4) || s1 % sizeof(Node) || s0 / sizeof(Tri4) > kMaxTri4 || s1 / sizeof(Node) > kMaxNodes) {
    *why = "block sizes";
    return false;
  }
  const uint64_t tp = p0 ? p0 : hdr_addr + kHeaderSize;
  const uint64_t np = p1 ? p1 : hdr_addr + kHeaderSize + s0;
  if (tp + s0 < tp || np + s1 < np || tp < 0x10000 || np < 0x10000) {  // wraps, or a null-page pointer
    *why = "block address";
    return false;
  }
  blocks->tri_ptr = tp;
  blocks->tri_bytes = s0;
  blocks->node_ptr = np;
  blocks->node_bytes = s1;
  blocks->root = root;
  tree->tris = reinterpret_cast<const Tri4*>(static_cast<uintptr_t>(tp));
  tree->nodes = reinterpret_cast<const Node*>(static_cast<uintptr_t>(np));
  tree->ntris = static_cast<uint32_t>(s0 / sizeof(Tri4));
  tree->nnodes = static_cast<uint32_t>(s1 / sizeof(Node));
  tree->root = static_cast<int32_t>(root);
  *why = "";
  return true;
}

struct Hit {
  int32_t id0 = -1, id1 = -1;
  float u = 0, v = 0, t = 0;
  float n[3] = {0, 0, 0};  // the hit triangle's Ng, normalised (not flipped toward the ray)
};

// A ray o + t d for t in (tmin, tmax). `ok` is false for a non-finite origin, direction or range, or a zero direction: such a
// ray hits nothing (and would otherwise visit the whole tree).
struct Ray {
  float o[3], d[3], rd[3];
  float tmin, tmax;
  bool ok;
  Ray(const float* org, const float* dir, float tmin_, float tmax_) : tmin(tmin_), tmax(tmax_), ok(true) {
    float len2 = 0;
    for (int k = 0; k < 3; ++k) {
      o[k] = org[k];
      d[k] = dir[k];
      ok = ok && isfinite(org[k]) && isfinite(dir[k]);
      len2 += dir[k] * dir[k];
      rd[k] = 1.0f / dir[k];  // 0 gives +-inf: the slab test below is exact for it (an origin on the slab's plane compares as NaN = inside)
    }
    ok = ok && len2 > 0.0f && isfinite(len2) && tmax_ >= tmin_;  // (a NaN range fails the last compare)
  }
};

// entry distance into child c of n, or -1 when the ray misses it within [tmin, tmax]
inline float BoxEntry(const Node& n, int c, const Ray& r, float tmax) {
  const float* b[3] = {n.x, n.y, n.z};
  float t0 = r.tmin, t1 = tmax;
  for (int k = 0; k < 3; ++k) {
    float a = (b[k][c] - r.o[k]) * r.rd[k], z = (b[k][c + 2] - r.o[k]) * r.rd[k];
    if (a > z) {
      const float s = a;
      a = z;
      z = s;
    }
    if (a > t0) t0 = a;
    if (z < t1) t1 = z;
  }
  return t0 <= t1 ? t0 : -1.0f;
}

// Moeller-Trumbore on lane l of q (two-sided); true (and t, u, v) for a hit in (tmin, tmax)
inline bool TriHit(const Tri4& q, int l, const Ray& r, float tmax, float& t, float& u, float& v) {
  const float a[3] = {-q.e1[0][l], -q.e1[1][l], -q.e1[2][l]};  // v1 - v0
  const float b[3] = {q.e2[0][l], q.e2[1][l], q.e2[2][l]};     // v2 - v0
  const float p[3] = {r.d[1] * b[2] - r.d[2] * b[1], r.d[2] * b[0] - r.d[0] * b[2], r.d[0] * b[1] - r.d[1] * b[0]};
  const float det = a[0] * p[0] + a[1] * p[1] + a[2] * p[2];
  if (det == 0.0f || !isfinite(det)) return false;  // parallel, or overflowed (IEEE would also end in a miss; this says so)
  const float inv = 1.0f / det;
  const float s[3] = {r.o[0] - q.v0[0][l], r.o[1] - q.v0[1][l], r.o[2] - q.v0[2][l]};
  u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
  if (u < 0.0f) return false;  // u > 1 is caught by u + v > 1 below (v >= 0)
  const float c[3] = {s[1] * a[2] - s[2] * a[1], s[2] * a[0] - s[0] * a[2], s[0] * a[1] - s[1] * a[0]};
  v = (r.d[0] * c[0] + r.d[1] * c[1] + r.d[2] * c[2]) * inv;
  if (v < 0.0f || u + v > 1.0f) return false;
  t = (b[0] * c[0] + b[1] * c[1] + b[2] * c[2]) * inv;
  return t > r.tmin && t < tmax;
}

// Nearest hit (any = false) or the first hit found (any = true) in (tmin, tmax).
// Returns false on a miss, a ray that is not ok, or a tree that is malformed along the way: an index outside the tree, a
// stack deeper than kStackSize, or more node visits than the tree has nodes (a cycle). It never reads outside the tree.
// `pops_out` (optional): how many stack entries were processed, for tests.
inline bool Trace(const Tree& tree, const Ray& r, bool any, Hit* out, uint32_t* pops_out = nullptr) {
  if (pops_out) *pops_out = 0;
  if (!r.ok) return false;
  uint32_t stack[kStackSize];
  float entry[kStackSize];
  int sp = 0;
  stack[sp] = static_cast<uint32_t>(tree.root);
  entry[sp] = r.tmin;
  ++sp;
  float best = r.tmax;
  const Tri4* bq = nullptr;
  int bl = 0;
  float bu = 0, bv = 0;
  uint64_t pops = 0;
  const uint64_t max_pops = 2ull * tree.nnodes + 2;  // a tree pops each node once and each leaf once
  while (sp) {
    if (++pops > max_pops) return false;
    if (pops_out) *pops_out = static_cast<uint32_t>(pops);
    --sp;
    if (entry[sp] > best) continue;
    int32_t id = static_cast<int32_t>(stack[sp]);
    // a child id goes on the stack as is: >= 0 inner (index id >> 3), < 0 leaf
    if (id >= 0) {
      const uint32_t ni = static_cast<uint32_t>(id) >> 3;
      if (ni >= tree.nnodes) return false;
      const Node& n = tree.nodes[ni];
      const float e0 = n.child[0] == INT32_MIN ? -1.0f : BoxEntry(n, 0, r, best);
      const float e1 = n.child[1] == INT32_MIN ? -1.0f : BoxEntry(n, 1, r, best);
      if (sp + 2 > kStackSize) return false;
      // push the farther child first so the nearer one is popped first
      const int nearc = (e0 >= 0 && (e1 < 0 || e0 <= e1)) ? 0 : 1, farc = 1 - nearc;
      const float en = nearc ? e1 : e0, ef = nearc ? e0 : e1;
      if (ef >= 0) {
        stack[sp] = static_cast<uint32_t>(n.child[farc]);
        entry[sp] = ef;
        ++sp;
      }
      if (en >= 0) {
        stack[sp] = static_cast<uint32_t>(n.child[nearc]);
        entry[sp] = en;
        ++sp;
      }
      continue;
    }
    const uint32_t li = (static_cast<uint32_t>(id) & 0x7fffffffu) >> 5, cnt = static_cast<uint32_t>(id) & 31u;
    if (static_cast<uint64_t>(li) + cnt > tree.ntris) return false;
    for (uint32_t i = 0; i < cnt; ++i) {
      const Tri4& q = tree.tris[li + i];
      for (int l = 0; l < 4; ++l) {
        if (q.id0[l] == -1) continue;
        float t, u, v;
        if (!TriHit(q, l, r, best, t, u, v)) continue;
        best = t;
        bq = &q;
        bl = l;
        bu = u;
        bv = v;
        if (any) goto done;
      }
    }
  }
done:
  if (!bq) return false;
  if (out) {
    out->id0 = bq->id0[bl];
    out->id1 = bq->id1[bl];
    out->u = bu;
    out->v = bv;
    out->t = best;
    const float g[3] = {bq->ng[0][bl], bq->ng[1][bl], bq->ng[2][bl]};
    const float len = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    // the stored Ng = e1 x e2 faces the play space (a floor under a player has Ng.y > 0); the levels' outer shells, seen
    // from outside, are back-facing
    for (int k = 0; k < 3; ++k) out->n[k] = len > 0 ? g[k] / len : 0.0f;
  }
  return true;
}

}  // namespace bvh
}  // namespace q15t
