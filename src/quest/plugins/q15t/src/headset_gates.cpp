#include "headset_gates.h"

#include <dlfcn.h>
#include <pthread.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "plugin_log.h"

namespace q15t {

#if defined(__aarch64__)
extern "C" {
// The GOT word of CVR::HeadsetType points at the trampoline, which hands the caller's sp and return address to the dispatcher.
__attribute__((visibility("hidden"), noinline)) std::int32_t q15t_headset_dispatch(std::uintptr_t bind_sp, std::uintptr_t ret);
__attribute__((visibility("hidden"))) void q15t_headset_tramp();
}
#endif

namespace {

std::atomic<std::uintptr_t> g_party_real{0}, g_head_real{0};
std::atomic<std::uintptr_t> g_party_lo{0}, g_party_hi{0}, g_head_lo{0}, g_head_hi{0}, g_link_lo{0}, g_link_hi{0};
std::atomic<std::int32_t> g_link_answer{5};
std::atomic<std::uint32_t> g_party_calls{0}, g_party_gated{0}, g_head_calls{0}, g_head_gated{0}, g_link_gated{0};
constexpr std::uint32_t kLogFirstGated = 8;

// ---- the link gate's library policy ----
enum class LinkMode : int { kAll = 0, kNone = 1, kList = 2 };
constexpr int kMaxFragments = 32;
constexpr std::size_t kFragmentLen = 48;  // a script library's file name is 20 characters
constexpr int kMaxSeenLibs = 64;
std::atomic<int> g_link_mode{static_cast<int>(LinkMode::kAll)};
char g_fragments[kMaxFragments][kFragmentLen];
std::atomic<int> g_fragment_count{0};

struct SeenLib {
  char name[kFragmentLen];
  std::uint32_t calls;
};
pthread_mutex_t g_seen_mutex = PTHREAD_MUTEX_INITIALIZER;  // a std::mutex would register a destructor at load time
SeenLib g_seen[kMaxSeenLibs];
std::uint32_t g_seen_count = 0;
std::atomic<std::uint32_t> g_seen_published{0};

bool Within(std::uintptr_t addr, std::atomic<std::uintptr_t>& lo, std::atomic<std::uintptr_t>& hi) {
  return addr - lo.load(std::memory_order_relaxed) < hi.load(std::memory_order_relaxed) - lo.load(std::memory_order_relaxed);
}

void LogGate(const char* what, std::uint32_t n, std::uintptr_t ret, std::int32_t real_answer) {
  char count[16], retbuf[24], real[16];
  std::snprintf(count, sizeof count, "%u", n);
  std::snprintf(retbuf, sizeof retbuf, "0x%llx", static_cast<unsigned long long>(ret));
  std::snprintf(real, sizeof real, "%d", real_answer);
  const NevrLogField f[] = {{"gate", what}, {"n", count}, {"return_address", retbuf}, {"real_answer", real}};
  Log(NEVR_LOG_INFO, "headset gate answered not-a-Quest", f, 4);
}

// The Bind (BindExpression<CR15HeadsetTypeExpression>) opens with `sub sp,sp,#0x20; stp x19,x30,[sp,#0x10]` and calls
// CVR::HeadsetType from that frame, so the wrapper's entry sp is the Bind's sp and its saved return address is at sp + 0x18:
// the code of the script library (lib<hash>.so) that called the Bind. 0 when there is no frame (a plain call on the host).
constexpr std::uintptr_t kBindSavedLrOffset = 0x18;
std::uintptr_t CallerOfBind(std::uintptr_t bind_sp) {
  if (bind_sp == 0 || (bind_sp & 0xf) != 0) return 0;
  return *reinterpret_cast<const std::uintptr_t*>(bind_sp + kBindSavedLrOffset);
}

// The file name (no directory) of the library that holds `addr`; false when dladdr does not know it.
bool LibraryName(std::uintptr_t addr, char* out, std::size_t n) {
  Dl_info di;
  if (addr == 0 || dladdr(reinterpret_cast<void*>(addr), &di) == 0 || di.dli_fname == nullptr) return false;
  const char* slash = std::strrchr(di.dli_fname, '/');
  std::snprintf(out, n, "%s", slash != nullptr ? slash + 1 : di.dli_fname);
  return out[0] != 0;
}

bool LinkLibAnswered(const char* lib, bool known) {
  switch (static_cast<LinkMode>(g_link_mode.load(std::memory_order_relaxed))) {
    case LinkMode::kAll: return true;
    case LinkMode::kNone: return false;
    case LinkMode::kList: {
      if (!known) return false;
      const int count = g_fragment_count.load(std::memory_order_relaxed);
      for (int i = 0; i < count; ++i)
        if (std::strstr(lib, g_fragments[i]) != nullptr) return true;
      return false;
    }
  }
  return false;
}

// One line the first time a library binds the expression, and again at its 64th bind.
void NoteLinkLib(const char* lib, bool known, bool answered, std::int32_t real_answer) {
  std::uint32_t calls = 0;
  bool first = false;
  {
    pthread_mutex_lock(&g_seen_mutex);
    SeenLib* hit = nullptr;
    for (std::uint32_t i = 0; i < g_seen_count; ++i)
      if (std::strcmp(g_seen[i].name, lib) == 0) hit = &g_seen[i];
    if (hit == nullptr && g_seen_count < static_cast<std::uint32_t>(kMaxSeenLibs)) {
      hit = &g_seen[g_seen_count++];
      std::snprintf(hit->name, sizeof hit->name, "%s", lib);
      hit->calls = 0;
      first = true;
      g_seen_published.store(g_seen_count, std::memory_order_relaxed);
    }
    if (hit != nullptr) calls = ++hit->calls;
    pthread_mutex_unlock(&g_seen_mutex);
  }
  if (!first && calls != 64) return;
  char count[16], real[16];
  std::snprintf(count, sizeof count, "%u", calls);
  std::snprintf(real, sizeof real, "%d", real_answer);
  const NevrLogField f[] = {{"lib", known ? lib : "unknown"}, {"answered", answered ? "not-a-Quest" : "real"}, {"real_answer", real}, {"calls", count}};
  Log(NEVR_LOG_INFO, "headset bind", f, 4);
}

__attribute__((noinline)) std::int32_t PartyWrapper(void* self, std::uint32_t index) {
  const auto ret = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
  const auto real = reinterpret_cast<std::int32_t (*)(void*, std::uint32_t)>(g_party_real.load(std::memory_order_acquire));
  const std::int32_t answer = real != nullptr ? real(self, index) : 0;
  g_party_calls.fetch_add(1, std::memory_order_relaxed);
  if (Within(ret, g_party_lo, g_party_hi)) {
    const std::uint32_t n = g_party_gated.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= kLogFirstGated) LogGate("party_member_headset_type", n, ret, answer);
    return kNotQuest;
  }
  return answer;
}

// `ret`: the caller's return address. `bind_sp`: the wrapper's entry sp (the caller's own sp), 0 where there is none.
__attribute__((noinline)) std::int32_t HeadsetCommon(std::uintptr_t ret, std::uintptr_t bind_sp) {
  const auto real = reinterpret_cast<std::int32_t (*)()>(g_head_real.load(std::memory_order_acquire));
  const std::int32_t answer = real != nullptr ? real() : 0;
  g_head_calls.fetch_add(1, std::memory_order_relaxed);
  if (Within(ret, g_head_lo, g_head_hi)) {
    const std::uint32_t n = g_head_gated.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= kLogFirstGated) LogGate("headset_type", n, ret, answer);
    return kNotQuest;
  }
  if (Within(ret, g_link_lo, g_link_hi)) {
    char lib[kFragmentLen] = "";
    const bool known = LibraryName(CallerOfBind(bind_sp), lib, sizeof lib);
    const bool answered = LinkLibAnswered(lib, known);
    NoteLinkLib(lib, known, answered, answer);
    if (!answered) return answer;
    g_link_gated.fetch_add(1, std::memory_order_relaxed);
    return g_link_answer.load(std::memory_order_relaxed);
  }
  return answer;
}

#if !defined(__aarch64__)
__attribute__((noinline)) std::int32_t HeadsetWrapper() {
  return HeadsetCommon(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)), 0);
}
#endif

}  // namespace

#if defined(__aarch64__)
// Entered by the game's call: takes the caller's sp and return address (a jump touches neither), then continues in C++ with the
// original return address still in x30, so HeadsetCommon returns straight to the caller. `hint #34` is `bti c`.
asm(R"(
  .text
  .p2align 2
  .hidden q15t_headset_tramp
  .type q15t_headset_tramp,%function
q15t_headset_tramp:
  hint #34
  mov x0, sp
  mov x1, x30
  b q15t_headset_dispatch
  .size q15t_headset_tramp, .-q15t_headset_tramp
)");

extern "C" std::int32_t q15t_headset_dispatch(std::uintptr_t bind_sp, std::uintptr_t ret) { return HeadsetCommon(ret, bind_sp); }
#endif

void SetLinkGateLibs(const char* spec) {
  g_fragment_count.store(0, std::memory_order_relaxed);
  g_link_mode.store(static_cast<int>(LinkMode::kAll), std::memory_order_relaxed);
  if (spec == nullptr) return;
  int count = 0;
  bool all = false;
  for (const char* p = spec; *p != 0;) {
    while (*p == ' ' || *p == ',' || *p == '\t') ++p;
    const char* start = p;
    while (*p != 0 && *p != ' ' && *p != ',' && *p != '\t') ++p;
    const std::size_t len = static_cast<std::size_t>(p - start);
    if (len == 0) continue;
    if (len == 3 && std::strncmp(start, "all", 3) == 0) {
      all = true;
    } else if (len == 4 && std::strncmp(start, "none", 4) == 0) {
      continue;
    } else if (count < kMaxFragments && len < kFragmentLen) {
      std::memcpy(g_fragments[count], start, len);
      g_fragments[count][len] = 0;
      ++count;
    }
  }
  g_fragment_count.store(count, std::memory_order_relaxed);
  g_link_mode.store(static_cast<int>(all ? LinkMode::kAll : count > 0 ? LinkMode::kList : LinkMode::kNone), std::memory_order_relaxed);
}

std::uint32_t LinkGateLibsSeen() { return g_seen_published.load(std::memory_order_relaxed); }

GateStats GetGateStats() {
  return {g_party_calls.load(), g_party_gated.load(), g_head_calls.load(), g_head_gated.load(), g_link_gated.load()};
}

HeadsetGateRanges GetHeadsetGateRangesForTest() {
  return {g_head_lo.load(), g_head_hi.load(), g_link_lo.load(), g_link_hi.load(), g_link_answer.load()};
}

InstallResult InstallPartyHeadsetGate(std::uintptr_t base, std::uintptr_t slot_vaddr, CallerRange range, std::uintptr_t real_vaddr) {
  g_party_lo.store(base + range.lo_vaddr);
  g_party_hi.store(base + range.hi_vaddr);
  return InstallImportHook("_ZNK10NRadEngine8NRadGame11CR15NetGame22PartyMemberHeadsetTypeEj", base, slot_vaddr,
                           reinterpret_cast<std::uintptr_t>(&PartyWrapper), &g_party_real, real_vaddr);
}

InstallResult InstallHeadsetGate(std::uintptr_t base, std::uintptr_t slot_vaddr, CallerRange range, std::uintptr_t real_vaddr,
                                 CallerRange link_range, std::int32_t link_answer) {
  g_head_lo.store(base + range.lo_vaddr);
  g_head_hi.store(base + range.hi_vaddr);
  g_link_lo.store(base + link_range.lo_vaddr);
  g_link_hi.store(base + link_range.hi_vaddr);
  g_link_answer.store(link_answer);
#if defined(__aarch64__)
  const std::uintptr_t entry = reinterpret_cast<std::uintptr_t>(&q15t_headset_tramp);
#else
  const std::uintptr_t entry = reinterpret_cast<std::uintptr_t>(&HeadsetWrapper);
#endif
  return InstallImportHook("_ZN10NRadEngine3CVR11HeadsetTypeEv", base, slot_vaddr, entry, &g_head_real, real_vaddr);
}

}  // namespace q15t
