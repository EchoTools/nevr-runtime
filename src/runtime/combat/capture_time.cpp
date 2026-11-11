#include "runtime/combat/capture_time.h"

#include <atomic>
#include <cstring>
#include <utility>

#include "core/logging.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::capture_time {
namespace {

using NodeFn = std::uint64_t (*)(void* node);

NodeFn g_original[kPageCount] = {};
std::atomic<volatile std::uint8_t*> g_mailbox{nullptr};
std::atomic_bool g_registered{false};

template <typename T>
T Read(const std::uint8_t* at) {
  T v{};
  std::memcpy(&v, at, sizeof v);
  return v;
}

// node -> [node] the context (a qword) -> [context+0x68] the globals (a qword).
std::uint8_t* GlobalsOfNode(const void* node) {
  const std::uint8_t* context = Read<const std::uint8_t*>(static_cast<const std::uint8_t*>(node));
  return Read<std::uint8_t*>(context + kContextGlobals);
}

template <std::size_t I>
std::uint64_t PageHook(void* node) {
  const rules::RulesState rules(g_mailbox.load(std::memory_order_relaxed));
  std::uint8_t* at = GlobalsOfNode(node) + kPageTime;
  const float clock = Read<float>(at);  // the match clock: put back after the function
  const float page = PageValue(rules);
  std::memcpy(at, &page, sizeof page);
  const std::uint64_t r = g_original[I](node);
  const float written = Read<float>(at);
  std::memcpy(at, &clock, sizeof clock);
  if (kPages[I].writes && r == 0) {
    rules.SetRoundTime(written, CaptureOf(written));
    // One Info line per press that changes T (the writing functions run on a podium press, not per frame).
    if (written != page) {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.COMBAT] capture time: T changed from=%g to=%g capture=%g script=%016llx (the RULES page's time rows, "
          "in seconds: unlock * 60 + capture; the capture point's times follow it, the match clock does not)",
          static_cast<double>(page), static_cast<double>(written), static_cast<double>(CaptureOf(written)),
          static_cast<unsigned long long>(kScript));
    }
  }
  return r;
}

template <std::size_t... I>
void RegisterPages(std::index_sequence<I...>) {
  const nevr_script_hooks::Hook rows[] = {
      {kScript, kPages[I].rva, kPages[I].expected, kPages[I].expected_len, reinterpret_cast<void*>(&PageHook<I>),
       reinterpret_cast<void**>(&g_original[I]), kPages[I].name}...,
      {kScript, 0x23240, kMinutesHelper23240, sizeof kMinutesHelper23240, nullptr, nullptr,
       "capture time: pin, 0xFA60's helper reads the page value"},
      {kScript, 0x28930, kMinutesHelper28930, sizeof kMinutesHelper28930, nullptr, nullptr,
       "capture time: pin, 0x14FB0's helper reads the page value"},
  };
  nevr_script_hooks::Register(rows, sizeof rows / sizeof rows[0]);
}

}  // namespace

float PageValue(const rules::RulesState& rules) { return rules.RoundTotalSet() ? rules.RoundTotal() : kDefaultTotal; }

float CaptureOf(float total) {
  const std::int32_t t = static_cast<std::int32_t>(total);  // cvttss2si: toward zero
  return static_cast<float>(t % 60);                         // idiv: the remainder takes the dividend's sign
}

void Register(volatile std::uint8_t* mailbox) {
  if (g_registered.exchange(true)) return;
  g_mailbox.store(mailbox);
  RegisterPages(std::make_index_sequence<kPageCount>{});
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] capture time fix registered script=%016llx hooks=%zu pins=2 (in Combat the RULES page's time rows "
      "set the capture point's unlock and capture times, kept apart from the match clock; installed when the game-mode "
      "script loads)",
      static_cast<unsigned long long>(kScript), kPageCount);
}

#ifdef NEVR_TEST_HOOKS
void ResetForTest() {
  g_registered = false;
  g_mailbox.store(nullptr);
  for (NodeFn& f : g_original) f = nullptr;
}
#endif

}  // namespace nevr_combat::capture_time
