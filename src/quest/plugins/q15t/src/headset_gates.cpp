#include "headset_gates.h"

#include <atomic>
#include <cstdio>

#include "plugin_log.h"

namespace q15t {
namespace {

std::atomic<std::uintptr_t> g_party_real{0}, g_head_real{0};
std::atomic<std::uintptr_t> g_party_lo{0}, g_party_hi{0}, g_head_lo{0}, g_head_hi{0}, g_link_lo{0}, g_link_hi{0};
std::atomic<std::int32_t> g_link_answer{kLinkHeadset};
std::atomic<std::uint32_t> g_party_calls{0}, g_party_gated{0}, g_head_calls{0}, g_head_gated{0}, g_link_gated{0};
constexpr std::uint32_t kLogFirstGated = 8;

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

__attribute__((noinline)) std::int32_t HeadsetWrapper() {
  const auto ret = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
  const auto real = reinterpret_cast<std::int32_t (*)()>(g_head_real.load(std::memory_order_acquire));
  const std::int32_t answer = real != nullptr ? real() : 0;
  g_head_calls.fetch_add(1, std::memory_order_relaxed);
  if (Within(ret, g_head_lo, g_head_hi)) {
    const std::uint32_t n = g_head_gated.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= kLogFirstGated) LogGate("headset_type", n, ret, answer);
    return kNotQuest;
  }
  if (Within(ret, g_link_lo, g_link_hi)) {
    const std::uint32_t n = g_link_gated.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= kLogFirstGated) LogGate("headset_type_expression", n, ret, answer);
    return g_link_answer.load(std::memory_order_relaxed);
  }
  return answer;
}

}  // namespace

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
  return InstallImportHook("_ZN10NRadEngine3CVR11HeadsetTypeEv", base, slot_vaddr,
                           reinterpret_cast<std::uintptr_t>(&HeadsetWrapper), &g_head_real, real_vaddr);
}

}  // namespace q15t
