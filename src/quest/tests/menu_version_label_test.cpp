// Host test for "the Quest main menu shows the nEVR version" (#747): the real handler
// (src/quest/social/menu_version_label.cpp), built without exceptions as the sentinel builds it, driven through the
// thunk's entry with a stand-in for libr15's CCanvasUICS::SetText. No GOT slot is written: got_pinned_test.cpp resolves
// the pinned slot (JUMP_SLOT 0x36ff908) in the real library. What the label says is the PC rules header, tested by
// src/runtime/tests/test_menu_version_label.cpp.
//
// Built and run by `just test-quest-hooks`.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "quest/social/menu_version_label.h"
#include "quest/tests/test_check.h"
#include "runtime/patch/menu_version_label_rules.h"

namespace {

namespace mv = quest_social::menu_version;

constexpr std::uint64_t kLobbyVersion = nevr_menu_version_label::kLobbyVersionItem;
constexpr std::uint64_t kPublisherLock = 0x1234567812345678ULL;  // any other item
constexpr const char* kLabel = "nEVR 5.17.0";

mv::Thunk::Fn Entry() { return reinterpret_cast<mv::Thunk::Fn>(mv::Thunk::EntryAddress()); }

// The stand-in for the game's function: it records its arguments and answers like the real one (0 for a null text).
int g_calls = 0;
void* g_canvas = nullptr;
std::uint32_t g_handle = 0;
std::uint64_t g_item = 0;
const char* g_text = nullptr;
char g_copied[64] = {};

std::uint64_t StandIn(void* canvas, std::uint32_t handle, std::uint64_t item, const char* text) {
  ++g_calls;
  g_canvas = canvas;
  g_handle = handle;
  g_item = item;
  g_text = text;
  if (text == nullptr) return 0;
  std::strncpy(g_copied, text, sizeof g_copied - 1);
  return 0x1000 + item;
}

void Reset() {
  g_calls = 0;
  g_canvas = nullptr;
  g_handle = 0;
  g_item = 0;
  g_text = nullptr;
  g_copied[0] = '\0';
  mv::ResetForTest();
  *mv::Thunk::OriginalOut() = reinterpret_cast<void*>(&StandIn);
}

int g_canvasObject = 0;
const char* const kStock = "2023-04-04T17:47:47+00:00";

// Without the hook armed the entry is the game's call: the stock date is drawn.
void WithoutTheHookTheGameDrawsItsText() {
  Reset();
  QCHECK(Entry()(&g_canvasObject, 7, kLobbyVersion, kStock) == 0x1000 + kLobbyVersion);
  QCHECK(g_text == kStock);
  QCHECK(mv::Replaced() == 0);
}

// The lobby_version item with a text is drawn as the label; the game's function gets the label, the canvas and the handle
// unchanged, and its answer comes back.
void TheLobbyVersionItemIsDrawnAsTheLabel() {
  Reset();
  mv::ArmForTest(kLabel);
  QCHECK(Entry()(&g_canvasObject, 7, kLobbyVersion, kStock) == 0x1000 + kLobbyVersion);
  QCHECK(g_calls == 1);
  QCHECK(std::strcmp(g_copied, kLabel) == 0);
  QCHECK(g_text != kStock);
  QCHECK(g_canvas == &g_canvasObject);
  QCHECK(g_handle == 7);
  QCHECK(g_item == kLobbyVersion);
  QCHECK(mv::Replaced() == 1);
}

// The script clears the item when its page is disabled: the empty text stays empty (the same pointer reaches the game).
void AnEmptyTextStaysEmpty() {
  Reset();
  mv::ArmForTest(kLabel);
  const char* empty = "";
  Entry()(&g_canvasObject, 7, kLobbyVersion, empty);
  QCHECK(g_calls == 1);
  QCHECK(g_text == empty);
  QCHECK(mv::Replaced() == 0);
}

// A null text is the game's own no-op (it returns 0): the hook does not turn it into a label.
void ANullTextIsLeftToTheGame() {
  Reset();
  mv::ArmForTest(kLabel);
  QCHECK(Entry()(&g_canvasObject, 7, kLobbyVersion, nullptr) == 0);
  QCHECK(g_calls == 1);
  QCHECK(g_text == nullptr);
  QCHECK(mv::Replaced() == 0);
}

// Every other item (the credits page sets text every frame; the other two items of the versions page are not ours) passes
// through untouched, whatever its text.
void EveryOtherItemPassesThrough() {
  Reset();
  mv::ArmForTest(kLabel);
  const char* text = "rad15_live";
  QCHECK(Entry()(&g_canvasObject, 3, kPublisherLock, text) == 0x1000 + kPublisherLock);
  QCHECK(g_text == text);
  QCHECK(g_item == kPublisherLock);
  QCHECK(g_handle == 3);
  QCHECK(mv::Replaced() == 0);
}

// The count is replacements only, and each one draws the label again (the page sets the item each time it is enabled).
void EveryReplacementDrawsTheLabelAndIsCounted() {
  Reset();
  mv::ArmForTest(kLabel);
  Entry()(&g_canvasObject, 1, kLobbyVersion, "a");
  Entry()(&g_canvasObject, 1, kPublisherLock, "b");
  QCHECK(std::strcmp(g_copied, "b") == 0);  // the other item was not replaced
  Entry()(&g_canvasObject, 1, kLobbyVersion, "c");
  QCHECK(g_calls == 3);
  QCHECK(mv::Replaced() == 2);
  QCHECK(std::strcmp(g_copied, kLabel) == 0);
}

// A label that is not set draws nothing: an armed hook with an empty label leaves the call alone.
void AnEmptyLabelLeavesTheCallAlone() {
  Reset();
  mv::ArmForTest("");
  Entry()(&g_canvasObject, 7, kLobbyVersion, kStock);
  QCHECK(g_text == kStock);
  QCHECK(mv::Replaced() == 0);
}

// The label is "nEVR <version>" as on PC; a development version loses its "+<commit>" build metadata, and a version too long
// for the slot is cut to its width (the PC rules, nevr_menu_version_label::FitLabel).
void TheLabelIsTheProjectVersion() {
  QCHECK(mv::Label() == "nEVR 5.17.0");  // the build's version: the host test's stand-in header says 5.17.0
  QCHECK(mv::LabelFor("5.17.0") == "nEVR 5.17.0");
  QCHECK(mv::LabelFor("5.17.1-dev.3+abcdef012345") == "nEVR 5.17.1-dev.3");
  QCHECK(mv::LabelFor("5.17.1-dev.1234567890123456789") == "nEVR 5.17.1-dev.12345678");  // 24 characters
  QCHECK(mv::LabelFor("5.17.0").size() <= nevr_menu_version_label::kMaxLabelChars);
}

// On a host libr15 is not loaded: the install is refused, says so, and reports 0 so the caller goes on.
void TheInstallIsRefusedWhenLibr15IsNotLoaded() {
  Reset();
  QCHECK(mv::InstallMenuVersionLabelHook() == 0);
}

}  // namespace

int main() {
  WithoutTheHookTheGameDrawsItsText();
  TheLobbyVersionItemIsDrawnAsTheLabel();
  AnEmptyTextStaysEmpty();
  ANullTextIsLeftToTheGame();
  EveryOtherItemPassesThrough();
  EveryReplacementDrawsTheLabelAndIsCounted();
  AnEmptyLabelLeavesTheCallAlone();
  TheLabelIsTheProjectVersion();
  TheInstallIsRefusedWhenLibr15IsNotLoaded();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "menu_version_label_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("menu_version_label_test: all checks pass\n");
  return 0;
}
