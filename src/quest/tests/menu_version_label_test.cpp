// Host test for the Quest menu version label (#747): the real handler (src/quest/social/menu_version_label.cpp), built
// without exceptions as the sentinel builds it, driven through the thunk's entry with a stand-in for the game's
// CCanvasUICS::SetText (libr15 0x149111c in ReVault; 0x139111c at link time). No libr15 and no inline patch is
// involved here: the prologue bytes are compared with the real library by got_pinned_test.cpp
// (`just test-quest-hooks-pinned`), and the relocation of the displaced `cbz` by arm64_reloc_test.cpp.
//
// Built and run by `just test-quest-hooks`.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "quest/social/menu_version_label.h"
#include "quest/tests/test_check.h"
#include "runtime/patch/menu_version_label_rules.h"

namespace {

namespace mv = quest_social::menu_version;
using nevr_menu_version_label::kLobbyVersionItem;

using SetTextFn = std::uint64_t (*)(void*, std::uint32_t, std::uint64_t, const char*);

// What the stand-in for the game's SetText was handed on its last call.
struct Seen {
  int calls = 0;
  void* canvas = nullptr;
  std::uint32_t handle = 0;
  std::uint64_t item = 0;
  bool textNull = false;
  std::string text;
};
Seen g_seen;

constexpr std::uint64_t kGameResult = 0x1badcafeULL;

std::uint64_t GameSetText(void* canvas, std::uint32_t handle, std::uint64_t item, const char* text) {
  ++g_seen.calls;
  g_seen.canvas = canvas;
  g_seen.handle = handle;
  g_seen.item = item;
  g_seen.textNull = text == nullptr;
  g_seen.text = text != nullptr ? text : "";
  return kGameResult;
}

SetTextFn Entry() { return reinterpret_cast<SetTextFn>(mv::SetTextEntryForTest()); }

void Reset() {
  mv::ResetForTest();
  g_seen = Seen{};
  mv::SetTextOriginalForTest(reinterpret_cast<void*>(&GameSetText));
}

int g_canvasObject = 0;
void* const kCanvas = &g_canvasObject;

constexpr const char* kStockText = "2023-04-04T17:47:47+00:00";

void TheLobbyVersionTextIsReplacedAndTheGameIsCalledOnceWithEverythingElseUnchanged() {
  Reset();
  mv::ArmForTest("nEVR 5.16.1");
  const std::uint64_t result = Entry()(kCanvas, 7u, kLobbyVersionItem, kStockText);
  QCHECK(result == kGameResult);
  QCHECK(g_seen.calls == 1);
  QCHECK(g_seen.text == "nEVR 5.16.1");
  QCHECK(g_seen.canvas == kCanvas);
  QCHECK(g_seen.handle == 7u);
  QCHECK(g_seen.item == kLobbyVersionItem);
}

void EveryOtherItemKeepsTheScriptsText() {
  Reset();
  mv::ArmForTest("nEVR 5.16.1");
  const std::uint64_t others[] = {0ULL, kLobbyVersionItem + 1, kLobbyVersionItem - 1, ~0ULL};
  for (const std::uint64_t item : others) {
    g_seen = Seen{};
    QCHECK(Entry()(kCanvas, 1u, item, "631547") == kGameResult);
    QCHECK(g_seen.calls == 1);
    QCHECK(g_seen.text == "631547");
    QCHECK(g_seen.item == item);
  }
}

// The script writes "" to the item when its page is disabled, and the label must not reappear then.
void AnEmptyTextClearsTheItemAndANullTextStaysNull() {
  Reset();
  mv::ArmForTest("nEVR 5.16.1");
  QCHECK(Entry()(kCanvas, 1u, kLobbyVersionItem, "") == kGameResult);
  QCHECK(g_seen.calls == 1 && !g_seen.textNull && g_seen.text.empty());
  g_seen = Seen{};
  QCHECK(Entry()(kCanvas, 1u, kLobbyVersionItem, nullptr) == kGameResult);
  QCHECK(g_seen.calls == 1 && g_seen.textNull);
}

void WithNoLabelSetTheCallPassesThrough() {
  Reset();
  QCHECK(Entry()(kCanvas, 1u, kLobbyVersionItem, kStockText) == kGameResult);
  QCHECK(g_seen.calls == 1 && g_seen.text == kStockText);
}

void ADisarmedHookPassesThrough() {
  Reset();
  mv::ArmForTest("nEVR 5.16.1");
  mv::ResetForTest();
  mv::SetTextOriginalForTest(reinterpret_cast<void*>(&GameSetText));
  QCHECK(Entry()(kCanvas, 1u, kLobbyVersionItem, kStockText) == kGameResult);
  QCHECK(g_seen.calls == 1 && g_seen.text == kStockText);
}

// The label buffer is fixed; a longer label is cut to fit and terminated, never overrun.
void ALabelLongerThanTheBufferIsCutAndTerminated() {
  Reset();
  const std::string longLabel(64, 'x');
  mv::ArmForTest(longLabel.c_str());
  QCHECK(Entry()(kCanvas, 1u, kLobbyVersionItem, kStockText) == kGameResult);
  QCHECK(g_seen.text.size() == 31);
  QCHECK(g_seen.text == std::string(31, 'x'));
}

// The first four words of CCanvasUICS::SetText, as the bytes ReVault prints for libr15 0x149111c
// (`revault_disassemble 0x149111c`): cbz x3, ... / sub sp, sp, #0x20 / stp x19, x30, [sp, #0x10] / mov x19, x3.
void ThePrologueWordsAreTheBytesReVaultShowsForSetText() {
  const unsigned char bytes[16] = {0x43, 0x02, 0x00, 0xb4, 0xff, 0x83, 0x00, 0xd1,
                                   0xf3, 0x7b, 0x01, 0xa9, 0xf3, 0x03, 0x03, 0xaa};
  std::uint32_t words[4] = {};
  std::memcpy(words, bytes, sizeof words);
  for (int i = 0; i < 4; ++i) QCHECK(words[i] == mv::kSetTextPrologueWords[i]);
  QCHECK(mv::kSetTextVaddr == 0x149111cULL - 0x100000ULL);
}

// No libr15 is loaded in a host process, so the install must refuse and install nothing.
void TheInstallRefusesWhenLibr15IsNotLoaded() {
  Reset();
  QCHECK(mv::InstallMenuVersionLabelHook() == 0);
  QCHECK(Entry()(kCanvas, 1u, kLobbyVersionItem, kStockText) == kGameResult);
  QCHECK(g_seen.text == kStockText);
}

}  // namespace

int main() {
  TheLobbyVersionTextIsReplacedAndTheGameIsCalledOnceWithEverythingElseUnchanged();
  EveryOtherItemKeepsTheScriptsText();
  AnEmptyTextClearsTheItemAndANullTextStaysNull();
  WithNoLabelSetTheCallPassesThrough();
  ADisarmedHookPassesThrough();
  ALabelLongerThanTheBufferIsCutAndTerminated();
  ThePrologueWordsAreTheBytesReVaultShowsForSetText();
  TheInstallRefusesWhenLibr15IsNotLoaded();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "menu_version_label_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("menu_version_label_test: all checks pass\n");
  return 0;
}
