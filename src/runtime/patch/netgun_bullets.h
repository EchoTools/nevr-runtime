#pragma once

// #1420: the magnum, smg, Chain and rifle bullets of the combat levels own no particle effect in the level data.
//
// Each net gun kind fires its own bullet actor (CR15NetGunCR +104). A bullet's effects are rows of the level's
// CParticleEffectCR, each owned by the bullet actor (+8), and the bullet's CR15NetBulletCR entry names the rows it
// plays by their name symbol (+0) in six slots. In the stock levels the assault, blaster, scout and rocket bullets
// have rows and named slots; the magnum, smg, Chain and rifle bullets exist with their own pools, scripts and (magnum)
// explosion, but own no row and name none (all six slots -1). That is the data; what a player sees of these bullets
// today, and whether their scripts play an effect of their own, is not read.
//
// The fix, from the stock data alone: each of the four gets copies of the scout bullet's rows, owned by its own actor
// and sized to its own pool, and its slots name them the way the scout's slots do. The scout's effects are the shared
// ones (pfx_wpn_cbt_shared_trail_base, pfx_wpn_cbt_assault_collision, pfx_wpn_cbt_scout_aoe_hit_b), all loaded with the
// level. The magnum, whose bullet explodes like the scout's (both carry CR15NetExplosionCR), also gets the scout's
// explosion; the other three get the trail and the impact.
//
// This header is the pure half: the layouts, the two rewrites and the decision "this buffer is the pinned stock
// resource". netgun_bullets.cpp hashes the buffer, registers the transforms and logs. The layout of both resources is
// read from the five stock levels (PC build 6323983201049540) and checked against every row of them; how the game
// resolves a slot to a row is not read from the binary.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "runtime/patch/resource_override.h"

namespace nevr_netgun_bullets {

/// The class symbols of the two resources: the class name without the package manifest's `Win10` suffix. That is the
/// logical type the loader callback reads at CResource + 0x28, and an entry keyed by it names buffer 0, the only buffer
/// a transform is offered (resource_buffer_key.h). No run has shown the callback report these two.
inline constexpr uint64_t kParticleType = EchoVR::CSymbol64Hash("CParticleEffectCR");
inline constexpr uint64_t kBulletType = EchoVR::CSymbol64Hash("CR15NetBulletCR");

/// The five levels that carry the net guns, in package order.
struct Level {
  const char* name;
  uint64_t hash;
};
inline constexpr Level kLevels[] = {
    {"mpl_combat_combustion", EchoVR::CSymbol64Hash("mpl_combat_combustion")},
    {"mpl_combat_dyson", EchoVR::CSymbol64Hash("mpl_combat_dyson")},
    {"mpl_combat_fission", EchoVR::CSymbol64Hash("mpl_combat_fission")},
    {"mpl_combat_gauss", EchoVR::CSymbol64Hash("mpl_combat_gauss")},
    {"mpl_lobby_b_combat", EchoVR::CSymbol64Hash("mpl_lobby_b_combat")},
};

/// SHA-256 of the stock resources, lowercase hex. CParticleEffectCR is one file in four levels; mpl_combat_gauss adds
/// five rows of its own (a door's) after the bullets'. CR15NetBulletCR is one file in all five. Any other bytes are
/// left alone.
inline constexpr char kParticleStockSha256[] = "7130808e89f3aa9400cb09fb7a30ca85c9a5a0be363557a647974ebd00283b8c";
inline constexpr char kParticleStockGaussSha256[] = "9f03344fa5e5ea183ffed2b42186a1fa6b7e214953dcbfa1c0308389e9f05750";
inline constexpr char kBulletStockSha256[] = "611f0ab4d6ada7f275ab1aaf974a08209d3c6e78e1a81b40c88fa11d9a215e6b";

/// Both resources: a 56-byte header, then `count` fixed-size entries and nothing after them (the size is exactly the
/// header plus `count` entries; ReadHeader refuses any other). The header holds the data size at +8 (count times the
/// entry size), the flags at +28 (1), the capacity at +40 and the count at +48 (equal on disk).
inline constexpr size_t kHeaderBytes = 56;
inline constexpr size_t kDataBytesAt = 8;
inline constexpr size_t kFlagsAt = 28;
inline constexpr size_t kCapacityAt = 40;
inline constexpr size_t kCountAt = 48;
inline constexpr uint32_t kFlags = 1;

/// A CParticleEffectCR row: +0 the row's name symbol (shared by the rows of one role across owners), +8 the owner
/// actor, +18 the owner's pool size (u16), +32 the CGParticleEffectResource it plays. +96 heads an array of 32-byte
/// entries kept after the rows; it is empty in every stock row, and a resource with bytes after its rows is refused,
/// so a copied row refers to nothing it does not carry.
inline constexpr size_t kRowBytes = 152;
inline constexpr size_t kRowNameAt = 0;
inline constexpr size_t kOwnerAt = 8;
inline constexpr size_t kPoolAt = 18;

/// A CR15NetBulletCR entry: +8 the bullet actor, +18 its pool size (u16), and from +88 six effect slots, each the name
/// symbol of one of the bullet's CParticleEffectCR rows or -1. The stock scout entry fills +88 (impact), +96 (trail)
/// and +104 (explosion); the rocket leaves +88 at -1 and fills +96 and +104 (its blue-team trail and explosion) and
/// +120 and +128 (its orange-team trail and explosion).
inline constexpr size_t kEntryBytes = 136;
inline constexpr size_t kSlotsAt = 88;
inline constexpr size_t kSlotCount = 6;
inline constexpr size_t kImpactSlotAt = 88;
inline constexpr size_t kTrailSlotAt = 96;
inline constexpr size_t kExplosionSlotAt = 104;
inline constexpr uint64_t kNoEffect = ~uint64_t{0};

/// The scout bullet's three rows, by name symbol, in their stock order: trail, impact, explosion.
inline constexpr uint64_t kTrailRow = 0xe49b915933b1f0e5ULL;
inline constexpr uint64_t kImpactRow = 0xe690045573533431ULL;
inline constexpr uint64_t kExplosionRow = 0xeb7d6a59038eb425ULL;

/// The scout bullet, whose rows and slots the four copy.
inline constexpr uint64_t kScoutBullet = 0x3ca5f39a4225acaeULL;

/// The four bullets that get effects, in the order of their actors in the levels' actor tables. `pool` is the actor's
/// pool size, the same number the stock CR15NetBulletCR entry carries at +18. `explodes` gives it the explosion too.
struct Bullet {
  const char* kind;
  uint64_t actor;
  uint16_t pool;
  bool explodes;
};
inline constexpr Bullet kBullets[] = {
    {"magnum", 0xdb4b483ed033c7dbULL, 65, true},
    {"smg", 0x7fe4c55be75f4685ULL, 200, false},
    {"Chain", 0x7322510dceaac128ULL, 150, false},
    {"rifle", 0xd03bbc02c2a0ea1dULL, 500, false},
};
inline constexpr size_t kBulletCount = std::size(kBullets);

enum class Status {
  Ok,
  Truncated,        // shorter than its header, or than the entries the header counts
  BadHeader,        // data size, capacity, count or flags not as on disk
  NoDonor,          // CParticleEffectCR: the scout's trail, impact or explosion row is missing or doubled;
                    // CR15NetBulletCR: the scout entry is missing, doubled, or its slots are not those three rows
  AlreadyPresent,   // CParticleEffectCR: one of the four already owns a row; CR15NetBulletCR: one of the four
                    // already names an effect
  MissingBullet,    // CR15NetBulletCR: an entry of the four is missing or doubled, or its pool differs
};

inline const char* StatusName(Status s) {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::Truncated: return "truncated";
    case Status::BadHeader: return "bad_header";
    case Status::NoDonor: return "no_donor";
    case Status::AlreadyPresent: return "already_present";
    case Status::MissingBullet: return "missing_bullet";
  }
  return "unknown";
}

enum class Verdict {
  Patched,   // the stock resource, with the effects added
  NotStock,  // not a pinned stock resource: left alone
  Refused,   // pinned, but the rewrite could not be made (status says why)
};

struct Result {
  Verdict verdict = Verdict::NotStock;
  Status status = Status::Ok;
  size_t entriesBefore = 0;
  size_t entriesAfter = 0;
  std::vector<uint8_t> bytes;  // the rewritten resource, when verdict is Patched
};

namespace detail {

inline uint64_t U64(const uint8_t* p, size_t at) {
  uint64_t v;
  std::memcpy(&v, p + at, sizeof(v));
  return v;
}
inline uint16_t U16(const uint8_t* p, size_t at) {
  uint16_t v;
  std::memcpy(&v, p + at, sizeof(v));
  return v;
}
inline void PutU64(uint8_t* p, size_t at, uint64_t v) { std::memcpy(p + at, &v, sizeof(v)); }
inline void PutU16(uint8_t* p, size_t at, uint16_t v) { std::memcpy(p + at, &v, sizeof(v)); }

/// The entry count of a resource whose header is as on disk for entries of `entryBytes`, or a status.
inline Status ReadHeader(const uint8_t* data, size_t size, size_t entryBytes, size_t* count) {
  if (data == nullptr || size < kHeaderBytes) return Status::Truncated;
  const uint64_t n = U64(data, kCountAt);
  uint32_t flags;
  std::memcpy(&flags, data + kFlagsAt, sizeof(flags));
  if (n == 0 || U64(data, kCapacityAt) != n || flags != kFlags) return Status::BadHeader;
  if (n > (size - kHeaderBytes) / entryBytes) return Status::Truncated;
  if (U64(data, kDataBytesAt) != n * entryBytes || size != kHeaderBytes + n * entryBytes) return Status::BadHeader;
  *count = static_cast<size_t>(n);
  return Status::Ok;
}

inline bool IsAddedBullet(uint64_t actor) {
  for (const Bullet& b : kBullets)
    if (b.actor == actor) return true;
  return false;
}

}  // namespace detail

/// CParticleEffectCR: the rows of the four bullets, copied from the scout bullet's and appended after the last row, in
/// kBullets order, each bullet's in the scout's order (trail, impact, explosion). Every stock row keeps its index, so a
/// client's rows are a server's (which runs no transform) with nine more at the end.
inline Result PatchParticles(const uint8_t* data, size_t size) {
  using namespace detail;
  Result r;
  r.verdict = Verdict::Refused;
  size_t n = 0;
  r.status = ReadHeader(data, size, kRowBytes, &n);
  if (r.status != Status::Ok) return r;
  r.entriesBefore = n;

  const uint8_t* rows = data + kHeaderBytes;
  const uint8_t* trail = nullptr;
  const uint8_t* impact = nullptr;
  const uint8_t* explosion = nullptr;
  int trails = 0, impacts = 0, explosions = 0;
  for (size_t e = 0; e < n; ++e) {
    const uint8_t* row = rows + e * kRowBytes;
    const uint64_t owner = U64(row, kOwnerAt);
    if (IsAddedBullet(owner)) {
      r.status = Status::AlreadyPresent;
      return r;
    }
    if (owner != kScoutBullet) continue;
    const uint64_t name = U64(row, kRowNameAt);
    if (name == kTrailRow) {
      trail = row;
      ++trails;
    } else if (name == kImpactRow) {
      impact = row;
      ++impacts;
    } else if (name == kExplosionRow) {
      explosion = row;
      ++explosions;
    }
  }
  if (trails != 1 || impacts != 1 || explosions != 1) {
    r.status = Status::NoDonor;
    return r;
  }

  std::vector<uint8_t> added;
  for (const Bullet& b : kBullets) {
    const uint8_t* donors[] = {trail, impact, explosion};
    const size_t k = b.explodes ? 3 : 2;
    for (size_t i = 0; i < k; ++i) {
      const size_t at = added.size();
      added.insert(added.end(), donors[i], donors[i] + kRowBytes);
      PutU64(added.data() + at, kOwnerAt, b.actor);
      PutU16(added.data() + at, kPoolAt, b.pool);
    }
  }

  const size_t m = n + added.size() / kRowBytes;
  r.bytes.reserve(kHeaderBytes + m * kRowBytes);
  r.bytes.insert(r.bytes.end(), data, data + size);
  r.bytes.insert(r.bytes.end(), added.begin(), added.end());
  PutU64(r.bytes.data(), kDataBytesAt, m * kRowBytes);
  PutU64(r.bytes.data(), kCapacityAt, m);
  PutU64(r.bytes.data(), kCountAt, m);
  r.entriesAfter = m;
  r.status = Status::Ok;
  r.verdict = Verdict::Patched;
  return r;
}

/// CR15NetBulletCR: the slots of the four bullets name the rows PatchParticles gives them, as the scout's name its own:
/// impact and trail, and the explosion for a bullet that explodes. Nothing else changes.
inline Result PatchBullets(const uint8_t* data, size_t size) {
  using namespace detail;
  Result r;
  r.verdict = Verdict::Refused;
  size_t n = 0;
  r.status = ReadHeader(data, size, kEntryBytes, &n);
  if (r.status != Status::Ok) return r;
  r.entriesBefore = n;
  r.entriesAfter = n;

  const uint8_t* entries = data + kHeaderBytes;
  int scouts = 0;
  const uint8_t* scout = nullptr;
  size_t at[kBulletCount] = {};
  int seen[kBulletCount] = {};
  for (size_t e = 0; e < n; ++e) {
    const uint8_t* entry = entries + e * kEntryBytes;
    const uint64_t actor = U64(entry, kOwnerAt);
    if (actor == kScoutBullet) {
      scout = entry;
      ++scouts;
    }
    for (size_t b = 0; b < kBulletCount; ++b) {
      if (kBullets[b].actor == actor) {
        at[b] = kHeaderBytes + e * kEntryBytes;
        ++seen[b];
      }
    }
  }
  if (scouts != 1 || U64(scout, kImpactSlotAt) != kImpactRow || U64(scout, kTrailSlotAt) != kTrailRow ||
      U64(scout, kExplosionSlotAt) != kExplosionRow) {
    r.status = Status::NoDonor;
    return r;
  }
  for (size_t b = 0; b < kBulletCount; ++b) {
    if (seen[b] != 1 || U16(data, at[b] + kPoolAt) != kBullets[b].pool) {
      r.status = Status::MissingBullet;
      return r;
    }
    for (size_t s = 0; s < kSlotCount; ++s) {
      if (U64(data, at[b] + kSlotsAt + s * 8) != kNoEffect) {
        r.status = Status::AlreadyPresent;
        return r;
      }
    }
  }

  r.bytes.assign(data, data + size);
  for (size_t b = 0; b < kBulletCount; ++b) {
    PutU64(r.bytes.data(), at[b] + kImpactSlotAt, kImpactRow);
    PutU64(r.bytes.data(), at[b] + kTrailSlotAt, kTrailRow);
    if (kBullets[b].explodes) PutU64(r.bytes.data(), at[b] + kExplosionSlotAt, kExplosionRow);
  }
  r.status = Status::Ok;
  r.verdict = Verdict::Patched;
  return r;
}

/// The two resources the transforms answer for.
enum class Resource { Particles, Bullets };

inline const char* ResourceName(Resource res) {
  return res == Resource::Particles ? "CParticleEffectCR" : "CR15NetBulletCR";
}

/// Decides what to serve for a buffer the loader delivered as `res` of a net-gun level. `sha256Hex` is the buffer's
/// digest; the rewrite runs only when it is one of `pins` (the stock digests of that resource).
inline Result Decide(Resource res, const uint8_t* data, size_t size, const std::string& sha256Hex,
                     const std::vector<std::string>& pins) {
  bool pinned = false;
  for (const std::string& pin : pins) pinned = pinned || sha256Hex == pin;
  if (!pinned) return Result{};
  return res == Resource::Particles ? PatchParticles(data, size) : PatchBullets(data, size);
}

/// The stock digests of `res`.
inline std::vector<std::string> StockPins(Resource res) {
  if (res == Resource::Particles) return {kParticleStockSha256, kParticleStockGaussSha256};
  return {kBulletStockSha256};
}

/// Registers the ten transforms (two resources, five levels) that answer every load of them (netgun_bullets.cpp)
/// through `reg`. Client only; logs its decision. Needs the resource override registry (InstallResourceOverride) to
/// exist.
using RegisterFn = bool (*)(uint64_t type_hash, uint64_t name_hash, ResourceTransformFn transform, void* user,
                            const char* label);
void Install(RegisterFn reg = &RegisterResourceTransform);

#ifdef NEVR_TEST_HOOKS
/// Replaces the stock digests the transforms accept (a test has no game bytes) and forgets what was served.
void TestHook_SetPins(const std::vector<std::string>& particles, const std::vector<std::string>& bullets);
/// Replaces the SHA-256 of the offered bytes (nevr_asset_cache::Sha256Hex), so a test can make it fail as a broken
/// crypto provider does; nullptr is the real one.
using DigestFn = std::string (*)(const void* data, size_t size);
void TestHook_SetDigest(DigestFn digest);
/// Back to the stock digests and the real SHA-256, nothing served.
void TestHook_Reset();
#endif

}  // namespace nevr_netgun_bullets
