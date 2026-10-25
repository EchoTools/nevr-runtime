#pragma once

// #341: under Wine a client whose X window is destroyed without a close request (XDestroyWindow:
// a window manager that kills the client's window, a nested display that goes away) keeps running with
// no window. Wine keeps the HWND valid ("destroyed from the outside"), so IsWindow stays true and the
// game's pump (echovr.exe 0x1400ea860) never sees WM_DESTROY or an invalid handle; its quit flag
// (engine+0x2ced0, read by CncaGame::RunMainLoop 0x14050bd28) stays 0.
//
// Wine records the X window of an HWND as the window property `__wine_x11_whole_window`. Measured on
// wine-11.19 / Xvfb with no window manager (results 2026-10-20, c03-probe/probe.log): the property is
// non-NULL while the window lives and NULL after XDestroyWindow, while IsWindow and IsWindowVisible stay
// 1. The watch arms when it has seen the property non-NULL (so it does nothing on native Windows, on
// a Wine driver that names the property otherwise, or before the window is mapped), and asks the game
// to quit when the property has then been NULL for kGraceMs. It asks the way the game's own
// ConsoleCloseHandler (0x1404f2a10) does: it stores 1 in the engine's quit flag.
// Pure: no window calls, no game memory, no logging; window_watch.cpp owns those.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nevr_window_watch {

/// How long the X window must stay gone before the quit is requested.
constexpr uint64_t kGraceMs = 2000;

struct Sample {
  bool windowValid;        // IsWindow(hwnd)
  bool x11WindowPresent;   // GetPropW(hwnd, L"__wine_x11_whole_window") != NULL
};

enum class Verdict {
  Nothing,
  Armed,        // the X window was seen for the first time: the watch is live
  RequestQuit,  // armed, and the X window has been gone for kGraceMs while the HWND is valid
};

class Watch {
 public:
  Verdict Observe(const Sample& sample, uint64_t nowMs) {
    if (fired_) return Verdict::Nothing;
    if (!sample.windowValid) {
      // The game's own pump sees an invalid handle and quits; nothing to add.
      missing_ = false;
      return Verdict::Nothing;
    }
    if (sample.x11WindowPresent) {
      missing_ = false;
      if (armed_) return Verdict::Nothing;
      armed_ = true;
      return Verdict::Armed;
    }
    if (!armed_) return Verdict::Nothing;
    if (!missing_) {
      missing_ = true;
      missingSinceMs_ = nowMs;
      return Verdict::Nothing;
    }
    if (nowMs - missingSinceMs_ < kGraceMs) return Verdict::Nothing;
    fired_ = true;
    return Verdict::RequestQuit;
  }

  bool Armed() const { return armed_; }
  bool Fired() const { return fired_; }

 private:
  bool armed_ = false;
  bool fired_ = false;
  bool missing_ = false;
  uint64_t missingSinceMs_ = 0;
};

/// echovr.exe 0x1404f2a25, in ConsoleCloseHandler: MOV RAX,[0x1420a0470]; TEST RAX,RAX; JZ +0xA;
/// MOV DWORD [RAX+0x2ced0],1. The bytes name both the engine global (through the rip-relative
/// displacement) and the quit flag's offset, so a match proves the two constants below.
constexpr uint8_t kQuitSiteBytes[22] = {0x48, 0x8B, 0x05, 0x44, 0xDA, 0xBA, 0x01, 0x48, 0x85, 0xC0, 0x74,
                                        0x0A, 0xC7, 0x80, 0xD0, 0xCE, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00};
constexpr uintptr_t kQuitSiteRva = 0x4F2A25;      // 0x1404f2a25 - 0x140000000
constexpr uintptr_t kEngineGlobalRva = 0x20A0470;  // 0x1420a0470: the pointer to the engine
constexpr uintptr_t kQuitFlagOffset = 0x2CED0;     // engine + 0x2ced0: non-zero ends RunMainLoop

inline bool QuitSiteMatches(const void* site) {
  return site != nullptr && std::memcmp(site, kQuitSiteBytes, sizeof(kQuitSiteBytes)) == 0;
}

/// Polls the game's window once a second on a client (nowMs: a millisecond clock); a no-op on a server.
void Poll(uint64_t nowMs);

}  // namespace nevr_window_watch
