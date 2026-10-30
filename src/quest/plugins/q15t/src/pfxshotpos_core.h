// The decisions of the pfxshotpos probe, free of the game: which effect ids are bullet effects and of what class, how a value
// falls into a histogram bin, which lines the per-kind budget lets through, and whether a node's world position moved like a
// re-placement. The probe reads the particle renderer's nodes and feeds these; nothing here calls the game. Header-only and
// constant-initialised, so a host test compiles it alone.
#pragma once

#include <cmath>
#include <cstdint>

namespace q15t {
namespace shotpos {

enum Class : int { kMuzzle = 0, kTrail = 1, kImpact = 2 };
constexpr int kClasses = 3;
constexpr const char* kClassName[kClasses] = {"muzzle", "trail", "impact"};

// The bullet effects the probe follows: the CSymbol64 hashes of the shipped muzzle, trail and impact effect ids. Muzzles first,
// then trails, then impacts; ClassOf returns the class of a row.
struct ShotId {
  uint64_t sym;
  int cls;
};
constexpr ShotId kShotIds[] = {
    {0xa8cd797f5e8693feull, kMuzzle}, {0x5859d9f1deba8f62ull, kMuzzle}, {0x32e0bdb6be54df9dull, kMuzzle},
    {0x56f6f9d8f5238854ull, kMuzzle}, {0x13bbad2a1bb49665ull, kMuzzle}, {0x591588d170a43fabull, kMuzzle},
    {0xcafac4e20c24d233ull, kMuzzle},
    {0xf677d365ee22d7acull, kTrail},  {0xa700f72d7ef654deull, kTrail},  {0xa1aca9b4b0f8789bull, kTrail},
    {0x14578d2c8f5af6cdull, kTrail},  {0x27dfaf5baf03d6a3ull, kTrail},
    {0x400fd23ed78f3bc4ull, kImpact}, {0x2869b2320808f879ull, kImpact}, {0xec92211825dda4aeull, kImpact},
    {0x8c7eef93937c7638ull, kImpact}, {0x3e0d00b4de4d893bull, kImpact}, {0x81cf9270aa39a8feull, kImpact},
    {0x99e95eca20f0a225ull, kImpact}, {0x99e95eca20fdb52full, kImpact},
};
constexpr int kShotIdCount = static_cast<int>(sizeof kShotIds / sizeof kShotIds[0]);

// The class of a symbol, -1 when the probe does not follow it.
inline int ClassOf(uint64_t sym) {
  for (const ShotId& s : kShotIds)
    if (s.sym == sym) return s.cls;
  return -1;
}

// The bin of `v` among `n` ascending edges: the count of edges at or below it, so a value on an edge goes up. NaN lands in
// the last bin (n), so a bad reading is counted, not dropped. The bins are n + 1 in all.
inline int Bin(float v, const float* edges, int n) {
  int b = 0;
  while (b < n && !(v < edges[b])) ++b;
  return b;
}

// Euclidean distance between two 3-vectors.
inline float Dist(const float* a, const float* b) {
  const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
  return std::sqrt(x * x + y * y + z * z);
}

// Lines per kind: the first kLines are written, then the 2^k-th (the line carries its count). `count` is the 1-based number of
// the line being written, so the caller increments before asking.
constexpr uint32_t kLines = 48;
inline bool Take(uint32_t count) { return count <= kLines || (count & (count - 1)) == 0; }

// A node whose world position moved more than kJumpM between two consecutive sim frames moved like a re-placement. A NaN
// position is not a jump (the comparison is false); the bin of NaN in the histograms is the last one.
constexpr float kJumpM = 2.f;
inline bool IsJump(const float* prev, const float* cur) { return Dist(prev, cur) > kJumpM; }

// One summary line every kSumMs while a count moved: due when that long has passed since the last one.
constexpr int64_t kSumMs = 10000;
inline bool SumDue(int64_t nowMs, int64_t lastMs) { return nowMs - lastMs >= kSumMs; }

// Histogram edges: the distance from the view-0 eye, a node's step between two sim frames, and a muzzle's distance from the eye.
constexpr float kEyeEdges[] = {0.5f, 1.f, 2.f, 4.f, 8.f, 16.f, 32.f, 64.f};
constexpr float kStepEdges[] = {0.05f, 0.25f, 0.5f, 1.f, 2.f, 4.f, 8.f, 32.f};
constexpr float kMuzzleHandEdges[] = {0.1f, 0.25f, 0.5f, 0.75f, 1.f, 1.5f, 2.f, 4.f};
constexpr int kEyeEdgeCount = static_cast<int>(sizeof kEyeEdges / sizeof kEyeEdges[0]);
constexpr int kStepEdgeCount = static_cast<int>(sizeof kStepEdges / sizeof kStepEdges[0]);
constexpr int kMuzzleHandEdgeCount = static_cast<int>(sizeof kMuzzleHandEdges / sizeof kMuzzleHandEdges[0]);

}  // namespace shotpos
}  // namespace q15t
