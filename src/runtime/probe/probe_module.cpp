// The probe module: it reads maps_probe.yaml once, writes the probe_hello line and drives the probes from the game-thread
// frame (TickModules, once per frame on the game loop thread; the probes read game memory only there).

#include <winsock2.h>  // before windows.h (tools/tests/test_header_include_order.py)
#include <windows.h>

#include <atomic>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "abi/echovr_functions.h"
#include "abi/game_level.h"
#include "core/logging.h"
#include "core/nevr_root.h"
#include "extension/module_interface.h"
#include "runtime/ext/module_loader.h"
#include "runtime/hook/addresses.h"
#include "runtime/lifecycle/cli.h"
#include "runtime/lifecycle/config.h"
#include "runtime/lifecycle/crash_recovery.h"
#include "runtime/lifecycle/game_image_guard.h"
#include "runtime/lifecycle/readable_memory.h"
#include "runtime/lifecycle/return_to_lobby_queue.h"
#include "runtime/probe/probe_game.h"

namespace nevr_map_probe {

namespace {

constexpr const wchar_t* kConfigFileName = L"maps_probe.yaml";
constexpr const char* kModuleName = "map_probes";

std::atomic<std::uint64_t> g_frame{0};

// Game thread only.
bool g_started = false;
bool g_active = false;
bool g_tickFailLogged = false;
ProbeConfig g_config;

// Reads the whole file: it is the owner's own file in the nEVR folder of a probe build, and its size is the file's.
bool ReadConfigFile(std::string* text) {
  const std::wstring root = nevr_root::RootW();
  if (root.empty()) return false;
  const std::wstring path = nevr_root::Join(root, kConfigFileName);
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size = {};
  bool ok = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0;
  if (ok) {
    text->resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    ok = text->empty() || (ReadFile(file, text->data(), static_cast<DWORD>(text->size()), &read, nullptr) != FALSE &&
                           read == text->size());
  }
  CloseHandle(file);
  return ok;
}

void Start() {
  g_started = true;
  std::string text;
  const bool found = ReadConfigFile(&text);
  if (found) g_config = ParseProbeConfig(text);
  const bool supported = EchoVR::g_GameBaseAddress != nullptr &&
                         nevr_game_image_guard::IsSupportedGameModule(EchoVR::g_GameBaseAddress);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.PROBE] probe_hello role=%s config=%s build=%s rules=%d skew=%d skew_expect=%s skew_version=%u sublevel=%d "
      "ignored=%u (a probe build: it measures and is never shipped)",
      g_isServer ? "server" : "client", found ? "read" : "missing", supported ? "supported" : "unsupported",
      g_config.rules ? 1 : 0, g_config.skew ? 1 : 0, SkewExpectName(g_config.skew_expect), g_config.skew_version,
      g_config.sublevel ? 1 : 0, g_config.ignored_lines);
  if (!supported) return;

  const char* refusal = nullptr;
  if (g_config.sublevel || g_config.skew) refusal = InstallLevelEvents();
  if (g_config.rules) RulesStart();
  if (g_config.sublevel) SublevelStart(refusal);
  if (g_config.skew) SkewStart(g_config, refusal);
  g_active = g_config.rules || g_config.sublevel || g_config.skew;
}

void OnFrame(const NevrModuleContext*) {
  const unsigned long loop = GameLoopThreadId();
  if (loop == 0 || loop != GetCurrentThreadId()) return;
  try {
    if (!g_started) Start();
    if (!g_active) return;
    const std::uint64_t frame = g_frame.fetch_add(1, std::memory_order_acq_rel) + 1;
    static std::vector<LevelEvent> events;
    DrainLevelEvents(&events);
    if (g_config.rules) RulesTick(frame);
    if (g_config.sublevel) SublevelTick(frame, events);
    if (g_config.skew) SkewTick(frame, events);
  } catch (const std::exception&) {
    if (!g_tickFailLogged) {
      g_tickFailLogged = true;
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PROBE] probe_error reason=tick_failed (a probe frame threw a std::exception; that frame's lines are lost)");
    }
  }
}

void OnShutdown() {
  if (!g_active || !g_config.sublevel) return;
  try {
    SublevelFinish();
  } catch (const std::exception&) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PROBE] probe_error reason=finish_failed (the P-SUBLEVEL summary was not written)");
  }
}

}  // namespace

void Register() {
  RegisterStaticModule(kModuleName, static_cast<uint32_t>(NEVR_MODULE_API_VERSION), &OnFrame, nullptr, &OnShutdown);
}

void LogNotPinned(const char* probe, const char* what, std::uint64_t va, const std::uint8_t* at, const std::uint8_t* expected,
                  std::size_t size, const char* consequence) {
  const bool readable = at != nullptr && nevr_crash_recovery::IsReadableMemory(at, size);
  Log(EchoVR::LogLevel::Warning, "[NEVR.PROBE] %s %s not pinned %s (the game build is not the one this probe was written against, so %s)",
      probe, what, NotPinnedFields(va, expected, readable ? at : nullptr, size).c_str(), consequence);
}

std::uint64_t ProbeFrame() { return g_frame.load(std::memory_order_acquire); }

void LogLines(const std::vector<std::string>& lines) {
  for (const std::string& line : lines) Log(EchoVR::LogLevel::Info, "[NEVR.PROBE] %s", line.c_str());
}

void* NetGame() { return nevr_return_to_lobby_queue::NetGameFromContext(g_pGame); }

std::int32_t NetState() {
  const void* net_game = NetGame();
  if (net_game == nullptr) return 0;
  std::int32_t state = 0;
  std::memcpy(&state, static_cast<const std::uint8_t*>(net_game) + nevr_patch_addresses::NETGAME_STATE_OFFSET, sizeof(state));
  return state;
}

std::uint64_t MainLevel() { return EchoVR::ReadMainLevelSymbol(g_pGame, EchoVR::kPcMainLevelSymbolOffset); }

}  // namespace nevr_map_probe
