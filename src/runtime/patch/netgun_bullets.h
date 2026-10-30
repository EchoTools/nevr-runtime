#pragma once

// #1420: the netgun entries of the four unfinished weapons (PROTOSTAR magnum, QUARK smg, BLAZAR Chain, STARBURST rifle)
// name the assault's bullet instance in every combat level's CR15NetGunCR, so those kinds fire the assault's projectile
// only if the bullet ids are repaired. The runtime serves the player's own stock resource with those 60 ids rewritten
// to the assault's id; no game bytes are stored here.
//
// This header is the pure half: the layout, the pins and the byte rule (the process half is netgun_bullets.cpp). The
// rule is the stock resource's own data, read through the offsets tools/echomod/clone_combat.py reads: the entry count
// at +48, the capacity at +40, the data size at +8, entries of 160 bytes from +56, and in each entry the weapon kind at
// +72 and the bullet instance's id at +104. Entries 0-14 are the assault, 15-59 the blaster, scout and rocket, and
// 60-119 the four kinds this fix changes, 15 entries each in the order magnum, smg, Chain, rifle.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nevr_netgun_bullets {

/// CR15NetGunCR, the logical type of the resource (symcache_data.cpp, `0x6ef7231bbb0f6342`). The package manifest
/// keys the same resource under CR15NetGunCRWin10; the loader callback reports the logical type.
inline constexpr uint64_t kResourceType = 0x6ef7231bbb0f6342ULL;

/// The five names the resource is requested under: mpl_combat_combustion, mpl_combat_dyson, mpl_combat_gauss,
/// mpl_combat_fission and mpl_lobby_b_combat (the lobby sub-level with guns). Each is served the same bytes.
inline constexpr uint64_t kNames[] = {
    0x42670F2BED45703CULL,
    0x43E2DA7914642604ULL,
    0xDF5CA7B7DFA383D4ULL,
    0x43E2DA7A0C623A19ULL,
    0xCB9977F7FC2B4526ULL,
};
inline constexpr size_t kNameCount = sizeof(kNames) / sizeof(kNames[0]);

/// SHA-256 of the stock resource (lowercase hex): the historical extraction of CR15NetGunCRWin10 for
/// mpl_combat_combustion (commit c38d9b69). Not yet confirmed against a pristine copy of the shipped build.
inline constexpr char kStockSha256[] = "c28109544de935ec480f9c7666914971ba5c17b659e8e2b74dbea59a24373c37";
/// SHA-256 of the file a community plugin ships for the same five names. The rule below reproduces it from the stock
/// bytes; it is logged next to the result, and is not a gate.
inline constexpr char kPluginFileSha256[] = "30d60a0ba49bfc6ce353289ed362fad5f34cfd91dc4c826d99d9d482ccf037e1";

inline constexpr size_t kHeaderBytes = 56;
inline constexpr size_t kEntryBytes = 160;
inline constexpr size_t kEntries = 120;
inline constexpr size_t kTotalBytes = kHeaderBytes + kEntries * kEntryBytes;  // 19256
inline constexpr size_t kCapacityOffset = 40;
inline constexpr size_t kDataSizeOffset = 8;
inline constexpr size_t kCountOffset = 48;
inline constexpr size_t kKindOffset = 72;   // within an entry
inline constexpr size_t kBulletOffset = 104;  // within an entry: the bullet instance's id (its first u64)
inline constexpr size_t kAssaultFirst = 0;
inline constexpr size_t kAssaultLast = 14;     // entries 0-14: the assault
inline constexpr size_t kRewriteFirst = 60;    // entries 60-119: magnum, smg, Chain, rifle
inline constexpr size_t kGroupEntries = 15;

/// The assault's bullet instance id, as the stock assault entries carry it. The rewrite is "the assault's id" read from
/// the buffer itself; this constant is the value the plugin's file uses, and the tests check the two agree.
inline constexpr uint64_t kAssaultBulletId = 0x12001003b8e9a2a6ULL;

enum class Verdict {
  Patched,   // the stock resource, with the four kinds' bullet ids rewritten
  NotStock,  // not the pinned bytes (another build, or already rewritten): left alone
  Refused,   // the pinned bytes, but the layout or the assault ids do not fit the rule: the stock bytes are served
};

inline const char* VerdictName(Verdict v) {
  switch (v) {
    case Verdict::Patched: return "patched";
    case Verdict::NotStock: return "not_stock";
    case Verdict::Refused: return "refused";
  }
  return "unknown";
}

struct Result {
  Verdict verdict = Verdict::NotStock;
  std::string reason;          // why it was refused; empty otherwise
  uint64_t assaultBulletId = 0;
  size_t rewritten = 0;        // entries whose bullet id changed
  std::vector<uint8_t> bytes;  // the rewritten resource, when verdict is Patched
};

namespace detail {

inline uint64_t ReadU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

inline void WriteU64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

}  // namespace detail

/// Decides what to serve for a buffer the loader delivered as CR15NetGunCR. `sha256Hex` is the buffer's digest; the rule
/// applies only when it equals kStockSha256. Every refusal names its reason and leaves the stock bytes in place.
inline Result Patch(const uint8_t* data, size_t size, const std::string& sha256Hex) {
  Result r;
  if (sha256Hex != kStockSha256) {
    r.verdict = Verdict::NotStock;
    return r;
  }
  auto refuse = [&r](const char* why) {
    r.verdict = Verdict::Refused;
    r.reason = why;
    r.bytes.clear();
    return r;
  };
  if (data == nullptr || size != kTotalBytes) return refuse("size");
  if (detail::ReadU64(data + kDataSizeOffset) != kEntries * kEntryBytes) return refuse("data_size");
  if (detail::ReadU64(data + kCapacityOffset) != kEntries) return refuse("capacity");
  if (detail::ReadU64(data + kCountOffset) != kEntries) return refuse("count");

  auto entry = [&](size_t i) { return data + kHeaderBytes + i * kEntryBytes; };

  // The assault's bullet id: every assault entry carries the same one, and it is not zero.
  const uint64_t assault = detail::ReadU64(entry(kAssaultFirst) + kBulletOffset);
  for (size_t i = kAssaultFirst; i <= kAssaultLast; ++i) {
    if (detail::ReadU64(entry(i) + kBulletOffset) != assault) return refuse("assault_ids_differ");
  }
  if (assault == 0) return refuse("assault_bullet_missing");

  // Each block of 15 entries is one weapon kind: its entries share one kind value.
  for (size_t g = 0; g < kEntries / kGroupEntries; ++g) {
    const uint64_t kind = detail::ReadU64(entry(g * kGroupEntries) + kKindOffset);
    for (size_t i = g * kGroupEntries; i < (g + 1) * kGroupEntries; ++i) {
      if (detail::ReadU64(entry(i) + kKindOffset) != kind) return refuse("kind_groups_differ");
    }
  }

  r.bytes.assign(data, data + size);
  r.assaultBulletId = assault;
  for (size_t i = kRewriteFirst; i < kEntries; ++i) {
    uint8_t* at = r.bytes.data() + kHeaderBytes + i * kEntryBytes + kBulletOffset;
    if (detail::ReadU64(at) == assault) continue;
    detail::WriteU64(at, assault);
    ++r.rewritten;
  }
  r.verdict = Verdict::Patched;
  return r;
}

/// Registers the transform for the five names (netgun_bullets.cpp). Client and server; logs its decision. Needs the
/// resource override registry (InstallResourceOverride) to exist.
void Install();

}  // namespace nevr_netgun_bullets
