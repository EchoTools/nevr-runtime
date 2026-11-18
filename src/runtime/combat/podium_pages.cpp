// #2242 (H-POD-a): the podium terminal's rules page 2, its page state and its start-button gate. See podium_pages.h for the six
// rows, what each does, why they are hooks and not Cat's caves, and the debt note.

#include "runtime/combat/podium_pages.h"

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstring>

#include "core/logging.h"
#include "runtime/combat/rules_state.h"
#include "runtime/hook/script_hook_calls.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::podium_pages {
namespace {

using NodeFn = std::uint64_t (*)(void* ctx);

// The stock bodies, set by the script hook table when each row installs (the trampolines). Null until then.
NodeFn g_restart = nullptr;
NodeFn g_menu1 = nullptr;
NodeFn g_rules = nullptr;
NodeFn g_enable = nullptr;
NodeFn g_match = nullptr;
void* g_alignStock = nullptr;  // the alignment row never calls its stock body; the table still wants a slot for it

const volatile std::uint8_t* g_mailbox = nullptr;  // echovr.exe's mailbox (rules::kMailboxRva), set at Register
std::atomic_bool g_registered{false};
PageState g_page;  // the terminal's page state. Written and read on the game loop thread only (every row runs there).

std::uint8_t PayloadMap() { return g_mailbox != nullptr ? g_mailbox[rules::kPayloadMap] : 0; }

// --- the detours -------------------------------------------------------------------------------------------------------

// NEXT on the rules page: the thread takes the next node the page's state gives, and the node returns 0 (the stock body's
// own pin: it ends here, as its cave did). Off the rules page the stock body runs.
std::uint64_t RestartHook(void* ctx) {
  NEVR_SCRIPT_HOOK_CALL("podium RESTART");
  if (MenuPageOf(ctx) != kMenuPageRules) return g_restart(ctx);
  const std::uint64_t next = RestartNext(g_page, PayloadMap());
  std::memcpy(static_cast<std::uint8_t*>(ctx) + kNodeNext, &next, sizeof next);
  return 0;
}

// The node that starts the rules page: the stock body sets MenuPage = 1 (and returns 0), then the page starts where the
// payload map says.
std::uint64_t Menu1Hook(void* ctx) {
  NEVR_SCRIPT_HOOK_CALL("podium MENU1");
  const std::uint64_t returned = g_menu1(ctx);
  MenuOneStarts(g_page, PayloadMap());
  return returned;
}

// RULES and BACK: the page and the press flag are set, then the stock body runs with the press as it was.
std::uint64_t RulesHook(void* ctx) {
  NEVR_SCRIPT_HOOK_CALL("podium RULES");
  RulesPress(g_page, MenuPageOf(ctx) == kMenuPageRules, PayloadMap());
  return g_rules(ctx);
}

// The start button's enable checks: EXEC_TRUE on the rules page, the stock check otherwise.
std::uint64_t EnableHook(void* ctx) {
  NEVR_SCRIPT_HOOK_CALL("podium enable check");
  if (EnableShowsTrue(MenuPageOf(ctx))) return kExecTrue;
  return g_enable(ctx);
}

std::uint64_t MatchHook(void* ctx) {
  NEVR_SCRIPT_HOOK_CALL("podium match check");
  if (EnableShowsTrue(MenuPageOf(ctx))) return kExecTrue;
  return g_match(ctx);
}

// The podium's alignment check returns 1 (his `mov al, 1; ret`): the layout pass runs. Its stock body is not called.
std::uint64_t AlignHook(void* /*ctx*/) {
  NEVR_SCRIPT_HOOK_CALL("podium alignment check");
  return kAlignResult;
}

}  // namespace

std::int32_t MenuPageOf(const void* ctx) {
  if (ctx == nullptr) return 0;
  std::uint64_t node = 0;  // [ctx] is the script's node pointer
  std::memcpy(&node, ctx, sizeof node);
  if (node == 0) return 0;
  std::uint64_t data = 0;  // [node + kDataPointer] is the script's data
  std::memcpy(&data, reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(node)) + kDataPointer, sizeof data);
  if (data == 0) return 0;
  std::int32_t menuPage = 0;
  std::memcpy(&menuPage, reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(data)) + kMenuPage,
              sizeof menuPage);
  return menuPage;
}

std::uint64_t RestartNext(PageState& state, std::uint8_t payloadMap) {
  if (payloadMap != 0) return kHideFrom;  // a payload map has one page: NEXT keeps it and only hides the labels
  state.page = (state.page == kPageOne) ? kPageTwo : kPageOne;
  return state.page == kPageTwo ? kHideFrom : kShowFrom;
}

void MenuOneStarts(PageState& state, std::uint8_t payloadMap) {
  state.page = payloadMap != 0 ? kPageTwo : kPageOne;
}

void RulesPress(PageState& state, bool backNow, std::uint8_t payloadMap) {
  if (backNow) {
    state.page = kPageOne;
    state.press = kPressBack;
    return;
  }
  state.page = payloadMap != 0 ? kPageTwo : kPageOne;
  state.press = kPressRules;
}

bool EnableShowsTrue(std::int32_t menuPage) { return menuPage == kMenuPageRules; }

void Rows(nevr_script_hooks::Hook (&out)[kRowCount]) {
  out[0] = {kScript, kRestartRva, kRestartBytes, sizeof kRestartBytes, reinterpret_cast<void*>(&RestartHook),
            reinterpret_cast<void**>(&g_restart), "podium RESTART: NEXT on the rules page"};
  out[1] = {kScript, kMenu1Rva, kMenu1Bytes, sizeof kMenu1Bytes, reinterpret_cast<void*>(&Menu1Hook),
            reinterpret_cast<void**>(&g_menu1), "podium MENU1: the rules page starts on page 1 or 2"};
  out[2] = {kScript, kRulesCheckRva, kRulesCheckBytes, sizeof kRulesCheckBytes, reinterpret_cast<void*>(&RulesHook),
            reinterpret_cast<void**>(&g_rules), "podium RULES/BACK: the page and the press flag"};
  out[3] = {kScript, kEnableCheckRva, kEnableCheckBytes, sizeof kEnableCheckBytes, reinterpret_cast<void*>(&EnableHook),
            reinterpret_cast<void**>(&g_enable), "podium start enable check 1: true on the rules page"};
  out[4] = {kScript, kMatchCheckRva, kMatchCheckBytes, sizeof kMatchCheckBytes, reinterpret_cast<void*>(&MatchHook),
            reinterpret_cast<void**>(&g_match), "podium start enable check 2: true on the rules page"};
  out[5] = {kScript, kAlignCheckRva, kAlignCheckBytes, sizeof kAlignCheckBytes, reinterpret_cast<void*>(&AlignHook),
            &g_alignStock, "podium alignment check: 1 (the layout pass runs)"};
}

void Register() {
  auto* exe = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
  if (exe == nullptr || g_registered.exchange(true)) return;
  g_mailbox = exe + rules::kMailboxRva;
  nevr_script_hooks::Hook rows[kRowCount];
  Rows(rows);
  nevr_script_hooks::Register(rows, kRowCount);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] podium pages fix registered script=%016llx hooks=6 (NEXT pages the rules page, RULES/BACK set its "
      "page, the start button is enabled on the rules page; installed when the terminal loads; NAMED DEBT, PLAN-v7 section 16)",
      static_cast<unsigned long long>(kScript));
}

#ifdef NEVR_TEST_HOOKS
void ResetForTest() {
  g_page = PageState{};
  g_mailbox = nullptr;
  g_registered.store(false);
}

void SetMailboxForTest(const volatile std::uint8_t* mailbox) { g_mailbox = mailbox; }
#endif

}  // namespace nevr_combat::podium_pages
