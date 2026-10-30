#include "scriptgate.h"

#include <dlfcn.h>
#include <time.h>

#include <atomic>
#include <cstdio>

#include "module_switch.h"
#include "plugin_log.h"

namespace q15t {
namespace {

constexpr std::uint32_t kMaxLoggedWrites = 4;  // per library: the first write and a few re-binds, then only the counter moves

struct Entry {
  std::int32_t* quest = nullptr;
  std::int32_t* rifts = nullptr;
  bool found = false;
  bool refused = false;
  std::uint32_t writes = 0;
};

Entry g_entry[kScriptGateLibCount];
std::atomic<bool> g_on{false};
std::atomic<std::uint32_t> g_found{0}, g_writes{0}, g_refused{0};
std::int64_t g_next_lookup_ms = 0;
std::int64_t g_period_ms = 1000;
std::uint32_t g_all_logged = 0;
ScriptGateWords (*g_resolver)(const char*) = nullptr;

std::int64_t NowMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// dlopen RTLD_NOLOAD: the library is only looked at if the engine already loaded it. The reference is kept (never dlclosed), so the
// words stay mapped for the process.
ScriptGateWords ResolveInProcess(const char* lib) {
  ScriptGateWords w{nullptr, nullptr, false};
  void* h = dlopen(lib, RTLD_NOW | RTLD_NOLOAD);
  if (h == nullptr) return w;
  w.loaded = true;
  w.quest = static_cast<std::int32_t*>(dlsym(h, kScriptGateQuestSym));
  w.rifts = static_cast<std::int32_t*>(dlsym(h, kScriptGateRiftsSym));
  return w;
}

void LogWrite(const char* lib, std::int32_t from, std::uint32_t writes) {
  char f[16], t[16], n[16];
  std::snprintf(f, sizeof f, "%d", static_cast<int>(from));
  std::snprintf(t, sizeof t, "%d", static_cast<int>(kRiftsSlot));
  std::snprintf(n, sizeof n, "%u", static_cast<unsigned>(writes));
  const NevrLogField fields[] = {{"lib", lib}, {"quest_offset_from", f}, {"quest_offset_to", t}, {"writes_on_lib", n}};
  Log(NEVR_LOG_INFO, "scriptgate: quest output of this script library now reads the rifts flag (its combat locks open)", fields, 4);
}

void LogRefused(const char* lib, const char* why, std::int32_t quest, std::int32_t rifts) {
  char q[16], r[16];
  std::snprintf(q, sizeof q, "%d", static_cast<int>(quest));
  std::snprintf(r, sizeof r, "%d", static_cast<int>(rifts));
  const NevrLogField fields[] = {{"lib", lib}, {"reason", why}, {"quest_offset", q}, {"rifts_offset", r}};
  Log(NEVR_LOG_ERROR, "scriptgate: this script library is left as the game has it (its combat locks stay)", fields, 4);
}

// One library, one look. Writes the quest word only where it holds the slot the engine stored for "quest" and the rifts word the slot
// for "rifts"; 0 in the quest word is a library the engine has not bound yet (looked at again next time); 1 is done (or put back
// after a re-bind). Anything else is a library that does not look like the pinned build's: refused once, nothing written.
void Look(std::size_t i) {
  Entry& e = g_entry[i];
  if (e.refused || e.quest == nullptr) return;
  const std::int32_t q = *e.quest;
  if (q == kRiftsSlot) return;
  if (q == 0) return;  // not bound yet
  if (q != kQuestSlot || *e.rifts != kRiftsSlot) {
    e.refused = true;
    g_refused.fetch_add(1, std::memory_order_relaxed);
    LogRefused(kScriptGateLibs[i], q != kQuestSlot ? "quest_word_unexpected" : "rifts_word_unexpected", q, *e.rifts);
    return;
  }
  *e.quest = kRiftsSlot;
  ++e.writes;
  g_writes.fetch_add(1, std::memory_order_relaxed);
  if (e.writes <= kMaxLoggedWrites) LogWrite(kScriptGateLibs[i], q, e.writes);
}

void Find(std::size_t i) {
  Entry& e = g_entry[i];
  const ScriptGateWords w = (g_resolver != nullptr ? g_resolver : &ResolveInProcess)(kScriptGateLibs[i]);
  if (!w.loaded) return;
  e.found = true;
  g_found.fetch_add(1, std::memory_order_relaxed);
  if (w.quest == nullptr || w.rifts == nullptr) {
    e.refused = true;
    g_refused.fetch_add(1, std::memory_order_relaxed);
    LogRefused(kScriptGateLibs[i], "word_not_exported", 0, 0);
    return;
  }
  e.quest = w.quest;
  e.rifts = w.rifts;
}

}  // namespace

InstallResult InstallScriptGate() {
  InstallResult r;
  if (ModuleSwitchPresent("scriptgate.off")) {
    r.reason = "off_file";
    const NevrLogField f[] = {{"file", "scriptgate.off"}};
    Log(NEVR_LOG_INFO, "scriptgate: off by its kill file; the combat locks stay as the game has them", f, 1);
    return r;
  }
  g_on.store(true, std::memory_order_release);
  r.status = SwapStatus::kOk;
  r.reason = "waiting";
  return r;
}

void ScriptGateTick(std::uintptr_t /*game*/) {
  if (!g_on.load(std::memory_order_acquire)) return;
  std::uint32_t found = 0;
  for (const Entry& e : g_entry) found += e.found ? 1 : 0;
  if (found < kScriptGateLibCount) {
    const std::int64_t now = NowMs();
    if (now >= g_next_lookup_ms) {
      g_next_lookup_ms = now + g_period_ms;
      for (std::size_t i = 0; i < kScriptGateLibCount; ++i)
        if (!g_entry[i].found) Find(i);
    }
  }
  for (std::size_t i = 0; i < kScriptGateLibCount; ++i) Look(i);
  const ScriptGateStats s = GetScriptGateStats();
  if (s.bound + s.refused == kScriptGateLibCount && g_all_logged == 0) {
    g_all_logged = 1;
    const KV n[] = {{"libraries", static_cast<long long>(kScriptGateLibCount)},
                    {"bound", s.bound},
                    {"refused", s.refused},
                    {"writes", s.writes}};
    LogNums(NEVR_LOG_INFO, "scriptgate: every script library of the set is settled", n, 4);
  }
}

ScriptGateStats GetScriptGateStats() {
  ScriptGateStats s;
  for (const Entry& e : g_entry) {
    if (e.found) ++s.found;
    if (e.quest != nullptr && !e.refused && *e.quest == kRiftsSlot) ++s.bound;
  }
  s.writes = g_writes.load(std::memory_order_relaxed);
  s.refused = g_refused.load(std::memory_order_relaxed);
  return s;
}

void SetScriptGateResolverForTest(ScriptGateWords (*resolve)(const char* lib)) { g_resolver = resolve; }
void SetScriptGatePeriodMsForTest(std::int64_t ms) { g_period_ms = ms; }

void ResetScriptGateForTest() {
  for (Entry& e : g_entry) e = Entry{};
  g_on.store(false);
  g_found.store(0);
  g_writes.store(0);
  g_refused.store(0);
  g_next_lookup_ms = 0;
  g_period_ms = 1000;
  g_all_logged = 0;
  g_resolver = nullptr;
}

}  // namespace q15t
