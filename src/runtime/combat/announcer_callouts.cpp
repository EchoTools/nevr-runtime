#include "runtime/combat/announcer_callouts.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "core/logging.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::announcer {
namespace {

// The stock layouts the hooks read and write (see the header for the functions they belong to).
constexpr std::uintptr_t kCountersAt = 0x68;        // the script context's pointer to the counters
constexpr std::uintptr_t kDataSlotAt = 0x08;        // the thread entry's pointer to its data slot
constexpr std::uintptr_t kThreadWordAt = 0x50;      // the thread entry's word from event+0x18
constexpr std::uintptr_t kThreadWord2At = 0x58;     // the thread entry's word from event+0x28
constexpr std::uintptr_t kCounterAddrAt = 0x70;     // the thread entry's pointer to the busy counter
constexpr std::size_t kEventSymbolIndex = 0;        // the event's CSymbol64 is its first qword
constexpr std::size_t kEventThreadWordIndex = 3;    // event+0x18
constexpr std::size_t kEventThreadWord2Index = 5;   // event+0x28
constexpr std::size_t kEventDataIndex = 6;          // event+0x30: the slot the thread's data pointer names
constexpr std::uintptr_t kOutputEventAt = 0x28;     // read_sound_cc_context's output state: the node's Wwise event

using StartRawFn = std::uint8_t* (*)(void* script, std::uint32_t node);

StockHandler g_handler = nullptr;
StockQuery g_query = nullptr;
StockRead g_read = nullptr;
std::atomic_bool g_registered{false};
std::atomic_bool g_deferred{false};
// The Wwise event PULSAR plays next, 0 for none. Written by the handler and read by the read, which the script runs on its
// own threads; the exchange in PlayEvent takes it once.
std::atomic<std::uint32_t> g_override{0};
// The table the query answers with: the game's 20 names, then ours. Rewritten by each answer with the same content.
const void* g_scratch[kTableSize] = {};

template <typename T>
T Load(const std::uint8_t* at) {
  T v{};
  std::memcpy(&v, at, sizeof v);
  return v;
}

template <typename T>
void Store(std::uint8_t* at, T v) {
  std::memcpy(at, &v, sizeof v);
}

std::uint8_t* StartThread(void* script, std::uint32_t node) {
  auto* base = static_cast<std::uint8_t*>(nevr_script_hooks::LoadedBase(kScript));
  return reinterpret_cast<StartRawFn>(base + kStartThreadRva)(script, node);
}

}  // namespace

int FindLine(std::uint64_t symbol) {
  for (std::size_t i = 0; i < kLineCount; ++i) {
    if (kLines[i].game_event == symbol) return static_cast<int>(i);
  }
  return -1;
}

bool HandleEvent(void* script, const std::uint64_t* event, StartFn start, std::atomic<std::uint32_t>& override_event) {
  const int line = FindLine(event[kEventSymbolIndex]);
  if (line < 0) return false;
  auto* script_bytes = static_cast<std::uint8_t*>(script);
  auto* counters = Load<std::uint8_t*>(script_bytes + kCountersAt);
  auto* busy = reinterpret_cast<std::int32_t*>(counters + kPulsarCounter);
  if (*busy >= kBusy) {
    Log(EchoVR::LogLevel::Debug, "[NEVR.COMBAT] announcer callout dropped line=%s reason=pulsar_busy",
        kLines[line].name);
    return true;  // PULSAR is playing: the event is taken and nothing starts
  }
  override_event.store(kLines[line].wwise, std::memory_order_relaxed);
  std::uint8_t* node = start(script, kPulsarNode);
  // the thread data the stock PULSAR listener writes (FUN_180004b30's PULSAR block, in its order)
  Store(Load<std::uint8_t*>(node + kDataSlotAt), event[kEventDataIndex]);
  Store(node + kThreadWordAt, event[kEventThreadWordIndex]);
  Store(node + kThreadWord2At, event[kEventThreadWord2Index]);
  ++*busy;
  Store(node + kCounterAddrAt, reinterpret_cast<std::uintptr_t>(busy));
  return true;
}

bool ExtendEvents(std::uint64_t key, std::uint32_t answer, const void** table, std::uint32_t* count,
                  const void** scratch) {
  if (key != kEventsKey || answer != kQueryAnswered || *count != kGameEventCount) return false;
  const auto* game = static_cast<const void* const*>(*table);
  for (std::size_t i = 0; i < kGameEventCount; ++i) scratch[i] = game[i];
  for (std::size_t i = 0; i < kAddedNames; ++i) scratch[kGameEventCount + i] = kLines[i].game_name;
  *table = scratch;
  *count = static_cast<std::uint32_t>(kTableSize);
  return true;
}

void PlayEvent(std::uint8_t* output, std::atomic<std::uint32_t>& override_event) {
  if (Load<std::uint32_t>(output + kOutputEventAt) != kPulsarEvent) return;
  const std::uint32_t taken = override_event.exchange(0, std::memory_order_relaxed);
  if (taken != 0) Store(output + kOutputEventAt, taken);
}

void RunHandler(void* script, const std::uint64_t* event, StartFn start, std::atomic<std::uint32_t>& override_event,
                StockHandler stock) {
  if (!HandleEvent(script, event, start, override_event)) stock(script, event);
}

std::uint32_t RunQuery(std::uint64_t key, const void** table, std::uint32_t* count, StockQuery stock,
                       const void** scratch) {
  const std::uint32_t answer = stock(key, table, count);
  ExtendEvents(key, answer, table, count, scratch);
  return answer;
}

std::uint64_t RunRead(std::uint8_t* output, void* node, std::atomic<std::uint32_t>& override_event, StockRead stock) {
  PlayEvent(output, override_event);
  return stock(output, node);
}

// The table's entry points: the three detours it installs and the free listener.
void HandlerDetour(void* script, const std::uint64_t* event) {
  RunHandler(script, event, &StartThread, g_override, g_handler);
}

std::uint32_t QueryDetour(std::uint64_t key, const void** table, std::uint32_t* count) {
  return RunQuery(key, table, count, g_query, g_scratch);
}

std::uint64_t ReadDetour(std::uint8_t* output, void* node) { return RunRead(output, node, g_override, g_read); }

void OnFreed(std::uint64_t /*script*/) { g_override.store(0, std::memory_order_relaxed); }

bool HoldsBank(const nevr_shadow::Shadow* shadow, const std::vector<std::string>& labels) {
  if (shadow == nullptr) return false;
  nevr_shadow::Found found;
  std::uint32_t user = 0;
  if (!shadow->Find(kBankType, kBankName, &found, &user)) return false;
  return user < labels.size() && labels[user] == kRecordsLabel;
}

void OnPublished(const nevr_shadow::Shadow* shadow, const std::vector<std::string>& labels, bool boot) {
  if (!HoldsBank(shadow, labels)) return;
  if (boot) {
    Register();
    return;
  }
  if (g_registered.load() || g_deferred.exchange(true)) return;
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] announcer callouts deferred reason=bank_arrived_after_boot bank=%016llx (the game read its sound bank before "
      "the fetch brought the patched one; the callouts start at the next launch)",
      static_cast<unsigned long long>(kBankName));
}

void Register() {
  if (g_registered.exchange(true)) return;
  const nevr_script_hooks::Hook rows[kHookRows] = {
      {kScript, kHandlerRva, kHandlerPin, sizeof kHandlerPin, reinterpret_cast<void*>(&HandlerDetour),
       reinterpret_cast<void**>(&g_handler), "announcer callouts: event handler"},
      {kScript, kEventsQueryRva, kQueryPin, sizeof kQueryPin, reinterpret_cast<void*>(&QueryDetour),
       reinterpret_cast<void**>(&g_query), "announcer callouts: event list query"},
      {kScript, kReadContextRva, kReadPin, sizeof kReadPin, reinterpret_cast<void*>(&ReadDetour),
       reinterpret_cast<void**>(&g_read), "announcer callouts: sound context read"},
  };
  static_assert(sizeof rows / sizeof rows[0] == kHookRows);
  nevr_script_hooks::Register(rows, kHookRows);
  nevr_script_hooks::AddFreeListener(kScript, &OnFreed);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] announcer callouts registered script=%016llx hooks=%zu lines=%zu (the gear lines with no announcer "
      "line play on PULSAR when equipped; the announcer script's rows apply from its next load)",
      static_cast<unsigned long long>(kScript), kHookRows, kLineCount);
}

std::atomic<std::uint32_t>& CurrentOverride() { return g_override; }

#ifdef NEVR_TEST_HOOKS
void ResetForTest() {
  g_registered = false;
  g_deferred = false;
  g_handler = nullptr;
  g_query = nullptr;
  g_read = nullptr;
  g_override.store(0, std::memory_order_relaxed);
}

bool RegisteredForTest() { return g_registered.load(); }

OriginalsForTest Originals() {
  return {reinterpret_cast<const void*>(g_handler), reinterpret_cast<const void*>(g_query),
          reinterpret_cast<const void*>(g_read)};
}

bool DeferredForTest() { return g_deferred.load(); }

#endif

}  // namespace nevr_combat::announcer
