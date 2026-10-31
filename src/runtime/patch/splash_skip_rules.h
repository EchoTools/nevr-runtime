#pragma once
// When a button press skips the splash screen, apart from the hook that applies it (splash_skip.cpp) so the decisions
// are unit-tested without the game.
//
// Evidence (disassembly of the game binaries; PC VAs are echovr.exe):
//   - The splash page's script (75041e1b64042faa.dll, symbol ui_mnu_splash_screen) starts its continue chain on the UI
//     event continue_clicked (CSymbol64 0x801dec62515b8c8a); it ends in a SENDEVENTGLOBAL of splash_screen_continue,
//     which f86bfd7f395c027c.dll (ui_mnu_main_page_splash_screen) turns into "accept the splash version, enable
//     initial_popups_page". Whole-project search for the symbol (1,041 binaries): 75041e1b64042faa.dll and its Quest
//     twin only, so an element whose click symbol is continue_clicked is the splash's Continue button.
//   - The pointer-ray menu widget (CR15VRMenuWidgetCS, per-frame Update 0x140a7bdd0) turns a click on a hovered
//     element into DispatchComponentEvent(ctx, <element click symbol>, link0, link1, 1) (0x14050fc90). The same call,
//     with the Continue element's own link keys, is the click.
//   - The widget reads buttons from a table (FUN_140f9b770: bit 2 of an entry's flags, set for one update when the
//     input became active; FUN_140f9bf40: bit 4, set while it stays active) and the game itself polls ids 0, 1, 4 and 5
//     for a click. Which physical control each id is has not been traced; the hook logs the id that skipped.
// The skip is the click the on-screen button takes, issued when a press arrives while that button is live, so
// nothing the game gates behind the button is passed: a click the game would not take is not issued.

#include <cstddef>
#include <cstdint>

namespace nevr_splash_skip {

// continue_clicked: the splash page script's continue event (symbol corpus; CSymbol64Hash("continue_clicked")).
constexpr std::uint64_t kContinueClickedSymbol = 0x801dec62515b8c8aULL;

// The button ids the widget's own click paths poll (ids 0 and 1 by the pointing hand, 4 and 5).
constexpr std::uint64_t kPressButtonIds[] = {0, 1, 4, 5};

// The Continue button must have been live this long before a press counts, so the press that dismissed the page
// before it (or one that is still held) does not skip the next screen, and a player sees the screen at all.
constexpr std::uint64_t kMinPresentMs = 750;

// After a skip, the next one waits this long, for the stale edge flags the widget keeps while its input step is
// on cooldown (the flags are recomputed only every few updates).
constexpr std::uint64_t kLockoutMs = 1500;

// The first press id for which `isEdge(id)` is true, or -1.
template <typename EdgeFn>
long long PressedButton(EdgeFn isEdge) {
  for (const std::uint64_t id : kPressButtonIds) {
    if (isEdge(id)) return static_cast<long long>(id);
  }
  return -1;
}

// The dense index of the first element whose click symbol is continue_clicked, or -1. `symbolAt(i)` is the click
// symbol of dense element i (-1 for none).
template <typename SymbolAt>
long long FindContinueElement(std::size_t count, SymbolAt symbolAt) {
  for (std::size_t i = 0; i < count; ++i) {
    if (symbolAt(i) == kContinueClickedSymbol) return static_cast<long long>(i);
  }
  return -1;
}

// The state across updates. Step is called once per update of the widget that holds the Continue element (and once
// with present=false when that element is gone). It returns true exactly when a skip is to be issued now.
class Skipper {
 public:
  bool Step(bool present, bool buttonDown, std::uint64_t nowMs) {
    if (!present) {
      presentSince_ = 0;
      wasDown_ = buttonDown;
      return false;
    }
    if (presentSince_ == 0) presentSince_ = nowMs == 0 ? 1 : nowMs;
    const bool rising = buttonDown && !wasDown_;
    wasDown_ = buttonDown;
    if (!rising) return false;
    if (nowMs - presentSince_ < kMinPresentMs) return false;
    if (lastSkipMs_ != 0 && nowMs - lastSkipMs_ < kLockoutMs) return false;
    lastSkipMs_ = nowMs == 0 ? 1 : nowMs;
    return true;
  }

  // How long the Continue element has been live, for the log line.
  std::uint64_t PresentForMs(std::uint64_t nowMs) const { return presentSince_ == 0 ? 0 : nowMs - presentSince_; }

 private:
  std::uint64_t presentSince_ = 0;
  std::uint64_t lastSkipMs_ = 0;
  bool wasDown_ = false;
};

}  // namespace nevr_splash_skip
