#include "weapon_arm.h"

#include <ctype.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "gamespace.h"
#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {
namespace {

constexpr std::uint64_t kEquipCsKey = 0xca14940c84b03f6eull;  // R14NetPlayerEquipment (CR15NetPlayerEquipmentCS::DefaultName)
constexpr std::uintptr_t kEquipCsVptr = 0x3698200;            // vtable 0x36981f0 + 0x10 (ELF vaddr)
constexpr double kEveryS = 1.0, kVerifyS = 10.0;
constexpr int kHold = 3, kMaxCs = 8;

WeaponArmFns g_f{};
bool g_have_fns = false;
bool g_init = false;
bool g_done = false;
std::uintptr_t g_base = 0;
int g_state = 0;  // 0 looking, 1 set sent
double g_next = 0, g_set_at = 0;
int g_hold = 0;
std::uintptr_t g_cs = 0;
std::uint64_t g_ph = 0;
char g_id[96] = "", g_marker[400] = "", g_why[96] = "", g_dir_override[256] = "";
double (*g_clock)() = nullptr;

double Now() {
  if (g_clock != nullptr) return g_clock();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

// The plugin's data directory: the app's own external files dir, /sdcard/Android/data/<pkg>/files/q15t (made when absent).
bool DataDir(char* out, std::size_t n) {
  if (g_dir_override[0] != 0) {
    std::snprintf(out, n, "%s", g_dir_override);
    return access(out, W_OK) == 0;
  }
  char cmd[256] = "";
  FILE* f = std::fopen("/proc/self/cmdline", "re");
  if (f == nullptr) return false;
  const std::size_t r = std::fread(cmd, 1, sizeof cmd - 1, f);
  std::fclose(f);
  cmd[r] = 0;
  if (char* colon = std::strchr(cmd, ':')) *colon = 0;
  if (std::strchr(cmd, '.') == nullptr || std::strchr(cmd, '/') != nullptr || std::strlen(cmd) > 160) return false;
  std::snprintf(out, n, "/sdcard/Android/data/%s/files/q15t", cmd);
  if (access(out, W_OK) != 0) mkdir(out, 0770);
  return access(out, W_OK) == 0;
}

const char* Tail(const char* id) {
  const std::size_t n = std::strlen(id);
  return n > 4 ? id + n - 4 : id;
}

void LogT26(std::uint32_t level, const char* message, const char* why, const char* id_tail, long long arm, long long prof_arm,
            const char* extra) {
  char a[24], p[24];
  std::snprintf(a, sizeof a, "%lld", arm);
  std::snprintf(p, sizeof p, "%lld", prof_arm);
  const NevrLogField f[] = {{"why", why}, {"id_tail", id_tail}, {"arm", a}, {"profile_weaponarm", p}, {"detail", extra}};
  Log(level, message, f, 5);
}

void Waiting(const char* why) {  // one line per distinct reason while looking
  if (std::strcmp(why, g_why) == 0) return;
  std::snprintf(g_why, sizeof g_why, "%s", why);
  LogT26(NEVR_LOG_INFO, "t26 waiting", why, "", -1, -1, "");
}

void WriteMarker(const char* what, std::uint32_t arm, long long prof_arm) {
  bool wrote = false;
  const int fd = open(g_marker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
  if (fd >= 0) {
    char line[96];
    const int len = std::snprintf(line, sizeof line, "weaponarm %u profile %lld %s\n", arm, prof_arm, what);
    wrote = len > 0 && write(fd, line, static_cast<std::size_t>(len)) == len;
    close(fd);
  }
  // loud when the marker cannot be written: without it the set repeats on every launch
  LogT26(wrote ? NEVR_LOG_INFO : NEVR_LOG_ERROR, wrote ? "t26 done" : "t26 marker WRITE FAILED", what, Tail(g_id), arm, prof_arm,
         g_marker);
  g_done = true;
}

void Step(std::uintptr_t game, double now) {
  if (now < g_next) return;
  g_next = now + kEveryS;
  if (!g_init) {
    g_init = true;
    char dir[256];
    if (!g_have_fns && !ResolveWeaponArmFns(&g_f)) {
      LogT26(NEVR_LOG_ERROR, "t26 off", "an export is missing (not the pinned build?)", "", -1, -1, "");
      g_done = true;
      return;
    }
    g_have_fns = true;
    if (DataDir(dir, sizeof dir)) {
      char off[300];
      std::snprintf(off, sizeof off, "%s/t26arm.off", dir);
      if (access(off, F_OK) == 0) {
        LogT26(NEVR_LOG_INFO, "t26 off", "t26arm.off present", "", -1, -1, off);
        g_done = true;
        return;
      }
    }
    LogT26(NEVR_LOG_INFO, "t26 on", "one-shot weaponarm 1 per new profile", "", -1, -1, "");
  }
  void* net = g_f.net_game(reinterpret_cast<void*>(game));
  void* prof = net != nullptr ? g_f.client_profile(net) : nullptr;
  const char* id = prof != nullptr ? g_f.t_string(prof, "xplatformid", nullptr, 0) : nullptr;
  if (id == nullptr || *id == 0) {
    Waiting(net == nullptr ? "no netgame" : prof == nullptr ? "no client profile" : "profile has no xplatformid");
    g_hold = 0;
    return;
  }
  if (std::strcmp(id, g_id) != 0) {  // a (new) login: start over for this id
    char safe[96];
    std::size_t k = 0;
    for (const char* p = id; *p != 0 && k < sizeof safe - 1; ++p)
      safe[k++] = (isalnum(static_cast<unsigned char>(*p)) || *p == '-' || *p == '_') ? *p : '_';
    safe[k] = 0;
    char dir[256];
    if (!DataDir(dir, sizeof dir)) {
      Waiting("no writable data directory");
      return;
    }
    std::snprintf(g_id, sizeof g_id, "%s", id);
    std::snprintf(g_marker, sizeof g_marker, "%s/t26arm.%s", dir, safe);
    g_state = 0;
    g_hold = 0;
    if (access(g_marker, F_OK) == 0) {
      LogT26(NEVR_LOG_INFO, "t26 done", "marker present: profile never touched", Tail(g_id), -1, -1, g_marker);
      g_done = true;
      return;
    }
  }
  CompSys found[kMaxCs];
  const int n = FindComponentSystems(game, kEquipCsKey, g_base + kEquipCsVptr, found, kMaxCs);
  std::uintptr_t cs = 0;
  std::uint64_t ph = 0;
  for (int i = 0; i < n && cs == 0; ++i) {
    bool ok = true;
    const auto slot = Rd<std::int16_t>(found[i].cs + 0x130, &ok);
    const auto tab = Rd<std::uintptr_t>(found[i].cs + 0xc8, &ok);
    if (!ok || slot < 0 || tab == 0) continue;
    const auto gen = Rd<std::uint16_t>(tab + static_cast<std::uint64_t>(static_cast<std::uint16_t>(slot)) * 4 + 2, &ok);
    const std::uint64_t h = static_cast<std::uint64_t>(static_cast<std::uint16_t>(slot)) | static_cast<std::uint64_t>(gen) << 16;
    if (ok && g_f.is_valid(reinterpret_cast<void*>(found[i].cs), h)) {
      cs = found[i].cs;
      ph = h;
    }
  }
  if (cs == 0) {
    Waiting(n != 0 ? "no local equipment component yet" : "no equipment CS yet");
    g_hold = 0;
    return;
  }
  const std::uint32_t arm = g_f.get_arm(reinterpret_cast<void*>(cs), ph);
  const long long prof_arm = g_f.json_int(prof, "weaponarm", -1, 0);
  if (g_state == 1) {
    if (arm == 1) {
      WriteMarker("weaponarm now 1", arm, prof_arm);
      return;
    }
    if (now - g_set_at > kVerifyS) {
      LogT26(NEVR_LOG_WARNING, "t26 gave up", "weaponarm did not read 1 after the set; no marker, the next launch retries", Tail(g_id),
             arm, prof_arm, "");
      g_done = true;
    }
    return;
  }
  if (cs != g_cs || ph != g_ph) {
    g_cs = cs;
    g_ph = ph;
    g_hold = 0;
  }
  if (++g_hold < kHold) return;
  if (arm == 1 || prof_arm == 1) {
    WriteMarker("weaponarm right already, nothing changed", arm, prof_arm);
    return;
  }
  bool ok = true;
  const auto gs = Rd<std::uintptr_t>(cs + 0x80, &ok);
  const int direct = ok && gs != 0 ? Rd<int>(gs + 0x368, &ok) : 0;
  if (!ok || gs == 0) {
    Waiting("gamespace unreadable");
    return;
  }
  if (direct != 0) {
    g_f.set_arm(reinterpret_cast<void*>(cs), ph & 0xffffffffull, 1);
  } else {  // the deferral the station's hand swap uses: SFunctor2<void, CS, SCompactPoolHandle, ull> {CS, &SetWeaponArm adj 0, ph, arg}
    char* q = static_cast<char*>(g_f.queue(reinterpret_cast<void*>(gs + 0x170), reinterpret_cast<void (*)(void*, char*)>(g_f.invoke)));
    if (q == nullptr) {
      Waiting("deferred queue returned no slot");
      return;
    }
    *reinterpret_cast<void**>(q) = reinterpret_cast<void*>(cs);
    *reinterpret_cast<std::uint32_t*>(q + 0x18) = static_cast<std::uint32_t>(ph);
    *reinterpret_cast<std::uint64_t*>(q + 0x20) = 1;
    *reinterpret_cast<void**>(q + 0x08) = reinterpret_cast<void*>(g_f.set_arm);
    *reinterpret_cast<std::int64_t*>(q + 0x10) = 0;
  }
  g_state = 1;
  g_set_at = now;
  LogT26(NEVR_LOG_INFO, "t26 set", direct != 0 ? "direct" : "deferred", Tail(g_id), arm, prof_arm, "weaponarm -> 1 (one-shot)");
}

}  // namespace

bool ResolveWeaponArmFns(WeaponArmFns* out) {
  void* h = dlopen("libr15.so", RTLD_NOW | RTLD_NOLOAD);
  if (h == nullptr) return false;
  auto S = [&](const char* name) { return dlsym(h, name); };
  WeaponArmFns f{};
  f.net_game = reinterpret_cast<void* (*)(void*)>(S("_ZNK10NRadEngine8NRadGame8CR15Game9NetGameMTEv"));
  f.client_profile = reinterpret_cast<void* (*)(void*)>(S("_ZNK10NRadEngine8NRadGame11CR15NetGame13ClientProfileEv"));
  f.t_string = reinterpret_cast<const char* (*)(void*, const char*, const char*, unsigned)>(S("_ZNK10NRadEngine5CJson7TStringEPKcS2_j"));
  f.json_int = reinterpret_cast<long long (*)(void*, const char*, long long, unsigned)>(S("_ZNK10NRadEngine5CJson3IntEPKcxj"));
  f.is_valid = reinterpret_cast<bool (*)(void*, std::uint64_t)>(
      S("_ZNK10NRadEngine8NRadGame24CR15NetPlayerEquipmentCS16IsComponentValidENS_18SCompactPoolHandleE"));
  f.get_arm = reinterpret_cast<std::uint32_t (*)(void*, std::uint64_t)>(
      S("_ZNK10NRadEngine8NRadGame24CR15NetPlayerEquipmentCS12GetWeaponArmENS_18SCompactPoolHandleE"));
  f.set_arm = reinterpret_cast<void (*)(void*, std::uint64_t, std::uint64_t)>(
      S("_ZN10NRadEngine8NRadGame24CR15NetPlayerEquipmentCS12SetWeaponArmENS_18SCompactPoolHandleEy"));
  f.invoke = reinterpret_cast<void (*)(void*, char*)>(
      S("_ZN10NRadEngine20CDeferredMethodQueue9SFunctor2IvNS_8NRadGame24CR15NetPlayerEquipmentCSENS_"
        "18SCompactPoolHandleEyE12InvokeMethodERS0_Pc"));
  f.queue = reinterpret_cast<void* (*)(void*, void (*)(void*, char*))>(
      S("_ZN10NRadEngine20CDeferredMethodQueue12QueueFunctorINS0_9SFunctor2IvNS_8NRadGame24CR15NetPlayerEquipmentCSENS_18"
        "SCompactPoolHandleEyEEEEPT_PFvRS0_PcE"));
  if (!f.net_game || !f.client_profile || !f.t_string || !f.json_int || !f.is_valid || !f.get_arm || !f.set_arm || !f.invoke ||
      !f.queue)
    return false;
  *out = f;
  return true;
}

void InitWeaponArm(std::uintptr_t base, const WeaponArmFns& fns) {
  g_base = base;
  g_f = fns;
  g_have_fns = fns.net_game != nullptr;  // an all-null set means "resolve from the game library on the first tick"
}

void WeaponArmTick(std::uintptr_t game) {
  if (g_done) return;
  Step(game, Now());
}

int WeaponArmPhase() { return g_done ? 2 : g_state; }

void SetWeaponArmClockForTest(double (*clock)()) { g_clock = clock; }
void SetWeaponArmDirForTest(const char* dir) { std::snprintf(g_dir_override, sizeof g_dir_override, "%s", dir != nullptr ? dir : ""); }
void ResetWeaponArmForTest() {
  g_have_fns = g_init = g_done = false;
  g_state = g_hold = 0;
  g_next = g_set_at = 0;
  g_cs = 0;
  g_ph = 0;
  g_id[0] = g_marker[0] = g_why[0] = 0;
}

}  // namespace q15t
