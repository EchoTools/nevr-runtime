// PB0: the probe module. It reads maps_probe.yaml once, writes the probe_hello line and drives the probes from the
// game-thread frame.

#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "abi/echovr_functions.h"
#include "abi/game_level.h"
#include "core/logging.h"
#include "core/nevr_root.h"
#include "extension/module_interface.h"
#include "runtime/ext/module_loader.h"
#include "runtime/lifecycle/cli.h"
#include "runtime/lifecycle/config.h"
#include "runtime/lifecycle/crash_recovery.h"
#include "runtime/lifecycle/game_image_guard.h"
#include "runtime/lifecycle/return_to_lobby_queue.h"
#include "runtime/maps/probe/probe_game.h"

namespace nevr_maps_probe {

namespace {

constexpr DWORD kMaxConfigBytes = 64 * 1024;

std::atomic<std::uint64_t> g_frame{0};

// Game thread only.
bool g_started = false;
bool g_active = false;
bool g_tickFailLogged = false;
ProbeConfig g_config;

bool ReadConfigFile(std::string* text) {
  const std::wstring root = nevr_root::RootW();
  if (root.empty()) return false;
  const std::wstring path = nevr_root::Join(root, L"maps_probe.yaml");
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  text->resize(kMaxConfigBytes);
  DWORD read = 0;
  const BOOL ok = ReadFile(file, text->data(), kMaxConfigBytes, &read, nullptr);
  CloseHandle(file);
  if (!ok) return false;
  text->resize(read);
  return true;
}

void Start() {
  g_started = true;
  std::string text;
  const bool found = ReadConfigFile(&text);
  if (found) g_config = ParseProbeConfig(text);
  const bool supported = EchoVR::g_GameBaseAddress != nullptr &&
                         nevr_game_image_guard::IsSupportedGameModule(EchoVR::g_GameBaseAddress);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.MAPS] probe_hello role=%s config=%s build=%s rules=%d skew=%d skew_expect=%s skew_version=%u "
      "sublevel=%d ignored=%u",
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
  } catch (...) {
    if (!g_tickFailLogged) {
      g_tickFailLogged = true;
      Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] probe_error reason=tick_failed (a probe frame threw; it is skipped)");
    }
  }
}

void OnShutdown() {
  if (!g_active || !g_config.sublevel) return;
  try {
    SublevelFinish();
  } catch (...) {
  }
}

}  // namespace

void Register() {
  RegisterStaticModule("maps_probes", static_cast<uint32_t>(NEVR_MODULE_API_VERSION), &OnFrame, nullptr, &OnShutdown);
}

std::uint64_t ProbeFrame() { return g_frame.load(std::memory_order_acquire); }

void LogLines(const std::vector<std::string>& lines) {
  for (const std::string& line : lines) Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] %s", line.c_str());
}

void* NetGame() { return nevr_return_to_lobby_queue::NetGameFromContext(g_pGame); }

std::int32_t NetState() {
  const void* net_game = NetGame();
  if (net_game == nullptr) return 0;
  std::int32_t state = 0;
  std::memcpy(&state, net_game, sizeof(state));
  return state;
}

std::uint64_t MainLevel() { return EchoVR::ReadMainLevelSymbol(g_pGame, EchoVR::kPcMainLevelSymbolOffset); }

}  // namespace nevr_maps_probe
