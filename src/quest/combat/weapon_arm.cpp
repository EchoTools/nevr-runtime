#include "quest/combat/weapon_arm.h"

#include <ctype.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/gamespace.h"
#include "quest/combat/gamespace_targets.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/netgame_targets.h"
#include "quest/combat/safe_read.h"
#include "quest/combat/weapon_arm_targets.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::weapon_arm {

namespace {

namespace wt = targets;
constexpr double kEveryS = 1.0, kVerifyS = 10.0;
constexpr int kHold = 3, kMaxCs = 8;

Fns g_f{};
std::uintptr_t g_base = 0;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_done{false};
std::atomic<int> g_state{0};  // 0 looking, 1 set sent
double g_next = 0, g_setAt = 0;
int g_hold = 0;
std::uintptr_t g_cs = 0;
std::uint64_t g_ph = 0;
char g_id[96] = "", g_marker[420] = "", g_why[96] = "";
double (*g_clock)() = nullptr;
std::atomic<std::uint32_t> g_passes{0}, g_scopedOut{0};

double Now() noexcept {
  if (g_clock != nullptr) return g_clock();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

void* DlLookup(const char* symbol) noexcept {
  void* const lib = dlopen("libr15.so", RTLD_NOW | RTLD_NOLOAD);
  return lib != nullptr ? dlsym(lib, symbol) : nullptr;
}

// The player's data directory: /sdcard/Android/data/<package>/files/q15t (legacy_switch.h), made when absent. A directory a test names is used as it is.
bool DataDir(char* out, std::size_t n) noexcept {
  if (!legacy::Directory(out, n)) return false;
  if (access(out, W_OK) != 0 && !legacy::g_dirSet.load(std::memory_order_acquire)) mkdir(out, 0770);
  return access(out, W_OK) == 0;
}

const char* Tail(const char* id) noexcept {
  const std::size_t n = std::strlen(id);
  return n > 4 ? id + n - 4 : id;
}

void Waiting(const char* why) noexcept {  // one line per distinct reason while looking
  if (std::strcmp(why, g_why) == 0) return;
  std::snprintf(g_why, sizeof(g_why), "%s", why);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_weapon_arm_waiting",
                      {{"why", why}, {"msg", "the gun hand is not set yet; nothing is called until the game has what it needs"}});
}

void WriteMarker(const char* outcome, std::uint32_t arm, long long profArm) noexcept {
  bool wrote = false;
  const int fd = open(g_marker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
  if (fd >= 0) {
    char line[96];
    const int len = std::snprintf(line, sizeof(line), "weaponarm %u profile %lld %s\n", arm, profArm, outcome);
    wrote = len > 0 && write(fd, line, static_cast<std::size_t>(len)) == len;
    close(fd);
  }
  if (!wrote) {  // loud: without the marker the set repeats on every launch
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_weapon_arm_marker_failed",
                        {{"id_tail", Tail(g_id)}, {"outcome", outcome},
                         {"msg", "the marker could not be written, so the gun hand is set again on the next launch"}});
  }
  sentinel::LogFields(wrote ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_weapon_arm_done",
                      {{"outcome", outcome}, {"marker", wrote ? "written" : "write_failed"}, {"id_tail", Tail(g_id)}, {"arm", arm},
                       {"profile_weaponarm", profArm}});
  g_done.store(true, std::memory_order_release);
}

void Step(std::uintptr_t game, double now) noexcept {
  if (now < g_next) return;
  g_next = now + kEveryS;
  g_passes.fetch_add(1, std::memory_order_relaxed);
  void* const net = g_f.netGame(reinterpret_cast<void*>(game));
  void* const prof = net != nullptr ? g_f.clientProfile(net) : nullptr;
  const char* const id = prof != nullptr ? g_f.tString(prof, "xplatformid", nullptr, 0) : nullptr;
  if (id == nullptr || *id == 0) {
    Waiting(net == nullptr ? "no netgame" : prof == nullptr ? "no client profile" : "profile has no xplatformid");
    g_hold = 0;
    return;
  }
  if (std::strcmp(id, g_id) != 0) {  // a (new) login: start over for this id
    char safe[96];
    std::size_t k = 0;
    for (const char* p = id; *p != 0 && k < sizeof(safe) - 1; ++p) {
      safe[k++] = (isalnum(static_cast<unsigned char>(*p)) || *p == '-' || *p == '_') ? *p : '_';
    }
    safe[k] = 0;
    char dir[300];
    if (!DataDir(dir, sizeof(dir))) {
      Waiting("no writable data directory");
      return;
    }
    std::snprintf(g_id, sizeof(g_id), "%s", id);
    std::snprintf(g_marker, sizeof(g_marker), "%s/t26arm.%s", dir, safe);
    g_state.store(0);
    g_hold = 0;
    if (access(g_marker, F_OK) == 0) {
      sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_weapon_arm_done",
                          {{"outcome", "marker present: nothing changed"}, {"marker", "present"}, {"id_tail", Tail(g_id)}, {"arm", -1},
                           {"profile_weaponarm", -1}});
      g_done.store(true, std::memory_order_release);
      return;
    }
  }
  // Arena shares the APK: nothing below this line runs unless a combat map is loaded and no Echo Arena level is, as the combat level answer last saw it
  // (combat_scope.h `InCombatMap`: a load and a test on an arena pass, no read of the game; a look older than 1 s is not believed). A combat map only: the
  // combat lobby is not in scope, because the lobby script is shared with the arena lobby and it is not established that an arena player never has
  // mpl_lobby_b_combat loaded (docs/quest/combat/headset_gates.md). The hand is only meaningful in a match.
  if (!scope::InCombatMap()) {
    g_scopedOut.fetch_add(1, std::memory_order_relaxed);
    g_hold = 0;
    return;
  }
  gamespace::CompSys found[kMaxCs];
  const int n = gamespace::FindComponentSystems(game, wt::kEquipCsKey, g_base + wt::kEquipCsVptrVaddr, found, kMaxCs);
  std::uintptr_t cs = 0;
  std::uint64_t ph = 0;
  for (int i = 0; i < n && cs == 0; ++i) {
    bool ok = true;
    const auto slot = SafeRead<std::int16_t>(found[i].cs + wt::kCsSlot, &ok);
    const auto tab = SafeRead<std::uintptr_t>(found[i].cs + wt::kCsGenerations, &ok);
    if (!ok || slot < 0 || tab == 0) continue;
    const auto gen = SafeRead<std::uint16_t>(tab + static_cast<std::uint64_t>(static_cast<std::uint16_t>(slot)) * 4 + 2, &ok);
    const std::uint64_t h = static_cast<std::uint64_t>(static_cast<std::uint16_t>(slot)) | static_cast<std::uint64_t>(gen) << 16;
    if (ok && g_f.isValid(reinterpret_cast<void*>(found[i].cs), h)) {
      cs = found[i].cs;
      ph = h;
    }
  }
  if (cs == 0) {
    Waiting(n != 0 ? "no local equipment component yet" : "no equipment CS yet");
    g_hold = 0;
    return;
  }
  const std::uint32_t arm = g_f.getArm(reinterpret_cast<void*>(cs), ph);
  const long long profArm = g_f.jsonInt(prof, "weaponarm", -1, 0);
  if (g_state.load() == 1) {
    if (arm == 1) {
      WriteMarker("weaponarm now 1", arm, profArm);
      return;
    }
    if (now - g_setAt > kVerifyS) {
      sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_weapon_arm_gave_up",
                          {{"id_tail", Tail(g_id)}, {"arm", arm}, {"profile_weaponarm", profArm},
                           {"msg", "weaponarm did not read 1 after the set; no marker is written and the next launch retries"}});
      g_done.store(true, std::memory_order_release);
    }
    return;
  }
  if (cs != g_cs || ph != g_ph) {
    g_cs = cs;
    g_ph = ph;
    g_hold = 0;
  }
  if (++g_hold < kHold) return;
  if (arm == 1) {
    WriteMarker("weaponarm right already, nothing changed", arm, profArm);
    return;
  }
  // The rule is the reference build's (T-26): a profile whose `weaponarm` is 1 (right) gets the marker only; every other profile, `weaponarm` 0 and an absent key
  // alike (CJson::Int answers -1 for an absent key), is set to the right hand once per missing marker. The service sends `weaponarm` for every profile and an
  // account that never chose reads 0, so "never chose" and "chose left" are the same value here: a left-handed player's gun is moved to the right once per missing
  // marker (a reinstall, cleared app data, a switch between the store and combat builds). SetWeaponArm is stored by SavePlayerEquipment into the profile's
  // `weaponarm` key, so a set here is a change to the saved profile.
  if (profArm == 1) {
    WriteMarker("profile weaponarm is 1, nothing changed", arm, profArm);
    return;
  }
  bool ok = true;
  const auto gs = SafeRead<std::uintptr_t>(cs + wt::kCsGamespace, &ok);
  const int direct = ok && gs != 0 ? SafeRead<int>(gs + wt::kGamespaceDirect, &ok) : 0;
  if (!ok || gs == 0) {
    Waiting("gamespace unreadable");
    return;
  }
  if (direct != 0) {
    g_f.setArm(reinterpret_cast<void*>(cs), ph & 0xffffffffull, 1);
  } else {  // the deferral the station's hand swap uses: SFunctor2<void, CS, SCompactPoolHandle, ull> {CS, &SetWeaponArm adj 0, ph, arg}
    char* const q = static_cast<char*>(g_f.queue(reinterpret_cast<void*>(gs + wt::kGamespaceQueue), reinterpret_cast<void (*)(void*, char*)>(g_f.invoke)));
    // QueueFunctor never returns null: it traps when the queue is full (0x24acd4c), so this branch is dead code kept as a guard, not a survival of a full queue.
    if (q == nullptr) {
      Waiting("deferred queue returned no slot");
      return;
    }
    *reinterpret_cast<void**>(q) = reinterpret_cast<void*>(cs);
    *reinterpret_cast<std::uint32_t*>(q + 0x18) = static_cast<std::uint32_t>(ph);
    *reinterpret_cast<std::uint64_t*>(q + 0x20) = 1;
    *reinterpret_cast<void**>(q + 0x08) = reinterpret_cast<void*>(g_f.setArm);
    *reinterpret_cast<std::int64_t*>(q + 0x10) = 0;
  }
  g_state.store(1);
  g_setAt = now;
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_weapon_arm_set",
                      {{"route", direct != 0 ? "direct" : "deferred"}, {"id_tail", Tail(g_id)}, {"arm", arm}, {"profile_weaponarm", profArm},
                       {"msg", "weaponarm -> 1 (one-shot): the profile does not hold the right hand (weaponarm 0 or absent), so the gun goes to the right hand"}});
}

ModuleResult Refused(const char* reason, const char* detail) noexcept {
  g_installed.store(false, std::memory_order_release);
  sentinel::LogFields(sentinel::LogLevel::kError, "combat_weapon_arm_install",
                      {{"result", "refused"}, {"reason", reason}, {"detail", detail},
                       {"msg", "the gun hand is not set by the runtime: the game keeps what the profile holds"}});
  return {false, reason};
}

void WarnLegacySwitch() noexcept {
  if (!legacy::Present("t26arm.off")) return;
  sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_weapon_arm_legacy_switch",
                      {{"file", "t26arm.off"}, {"replaced_by", "combat.weapon_arm"},
                       {"msg", "the plugin's kill file is no longer read and changes nothing; combat.weapon_arm in config.yaml decides (on unless it says false)"}});
}

bool PickOne(SymbolLookup lookup, const pinned::Symbol& pin, std::uintptr_t base, std::uintptr_t* out, int* named, const char** why) noexcept {
  void* const p = lookup != nullptr ? lookup(pin.symbol) : nullptr;
  if (p != nullptr && reinterpret_cast<std::uintptr_t>(p) != base + pin.vaddr) {
    *why = pin.symbol;
    return false;
  }
  if (p != nullptr) ++*named;
  *out = base + pin.vaddr;
  return true;
}

}  // namespace

bool Resolve(std::uintptr_t base, SymbolLookup lookup, Fns* out, const char** why) noexcept {
  Fns f{};
  std::uintptr_t a[9] = {};
  const pinned::Symbol* const pins[9] = {&netgame::kNetGameMTPin, &wt::kClientProfile, &wt::kTString, &wt::kJsonInt, &wt::kIsValid,
                                  &wt::kGetArm,           &wt::kSetArm,         &wt::kInvoke,  &wt::kQueue};
  for (int i = 0; i < 9; ++i) {
    if (!PickOne(lookup, *pins[i], base, &a[i], &f.named, why)) return false;
  }
  f.netGame = reinterpret_cast<void* (*)(void*)>(a[0]);
  f.clientProfile = reinterpret_cast<void* (*)(void*)>(a[1]);
  f.tString = reinterpret_cast<const char* (*)(void*, const char*, const char*, unsigned)>(a[2]);
  f.jsonInt = reinterpret_cast<long long (*)(void*, const char*, long long, unsigned)>(a[3]);
  f.isValid = reinterpret_cast<bool (*)(void*, std::uint64_t)>(a[4]);
  f.getArm = reinterpret_cast<std::uint32_t (*)(void*, std::uint64_t)>(a[5]);
  f.setArm = reinterpret_cast<void (*)(void*, std::uint64_t, std::uint64_t)>(a[6]);
  f.invoke = reinterpret_cast<void (*)(void*, char*)>(a[7]);
  f.queue = reinterpret_cast<void* (*)(void*, void (*)(void*, char*))>(a[8]);
  *out = f;
  return true;
}

ModuleResult InstallResolved(std::uintptr_t base, const Fns& fns) noexcept {
  g_base = base;
  g_f = fns;
  g_next = 0;
  if (!tick::Add(&Tick)) return Refused("tick_table_full", "");
  g_installed.store(true, std::memory_order_release);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_weapon_arm_install",
                      {{"result", "installed"}, {"reason", "ok"}, {"named", fns.named},
                       {"msg", "a profile with no marker whose weaponarm is not 1 gets the gun in the right hand once, in a combat level; it installs no hook"}});
  return {true, "installed"};
}

ModuleResult InstallAt(sentinel::ImageLookup lookup, SymbolLookup symbols, bool on) noexcept {
  WarnLegacySwitch();
  if (!on) {
    g_installed.store(false, std::memory_order_release);
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_weapon_arm_install",
                        {{"result", "off"}, {"reason", "off_config"}, {"switch", "combat.weapon_arm"},
                         {"msg", "combat.weapon_arm is false: the gun hand stays as the game gives it"}});
    return {false, "off_config", true};
  }
  // The pass acts only while the combat level answer says a combat map is loaded: when that answer cannot change (the tick or the scope row's refresh is
  // not installed) the module would never act, so it is not installed (combat_scope.h `Live`).
  if (!scope::Live()) return Refused("scope_not_live", "");
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) return Refused("module_not_loaded", "");
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused("no_build_id", "");
  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return Refused("build_id_mismatch", "");
  const pinned::Pins lists[] = {wt::kPins, netgame::kPins, gamespace::targets::kPins};
  pinned::Failure failure;
  if (pinned::FirstFailure(image.base, lists, sizeof(lists) / sizeof(lists[0]), &failure)) {
    char at[19], haveText[19], wantText[19];
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_weapon_arm_pinned_word",
                        {{"vaddr", sentinel::HexString(at, failure.vaddr)}, {"have", failure.readable ? sentinel::HexString(haveText, failure.have) : "unreadable"},
                         {"want", sentinel::HexString(wantText, failure.want)}, {"what", failure.what}});
    return Refused("pinned_word_mismatch", failure.what);
  }
  Fns fns{};
  const char* why = "";
  if (!Resolve(image.base, symbols, &fns, &why)) return Refused("export_elsewhere", why);
  return InstallResolved(image.base, fns);
}

ModuleResult Install(bool on) noexcept { return InstallAt(&sentinel::FindLoadedImage, &DlLookup, on); }

void Tick(std::uintptr_t game) noexcept {
  if (!g_installed.load(std::memory_order_acquire) || g_done.load(std::memory_order_acquire)) return;
  Step(game, Now());
}

int Phase() noexcept { return g_done.load() || !g_installed.load() ? 2 : g_state.load(); }

Stats CurrentStats() noexcept { return {g_passes.load(), g_scopedOut.load()}; }

void SetClockForTest(double (*clock)()) noexcept { g_clock = clock; }
void SetLegacyDirForTest(const char* dir) noexcept { legacy::SetDirectoryForTest(dir); }

void ResetForTest() noexcept {
  g_f = Fns{};
  g_base = 0;
  g_installed.store(false);
  g_done.store(false);
  g_state.store(0);
  g_next = g_setAt = 0;
  g_hold = 0;
  g_cs = 0;
  g_ph = 0;
  g_id[0] = g_marker[0] = g_why[0] = 0;
  g_passes.store(0);
  g_scopedOut.store(0);
}

}  // namespace nevr_quest::combat::weapon_arm
