#include "music.h"

#include <atomic>
#include <cstdio>
#include <ctime>

#include "module_switch.h"
#include "plugin_log.h"
#include "refresh_rate.h"
#include "safe_read.h"

namespace q15t {
namespace {

// Symbol of the imported function (the JUMP_SLOT's symbol in R0_readelf_r.txt line 118641; source the source build's music:52).
constexpr char kSymbol[] = "_ZN10NRadEngine12CWwiseEngine14PostSoundEventENS_12CSoundSymbolEjNS_7CFlagsTIjLy1EEEPFviPvES4_jRj";
constexpr std::uint32_t kNoObject = 0xFFFFFFFFu;  // the source's "no previous Play" sentinel (the source build's music:67,98)
constexpr int kMaxLines = 4000;                    // per process (the source build's music:59); music posts are tens a match
constexpr double kLookEvery = 0.5;                 // the tick's period, seconds (the source build's music:157)

// PostSoundEvent(self x0, event w1, object w2, &flags x3, callback x4, cookie x5, seek ms w6, &playing id x7) -> w0
// (the source build's music:83; the instruction words are pinned in music.h).
using PostFn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                 std::uint64_t, std::uint64_t);

enum class Kind { kNone, kPlay, kStop, kState };

std::atomic<std::uintptr_t> g_real{0};
std::atomic<std::uint32_t> g_last_obj{kNoObject};  // object of the last music Play
std::atomic<std::uint64_t> g_self{0};              // its CWwiseEngine (PostSoundEvent's x0)
std::atomic<bool> g_playing{false};                // a music Play since the last unload stop
std::atomic<int> g_lines{0};
std::atomic<std::uint64_t> g_bank_posts{0}, g_extra_stops{0}, g_tick_stops{0};
double g_next_look = 0;  // game thread only
bool g_combat = false;   // game thread only: a combat map was loaded at the last look

Kind Classify(std::uint32_t ev) {
  for (const std::uint32_t e : kMusicPlay)
    if (e == ev) return Kind::kPlay;
  for (const std::uint32_t e : kMusicStop)
    if (e == ev) return Kind::kStop;
  for (const std::uint32_t e : kMusicState)
    if (e == ev) return Kind::kState;
  return Kind::kNone;
}

const char* KindName(Kind k) { return k == Kind::kPlay ? "play" : k == Kind::kStop ? "stop" : "state"; }

// True while the per-process line budget lasts. The line that crosses it logs one WARNING, so a silenced log is never silent.
bool LineBudget() {
  const int n = g_lines.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= kMaxLines) return true;
  if (n == kMaxLines + 1) {
    const KV f[] = {{"max_lines", kMaxLines}};
    LogNums(NEVR_LOG_WARNING, "music: line budget used up, further music posts are counted but not logged", f, 1);
  }
  return false;
}

// One extra Stop through the real PostSoundEvent: PostEvent(stop, object, flags 0, no callback, no cookie), seek 0.
std::uint64_t PostStop(PostFn real, std::uint64_t self, std::uint32_t ev, std::uint32_t obj, std::uint32_t* id) {
  std::uint32_t flags = 0;
  *id = 0;
  g_extra_stops.fetch_add(1, std::memory_order_relaxed);
  return real(self, ev, obj, reinterpret_cast<std::uint64_t>(&flags), 0, 0, 0, reinterpret_cast<std::uint64_t>(id));
}

void AppendStop(char* s, std::size_t cap, std::size_t* n, std::uint32_t st, std::uint32_t obj, std::uint64_t r, std::uint32_t id) {
  if (*n + 1 >= cap) return;
  const int w = std::snprintf(s + *n, cap - *n, " %08x obj %u -> %d id %u;", st, obj, static_cast<int>(r & 0xff), id);
  if (w > 0) *n += static_cast<std::size_t>(w) < cap - *n ? static_cast<std::size_t>(w) : cap - *n - 1;
}

__attribute__((noinline)) std::uint64_t PostWrapper(std::uint64_t self, std::uint64_t sym, std::uint64_t obj, std::uint64_t x3,
                                                    std::uint64_t x4, std::uint64_t x5, std::uint64_t x6, std::uint64_t x7) {
  const auto real = reinterpret_cast<PostFn>(g_real.load(std::memory_order_acquire));
  if (real == nullptr) return 0;
  const std::uint32_t ev = static_cast<std::uint32_t>(sym), o = static_cast<std::uint32_t>(obj);
  const Kind kind = Classify(ev);
  if (kind == Kind::kNone) return real(self, sym, obj, x3, x4, x5, x6, x7);  // the hot path: no music event, no work, no log
  g_bank_posts.fetch_add(1, std::memory_order_relaxed);
  if (kind == Kind::kPlay) {
    const std::uint32_t prev = g_last_obj.exchange(o, std::memory_order_acq_rel);
    const std::uint32_t targets[2] = {o, prev != o ? prev : kNoObject};
    char s[256] = "";
    std::size_t n = 0;
    for (const std::uint32_t target : targets) {
      if (target == kNoObject) continue;
      for (const std::uint32_t st : kMusicStop) {
        std::uint32_t id;
        const std::uint64_t r = PostStop(real, self, st, target, &id);
        AppendStop(s, sizeof s, &n, st, target, r, id);
      }
    }
    if (LineBudget()) {
      char e[16], ob[16], pv[16];
      std::snprintf(e, sizeof e, "%08x", ev);
      std::snprintf(ob, sizeof ob, "%u", o);
      std::snprintf(pv, sizeof pv, "%u", prev);
      const NevrLogField f[] = {{"event", e}, {"obj", ob}, {"prev_obj", pv}, {"stops", s}};
      Log(NEVR_LOG_INFO, "music play: both music Stops posted first", f, 4);
    }
    g_self.store(self, std::memory_order_relaxed);
    g_playing.store(true, std::memory_order_release);
  }
  const std::uint64_t r = real(self, sym, obj, x3, x4, x5, x6, x7);
  bool ok = true;
  const std::uint32_t id = x7 != 0 ? Rd<std::uint32_t>(static_cast<std::uintptr_t>(x7), &ok) : 0;
  if (LineBudget()) {
    char e[16], ob[16], rc[16], pid[16];
    std::snprintf(e, sizeof e, "%08x", ev);
    std::snprintf(ob, sizeof ob, "%u", o);
    std::snprintf(rc, sizeof rc, "%d", static_cast<int>(r & 0xff));
    std::snprintf(pid, sizeof pid, "%u", id);
    const NevrLogField f[] = {{"event", e}, {"kind", KindName(kind)}, {"obj", ob}, {"rc", rc}, {"playing_id", pid}};
    Log(NEVR_LOG_INFO, "music post", f, 5);
  }
  return r;
}

double Now() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

}  // namespace

void MusicTick(std::uintptr_t game) {
  const double now = Now();
  if (now < g_next_look) return;
  g_next_look = now + kLookEvery;
  std::uint64_t level = 0;
  const bool combat = CombatMapLoaded(game, &level);
  const bool left = g_combat && !combat;
  g_combat = combat;
  if (!left || !g_playing.exchange(false, std::memory_order_acq_rel)) return;
  const auto real = reinterpret_cast<PostFn>(g_real.load(std::memory_order_acquire));
  if (real == nullptr) return;
  const std::uint32_t o = g_last_obj.load(std::memory_order_acquire);
  char s[256] = "";
  std::size_t n = 0;
  for (const std::uint32_t st : kMusicStop) {
    std::uint32_t id;
    const std::uint64_t r = PostStop(real, g_self.load(std::memory_order_relaxed), st, o, &id);
    AppendStop(s, sizeof s, &n, st, o, r, id);
  }
  g_tick_stops.fetch_add(1, std::memory_order_relaxed);
  if (LineBudget()) {
    char ob[16];
    std::snprintf(ob, sizeof ob, "%u", o);
    const NevrLogField f[] = {{"obj", ob}, {"stops", s}};
    Log(NEVR_LOG_INFO, "music: combat map gone, both music Stops posted", f, 2);
  }
}

InstallResult InstallMusicAt(std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t real_vaddr) {
  if (ModuleSwitchPresent("music.off")) {
    const NevrLogField f[] = {{"why", "music.off present in the q15t data directory"}};
    Log(NEVR_LOG_INFO, "music off", f, 1);
    InstallResult r;
    r.reason = "off_file";
    return r;
  }
  const InstallResult r = InstallImportHook(kSymbol, base, slot_vaddr, reinterpret_cast<std::uintptr_t>(&PostWrapper), &g_real, real_vaddr);
  char slot[24], real[24];
  std::snprintf(slot, sizeof slot, "0x%llx", static_cast<unsigned long long>(slot_vaddr));
  std::snprintf(real, sizeof real, "0x%llx", static_cast<unsigned long long>(real_vaddr));
  const NevrLogField f[] = {{"slot_vaddr", slot}, {"real_vaddr", real}, {"status", StatusName(r.status)}, {"reason", r.reason}};
  if (r.status == SwapStatus::kOk)
    Log(NEVR_LOG_INFO, "music on: CWwiseEngine::PostSoundEvent posts both music Stops before each music Play; MusicTick must be registered", f, 4);
  else
    LogSlotRefusal("music refused: nothing changed (or restore failed, see status)", base, slot_vaddr, real_vaddr, r);
  return r;
}

bool MusicWordPinsMatch(std::uintptr_t base) { return CheckWordPins("music", base, kMusicWordPins, kMusicWordPinCount); }

InstallResult InstallMusic(std::uintptr_t base) {
  if (!MusicWordPinsMatch(base)) {
    InstallResult r;
    r.reason = "word_pin_mismatch";
    return r;
  }
  return InstallMusicAt(base, kMusicSlot, kMusicReal);
}

MusicStats GetMusicStats() {
  return {g_bank_posts.load(std::memory_order_relaxed), g_extra_stops.load(std::memory_order_relaxed),
          g_tick_stops.load(std::memory_order_relaxed)};
}

void ResetMusicForTest() {
  g_real.store(0);
  g_last_obj.store(kNoObject);
  g_self.store(0);
  g_playing.store(false);
  g_lines.store(0);
  g_bank_posts.store(0);
  g_extra_stops.store(0);
  g_tick_stops.store(0);
  g_next_look = 0;
  g_combat = false;
}
void ResetMusicScheduleForTest() { g_next_look = 0; }
void MusicForceStateForTest(bool playing, std::uint64_t self, std::uint32_t obj) {
  g_self.store(self);
  g_last_obj.store(obj);
  g_playing.store(playing);
}

}  // namespace q15t
