#pragma once

// #1420: synthetic CParticleEffectCR and CR15NetBulletCR resources for the netgun bullet tests, written from the layout
// as the stock levels have it (56-byte header; 152-byte rows, 136-byte entries) with literal offsets and ids, not with
// the constants of runtime/patch/netgun_bullets.h, so a wrong constant there fails a test here. No game bytes: every
// field the rewrite does not read is filled with a pattern unique to its row and offset, so a copy from the wrong row
// or a stray write shows up in a byte compare.

#include <cstdint>
#include <cstring>
#include <vector>

namespace netgun_fixture {

constexpr uint64_t kAssault = 0x12001003b8e9a2a6ULL;
constexpr uint64_t kBlaster = 0x8684283916089a68ULL;
constexpr uint64_t kScout = 0x3ca5f39a4225acaeULL;
constexpr uint64_t kRocket = 0xf90ca2ce086a1d58ULL;
constexpr uint64_t kMagnum = 0xdb4b483ed033c7dbULL;
constexpr uint64_t kSmg = 0x7fe4c55be75f4685ULL;
constexpr uint64_t kChain = 0x7322510dceaac128ULL;
constexpr uint64_t kRifle = 0xd03bbc02c2a0ea1dULL;
constexpr uint64_t kDoor = 0xf5b0557e1ebc351fULL;   // mpl_combat_gauss: a door actor's rows follow the bullets'
constexpr uint64_t kNinth = 0xa8f84e7dd40b806eULL;  // the ninth stock CR15NetBulletCR entry, no effects

constexpr uint64_t kTrail = 0xe49b915933b1f0e5ULL;
constexpr uint64_t kImpact = 0xe690045573533431ULL;
constexpr uint64_t kExplosion = 0xeb7d6a59038eb425ULL;
constexpr uint64_t kNone = 0xffffffffffffffffULL;

inline void Put64(std::vector<uint8_t>& b, size_t at, uint64_t v) { std::memcpy(b.data() + at, &v, 8); }
inline void Put32(std::vector<uint8_t>& b, size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
inline void Put16(std::vector<uint8_t>& b, size_t at, uint16_t v) { std::memcpy(b.data() + at, &v, 2); }
inline uint64_t Get64(const std::vector<uint8_t>& b, size_t at) {
  uint64_t v;
  std::memcpy(&v, b.data() + at, 8);
  return v;
}
inline uint16_t Get16(const std::vector<uint8_t>& b, size_t at) {
  uint16_t v;
  std::memcpy(&v, b.data() + at, 2);
  return v;
}

// One CParticleEffectCR row: name symbol at +0, owner at +8, 0xffff at +16, pool at +18, and every other byte a
// pattern of (tag, offset).
struct Row {
  uint64_t name;
  uint64_t owner;
  uint16_t pool;
  uint8_t tag;
};

inline std::vector<uint8_t> RowBytes(const Row& r) {
  std::vector<uint8_t> b(152);
  for (size_t i = 0; i < b.size(); ++i) b[i] = static_cast<uint8_t>(r.tag * 31 + i);
  Put64(b, 0, r.name);
  Put64(b, 8, r.owner);
  Put16(b, 16, 0xffff);
  Put16(b, 18, r.pool);
  return b;
}

// A resource: the 56-byte header as on disk (data size at +8, flags 1 at +28, capacity at +40, count at +48; the
// other header bytes a pattern), then the entries.
inline std::vector<uint8_t> Resource(const std::vector<std::vector<uint8_t>>& entries, size_t entryBytes) {
  std::vector<uint8_t> b(56);
  for (size_t i = 0; i < b.size(); ++i) b[i] = static_cast<uint8_t>(0xa0 + i);
  const uint64_t n = entries.size();
  Put64(b, 0, 0);
  Put64(b, 8, n * entryBytes);
  Put32(b, 24, 0);
  Put32(b, 28, 1);
  Put64(b, 40, n);
  Put64(b, 48, n);
  for (const auto& e : entries) b.insert(b.end(), e.begin(), e.end());
  return b;
}

// The stock row list (assault 2, blaster 2, scout 3, rocket 4), plus `door` rows of another actor after them.
inline std::vector<Row> StockRows(int door = 0) {
  std::vector<Row> rows = {
      {kTrail, kAssault, 500, 1},   {kImpact, kAssault, 500, 2},     {kTrail, kBlaster, 1000, 3},
      {kImpact, kBlaster, 1000, 4}, {kTrail, kScout, 75, 5},         {kImpact, kScout, 75, 6},
      {kExplosion, kScout, 75, 7},  {0x9253242e14507924ULL, kRocket, 75, 8},
      {0x3d6e1ddbc3795fd4ULL, kRocket, 75, 9}, {0xb600c6d35d03104cULL, kRocket, 75, 10},
      {0xfae9b6164ab5ae74ULL, kRocket, 75, 11},
  };
  for (int i = 0; i < door; ++i)
    rows.push_back({0x1000 + static_cast<uint64_t>(i), kDoor, 0xffff, static_cast<uint8_t>(40 + i)});
  return rows;
}

inline std::vector<uint8_t> Particles(const std::vector<Row>& rows) {
  std::vector<std::vector<uint8_t>> e;
  for (const Row& r : rows) e.push_back(RowBytes(r));
  return Resource(e, 152);
}

// The scout's row `name` (tag 5, 6 or 7) as owned by `owner` with `pool`: the bytes a correct rewrite adds.
inline std::vector<uint8_t> ScoutRowFor(uint64_t name, uint64_t owner, uint16_t pool) {
  const uint8_t tag = name == kTrail ? 5 : name == kImpact ? 6 : 7;
  std::vector<uint8_t> b = RowBytes({name, kScout, 75, tag});
  Put64(b, 8, owner);
  Put16(b, 18, pool);
  return b;
}

// What PatchParticles must write for StockRows(door): every row it was given, in place, then the nine new ones.
inline std::vector<uint8_t> PatchedParticles(int door = 0) {
  std::vector<std::vector<uint8_t>> e;
  for (const Row& r : StockRows(door)) e.push_back(RowBytes(r));
  e.push_back(ScoutRowFor(kTrail, kMagnum, 65));
  e.push_back(ScoutRowFor(kImpact, kMagnum, 65));
  e.push_back(ScoutRowFor(kExplosion, kMagnum, 65));
  e.push_back(ScoutRowFor(kTrail, kSmg, 200));
  e.push_back(ScoutRowFor(kImpact, kSmg, 200));
  e.push_back(ScoutRowFor(kTrail, kChain, 150));
  e.push_back(ScoutRowFor(kImpact, kChain, 150));
  e.push_back(ScoutRowFor(kTrail, kRifle, 500));
  e.push_back(ScoutRowFor(kImpact, kRifle, 500));
  return Resource(e, 152);
}

// One CR15NetBulletCR entry: a symbol at +0, the bullet at +8, 0xffff at +16, the pool at +18, the six slots from +88,
// and every other byte a pattern of (tag, offset).
struct Entry {
  uint64_t actor;
  uint16_t pool;
  uint64_t slots[6];
  uint8_t tag;
};

inline std::vector<uint8_t> EntryBytes(const Entry& en) {
  std::vector<uint8_t> b(136);
  for (size_t i = 0; i < b.size(); ++i) b[i] = static_cast<uint8_t>(en.tag * 17 + i);
  Put64(b, 0, 0x48037cde0e07c1b2ULL);
  Put64(b, 8, en.actor);
  Put16(b, 16, 0xffff);
  Put16(b, 18, en.pool);
  for (size_t s = 0; s < 6; ++s) Put64(b, 88 + s * 8, en.slots[s]);
  return b;
}

// The nine stock entries, in stock order.
inline std::vector<Entry> StockEntries() {
  return {
      {kAssault, 500, {kImpact, kTrail, kNone, kNone, kNone, kNone}, 1},
      {kBlaster, 1000, {kImpact, kTrail, kNone, kNone, kNone, kNone}, 2},
      {kScout, 75, {kImpact, kTrail, kExplosion, kNone, kNone, kNone}, 3},
      {kRocket,
       75,
       {kNone, 0x9253242e14507924ULL, 0x3d6e1ddbc3795fd4ULL, kNone, 0xb600c6d35d03104cULL, 0xfae9b6164ab5ae74ULL},
       4},
      {kMagnum, 65, {kNone, kNone, kNone, kNone, kNone, kNone}, 5},
      {kSmg, 200, {kNone, kNone, kNone, kNone, kNone, kNone}, 6},
      {kChain, 150, {kNone, kNone, kNone, kNone, kNone, kNone}, 7},
      {kRifle, 500, {kNone, kNone, kNone, kNone, kNone, kNone}, 8},
      {kNinth, 200, {kNone, kNone, kNone, kNone, kNone, kNone}, 9},
  };
}

inline std::vector<uint8_t> Bullets(const std::vector<Entry>& entries) {
  std::vector<std::vector<uint8_t>> e;
  for (const Entry& en : entries) e.push_back(EntryBytes(en));
  return Resource(e, 136);
}

// What PatchBullets must write for StockEntries(): the four get the scout's impact and trail, the magnum its explosion.
inline std::vector<uint8_t> PatchedBullets() {
  std::vector<Entry> e = StockEntries();
  for (Entry& en : e) {
    if (en.actor == kMagnum || en.actor == kSmg || en.actor == kChain || en.actor == kRifle) {
      en.slots[0] = kImpact;
      en.slots[1] = kTrail;
    }
    if (en.actor == kMagnum) en.slots[2] = kExplosion;
  }
  return Bullets(e);
}

}  // namespace netgun_fixture
