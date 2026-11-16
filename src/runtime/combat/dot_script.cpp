#include "runtime/combat/dot_script.h"

#include <atomic>
#include <cstring>
#include <iterator>

#include "core/logging.h"
#include "runtime/hook/script_hook_calls.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::dot_script {
namespace {

using ExecuteFn = std::uint64_t (*)(void* node, void* ctx);
using NextNodeFn = std::uint64_t (*)(std::uint64_t node, std::uint64_t port);
using ReadContextFn = std::uint64_t (*)(void* block, void* node);

// The rows' original slots. Every row is detour-only: the two stock E rows call their original through these, and the
// three scene-set N rows never call theirs (the slots exist because the table writes one on each install).
ExecuteFn g_execute = nullptr;
NextNodeFn g_next_node = nullptr;
void* g_show_original = nullptr;
void* g_hide_original = nullptr;
void* g_bubble_original = nullptr;
std::atomic_bool g_registered{false};

std::uint64_t PlaySoundExecute(void* node, void* ctx) {
  auto* at = static_cast<std::uint8_t*>(node) + kEventOffset;
  std::uint32_t event = 0;
  std::memcpy(&event, at, sizeof event);  // a dword: the builders write it with `mov dword [rbp-0x11], imm32`
  const std::uint32_t played = Event(event);
  if (played != event) std::memcpy(at, &played, sizeof played);
  return g_execute(node, ctx);
}

std::uint64_t NextNodeHook(std::uint64_t node, std::uint64_t port) {
  NEVR_SCRIPT_HOOK_CALL("DOT FIELD resolve_next_node");
  return NextNode(node, g_next_node(node, port));
}

// The scene-set hooks: the rebuilt nodes' return is the helper's, as the stock nodes' is (see SceneSetNode).
std::uint64_t ShowSceneSetHook(std::uint64_t* node) { return SceneSetNode(node, 1); }    // ARC_SCENE on
std::uint64_t HideSceneSetHook(std::uint64_t* node) { return SceneSetNode(node, 0); }    // ARC_SCENE off (the hide node)
std::uint64_t BubbleSceneSetHook(std::uint64_t* node) { return SceneSetNode(node, 0); }  // e94b7b9c: flag 0, as in his build

}  // namespace

std::uint32_t Event(std::uint32_t stock) {
  if (stock == kFieldOnEvent) return kStunActivate;
  if (stock == kFieldStopEvent) return kStunStop;
  return stock;
}

std::uint64_t NextNode(std::uint64_t node, std::uint64_t stock) {
  if (node == kSetupNode && stock == kFirstSceneSetNode) return kAfterSceneSetNodes;
  if (node == kHideNode && stock == kHideStockNext) return kSoundStop;                      // hide, then the sound stops
  if (node == kSoundNode && stock == kSoundStartNext) return kShowNode;                     // the field's sound, then the show
  if (node == kShowNode && stock == kShowStockNext) return kChainEnd;                       // the show node ends the chain
  return stock;
}

std::uint64_t SceneSetNode(std::uint64_t* node, std::uint16_t flag) {
  const auto* object = reinterpret_cast<const std::uint8_t*>(node[0]);  // *param_1
  const auto* context = *reinterpret_cast<const std::uint8_t* const*>(object + kContextPointer);
  std::uint8_t block[kBlockSize];
  FillSceneSetBlock(block, context, kArcScene, flag);
  auto* base = static_cast<std::uint8_t*>(nevr_script_hooks::LoadedBase(kScript));
  if (base == nullptr) return 0;  // the module is not loaded: nothing to call; 0 is the game's success value
  return reinterpret_cast<ReadContextFn>(base + kReadContextVariablesRva)(block, node);
}

void FillSceneSetBlock(std::uint8_t* block, const std::uint8_t* context, std::uint64_t scene, std::uint16_t flag) {
  std::memset(block, 0, kBlockSize);
  for (std::size_t i = 0; i < std::size(kBlockContextValues); ++i) {
    std::memcpy(block + kBlockContextValues[i], context + kContextFirst + i * sizeof(std::uint64_t),
                sizeof(std::uint64_t));
  }
  const std::uint64_t component = kFieldComponent;
  std::memcpy(block + kBlockComponent, &component, sizeof component);
  std::memcpy(block + kBlockScene, &scene, sizeof scene);
  std::memcpy(block + kBlockFlag, &flag, sizeof flag);  // the flag is the low byte of a word: the high byte stays 0
  const std::uint32_t tail = kBlockTailValue;
  std::memcpy(block + kBlockTail, &tail, sizeof tail);
}

void Register() {
  if (g_registered.exchange(true)) return;
  const nevr_script_hooks::Hook rows[] = {
      {kScript, kPlaySoundExecuteRva, kPlaySoundPrologue, sizeof kPlaySoundPrologue,
       reinterpret_cast<void*>(&PlaySoundExecute), reinterpret_cast<void**>(&g_execute),
       "DOT FIELD: the stun grenade's sound events"},
      {kScript, kNextNodeRva, kNextNodePrologue, sizeof kNextNodePrologue, reinterpret_cast<void*>(&NextNodeHook),
       reinterpret_cast<void**>(&g_next_node), "DOT FIELD: setup skips the three scene-set nodes; the sound leads to the show node; hide goes on to the sound stop"},
      {kScript, kShowSceneSetRva, kShowSceneSetPrologue, sizeof kShowSceneSetPrologue,
       reinterpret_cast<void*>(&ShowSceneSetHook), &g_show_original,
       "DOT FIELD: show node (9507dc91) sets the field's bubble scene on"},
      {kScript, kHideSceneSetRva, kHideSceneSetPrologue, sizeof kHideSceneSetPrologue,
       reinterpret_cast<void*>(&HideSceneSetHook), &g_hide_original,
       "DOT FIELD: hide node (69412c20) sets the field's bubble scene off"},
      {kScript, kBubbleSceneSetRva, kBubbleSceneSetPrologue, sizeof kBubbleSceneSetPrologue,
       reinterpret_cast<void*>(&BubbleSceneSetHook), &g_bubble_original,
       "DOT FIELD: node e94b7b9c stores the bubble scene with flag 0"},
      {kScript, kReadContextVariablesRva, kReadContextPrologue, sizeof kReadContextPrologue, nullptr, nullptr,
       "DOT FIELD: the scene-set helper the rebuilt nodes call (pinned, not hooked)"},
  };
  constexpr std::size_t kRowCount = sizeof rows / sizeof rows[0];
  std::size_t hooks = 0;  // rows with a detour
  std::size_t pins = 0;   // rows that only check their bytes
  for (const nevr_script_hooks::Hook& row : rows) (row.detour != nullptr ? hooks : pins) += 1;
  nevr_script_hooks::Register(rows, kRowCount);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] DOT FIELD fix registered script=%016llx hooks=%llu pins=%llu (the field plays the stun grenade's "
      "sounds; its setup skips three scene-set nodes on a model it does not have; its sound starts the bubble's show node "
      "and its hide node goes on to the field sound's stop; the scene sets load the bubble's scene; installed when the "
      "DOT's script loads)",
      static_cast<unsigned long long>(kScript), static_cast<unsigned long long>(hooks),
      static_cast<unsigned long long>(pins));
}

#ifdef NEVR_TEST_HOOKS
void ResetForTest() { g_registered = false; }
#endif

}  // namespace nevr_combat::dot_script
