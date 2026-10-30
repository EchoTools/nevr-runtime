// Host test of the pure BVH code (src/quest/plugins/q15t/src/bvh_core.h): synthetic meshes in the game's Embree BVH2<Triangle4> format, built by
// src/quest/tests/q15t/bvh_build.h. No game-derived bytes. Ray hits, misses and occlusion against known geometry, a random soup against two
// brute-force references, degenerate input, malformed trees, and the node / stack / size limits.
#include <math.h>

#include <cstdio>
#include <vector>

#include "bvh_build.h"
#include "bvh_core.h"

using namespace q15t::bvh;

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

static Tree TreeOf(const bt::Built& b) {
  Tree t;
  t.tris = b.tris.data();
  t.ntris = static_cast<uint32_t>(b.tris.size());
  t.nodes = b.nodes.data();
  t.nnodes = static_cast<uint32_t>(b.nodes.size());
  t.root = static_cast<int32_t>(b.root);
  return t;
}

static bool Near(float a, float b, float eps = 1e-4f) { return fabsf(a - b) <= eps * fmaxf(1.0f, fabsf(b)); }

static bool Cast(const Tree& t, const float* o, const float* d, float tmin, float tmax, bool any, Hit* h) {
  return Trace(t, Ray(o, d, tmin, tmax), any, h);
}
static bool Seg(const Tree& t, const float* a, const float* b) {  // Occluded: the segment a..b is t in (0, 1)
  const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
  return Trace(t, Ray(a, d, 0.0f, 1.0f), true, nullptr);
}

static bt::TriIn Tri(const float* a, const float* b, const float* c, int id0, int id1) {
  bt::TriIn t;
  for (int k = 0; k < 3; ++k) {
    t.a[k] = a[k];
    t.b[k] = b[k];
    t.c[k] = c[k];
  }
  t.id0 = id0;
  t.id1 = id1;
  return t;
}

// ---------------------------------------------------------------------------------------------------------------------
static void TestParse() {
  const char* why = nullptr;
  Tree tree;
  Blocks bl;
  uint8_t h[kHeaderSize];
  auto put = [&](uint32_t off, uint64_t v) { std::memcpy(h + off, &v, 8); };
  auto fresh = [&]() { std::memset(h, 0, sizeof h); };

  fresh();  // an all-zero SBVHTreeData: no geometry
  CHECK(!Parse(h, 0x100000, &tree, &bl, &why) && !std::strcmp(why, "empty"), "zero header: %s", why);
  fresh();
  put(0x08, 0xe0);  // triangles but no nodes
  CHECK(!Parse(h, 0x100000, &tree, &bl, &why) && !std::strcmp(why, "empty"), "no nodes: %s", why);
  fresh();
  put(0x28, 0x40);  // nodes but no triangles
  CHECK(!Parse(h, 0x100000, &tree, &bl, &why) && !std::strcmp(why, "empty"), "no triangles: %s", why);

  fresh();
  put(0x08, 0xe1);
  put(0x28, 0x40);
  CHECK(!Parse(h, 0x100000, &tree, &bl, &why) && !std::strcmp(why, "block sizes"), "tri size not a multiple: %s", why);
  fresh();
  put(0x08, 0xe0);
  put(0x28, 0x41);
  CHECK(!Parse(h, 0x100000, &tree, &bl, &why) && !std::strcmp(why, "block sizes"), "node size not a multiple: %s", why);

  // the limits: kMaxTri4 records and kMaxNodes nodes parse, one more does not
  fresh();
  put(0x00, 0x10000000);
  put(0x08, kMaxTri4 * sizeof(Tri4));
  put(0x20, 0x20000000);
  put(0x28, 0x40);
  CHECK(Parse(h, 0x100000, &tree, &bl, &why) && tree.ntris == kMaxTri4, "max triangle count refused: %s", why);
  put(0x08, (kMaxTri4 + 1) * sizeof(Tri4));
  CHECK(!Parse(h, 0x100000, &tree, &bl, &why) && !std::strcmp(why, "block sizes"), "triangle count over the limit: %s", why);
  fresh();
  put(0x00, 0x10000000);
  put(0x08, 0xe0);
  put(0x20, 0x20000000);
  put(0x28, kMaxNodes * sizeof(Node));
  CHECK(Parse(h, 0x100000, &tree, &bl, &why) && tree.nnodes == kMaxNodes, "max node count refused: %s", why);
  put(0x28, (kMaxNodes + 1) * sizeof(Node));
  CHECK(!Parse(h, 0x100000, &tree, &bl, &why) && !std::strcmp(why, "block sizes"), "node count over the limit: %s", why);

  // zero pointers: the blocks follow the header (triangles, then nodes); non-zero pointers are used as they are
  fresh();
  put(0x08, 2 * sizeof(Tri4));
  put(0x28, 3 * sizeof(Node));
  CHECK(Parse(h, 0x100000, &tree, &bl, &why), "file-form header refused: %s", why);
  CHECK(bl.tri_ptr == 0x100050 && bl.node_ptr == 0x100050 + 2 * sizeof(Tri4), "block addresses %llx %llx",
        (unsigned long long)bl.tri_ptr, (unsigned long long)bl.node_ptr);
  CHECK(tree.ntris == 2 && tree.nnodes == 3 && tree.root == 0, "counts %u %u root %d", tree.ntris, tree.nnodes, tree.root);
  put(0x00, 0x7000);
  put(0x20, 0x9000);  // both below the null-page guard? 0x7000 < 0x10000: refused
  CHECK(!Parse(h, 0x100000, &tree, &bl, &why) && !std::strcmp(why, "block address"), "null-page pointer: %s", why);
  put(0x00, 0x30000);
  put(0x20, 0x50000);
  CHECK(Parse(h, 0x100000, &tree, &bl, &why) && bl.tri_ptr == 0x30000 && bl.node_ptr == 0x50000, "fixed pointers not used");
  put(0x00, 0xffffffffffffff00ull);  // the block would wrap the address space
  CHECK(!Parse(h, 0x100000, &tree, &bl, &why) && !std::strcmp(why, "block address"), "wrapping block: %s", why);
  fresh();
  put(0x08, 0xe0);
  put(0x28, 0x40);
  const uint32_t root = 0x18;
  std::memcpy(h + 0x40, &root, 4);
  CHECK(Parse(h, 0x100000, &tree, &bl, &why) && tree.root == 0x18 && bl.root == 0x18, "root reference not read from +0x40");

  // the limits are the widths of the references: a leaf's first record is 26 bits (>> 5 of 31), an inner node's index 28 bits (>> 3)
  CHECK(kMaxTri4 == (0x7fffffffull >> 5) && kMaxNodes == (0x7fffffffull >> 3), "limits %llx %llx", (unsigned long long)kMaxTri4,
        (unsigned long long)kMaxNodes);
  // each block pointer is guarded on its own
  fresh();
  put(0x08, 0xe0);
  put(0x28, 0x40);
  put(0x00, 0x30000);
  put(0x20, 0x8000);
  CHECK(!Parse(h, 0x100000, &tree, &bl, &why) && !std::strcmp(why, "block address"), "null-page node pointer: %s", why);
  put(0x00, 0x8000);
  put(0x20, 0x30000);
  CHECK(!Parse(h, 0x100000, &tree, &bl, &why) && !std::strcmp(why, "block address"), "null-page triangle pointer: %s", why);
  put(0x00, 0x30000);
  put(0x20, 0xffffffffffffffe0ull);  // the node block would wrap
  CHECK(!Parse(h, 0x100000, &tree, &bl, &why) && !std::strcmp(why, "block address"), "wrapping node block: %s", why);
  put(0x20, 0x50000);
  CHECK(Parse(h, 0x100000, &tree, &bl, &why), "the same header with sane pointers is refused: %s", why);
}

// ---------------------------------------------------------------------------------------------------------------------
static void TestRayValidity() {
  const float o[3] = {0, 0, 0}, d[3] = {1, 0, 0}, big[3] = {1e30f, 0, 0}, inf[3] = {INFINITY, 0, 0}, nan_[3] = {NAN, 0, 0}, zero[3] = {0, 0, 0};
  CHECK(Ray(o, d, 0, 1).ok, "an ordinary ray is not ok");
  CHECK(Ray(o, d, 1, 1).ok, "an empty range is not ok (tmax == tmin is allowed)");
  CHECK(!Ray(o, d, 2, 1).ok, "tmax < tmin accepted");
  CHECK(!Ray(o, d, NAN, 1).ok && !Ray(o, d, 0, NAN).ok, "a NaN range accepted");
  CHECK(!Ray(o, zero, 0, 1).ok, "a zero direction accepted");
  CHECK(!Ray(o, big, 0, 1).ok, "a direction whose squared length overflows accepted");
  CHECK(!Ray(o, inf, 0, 1).ok && !Ray(o, nan_, 0, 1).ok, "a non-finite direction accepted");
  CHECK(!Ray(inf, d, 0, 1).ok && !Ray(nan_, d, 0, 1).ok, "a non-finite origin accepted");
  CHECK(Ray(o, d, 0, INFINITY).ok, "tmax = inf (the game's casts) is not ok");
}

// ---------------------------------------------------------------------------------------------------------------------
static void TestSingleTriangle() {
  const float A[3] = {0, 0, 0}, B[3] = {1, 0, 0}, C[3] = {0, 1, 0};
  std::vector<bt::TriIn> in = {Tri(A, B, C, 7, 42)};
  const bt::Built b = bt::Builder(in, 4).Build();
  const Tree t = TreeOf(b);
  Hit h;

  const float o[3] = {0.2f, 0.5f, 1.0f}, down[3] = {0, 0, -1};
  CHECK(Cast(t, o, down, 0, INFINITY, false, &h), "no hit on the triangle");
  CHECK(Near(h.t, 1.0f) && h.id0 == 7 && h.id1 == 42, "t %.4f ids %d/%d", h.t, h.id0, h.id1);
  CHECK(Near(h.u, 0.2f) && Near(h.v, 0.5f), "u %.4f v %.4f (expected 0.2 / 0.5: u along v1-v0, v along v2-v0)", h.u, h.v);
  // Ng = e1 x e2 with e1 = v0 - v1 = (-1 0 0), e2 = v2 - v0 = (0 1 0): (0, 0, -1), normalised
  CHECK(Near(h.n[0], 0) && Near(h.n[1], 0) && Near(h.n[2], -1.0f), "normal (%.3f %.3f %.3f)", h.n[0], h.n[1], h.n[2]);

  // two-sided, and the normal is not flipped toward the ray
  const float below[3] = {0.2f, 0.5f, -3.0f}, up[3] = {0, 0, 1};
  CHECK(Cast(t, below, up, 0, INFINITY, false, &h) && Near(h.t, 3.0f) && Near(h.n[2], -1.0f), "back side: t %.3f nz %.3f", h.t, h.n[2]);
  // t is in units of the direction: a direction of length 2 halves it
  const float down2[3] = {0, 0, -2};
  CHECK(Cast(t, o, down2, 0, INFINITY, false, &h) && Near(h.t, 0.5f), "t in units of dir: %.4f", h.t);

  // misses: beside the triangle, outside u+v <= 1, behind the origin, parallel to the plane
  const float side[3] = {1.5f, 0.2f, 1}, outside[3] = {0.7f, 0.7f, 1}, behind[3] = {0.2f, 0.5f, -1}, par[3] = {1, 0, 0};
  CHECK(!Cast(t, side, down, 0, INFINITY, false, &h), "hit beside the triangle");
  CHECK(!Cast(t, outside, down, 0, INFINITY, false, &h), "hit with u+v > 1");
  CHECK(!Cast(t, behind, down, 0, INFINITY, false, &h), "hit behind the origin");
  const float inplane[3] = {-1, 0.2f, 0};
  CHECK(!Cast(t, inplane, par, 0, INFINITY, false, &h), "hit on a ray parallel to (and in) the plane");

  // the range is open at both ends: a hit exactly at tmin or at tmax does not count
  const float on[3] = {0.2f, 0.5f, 0.0f};
  CHECK(!Cast(t, on, down, 0, INFINITY, false, &h), "a hit at t == tmin counted");
  CHECK(Cast(t, on, down, -1, INFINITY, false, &h) && Near(h.t, 0.0f), "no hit at t == 0 with tmin below it");
  CHECK(!Cast(t, o, down, 0, 1.0f, false, &h), "a hit at t == tmax counted");
  CHECK(Cast(t, o, down, 0, nextafterf(1.0f, 2.0f), false, &h), "no hit with tmax just above t");
  // the range is open: (tmin, tmax)
  CHECK(!Cast(t, o, down, 0, 0.9f, false, &h), "hit beyond tmax");
  CHECK(Cast(t, o, down, 0, 1.1f, false, &h), "no hit within tmax");
  CHECK(!Cast(t, o, down, 1.1f, INFINITY, false, &h), "hit before tmin");
  CHECK(Cast(t, o, down, 0.9f, INFINITY, false, &h), "no hit after tmin");

  // Occluded: the segment a..b is t in (0, 1)
  const float a[3] = {0.2f, 0.5f, 1}, e[3] = {0.2f, 0.5f, 0.5f}, e2[3] = {0.2f, 0.5f, -0.5f};
  CHECK(!Seg(t, a, e), "segment that stops short is blocked");
  CHECK(Seg(t, a, e2), "segment through the triangle is not blocked");
  CHECK(!Seg(t, a, a), "zero-length segment is blocked");

  // an out pointer is optional; a miss leaves the hit as it was
  Hit untouched;
  untouched.id0 = 5;
  CHECK(!Cast(t, side, down, 0, INFINITY, false, &untouched) && untouched.id0 == 5, "miss wrote the hit");
  CHECK(Cast(t, o, down, 0, INFINITY, false, nullptr), "no hit with a null out");
}

// ---------------------------------------------------------------------------------------------------------------------
static void TestRoom(int leaf) {
  const float lo[3] = {-5, -5, -5}, hi[3] = {5, 5, 5};
  std::vector<bt::TriIn> in;
  bt::AddBox(in, lo, hi, 100, true);  // a room: 12 triangles, Ng faces inward, ids 100..105 = -x +x -y +y -z +z
  const bt::Built b = bt::Builder(in, leaf).Build();
  const Tree t = TreeOf(b);
  const float c[3] = {0, 0, 0};
  const float dirs[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
  for (int f = 0; f < 6; ++f) {
    Hit h;
    const bool got = Cast(t, c, dirs[f], 0, INFINITY, false, &h);
    CHECK(got && Near(h.t, 5.0f) && h.id0 == 100 + f, "leaf %d face %d: got %d t %.3f id0 %d", leaf, f, got, h.t, h.id0);
    // the inward normal faces back at the ray
    const float dn = h.n[0] * dirs[f][0] + h.n[1] * dirs[f][1] + h.n[2] * dirs[f][2];
    CHECK(Near(dn, -1.0f), "leaf %d face %d: normal (%.2f %.2f %.2f) does not face the ray", leaf, f, h.n[0], h.n[1], h.n[2]);
  }
  // the floor under a player faces up (Y up), the property the game's callers rely on
  Hit h;
  const float head[3] = {1, 1.5f, 2}, down[3] = {0, -1, 0};
  CHECK(Cast(t, head, down, 0, INFINITY, false, &h) && Near(h.t, 6.5f) && h.id0 == 102 && h.n[1] > 0.99f, "floor: t %.3f id0 %d ny %.2f", h.t,
        h.id0, h.n[1]);
  // a diagonal: from (1, 2, 3) toward (+1 +1 +1) reaches x = 5 at t = 4, y = 5 at t = 3, z = 5 at t = 2: leaves through +z
  const float o2[3] = {1, 2, 3}, d2[3] = {1, 1, 1};
  CHECK(Cast(t, o2, d2, 0, INFINITY, false, &h) && Near(h.t, 2.0f) && h.id0 == 105, "diagonal: t %.3f id0 %d", h.t, h.id0);

  // line of sight: inside the room is clear, through a wall is blocked, from outside into the room is blocked
  const float p1[3] = {-4, -4, -4}, p2[3] = {4, 4, 4}, out1[3] = {0, 0, 5.5f}, out2[3] = {9, 9, 9}, near_wall[3] = {0, 0, 4.99f};
  CHECK(!Seg(t, p1, p2), "leaf %d: a clear line inside the room is blocked", leaf);
  CHECK(Seg(t, c, out1), "leaf %d: a segment through the +z wall is not blocked", leaf);
  CHECK(!Seg(t, c, near_wall), "leaf %d: a segment that stops short of the wall is blocked", leaf);
  CHECK(Seg(t, out2, p2), "leaf %d: a segment from outside into the room is not blocked (two-sided)", leaf);
  const float far1[3] = {20, 20, 20};
  CHECK(!Seg(t, out2, far1), "leaf %d: a segment wholly outside the room is blocked", leaf);
}

static void TestNearestVsFirst() {
  // two parallel sheets: the nearest hit is the near one; "any" may return either but must return a hit
  const float a0[3] = {-5, -5, 0}, b0[3] = {5, -5, 0}, c0[3] = {0, 5, 0};
  const float a1[3] = {-5, -5, 3}, b1[3] = {5, -5, 3}, c1[3] = {0, 5, 3};
  std::vector<bt::TriIn> in = {Tri(a0, b0, c0, 1, 0), Tri(a1, b1, c1, 2, 0)};
  for (int leaf : {1, 4}) {
    const bt::Built b = bt::Builder(in, leaf).Build();
    const Tree t = TreeOf(b);
    const float o[3] = {0, 0, 10}, d[3] = {0, 0, -1};
    Hit h;
    CHECK(Cast(t, o, d, 0, INFINITY, false, &h) && h.id0 == 2 && Near(h.t, 7.0f), "nearest sheet: id0 %d t %.3f", h.id0, h.t);
    const float o2[3] = {0, 0, -10}, d2[3] = {0, 0, 1};
    CHECK(Cast(t, o2, d2, 0, INFINITY, false, &h) && h.id0 == 1 && Near(h.t, 10.0f), "nearest sheet from below: id0 %d t %.3f", h.id0, h.t);
    CHECK(Cast(t, o, d, 0, INFINITY, true, nullptr), "any: no hit");
    CHECK(!Cast(t, o, d, 0, 6.0f, true, nullptr), "any: hit beyond tmax");
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// independent double-precision reference (shares no code with TriHit)
static bool RefHit(const bt::TriIn& tr, const float* o, const float* d, double tmax, double* t) {
  double v0[3], e1[3], e2[3];
  for (int k = 0; k < 3; ++k) {
    v0[k] = tr.a[k];
    e1[k] = static_cast<double>(tr.b[k]) - tr.a[k];
    e2[k] = static_cast<double>(tr.c[k]) - tr.a[k];
  }
  // solve o + t d = v0 + u e1 + v e2 by Cramer's rule
  const double D[3] = {d[0], d[1], d[2]};
  auto det3 = [](const double* a, const double* b, const double* c) {
    return a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
  };
  const double nd[3] = {-D[0], -D[1], -D[2]};
  double r[3] = {o[0] - v0[0], o[1] - v0[1], o[2] - v0[2]};
  // r = u e1 + v e2 - t D  ->  [e1 e2 -D] (u v t)^T = r
  const double det = det3(e1, e2, nd);
  if (fabs(det) < 1e-12) return false;
  const double u = det3(r, e2, nd) / det, v = det3(e1, r, nd) / det, tt = det3(e1, e2, r) / det;
  if (u < 0 || v < 0 || u + v > 1) return false;
  if (!(tt > 0 && tt < tmax)) return false;
  *t = tt;
  return true;
}

static void TestSoup() {
  bt::Lcg rng(12345);
  std::vector<bt::TriIn> in;
  for (int i = 0; i < 3000; ++i) {
    float c[3] = {rng.U() * 100 - 50, rng.U() * 100 - 50, rng.U() * 100 - 50};
    bt::TriIn t;
    for (int k = 0; k < 3; ++k) {
      t.a[k] = c[k] + rng.U() * 6 - 3;
      t.b[k] = c[k] + rng.U() * 6 - 3;
      t.c[k] = c[k] + rng.U() * 6 - 3;
    }
    t.id0 = i % 7;
    t.id1 = i;
    in.push_back(t);
  }
  for (int leaf : {1, 4, 16, 124}) {
    const bt::Built b = bt::Builder(in, leaf).Build();
    const Tree t = TreeOf(b);
    bt::Lcg r2(777);
    int agree = 0, hits = 0, N = 1500, occ_agree = 0, M = 1500;
    for (int i = 0; i < N; ++i) {
      float o[3], d[3];
      for (int k = 0; k < 3; ++k) {
        o[k] = r2.U() * 120 - 60;
        d[k] = r2.U() * 2 - 1;
      }
      if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 1e-3f) continue;
      double best = 1e30;
      int bi = -1;
      for (size_t j = 0; j < in.size(); ++j) {
        double tt;
        if (RefHit(in[j], o, d, best, &tt)) {
          best = tt;
          bi = static_cast<int>(j);
        }
      }
      Hit h;
      const bool got = Cast(t, o, d, 0, INFINITY, false, &h);
      hits += bi >= 0;
      if (got == (bi >= 0) && (!got || (fabs(h.t - best) <= 1e-3 * fmax(1.0, best)))) ++agree;
      else std::printf("  leaf %d ray %d: bvh %d t %.5f vs reference %d t %.5f\n", leaf, i, got, got ? h.t : 0.0f, bi >= 0, bi >= 0 ? best : 0.0);
    }
    CHECK(agree == N, "leaf %d: %d of %d random rays agree with the double-precision reference (%d hit)", leaf, agree, N, hits);
    CHECK(hits > N / 20, "the soup is too sparse to test anything: %d hits", hits);
    for (int i = 0; i < M; ++i) {  // Occluded == "the nearest hit is closer than the segment end"
      float o[3], d[3];
      for (int k = 0; k < 3; ++k) {
        o[k] = r2.U() * 120 - 60;
        d[k] = r2.U() * 2 - 1;
      }
      const float len = 0.5f + 60 * r2.U();
      Hit h;
      const bool nearer = Cast(t, o, d, 0, INFINITY, false, &h) && h.t < len;  // d is not unit: t in units of d; len in the same
      const float dl[3] = {d[0], d[1], d[2]};
      const bool occ = Trace(t, Ray(o, dl, 0.0f, len), true, nullptr);
      occ_agree += nearer == occ;
    }
    CHECK(occ_agree == M, "leaf %d: Occluded agrees with nearest-hit on %d of %d segments", leaf, occ_agree, M);
  }
}

// ---------------------------------------------------------------------------------------------------------------------
static void TestDegenerate() {
  const float O[3] = {0, 0, 0};
  const float F[3] = {0, 0, -1};
  // a zero-area triangle and a collinear one are never hit; a real one next to them still is
  const float p[3] = {1, 1, 0}, q[3] = {2, 2, 0}, r[3] = {3, 3, 0}, a[3] = {-1, -1, -2}, b[3] = {2, -1, -2}, c[3] = {-1, 2, -2};
  std::vector<bt::TriIn> in = {Tri(p, p, p, 1, 0), Tri(p, q, r, 2, 1), Tri(a, b, c, 3, 2)};
  const bt::Built bl = bt::Builder(in, 4).Build();
  const Tree t = TreeOf(bl);
  Hit h;
  const float o[3] = {0, 0, 5}, o1[3] = {1, 1, 5};
  CHECK(!Cast(t, o1, F, 0, INFINITY, false, &h), "the zero-area triangle at (1 1 0) was hit");
  CHECK(Cast(t, o, F, 0, INFINITY, false, &h) && h.id0 == 3 && Near(h.t, 7.0f), "degenerate neighbours: id0 %d t %.3f", h.id0, h.t);
  const float o2[3] = {2, 2, 5};
  CHECK(!Cast(t, o2, F, 0, INFINITY, false, &h), "a zero-area / collinear triangle was hit");

  // an empty lane (id0 == -1) is skipped even where its geometry would be hit
  bt::Built e = bt::Builder(in, 4).Build();
  for (auto& rec : e.tris)
    for (int l = 0; l < 4; ++l)
      if (rec.id0[l] == 3) rec.id0[l] = -1;
  CHECK(!Cast(TreeOf(e), o, F, 0, INFINITY, false, &h), "an empty lane was hit");
  // a record of 5 triangles = one full record + one record with 3 empty lanes
  std::vector<bt::TriIn> five;
  for (int i = 0; i < 5; ++i) {
    const float z = -static_cast<float>(i);
    const float a5[3] = {-1, -1, z}, b5[3] = {2, -1, z}, c5[3] = {-1, 2, z};
    five.push_back(Tri(a5, b5, c5, 10 + i, i));
  }
  const bt::Built b5 = bt::Builder(five, 8).Build();
  CHECK(b5.tris.size() == 2, "5 triangles should be 2 records, got %zu", b5.tris.size());
  const float up5[3] = {0, 0, 1};
  const float o5[3] = {0, 0, -10};
  CHECK(Cast(TreeOf(b5), o5, up5, 0, INFINITY, false, &h) && h.id0 == 14 && Near(h.t, 6.0f), "5 triangles: id0 %d t %.3f", h.id0, h.t);

  // non-finite and degenerate rays hit nothing and cost nothing
  const float nan = NAN, inf = INFINITY;
  const float onan[3] = {nan, 0, 0}, dinf[3] = {inf, 0, 0}, dnan[3] = {0, 0, nan}, zero[3] = {0, 0, 0};
  uint32_t pops = 99;
  CHECK(!Trace(t, Ray(onan, F, 0, INFINITY), false, &h, &pops) && pops == 0, "NaN origin: pops %u", pops);
  pops = 99;
  CHECK(!Trace(t, Ray(O, dinf, 0, INFINITY), false, &h, &pops) && pops == 0, "infinite direction: pops %u", pops);
  pops = 99;
  CHECK(!Trace(t, Ray(O, dnan, 0, INFINITY), false, &h, &pops) && pops == 0, "NaN direction: pops %u", pops);
  pops = 99;
  CHECK(!Trace(t, Ray(O, zero, 0, INFINITY), false, &h, &pops) && pops == 0, "zero direction: pops %u", pops);
  pops = 99;
  CHECK(!Trace(t, Ray(o, F, nan, INFINITY), false, &h, &pops) && pops == 0, "NaN tmin: pops %u", pops);
  pops = 99;
  CHECK(!Trace(t, Ray(o, F, 0, nan), false, &h, &pops) && pops == 0, "NaN tmax: pops %u", pops);
  pops = 99;
  CHECK(!Trace(t, Ray(o, F, 5, 1), false, &h, &pops) && pops == 0, "tmax < tmin: pops %u", pops);
  pops = 0;
  CHECK(Trace(t, Ray(o, F, 0, INFINITY), false, &h, &pops) && pops > 0, "a good ray must visit the tree: pops %u", pops);

  // axis-aligned directions (a zero component) work: the reciprocal is clamped, not infinite
  const float ax[3] = {0, 0, -1};
  const float oa[3] = {0, 0, 5};
  CHECK(Cast(t, oa, ax, 0, INFINITY, false, &h) && h.id0 == 3, "axis-aligned ray: id0 %d", h.id0);

  // a tree with no nodes answers "miss"
  Tree none = t;
  none.nnodes = 0;
  CHECK(!Cast(none, o, F, 0, INFINITY, false, &h), "a tree without nodes answered a hit");
}

// ---------------------------------------------------------------------------------------------------------------------
static void TestRootAndMalformed() {
  const float a[3] = {-1, -1, 0}, b[3] = {2, -1, 0}, c[3] = {-1, 2, 0};
  const float o[3] = {0, 0, 5}, d[3] = {0, 0, -1};
  Hit h;
  {  // the root is the header's reference: node 3 here, with unused nodes before it
    std::vector<bt::TriIn> in = {Tri(a, b, c, 1, 0), Tri(c, b, a, 2, 1)};
    const bt::Built bl = bt::Builder(in, 1).Build(3);
    CHECK(bl.root == (3u << 3), "root reference %u", bl.root);
    CHECK(Cast(TreeOf(bl), o, d, 0, INFINITY, false, &h) && Near(h.t, 5.0f), "root at node 3 not used");
    bt::Built wrong = bl;
    wrong.root = 0;  // node 0 is an unused node: nothing to hit
    CHECK(!Cast(TreeOf(wrong), o, d, 0, INFINITY, false, &h), "node 0 was used although the header names another root");
    bt::Built oob = bl;
    oob.root = static_cast<uint32_t>(oob.nodes.size()) << 3;  // one past the last node
    CHECK(!Cast(TreeOf(oob), o, d, 0, INFINITY, false, &h), "a root outside the node block answered a hit");
  }
  {  // a leaf root
    std::vector<bt::TriIn> in = {Tri(a, b, c, 1, 0)};
    const bt::Built bl = bt::Builder(in, 4).BuildLeafRoot();
    CHECK(static_cast<int32_t>(bl.root) < 0, "leaf root not negative");
    CHECK(Cast(TreeOf(bl), o, d, 0, INFINITY, false, &h) && h.id0 == 1, "leaf root not traversed");
  }
  {  // a leaf whose records run past the triangle block: refused, nothing read beyond it
    std::vector<bt::TriIn> in = {Tri(a, b, c, 1, 0)};
    bt::Built bl = bt::Builder(in, 4).BuildLeafRoot();
    bl.root = 0x80000000u | (0u << 5) | 2u;  // 2 records, the block has 1
    CHECK(!Cast(TreeOf(bl), o, d, 0, INFINITY, false, &h), "a leaf past the end of the triangle block was read");
    bl.root = 0x80000000u | (1u << 5) | 1u;  // starts at the end
    CHECK(!Cast(TreeOf(bl), o, d, 0, INFINITY, false, &h), "a leaf starting at the end of the block was read");
    bl.root = 0x80000000u | (0u << 5) | 1u;  // exactly the block
    CHECK(Cast(TreeOf(bl), o, d, 0, INFINITY, false, &h), "a leaf that exactly fills the block was refused");
    bl.root = 0x80000000u | (0u << 5) | 0u;  // empty leaf
    CHECK(!Cast(TreeOf(bl), o, d, 0, INFINITY, false, &h), "an empty leaf answered a hit");
  }
  {  // a child index outside the node block, mid-tree
    std::vector<bt::TriIn> in = {Tri(a, b, c, 1, 0), Tri(c, b, a, 2, 1)};
    bt::Built bl = bt::Builder(in, 1).Build();
    bl.nodes[0].child[0] = static_cast<int32_t>(500u << 3);
    bl.nodes[0].child[1] = static_cast<int32_t>(500u << 3);
    for (int k = 0; k < 4; ++k) bl.nodes[0].x[k] = bl.nodes[0].y[k] = bl.nodes[0].z[k] = (k < 2) ? -100.0f : 100.0f;
    CHECK(!Cast(TreeOf(bl), o, d, 0, INFINITY, false, &h), "a child outside the node block was followed");
  }
  {  // a cycle: node 0's near child is node 0. Terminates in at most 2 * nnodes + 2 pops.
    bt::Built bl;
    bl.nodes.push_back(bt::Builder::DummyNode());
    bl.nodes[0].child[0] = 0;
    for (int k = 0; k < 4; ++k) bl.nodes[0].x[k] = bl.nodes[0].y[k] = bl.nodes[0].z[k] = (k < 2) ? -100.0f : 100.0f;
    bl.tris.push_back(bt::EmptyRecord());
    uint32_t pops = 0;
    CHECK(!Trace(TreeOf(bl), Ray(o, d, 0, INFINITY), false, &h, &pops), "a cyclic tree answered a hit");
    CHECK(pops == 2u * 1 + 2, "cycle: %u pops (the budget is 2 * nodes + 2 = 4)", pops);
  }
  {  // two nodes pointing at each other
    bt::Built bl;
    bl.nodes.push_back(bt::Builder::DummyNode());
    bl.nodes.push_back(bt::Builder::DummyNode());
    bl.nodes[0].child[0] = 1 << 3;
    bl.nodes[1].child[0] = 0;
    for (int n = 0; n < 2; ++n)
      for (int k = 0; k < 4; ++k) bl.nodes[n].x[k] = bl.nodes[n].y[k] = bl.nodes[n].z[k] = (k < 2) ? -100.0f : 100.0f;
    bl.tris.push_back(bt::EmptyRecord());
    CHECK(!Cast(TreeOf(bl), o, d, 0, INFINITY, false, &h), "a two-node cycle answered a hit");
  }
}

// a chain of `depth` inner nodes: node k's near child is node k+1 (its box starts first), its far child a leaf holding a
// triangle the ray misses; the last node's near child is a leaf with the triangle the ray hits. Every level leaves one far
// leaf on the stack.
static bt::Built Chain(int depth) {
  bt::Built bl;
  const float far_a[3] = {50, 50, 0}, far_b[3] = {51, 50, 0}, far_c[3] = {50, 51, 0};  // off the ray (y = 0)
  const float hit_a[3] = {80, -1, -1}, hit_b[3] = {80, 1, -1}, hit_c[3] = {80, 0, 1};  // the plane x = 80, around (80, 0, 0)
  bt::Tri4 far_rec = bt::EmptyRecord(), hit_rec = bt::EmptyRecord();
  bt::SetLane(far_rec, 0, Tri(far_a, far_b, far_c, 9, 0));
  bt::SetLane(hit_rec, 0, Tri(hit_a, hit_b, hit_c, 77, 0));
  bl.tris.push_back(far_rec);  // record 0: far leaf
  bl.tris.push_back(hit_rec);  // record 1: the target
  const int32_t far_leaf = static_cast<int32_t>(0x80000000u | (0u << 5) | 1u);
  const int32_t hit_leaf = static_cast<int32_t>(0x80000000u | (1u << 5) | 1u);
  for (int k = 0; k < depth; ++k) {
    bt::Node n = bt::Builder::DummyNode();
    n.child[0] = (k + 1 < depth) ? static_cast<int32_t>((k + 1) << 3) : hit_leaf;  // near
    n.child[1] = far_leaf;                                                          // far
    // child 0's box starts at x = 0, child 1's at x = 40: the ray along +x from x = -10 reaches child 0 first
    const float lo0[3] = {0, -5, -5}, hi0[3] = {100, 5, 5}, lo1[3] = {40, -5, -5}, hi1[3] = {60, 55, 5};
    n.x[0] = lo0[0]; n.y[0] = lo0[1]; n.z[0] = lo0[2]; n.x[2] = hi0[0]; n.y[2] = hi0[1]; n.z[2] = hi0[2];
    n.x[1] = lo1[0]; n.y[1] = lo1[1]; n.z[1] = lo1[2]; n.x[3] = hi1[0]; n.y[3] = hi1[1]; n.z[3] = hi1[2];
    bl.nodes.push_back(n);
  }
  return bl;
}

static void TestStackLimit() {
  const float o[3] = {-10, 0, 0}, d[3] = {1, 0, 0};
  Hit h;
  // node k is processed with k far leaves on the stack and needs room for two more: 127 nodes fit in kStackSize = 128, 128 do not
  const bt::Built ok = Chain(127);
  uint32_t pops = 0;
  CHECK(Trace(TreeOf(ok), Ray(o, d, 0, INFINITY), false, &h, &pops) && h.id0 == 77 && Near(h.t, 90.0f), "a chain of 127 nodes: id0 %d t %.2f", h.id0,
        h.t);
  CHECK(kStackSize == 128, "the stack size is part of the contract of this test");
  const bt::Built deep = Chain(128);
  CHECK(!Cast(TreeOf(deep), o, d, 0, INFINITY, false, &h), "a chain of 128 nodes overflowed the stack and still answered a hit");
  const bt::Built shallow = Chain(20);
  CHECK(Cast(TreeOf(shallow), o, d, 0, INFINITY, false, &h) && h.id0 == 77, "a chain of 20 nodes");
  // a balanced tree of 100000 triangles stays far inside the stack
  bt::Lcg rng(99);
  std::vector<bt::TriIn> in;
  for (int i = 0; i < 20000; ++i) {
    const float x = rng.U() * 1000, y = rng.U() * 1000, z = rng.U() * 10;
    const float a[3] = {x, y, z}, b[3] = {x + 1, y, z}, c[3] = {x, y + 1, z};
    in.push_back(Tri(a, b, c, 1, i));
  }
  const bt::Built big = bt::Builder(in, 8).Build();
  const float od[3] = {500.2f, 500.2f, 100}, dd[3] = {0, 0, -1};
  uint32_t visited = 0;
  Trace(TreeOf(big), Ray(od, dd, 0, INFINITY), false, &h, &visited);
  CHECK(visited < 200, "a vertical ray through a balanced 20000-triangle tree visited %u entries", visited);
}

// a zero Ng (a degenerate record) gives a zero normal, not NaN
static void TestZeroNormal() {
  const float A[3] = {0, 0, 0}, B[3] = {1, 0, 0}, C[3] = {0, 1, 0};
  std::vector<bt::TriIn> in = {Tri(A, B, C, 1, 2)};
  bt::Built b = bt::Builder(in, 4).Build();
  for (auto& q : b.tris)
    for (int k = 0; k < 3; ++k)
      for (int l = 0; l < 4; ++l) q.ng[k][l] = 0.0f;
  const float o[3] = {0.2f, 0.5f, 1.0f}, down[3] = {0, 0, -1};
  Hit h;
  CHECK(Cast(TreeOf(b), o, down, 0, INFINITY, false, &h) && h.n[0] == 0 && h.n[1] == 0 && h.n[2] == 0, "zero Ng: normal (%f %f %f)", h.n[0], h.n[1],
        h.n[2]);
}

// an empty child (INT32_MIN) is never entered, even when the ray passes through its (here zero) bounds; the pops count shows it
static void TestEmptyChild() {
  const float A[3] = {-1, -1, 1}, B[3] = {2, -1, 1}, C[3] = {-1, 2, 1};
  std::vector<bt::TriIn> in = {Tri(A, B, C, 3, 4)};
  const float o[3] = {0, 0, -5}, up[3] = {0, 0, 1};
  for (int side = 0; side < 2; ++side) {
    bt::Built b = bt::Builder(in, 4).Build();  // root node: child 0 = the triangle's leaf, child 1 = empty with zero bounds
    if (side == 1) {                          // the same tree with the children swapped
      Node& n = b.nodes[0];
      std::swap(n.child[0], n.child[1]);
      for (float* v : {n.x, n.y, n.z}) {
        std::swap(v[0], v[1]);
        std::swap(v[2], v[3]);
      }
    }
    const Tree t = TreeOf(b);
    Hit h;
    uint32_t pops = 0;
    CHECK(Trace(t, Ray(o, up, 0, INFINITY), false, &h, &pops) && h.id0 == 3 && Near(h.t, 6.0f), "side %d: hit %d t %.3f", side, h.id0, h.t);
    CHECK(pops == 2, "side %d: %u pops (the root and the triangle's leaf; the empty child must not be entered)", side, pops);
  }
}

// a node reference one past the node block is a miss, not a read of whatever follows it
static void TestNodeBound() {
  const float lo[3] = {-5, -5, -5}, hi[3] = {5, 5, 5};
  std::vector<bt::TriIn> in;
  bt::AddBox(in, lo, hi, 100, true);
  const bt::Built b = bt::Builder(in, 2).Build();
  Tree full = TreeOf(b), cut = full;
  cut.nnodes = full.nnodes - 1;  // the last node is out of bounds for `cut`
  bt::Lcg rng(7);
  int misses_cut = 0, hits_full = 0;
  for (int i = 0; i < 400; ++i) {
    const float o[3] = {0, 0, 0}, d[3] = {rng.U() * 2 - 1, rng.U() * 2 - 1, rng.U() * 2 - 1};
    if (d[0] == 0 && d[1] == 0 && d[2] == 0) continue;
    Hit h;
    if (!Cast(full, o, d, 0, INFINITY, false, &h)) continue;
    ++hits_full;
    if (!Cast(cut, o, d, 0, INFINITY, false, &h)) ++misses_cut;
  }
  CHECK(hits_full > 300 && misses_cut > 0, "rays that need the hidden node must miss: %d of %d", misses_cut, hits_full);
}

int main() {
  TestParse();
  TestRayValidity();
  TestZeroNormal();
  TestEmptyChild();
  TestNodeBound();
  TestSingleTriangle();
  for (int leaf : {1, 2, 4, 31 * 4}) TestRoom(leaf);
  TestNearestVsFirst();
  TestSoup();
  TestDegenerate();
  TestRootAndMalformed();
  TestStackLimit();
  if (g_fail == 0) std::printf("bvh_core_test: all checks passed\n");
  else std::printf("bvh_core_test: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
