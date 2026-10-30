// Test-only: builds Embree BVH2<Triangle4> trees (the game's static BVH format, src/quest/plugins/q15t/src/bvh_core.h) from synthetic triangles, and
// lays them out as a CBVHResource's SBVHTreeData. Nothing here is game-derived: every mesh is made by the tests.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "bvh_core.h"

namespace bt {

using q15t::bvh::Node;
using q15t::bvh::Tri4;

struct TriIn {
  float a[3], b[3], c[3];
  int id0, id1;
};

struct Built {
  std::vector<Tri4> tris;
  std::vector<Node> nodes;
  uint32_t root = 0;  // the header's root reference
};

inline void Cross(const float* x, const float* y, float* o) {
  o[0] = x[1] * y[2] - x[2] * y[1];
  o[1] = x[2] * y[0] - x[0] * y[2];
  o[2] = x[0] * y[1] - x[1] * y[0];
}

// lane l of record q from a triangle: v0 = a, e1 = v0 - v1 = a - b, e2 = v2 - v0 = c - a, Ng = e1 x e2
inline void SetLane(Tri4& q, int l, const TriIn& t) {
  float e1[3], e2[3], ng[3];
  for (int k = 0; k < 3; ++k) {
    e1[k] = t.a[k] - t.b[k];
    e2[k] = t.c[k] - t.a[k];
  }
  Cross(e1, e2, ng);
  for (int k = 0; k < 3; ++k) {
    q.v0[k][l] = t.a[k];
    q.e1[k][l] = e1[k];
    q.e2[k][l] = e2[k];
    q.ng[k][l] = ng[k];
  }
  q.id0[l] = t.id0;
  q.id1[l] = t.id1;
}

inline Tri4 EmptyRecord() {
  Tri4 q;
  std::memset(&q, 0, sizeof q);
  for (int l = 0; l < 4; ++l) q.id0[l] = -1;
  return q;
}

class Builder {
 public:
  Builder(const std::vector<TriIn>& in, int leaf_tris) : in_(in), leaf_(leaf_tris) {}

  // A tree whose root is an inner node placed after `pad` unused nodes (root reference = index << 3).
  Built Build(int pad = 0) {
    out_ = Built();
    for (int i = 0; i < pad; ++i) out_.nodes.push_back(DummyNode());
    std::vector<int> idx;
    for (size_t i = 0; i < in_.size(); ++i) idx.push_back(static_cast<int>(i));
    const int32_t ref = Make(idx, true);
    out_.root = static_cast<uint32_t>(ref);
    return out_;
  }

  // A tree whose root is a leaf (at most 31 records of 4 triangles); one unused node keeps the node block non-empty.
  Built BuildLeafRoot() {
    out_ = Built();
    out_.nodes.push_back(DummyNode());
    std::vector<int> idx;
    for (size_t i = 0; i < in_.size(); ++i) idx.push_back(static_cast<int>(i));
    out_.root = static_cast<uint32_t>(Leaf(idx));
    return out_;
  }

  static Node DummyNode() {
    Node n;
    std::memset(&n, 0, sizeof n);
    n.child[0] = n.child[1] = INT32_MIN;
    return n;
  }

 private:
  int32_t Leaf(const std::vector<int>& idx) {
    const uint32_t first = static_cast<uint32_t>(out_.tris.size());
    uint32_t cnt = 0;
    for (size_t i = 0; i < idx.size(); i += 4) {
      Tri4 q = EmptyRecord();
      for (size_t l = 0; l < 4 && i + l < idx.size(); ++l) SetLane(q, static_cast<int>(l), in_[idx[i + l]]);
      out_.tris.push_back(q);
      ++cnt;
    }
    return static_cast<int32_t>(0x80000000u | (first << 5) | cnt);
  }

  void Bounds(const std::vector<int>& idx, float* lo, float* hi) const {
    for (int k = 0; k < 3; ++k) {
      lo[k] = 1e30f;
      hi[k] = -1e30f;
    }
    for (int i : idx) {
      const float* p[3] = {in_[i].a, in_[i].b, in_[i].c};
      for (int v = 0; v < 3; ++v)
        for (int k = 0; k < 3; ++k) {
          if (p[v][k] < lo[k]) lo[k] = p[v][k];
          if (p[v][k] > hi[k]) hi[k] = p[v][k];
        }
    }
  }

  // returns the child reference for the set: a leaf, or the inner node's byte offset (index << 3)
  int32_t Make(std::vector<int> idx, bool force_inner) {
    if (!force_inner && static_cast<int>(idx.size()) <= leaf_) return Leaf(idx);
    const uint32_t ni = static_cast<uint32_t>(out_.nodes.size());
    out_.nodes.push_back(DummyNode());
    std::vector<int> left, right;
    if (idx.size() <= 1) {
      left = idx;
    } else {
      float lo[3], hi[3];
      Bounds(idx, lo, hi);
      int axis = 0;
      for (int k = 1; k < 3; ++k)
        if (hi[k] - lo[k] > hi[axis] - lo[axis]) axis = k;
      // sort by centroid along the widest axis, split in the middle
      for (size_t i = 1; i < idx.size(); ++i)
        for (size_t j = i; j > 0 && Centroid(idx[j], axis) < Centroid(idx[j - 1], axis); --j) std::swap(idx[j], idx[j - 1]);
      const size_t mid = idx.size() / 2;
      left.assign(idx.begin(), idx.begin() + mid);
      right.assign(idx.begin() + mid, idx.end());
    }
    const int32_t c0 = left.empty() ? INT32_MIN : Make(left, false);
    const int32_t c1 = right.empty() ? INT32_MIN : Make(right, false);
    Node n = DummyNode();
    n.child[0] = c0;
    n.child[1] = c1;
    float lo[3], hi[3];
    if (!left.empty()) {
      Bounds(left, lo, hi);
      for (int k = 0; k < 3; ++k) {
        (k == 0 ? n.x : k == 1 ? n.y : n.z)[0] = lo[k];
        (k == 0 ? n.x : k == 1 ? n.y : n.z)[2] = hi[k];
      }
    }
    if (!right.empty()) {
      Bounds(right, lo, hi);
      for (int k = 0; k < 3; ++k) {
        (k == 0 ? n.x : k == 1 ? n.y : n.z)[1] = lo[k];
        (k == 0 ? n.x : k == 1 ? n.y : n.z)[3] = hi[k];
      }
    }
    out_.nodes[ni] = n;
    return static_cast<int32_t>(ni << 3);
  }

  float Centroid(int i, int axis) const { return (in_[i].a[axis] + in_[i].b[axis] + in_[i].c[axis]) / 3.0f; }

  const std::vector<TriIn>& in_;
  int leaf_;
  Built out_;
};

// An SBVHTreeData followed by its two blocks, as the resource file lays them out. `fixed_ptrs`: the header's two pointers are
// set to the blocks (as after CBVHResource::Attach); else they are 0 (the file's own form).
struct Resource {
  std::vector<uint8_t> bytes;
  const uint8_t* header() const { return bytes.data(); }
};
inline Resource MakeResource(const Built& b, bool fixed_ptrs) {
  Resource r;
  const uint64_t s0 = b.tris.size() * sizeof(Tri4), s1 = b.nodes.size() * sizeof(Node);
  r.bytes.assign(q15t::bvh::kHeaderSize + s0 + s1, 0);
  uint8_t* d = r.bytes.data();
  std::memcpy(d + q15t::bvh::kHeaderSize, b.tris.data(), s0);
  std::memcpy(d + q15t::bvh::kHeaderSize + s0, b.nodes.data(), s1);
  const uint64_t p0 = fixed_ptrs ? reinterpret_cast<uintptr_t>(d + q15t::bvh::kHeaderSize) : 0;
  const uint64_t p1 = fixed_ptrs ? reinterpret_cast<uintptr_t>(d + q15t::bvh::kHeaderSize + s0) : 0;
  std::memcpy(d + 0x00, &p0, 8);
  std::memcpy(d + 0x08, &s0, 8);
  std::memcpy(d + 0x20, &p1, 8);
  std::memcpy(d + 0x28, &s1, 8);
  std::memcpy(d + 0x40, &b.root, 4);
  return r;
}

// ---- mesh helpers ----

// A quad a,b,c,d (in order) as two triangles, wound so that Ng (= e1 x e2) faces `toward`.
inline void AddQuad(std::vector<TriIn>& out, const float* a, const float* b, const float* c, const float* d, const float* toward,
                    int id0, int& id1) {
  const float* q[2][3] = {{a, b, c}, {a, c, d}};
  for (auto& t : q) {
    TriIn tri;
    for (int k = 0; k < 3; ++k) {
      tri.a[k] = t[0][k];
      tri.b[k] = t[1][k];
      tri.c[k] = t[2][k];
    }
    tri.id0 = id0;
    tri.id1 = id1++;
    float e1[3], e2[3], ng[3];
    for (int k = 0; k < 3; ++k) {
      e1[k] = tri.a[k] - tri.b[k];
      e2[k] = tri.c[k] - tri.a[k];
    }
    Cross(e1, e2, ng);
    float cen[3], dot = 0;
    for (int k = 0; k < 3; ++k) {
      cen[k] = (tri.a[k] + tri.b[k] + tri.c[k]) / 3.0f;
      dot += ng[k] * (toward[k] - cen[k]);
    }
    if (dot < 0) {  // flip the winding
      for (int k = 0; k < 3; ++k) std::swap(tri.b[k], tri.c[k]);
    }
    out.push_back(tri);
  }
}

// An axis-aligned box [lo, hi] as 12 triangles; face ids id0_base .. id0_base+5 (-x +x -y +y -z +z); Ng faces inward (a room)
// or outward (a solid).
inline void AddBox(std::vector<TriIn>& out, const float* lo, const float* hi, int id0_base, bool inward) {
  const float cen[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
  int id1 = 0;
  for (int face = 0; face < 6; ++face) {
    const int axis = face / 2, hiside = face % 2;
    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
    float p[4][3];
    for (int i = 0; i < 4; ++i) {
      p[i][axis] = hiside ? hi[axis] : lo[axis];
      p[i][u] = (i == 0 || i == 3) ? lo[u] : hi[u];
      p[i][v] = (i < 2) ? lo[v] : hi[v];
    }
    // toward: the box centre for a room; a far point on the face's outside for a solid
    float toward[3] = {cen[0], cen[1], cen[2]};
    if (!inward) toward[axis] = (hiside ? hi[axis] : lo[axis]) + (hiside ? 1000.0f : -1000.0f);
    AddQuad(out, p[0], p[1], p[2], p[3], toward, id0_base + face, id1);
  }
}

struct Lcg {
  uint64_t s;
  explicit Lcg(uint64_t seed) : s(seed) {}
  float U() {  // [0, 1)
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<float>((s >> 40) & 0xffffff) / 16777216.0f;
  }
};

}  // namespace bt
