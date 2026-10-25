#include "runtime/frame/window_watch.h"

#include <windows.h>

#include <cstring>

#include "abi/echovr_functions.h"
#include "core/globals.h"
#include "core/logging.h"
#include "runtime/lifecycle/cli.h"  // g_isServer

namespace nevr_window_watch {

namespace {

constexpr uint64_t kPollIntervalMs = 1000;
constexpr wchar_t kGameWindowClass[] = L"WindowsClass";  // registered by echovr.exe 0x1400e9ef0
constexpr wchar_t kWineX11WholeWindow[] = L"__wine_x11_whole_window";

Watch g_watch;
HWND g_hwnd = nullptr;
uint64_t g_lastPollMs = 0;

BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM out) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId()) return TRUE;
  wchar_t name[32] = {};
  if (GetClassNameW(hwnd, name, 31) == 0 || wcscmp(name, kGameWindowClass) != 0) return TRUE;
  *reinterpret_cast<HWND*>(out) = hwnd;
  return FALSE;
}

// Stores 1 in the engine's quit flag, as the game's own ConsoleCloseHandler does. The bytes of that
// function's store are checked first (a different build has different offsets).
bool RequestGameQuit() {
  const char* base = EchoVR::g_GameBaseAddress;
  if (base == nullptr) return false;
  if (!QuitSiteMatches(base + kQuitSiteRva)) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] window watch: the quit-flag site at game+0x%llX does not match — not requesting a quit (#341)",
        static_cast<unsigned long long>(kQuitSiteRva));
    return false;
  }
  void* engine = nullptr;
  std::memcpy(&engine, base + kEngineGlobalRva, sizeof(engine));
  if (engine == nullptr) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] window watch: the engine pointer is null — not requesting a quit (#341)");
    return false;
  }
  const LONG one = 1;
  std::memcpy(static_cast<char*>(engine) + kQuitFlagOffset, &one, sizeof(one));
  return true;
}

}  // namespace

void Poll(uint64_t nowMs) {
  if (g_isServer) return;
  if (g_watch.Fired()) return;
  if (g_lastPollMs != 0 && nowMs - g_lastPollMs < kPollIntervalMs) return;
  g_lastPollMs = nowMs;

  if (g_hwnd == nullptr || !IsWindow(g_hwnd)) {
    g_hwnd = nullptr;
    HWND found = nullptr;
    EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&found));
    if (found == nullptr) return;
    g_hwnd = found;
  }

  Sample sample = {};
  sample.windowValid = IsWindow(g_hwnd) != FALSE;
  sample.x11WindowPresent = GetPropW(g_hwnd, kWineX11WholeWindow) != nullptr;
  switch (g_watch.Observe(sample, nowMs)) {
    case Verdict::Nothing:
      return;
    case Verdict::Armed:
      Log(EchoVR::LogLevel::Info,
          "[NEVR.PATCH] window watch armed hwnd=%p x11_window=%p — the game's X window is tracked (#341)",
          static_cast<void*>(g_hwnd), GetPropW(g_hwnd, kWineX11WholeWindow));
      return;
    case Verdict::RequestQuit: {
      const bool requested = RequestGameQuit();
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] window watch: the game's X window is gone but its HWND %p is still valid (destroyed from "
          "the outside) — %s (#341)",
          static_cast<void*>(g_hwnd), requested ? "quit requested through the game's quit flag" : "quit NOT requested");
      return;
    }
  }
}

}  // namespace nevr_window_watch
