#include "chassisfork.h"

#include <time.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "module_switch.h"
#include "nevr/symbol_hash.h"
#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {
namespace {

// The 32 Quest-only combat chassis bodies: the game's model id of each body's COMBAT Quest model. Same order as kSocialStock.
constexpr std::uint64_t kCombatStock[32] = {
    0x86a566d23f13a808ull,  // rwd_chassis_body_s11_a
    0x205aeac4c6944916ull,  // rwd_chassis_body_s10_a
    0x2c6fa08303810480ull,  // rwd_chassis_trex_s1_a
    0xa358a04872ec1310ull,  // rwd_chassis_mako_s1_a
    0x383f8343f802c860ull,  // rwd_chassis_s8b_a
    0xf2631c8f7e25d30cull,  // rwd_chassis_s11_flame_a
    0x48c932f102c40f28ull,  // rwd_chassis_s11_retro_a
    0x22c4661be38303a6ull,  // rwd_chassis_anubis_s2_a
    0x9d257d49fe751e54ull,  // rwd_chassis_automaton_s2_a
    0xb06fcccfd2d6690eull,  // rwd_chassis_herosuit_a
    0xd6f162e3d5c4587eull,  // rwd_chassis_samurai_a
    0x5153837673266c7eull,  // rwd_chassis_spartan_a
    0xc55405e8e394f934ull,  // rwd_chassis_sporty_a
    0xd3c5d07c343c4012ull,  // rwd_chassis_funk_a
    0x93dbf676ffe9e726ull,  // rwd_chassis_streetwear_a
    0xef40c46f18ad75c4ull,  // rwd_chassis_frost_a
    0xad99e41ff2b8cc76ull,  // rwd_chassis_wasteland_a
    0x43bb13e40e718808ull,  // rwd_chassis_shark_a
    0x7a72b040adfc309eull,  // rwd_chassis_junkyard_a
    0x0dcc645bacccb044ull,  // rwd_chassis_exo_a
    0x72d09fb639c7c844ull,  // rwd_chassis_scuba_a
    0xf9670d3a25198d7eull,  // rwd_chassis_wolf_a
    0x2cb918e3f136f172ull,  // rwd_chassis_noble_a
    0x71677b360637dad6ull,  // rwd_chassis_ranger_a
    0x726f83e2ca301444ull,  // rwd_chassis_overgrown_a
    0x979ad2f64503ab40ull,  // rwd_chassis_ninja_a
    0x4ce0f9ccd6c5966bull,  // rwd_chassis_samurai_a_oni
    0xe6705ddd2a0f4f60ull,  // rwd_chassis_trex_a_skelerex
    0x240d8e3641e4ee20ull,  // rwd_chassis_plagueknight_a
    0xeac22d7bf0defce7ull,  // rwd_chassis_spartan_a_hero
    0xae1e8593e9f7089bull,  // rwd_chassis_anubis_a_horus
    0x1a0dcb36399391a8ull,  // rwd_chassis_shark_a_tropical
};
// The same bodies' SOCIAL Quest models (the model a player wears in the lobby and the war room).
constexpr std::uint64_t kSocialStock[32] = {
    0xbd16ff8db4cf18b8ull, 0x964fd9deec588bd2ull, 0xfabcdd3874f6b6faull, 0x98ac69a0257e1700ull, 0x1094b0dc3c1a692cull,
    0x2e8400bfebbb8148ull, 0x363e24a1da17097aull, 0xb2b854e4a234ff94ull, 0x9e0aea794a1e1806ull, 0x2ff055d1473abaf6ull,
    0x27a54efa90b11c58ull, 0x86fb842f6c0544d8ull, 0xec8b9250f33fd90cull, 0xc59afe94b2d8bf00ull, 0xf0fbe86a532b08c4ull,
    0x8f69b19be193502eull, 0x8cbc9a72337b62a4ull, 0xd9eb714897aa0c88ull, 0x1304ff25862526ccull, 0xf49ade481455dcbcull,
    0x2945ef15d51ad8faull, 0x68b4815c4f1fe49aull, 0x4bd21814f06f5b42ull, 0x4ad001b403794826ull, 0xc869060098fc9e90ull,
    0xabf55e7d4dadc432ull, 0xa5db7afdc8fe9883ull, 0x5006f8ea272a07d4ull, 0x3731e57edff9b9bcull, 0x09e82c0d71b521e1ull,
    0x60990fd43cc362d5ull, 0xe6dbddbaf8ab6f76ull,
};

// Level ids (CSymbol64 of the level name). Combat: the four combat maps and their _lowspec, war_room, and the combat sublevels.
constexpr std::uint64_t kCombatLevels[] = {
    0x43e2da7914642604ull, 0xa61e1d106ed92f27ull, 0x42670f2bed45703cull, 0xd4de07c5c5826b6bull, 0xdf5ca7b7dfa383d4ull,
    0xcc011548ad1b0d39ull, 0x43e2da7a0c623a19ull, 0x64bd721fd4a497efull, 0x08a1af9e108def0bull, 0x555be9bcb4759006ull,
    0x555be9bcb4759007ull, 0xbe0ff249e3c43780ull, 0xbe0ff249e3c43783ull, 0x836c5b14ccc58201ull, 0x906c4707cbc28c16ull,
    0x907f461fccc0961dull, 0xf919f210bbda872cull, 0xe1a3ae700140d9d5ull, 0x61b0162adbd446ffull,
};
// Neutral parents: global_mp (0x3f9915d3001dc28e), global_root and the menu levels.
constexpr std::uint64_t kNeutralLevels[] = {
    0x3f9915d3001dc28eull, 0x9a3717b576c23967ull, 0x43934c379cf1e366ull, 0xbb75979f708e523bull, 0xac360e41e4ede056ull,
    0xf927772b9e2aefb1ull, 0x4d82118c7c91b6bbull, 0xe962a897e2cb8f07ull, 0x4d8208836e83bebaull, 0x46cf227cb0475934ull,
};
// The lobby: mpl_lobby_b2 and the two levels it loads with.
constexpr std::uint64_t kLobbyLevels[] = {0xd09afd15b1c75c04ull, 0xcb9977f7fc2b4526ull, 0x6daa00a6d33d44b7ull};

constexpr double kLookSeconds = 0.5;
constexpr int kSceneLines = 32;     // scene change lines, INFO, the first ones
constexpr int kSeenPerLevel = 64;   // ids logged once per level

// "<stock as 16 lowercase hex>" + suffix, hashed as the content pack names the fork.
constexpr std::uint64_t ForkId(std::uint64_t stock, char s0, char s1, char s2) {
  char name[21] = {};
  for (int i = 0; i < 16; ++i) {
    const unsigned nib = static_cast<unsigned>((stock >> (60 - 4 * i)) & 0xf);
    name[i] = static_cast<char>(nib < 10 ? '0' + nib : 'a' + nib - 10);
  }
  name[16] = '_';
  name[17] = s0;
  name[18] = s1;
  name[19] = s2;
  return EchoVR::CSymbol64Hash(name);
}

std::uintptr_t g_base = 0;
ChassisForkPins g_pins{};
std::atomic<std::uintptr_t> g_real{0};
bool g_lobby = true, g_social = true;  // set at install, before the slot is swapped
std::atomic<bool> g_scene_combat{false};
std::atomic<std::uint64_t> g_scene_map{0};
std::atomic<std::uint64_t> g_rewritten{0}, g_passed{0}, g_closed{0}, g_changes{0};

// game thread only
double g_next_look = 0;
std::uint64_t g_sig[4] = {};
bool g_pending = false;
int g_scene_lines = 0;
bool g_installed = false;

std::mutex g_seen_mu;  // once-per-(level, id) log lines
std::uint64_t g_seen_level = 0;
std::uint64_t g_seen[kSeenPerLevel];
int g_seen_n = 0;

double NowReal() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}
double (*g_now)() = &NowReal;

bool FirstOnLevel(std::uint64_t level, std::uint64_t id) {
  std::lock_guard<std::mutex> lock(g_seen_mu);
  if (level != g_seen_level) {
    g_seen_level = level;
    g_seen_n = 0;
  }
  for (int i = 0; i < g_seen_n; ++i)
    if (g_seen[i] == id) return false;
  if (g_seen_n < kSeenPerLevel) g_seen[g_seen_n++] = id;
  return true;
}

void Hex(char* out, std::size_t n, std::uint64_t v) { std::snprintf(out, n, "%016llx", static_cast<unsigned long long>(v)); }

bool ForkPresent(std::uint64_t fork) {
  const std::uintptr_t global = g_base + g_pins.res_mgr;
  return PackageHas(global, kTypeScene, fork) && PackageHas(global, kTypeMeshList, fork) && PackageHas(global, kTypeArchive, fork);
}

using SwapFn = std::uint64_t (*)(std::uintptr_t, std::uint64_t, std::uint64_t);

// CModelSwapperCS::BeginModelSwap(this, SCompactPoolHandle, CSymbol64 target): once per model swap.
std::uint64_t SwapWrapper(std::uintptr_t self, std::uint64_t handle, std::uint64_t target) {
  const auto real = reinterpret_cast<SwapFn>(g_real.load(std::memory_order_acquire));
  const std::uint64_t fork = ForkFor(target, g_social);
  if (fork == 0) return real(self, handle, target);
  const bool combat = g_scene_combat.load(std::memory_order_acquire);
  const std::uint64_t level = combat ? g_scene_map.load(std::memory_order_relaxed) : 0;
  char t[24], fk[24], lv[24];
  Hex(t, sizeof t, target);
  Hex(fk, sizeof fk, fork);
  Hex(lv, sizeof lv, level);
  std::uint64_t use = target;
  if (!combat) {
    g_passed.fetch_add(1, std::memory_order_relaxed);
    if (FirstOnLevel(level, target)) {
      const NevrLogField f[] = {{"target", t}, {"scene", "off"}};
      Log(NEVR_LOG_DEBUG, "chassisfork: stock body kept outside a combat scene", f, 2);
    }
  } else if (!ForkPresent(fork)) {
    g_closed.fetch_add(1, std::memory_order_relaxed);
    if (FirstOnLevel(level, target)) {
      const NevrLogField f[] = {{"target", t}, {"fork", fk}, {"level", lv}};
      Log(NEVR_LOG_WARNING, "chassisfork: fork body not in the installed content pack; the stock body loads without the scanner outline", f, 3);
    }
  } else {
    use = fork;
    const std::uint64_t n = g_rewritten.fetch_add(1, std::memory_order_relaxed) + 1;
    if (FirstOnLevel(level, target)) {
      char cnt[24];
      std::snprintf(cnt, sizeof cnt, "%llu", static_cast<unsigned long long>(n));
      const NevrLogField f[] = {{"target", t}, {"fork", fk}, {"level", lv}, {"rewrites", cnt}};
      Log(NEVR_LOG_INFO, "chassisfork: combat body loads as its fork, so the threat scanner can outline it", f, 4);
    }
  }
  return real(self, handle, use);
}

InstallResult Refuse(const char* reason, const char* detail, SwapStatus status) {
  InstallResult r;
  r.status = status;
  r.reason = reason;
  const NevrLogField f[] = {{"reason", reason}, {"detail", detail}, {"status", StatusName(status)}};
  Log(NEVR_LOG_ERROR, "chassisfork refused: nothing changed, every body loads stock", f, 3);
  return r;
}

}  // namespace

std::uint64_t ForkIdCombat(std::uint64_t stock) { return ForkId(stock, 'c', 'b', 't'); }
std::uint64_t ForkIdSocial(std::uint64_t stock) { return ForkId(stock, 's', 'o', 'c'); }

LevelKind ClassifyLevel(std::uint64_t level, bool lobby_counts) {
  for (const std::uint64_t c : kCombatLevels)
    if (level == c) return LevelKind::kCombat;
  for (const std::uint64_t c : kNeutralLevels)
    if (level == c) return LevelKind::kNeutral;
  if (lobby_counts)
    for (const std::uint64_t c : kLobbyLevels)
      if (level == c) return LevelKind::kLobby;
  return LevelKind::kOther;
}

bool SceneIsCombat(const std::uint64_t* levels, int n, bool read_failed, bool lobby_counts) {
  if (read_failed) return false;
  bool match = false;
  for (int i = 0; i < n; ++i) {
    const LevelKind k = ClassifyLevel(levels[i], lobby_counts);
    if (k == LevelKind::kOther) return false;
    match = match || k == LevelKind::kCombat || k == LevelKind::kLobby;
  }
  return match;
}

std::uint64_t ForkFor(std::uint64_t target, bool social) {
  for (const std::uint64_t s : kCombatStock)
    if (s == target) return ForkIdCombat(s);
  if (social)
    for (const std::uint64_t s : kSocialStock)
      if (s == target) return ForkIdSocial(s);
  return 0;
}

bool PackageHas(std::uintptr_t res_mgr_global, std::uint64_t type, std::uint64_t id) {
  std::uintptr_t mgr = 0, db = 0, rows = 0;
  std::uint64_t count = 0;
  if (!SafeRead(res_mgr_global, &mgr, sizeof mgr) || mgr == 0) return false;
  if (!SafeRead(mgr + kResDb, &db, sizeof db) || db == 0) return false;
  if (!SafeRead(db + kPkgCount, &count, sizeof count) || !SafeRead(db + kPkgRows, &rows, sizeof rows)) return false;
  if (rows == 0 || count == 0 || count > kPkgMaxRows) return false;
  const auto kt = static_cast<std::int64_t>(type), ki = static_cast<std::int64_t>(id);
  std::uint64_t lo = 0, hi = count;
  while (lo < hi) {  // the order LookupResourcePath searches in: (type, id) as signed 64-bit values
    const std::uint64_t mid = lo + (hi - lo) / 2;
    std::int64_t key[2];
    if (!SafeRead(rows + mid * kPkgRow, key, sizeof key)) return false;
    if (key[0] == kt && key[1] == ki) return true;
    if (key[0] < kt || (key[0] == kt && key[1] < ki))
      lo = mid + 1;
    else
      hi = mid;
  }
  return false;
}

void ChassisForkTick(std::uintptr_t game) {
  if (!g_installed || game == 0) return;
  std::uint64_t sig[4];
  bool ok = true;
  for (int t = 0; t < 2; ++t) {
    sig[2 * t] = Rd<std::uint64_t>(game + kGameTables[t][0], &ok);
    sig[2 * t + 1] = Rd<std::uint64_t>(game + kGameTables[t][1], &ok);
  }
  const double now = g_now();
  const bool moved = !ok || std::memcmp(sig, g_sig, sizeof sig) != 0;
  if (!moved && now < g_next_look) return;
  std::memcpy(g_sig, sig, sizeof sig);
  g_next_look = now + kLookSeconds;

  std::uint64_t levels[kMaxLevels];
  int n = 0;
  bool failed = !ok;
  for (int t = 0; t < 2 && !failed; ++t) {
    const std::uintptr_t tab = sig[2 * t];
    const std::uint64_t cnt = sig[2 * t + 1];
    if (tab == 0 || cnt == 0) continue;
    if (cnt > static_cast<std::uint64_t>(kMaxLevels)) {
      failed = true;
      break;
    }
    for (std::uint64_t i = 0; i < cnt && !failed; ++i) {
      bool r = true;
      const auto gs = Rd<std::uintptr_t>(tab + 16 * i + 8, &r);
      if (r && gs == 0) continue;
      const auto owner = r ? Rd<std::uintptr_t>(gs + kGsGame, &r) : 0;
      if (r && owner != game) continue;
      const auto level = r ? Rd<std::uint64_t>(gs + kGsLevel, &r) : 0;
      if (!r || n >= kMaxLevels) {
        failed = true;
        break;
      }
      levels[n++] = level;
    }
  }
  const bool look = SceneIsCombat(levels, n, failed, g_lobby);
  const bool was = g_scene_combat.load(std::memory_order_relaxed);
  bool now_combat = was;
  if (!look) {
    now_combat = false;  // off at once
    g_pending = false;
  } else if (was) {
    g_pending = false;
  } else if (g_pending) {
    now_combat = true;  // the second combat look in a row
    g_pending = false;
  } else {
    g_pending = true;
  }
  if (now_combat) {
    std::uint64_t map = 0;
    for (int i = 0; i < n && map == 0; ++i) {
      const LevelKind k = ClassifyLevel(levels[i], g_lobby);
      if (k == LevelKind::kCombat || k == LevelKind::kLobby) map = levels[i];
    }
    g_scene_map.store(map, std::memory_order_relaxed);
  }
  if (now_combat != was) {
    g_scene_combat.store(now_combat, std::memory_order_release);
    g_changes.fetch_add(1, std::memory_order_relaxed);
    if (g_scene_lines++ < kSceneLines) {
      char cnt[24], map[24];
      std::snprintf(cnt, sizeof cnt, "%d", n);
      Hex(map, sizeof map, g_scene_map.load(std::memory_order_relaxed));
      const NevrLogField f[] = {{"from", was ? "combat" : "off"}, {"to", now_combat ? "combat" : "off"}, {"levels", cnt},
                                {"map", now_combat ? map : "none"}, {"read_failed", failed ? "true" : "false"}};
      Log(NEVR_LOG_INFO, now_combat ? "chassisfork: combat scene, chassis bodies load as their outlined forks"
                                    : "chassisfork: scene off, chassis bodies load stock",
          f, 5);
    }
  }
}

InstallResult InstallChassisForkAt(std::uintptr_t base, const ChassisForkPins& pins, const WordPin* words, std::size_t n_words) {
  if (ModuleSwitchPresent("chassisfork.off")) {
    const NevrLogField f[] = {{"why", "chassisfork.off present in the q15t data directory"}};
    Log(NEVR_LOG_INFO, "chassisfork off: every chassis body loads stock", f, 1);
    InstallResult r;
    r.reason = "off_file";
    return r;
  }
  if (base == 0) return Refuse("no_base", "", SwapStatus::kNullSlot);
  if (!CheckWordPins("chassisfork", base, words, n_words, "nothing changed, every body loads stock")) {
    InstallResult r;
    r.status = SwapStatus::kUnexpectedOriginal;
    r.reason = "word_pin";
    return r;
  }
  g_base = base;
  g_pins = pins;
  g_lobby = !ModuleSwitchPresent("chassisfork_lobby.off");
  g_social = !ModuleSwitchPresent("chassisfork_soc.off");
  const InstallResult r = InstallImportHook("_ZN10NRadEngine15CModelSwapperCS14BeginModelSwapENS_18SCompactPoolHandleENS_9CSymbol64E", base,
                                            pins.swap_slot, reinterpret_cast<std::uintptr_t>(&SwapWrapper), &g_real, pins.swap_fn);
  char slot[24], real[24];
  std::snprintf(slot, sizeof slot, "0x%llx", static_cast<unsigned long long>(pins.swap_slot));
  std::snprintf(real, sizeof real, "0x%llx", static_cast<unsigned long long>(pins.swap_fn));
  const NevrLogField f[] = {{"event", "q15t_chassisfork_install"}, {"hook", "CModelSwapperCS::BeginModelSwap"}, {"slot_vaddr", slot},
                            {"real_vaddr", real}, {"status", StatusName(r.status)}, {"reason", r.reason},
                            {"lobby", g_lobby ? "on" : "off (chassisfork_lobby.off)"}, {"social", g_social ? "on" : "off (chassisfork_soc.off)"}};
  if (r.status != SwapStatus::kOk) {
    Log(NEVR_LOG_ERROR, "chassisfork hook refused: every body loads stock", f, 8);
    return r;
  }
  g_installed = true;
  Log(NEVR_LOG_INFO, "chassisfork on: in a combat scene the 32 combat chassis bodies load as their outlined forks", f, 8);
  return r;
}

InstallResult InstallChassisFork(std::uintptr_t base) {
  return InstallChassisForkAt(base, kChassisForkPinned, kChassisForkWords, sizeof kChassisForkWords / sizeof kChassisForkWords[0]);
}

ChassisForkCounters ChassisForkGetCounters() { return {g_rewritten.load(), g_passed.load(), g_closed.load(), g_changes.load()}; }
bool ChassisForkSceneIsCombat() { return g_scene_combat.load(); }

void ResetChassisForkForTest() {
  g_real.store(0);
  g_scene_combat.store(false);
  g_scene_map.store(0);
  g_rewritten = g_passed = g_closed = g_changes = 0;
  g_next_look = 0;
  std::memset(g_sig, 0, sizeof g_sig);
  g_pending = false;
  g_scene_lines = 0;
  g_installed = false;
  g_seen_level = 0;
  g_seen_n = 0;
}
void SetChassisForkClockForTest(double (*now)()) { g_now = now != nullptr ? now : &NowReal; }
std::uintptr_t ChassisForkWrapperForTest() { return reinterpret_cast<std::uintptr_t>(&SwapWrapper); }

}  // namespace q15t
