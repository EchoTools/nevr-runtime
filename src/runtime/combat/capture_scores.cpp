#include "runtime/combat/capture_scores.h"

#include <atomic>
#include <cstring>
#include <utility>

#include "core/logging.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::capture_scores {
namespace {

using NodeFn = std::uint64_t (*)(void* node);
using SetterFn = std::uint64_t (*)(void* record, void* node);

constexpr std::uint32_t kPublishRvas[] = {0xCB60, 0xCCA0, 0xD480, 0x16380, 0x16A60, 0x184A0};
constexpr std::size_t kPublishCount = sizeof kPublishRvas / sizeof kPublishRvas[0];

NodeFn g_publish[kPublishCount] = {};
NodeFn g_blue_reset = nullptr;
NodeFn g_orange_reset = nullptr;
NodeFn g_dispatcher = nullptr;
NodeFn g_initializer = nullptr;  // `(context)`: it takes the script's context, not a node
NodeFn g_blue_progress = nullptr;
NodeFn g_orange_progress = nullptr;
std::atomic<volatile std::uint8_t*> g_mailbox{nullptr};
std::atomic_bool g_registered{false};

rules::RulesState Rules() { return rules::RulesState(g_mailbox.load(std::memory_order_relaxed)); }

template <typename T>
T Read(const std::uint8_t* at) {
  T v{};
  std::memcpy(&v, at, sizeof v);
  return v;
}

// [context+0x68]: the script's globals (a qword).
std::uint8_t* GlobalsOfContext(const void* context) {
  return Read<std::uint8_t*>(static_cast<const std::uint8_t*>(context) + kContextGlobals);
}
// node -> [node] the context (a qword) -> its globals.
std::uint8_t* GlobalsOfNode(const void* node) {
  return GlobalsOfContext(Read<const void*>(static_cast<const std::uint8_t*>(node)));
}

void SetCharge(std::uint8_t* globals, rules::Team team) {
  const float points = Rules().StartScore(team);
  std::memcpy(globals + (team == rules::Team::kBlue ? kBlueCharge : kOrangeCharge), &points, sizeof points);
}

template <std::size_t I>
std::uint64_t Publish(void* node) {
  CopyScores(GlobalsOfNode(node), Rules());
  return g_publish[I](node);
}

std::uint64_t BlueReset(void* node) {
  const std::uint64_t r = g_blue_reset(node);
  SetCharge(GlobalsOfNode(node), rules::Team::kBlue);
  return r;
}

std::uint64_t OrangeReset(void* node) {
  const std::uint64_t r = g_orange_reset(node);
  SetCharge(GlobalsOfNode(node), rules::Team::kOrange);
  return r;
}

std::uint64_t Dispatcher(void* node) {
  const std::uint64_t id = Read<std::uint64_t>(static_cast<const std::uint8_t*>(node) + kNodeId);
  const std::uint64_t r = g_dispatcher(node);
  if (id == kBlueResetId) SetCharge(GlobalsOfNode(node), rules::Team::kBlue);
  if (id == kOrangeResetId) SetCharge(GlobalsOfNode(node), rules::Team::kOrange);
  return r;
}

std::uint64_t Initializer(void* context) {
  const std::uint64_t r = g_initializer(context);
  std::uint8_t* globals = GlobalsOfContext(context);
  SetCharge(globals, rules::Team::kBlue);
  SetCharge(globals, rules::Team::kOrange);
  return r;
}

std::uint64_t ProgressNode(void* node, rules::Team team, NodeFn original, std::uint64_t symbol) {
  const float start = Rules().StartScore(team);
  void* base = start != 0.0f ? nevr_script_hooks::LoadedBase(kPointScript) : nullptr;
  if (base == nullptr) return original(node);  // no starting score (or no module): the stock 0
  ProgressRecord record = MakeProgressRecord(symbol, Progress(start));
  return reinterpret_cast<SetterFn>(static_cast<std::uint8_t*>(base) + kProgressSetterRva)(record.bytes, node);
}

std::uint64_t BlueProgress(void* node) {
  return ProgressNode(node, rules::Team::kBlue, g_blue_progress, kBlueProgressSymbol);
}

std::uint64_t OrangeProgress(void* node) {
  return ProgressNode(node, rules::Team::kOrange, g_orange_progress, kOrangeProgressSymbol);
}

template <std::size_t... I>
void RegisterPublish(std::index_sequence<I...>) {
  static_assert(sizeof kPublishCB60 == sizeof kPublish184A0 && sizeof kPublishD480 == sizeof kPublish16A60);
  static constexpr const std::uint8_t* kPins[] = {kPublishCB60, kPublishCCA0, kPublishD480,
                                                  kPublish16380, kPublish16A60, kPublish184A0};
  const nevr_script_hooks::Hook rows[] = {
      {kModeScript, kPublishRvas[I], kPins[I], sizeof kPublishCB60, reinterpret_cast<void*>(&Publish<I>),
       reinterpret_cast<void**>(&g_publish[I]), "capture scores: a starting score published"}...,
  };
  nevr_script_hooks::Register(rows, sizeof rows / sizeof rows[0]);
}

}  // namespace

void CopyScores(const std::uint8_t* globals6fa8, const rules::RulesState& rules) {
  const float blue = static_cast<float>(Read<std::int32_t>(globals6fa8 + kBlueScore));  // cvtsi2ss from a dword
  const float red = static_cast<float>(Read<std::int32_t>(globals6fa8 + kRedScore));
  rules.SetStartScores(blue, red);
}

float Progress(float start_score) { return start_score / 100.0f; }

ProgressRecord MakeProgressRecord(std::uint64_t symbol, float value) {
  ProgressRecord r{};
  const std::uint64_t unset = ~std::uint64_t{0};
  std::memcpy(r.bytes + 0x10, &unset, sizeof unset);
  std::memcpy(r.bytes + 0x18, &unset, sizeof unset);
  std::memcpy(r.bytes + 0x20, &symbol, sizeof symbol);
  std::memcpy(r.bytes + 0x28, &value, sizeof value);
  return r;  // +0x00 qword 0, +0x08 byte 0, +0x2C byte 0: zero
}

void Register(volatile std::uint8_t* mailbox) {
  if (g_registered.exchange(true)) return;
  g_mailbox.store(mailbox);
  RegisterPublish(std::make_index_sequence<kPublishCount>{});
  const nevr_script_hooks::Hook rows[] = {
      {kPointScript, 0x14780, kBlueResetNode, sizeof kBlueResetNode, reinterpret_cast<void*>(&BlueReset),
       reinterpret_cast<void**>(&g_blue_reset), "capture scores: blue's points reset"},
      {kPointScript, 0x14E90, kOrangeResetNode, sizeof kOrangeResetNode, reinterpret_cast<void*>(&OrangeReset),
       reinterpret_cast<void**>(&g_orange_reset), "capture scores: orange's points reset"},
      {kPointScript, 0xB2D0, kDispatcherHead, sizeof kDispatcherHead, reinterpret_cast<void*>(&Dispatcher),
       reinterpret_cast<void**>(&g_dispatcher), "capture scores: node dispatcher (its two inline resets)"},
      {kPointScript, 0xB9A4, kDispatcherBlueReset, sizeof kDispatcherBlueReset, nullptr, nullptr,
       "capture scores: pin, the dispatcher's inline blue reset"},
      {kPointScript, 0xB8A4, kDispatcherOrangeReset, sizeof kDispatcherOrangeReset, nullptr, nullptr,
       "capture scores: pin, the dispatcher's inline orange reset"},
      {kPointScript, 0x1CDE0, kInitializerHead, sizeof kInitializerHead, reinterpret_cast<void*>(&Initializer),
       reinterpret_cast<void**>(&g_initializer), "capture scores: variable initializer"},
      {kPointScript, 0x1CE53, kInitializerOrangeWrite, sizeof kInitializerOrangeWrite, nullptr, nullptr,
       "capture scores: pin, the initializer's orange write"},
      {kPointScript, 0x1CE69, kInitializerBlueWrite, sizeof kInitializerBlueWrite, nullptr, nullptr,
       "capture scores: pin, the initializer's blue write"},
      {kPointScript, 0xFA40, kBlueProgressNode, sizeof kBlueProgressNode, reinterpret_cast<void*>(&BlueProgress),
       reinterpret_cast<void**>(&g_blue_progress), "capture scores: blue's HUD score reset"},
      {kPointScript, 0x11180, kOrangeProgressNode, sizeof kOrangeProgressNode, reinterpret_cast<void*>(&OrangeProgress),
       reinterpret_cast<void**>(&g_orange_progress), "capture scores: orange's HUD score reset"},
  };
  nevr_script_hooks::Register(rows, sizeof rows / sizeof rows[0]);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] capture scores fix registered mode_script=%016llx hooks=6 point_script=%016llx hooks=6 pins=4 (the "
      "RULES page's starting scores become each team's capture-point points at round start, and the HUD shows them; "
      "installed when each script loads)",
      static_cast<unsigned long long>(kModeScript), static_cast<unsigned long long>(kPointScript));
}

#ifdef NEVR_TEST_HOOKS
void ResetForTest() {
  g_registered = false;
  g_mailbox.store(nullptr);
  for (NodeFn& f : g_publish) f = nullptr;
  g_blue_reset = g_orange_reset = g_dispatcher = g_initializer = g_blue_progress = g_orange_progress = nullptr;
}
#endif

}  // namespace nevr_combat::capture_scores
