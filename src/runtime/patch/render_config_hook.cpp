#include "runtime/patch/render_config_hook.h"

#include <winsock2.h>
#include <windows.h>

#include <dxgi.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>

#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/patching.h"

namespace nevr_render_config_hook {
namespace {

using nevr_render_config::Collector;
using nevr_render_config::Externals;
using nevr_render_config::MemoryReader;
using nevr_render_config::SettingsObserver;

// ApplyRuntimeGfxSettings @ 0x140c31870 (ReVault echovr.exe, read back): the first 27 bytes, up to and including
// `SUB RSP,0x60`. Nothing in them is position dependent.
constexpr std::array<std::uint8_t, 27> kApplyPrologue = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24,
                                                          0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x55, 0x41, 0x56,
                                                          0x41, 0x57, 0x48, 0x8b, 0xec, 0x48, 0x83, 0xec, 0x60};
// The GPU auto-selector @ 0x140c31e00 (read back): the first 26 bytes, up to and including `MOV RBX,RCX`.
constexpr std::array<std::uint8_t, 26> kSelectorPrologue = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24,
                                                             0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83,
                                                             0xec, 0x20, 0x48, 0x8b, 0xea, 0x48, 0x8b, 0xd9};

// Both return a 64-bit value. The game's functions return nothing the callers read (the callers at 0x14010920e and
// 0x1401091e9 ignore RAX), but passing RAX through unchanged makes the observers exact whatever the original leaves
// there.
using ApplyFn = std::uint64_t (*)(void* settings, void* game, int logFlag);
using SelectorFn = std::uint64_t (*)(void* settings, void* game);

// Reads this process's own memory. ReadProcessMemory on the current process answers FALSE for an unmapped or
// protected page instead of faulting, and the answer and the copy are one system call: a VirtualQuery check followed by
// a memcpy leaves a window in which the page can go away. The reads are a handful of fixed game globals once a second
// and one 40-byte settings struct per Apply call, so the system call costs nothing that matters.
class ProcessReader final : public MemoryReader {
 public:
  bool Read(std::uintptr_t address, void* out, std::size_t length) const override {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, length, &got) != 0 &&
           got == length;
  }
};

const ProcessReader g_processReader;

// ---- The two values that are not game memory. Both run once, at the first line. ----

std::string QueryGpuDriver(const std::string& gpuName) {
  // DXGI's IDXGIAdapter::CheckInterfaceSupport(IDXGIDevice) answers the user-mode driver version as a LARGE_INTEGER,
  // four 16-bit parts. The game has no NVAPI import, so this is the one source for it that needs nothing new in the
  // process: dxgi.dll is loaded by the game already and is looked up, never loaded here.
  HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
  if (dxgi == nullptr) return "unknown";
  using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
  const auto create =
      reinterpret_cast<CreateFactoryFn>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
  if (create == nullptr) return "unknown";
  IDXGIFactory1* factory = nullptr;
  if (FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || factory == nullptr) {
    return "unknown";
  }
  std::string driver = "unknown";
  for (UINT index = 0;; ++index) {
    IDXGIAdapter* adapter = nullptr;
    if (factory->EnumAdapters(index, &adapter) != S_OK || adapter == nullptr) break;
    DXGI_ADAPTER_DESC desc = {};
    if (SUCCEEDED(adapter->GetDesc(&desc))) {
      char narrow[256] = {};
      WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
      if (nevr_render_config::CleanText(narrow) == gpuName) {
        LARGE_INTEGER umd = {};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
          char text[48];
          std::snprintf(text, sizeof(text), "%u.%u.%u.%u", static_cast<unsigned>((umd.HighPart >> 16) & 0xFFFF),
                        static_cast<unsigned>(umd.HighPart & 0xFFFF), static_cast<unsigned>((umd.LowPart >> 16) & 0xFFFF),
                        static_cast<unsigned>(umd.LowPart & 0xFFFF));
          driver = text;
        }
        adapter->Release();
        break;
      }
    }
    adapter->Release();
  }
  factory->Release();
  return driver;
}

std::string QueryRuntimeVersion() {
  // The game loads LibOVRRT64_1.dll itself and resolves ovr_* by name; the version string comes the same way.
  HMODULE runtime = GetModuleHandleW(L"LibOVRRT64_1.dll");
  if (runtime == nullptr) return "unknown";
  using VersionFn = const char*(__cdecl*)();
  const auto version =
      reinterpret_cast<VersionFn>(reinterpret_cast<void*>(GetProcAddress(runtime, "ovr_GetVersionString")));
  if (version == nullptr) return "unknown";
  const char* text = version();
  if (text == nullptr) return "unknown";
  const std::string clean = nevr_render_config::CleanText(text);
  return clean.empty() ? std::string("unknown") : clean;
}

// ---- State. Written by Install and the hooks, read by Poll. ----

ApplyFn g_originalApply = nullptr;
SelectorFn g_originalSelector = nullptr;
std::atomic<const MemoryReader*> g_reader{&g_processReader};
Externals g_externals = {&QueryGpuDriver, &QueryRuntimeVersion};
SettingsObserver g_observer;
std::atomic<std::uintptr_t> g_bootReturn{0};
std::atomic<std::uintptr_t> g_lastApplyReturn{0};
std::atomic<std::uint64_t> g_swallowed{0};  // exceptions the hooks caught: they cannot log on the game's path
std::unique_ptr<Collector> g_collector;
std::atomic<bool> g_active{false};
std::atomic<bool> g_serverSkipLogged{false};
std::atomic<bool> g_checkFailureLogged{false};
std::atomic<bool> g_traceLogged{false};

// noinline: __builtin_return_address(0) must name this function's caller, the game's call site.
__attribute__((noinline)) std::uint64_t ApplyHook(void* settings, void* game, int logFlag) {
  const auto returnAddress = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
  const std::uint64_t result = g_originalApply(settings, game, logFlag);
  try {
    g_lastApplyReturn.store(returnAddress, std::memory_order_relaxed);
    g_observer.OnApply(*g_reader.load(std::memory_order_acquire), reinterpret_cast<std::uintptr_t>(settings),
                       returnAddress, g_bootReturn.load(std::memory_order_relaxed));
  } catch (const std::exception&) {
    g_swallowed.fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

std::uint64_t SelectorHook(void* settings, void* game) {
  const std::uint64_t result = g_originalSelector(settings, game);
  try {
    g_observer.OnSelector(reinterpret_cast<std::uintptr_t>(settings));
  } catch (const std::exception&) {
    g_swallowed.fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

std::string HexBytes(const void* address, std::size_t count) {
  if (address == nullptr) return "unreadable";
  std::string out;
  char byte[4];
  for (std::size_t i = 0; i < count; ++i) {
    std::snprintf(byte, sizeof(byte), "%02x", static_cast<const std::uint8_t*>(address)[i]);
    out += byte;
  }
  return out;
}

bool PrologueMatches(const char* name, std::uint64_t va, void* target, const std::uint8_t* expected,
                     std::size_t length) {
  if (nevr::ValidatePrologue(target, expected, length)) return true;
  Log(EchoVR::LogLevel::Warning,
      "[NEVR.RENDER] hook off name=%s va=0x%llx reason=prologue_mismatch expected=%s actual=%s "
      "(the settings fields of the render_config line read unavailable)",
      name, static_cast<unsigned long long>(va), HexBytes(expected, 8).c_str(), HexBytes(target, 8).c_str());
  return false;
}

}  // namespace

bool Install(std::uintptr_t gameBase, bool isServer) {
  if (isServer) {
    if (!g_serverSkipLogged.exchange(true)) {
      Log(EchoVR::LogLevel::Info, "[NEVR.RENDER] render_config skipped reason=server_mode (a server renders nothing)");
    }
    return false;
  }
  if (g_active.load(std::memory_order_acquire)) return true;
  if (gameBase == 0) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.RENDER] render_config off reason=no_game_base");
    return false;
  }

  g_bootReturn.store(nevr_render_config::Resolve(gameBase, nevr_render_config::kBootApplyReturnVa),
                     std::memory_order_relaxed);
  g_collector = std::make_unique<Collector>(*g_reader.load(std::memory_order_acquire), gameBase, g_observer, g_externals);
  // The check works with or without the hooks: without them the settings fields read unavailable.
  g_active.store(true, std::memory_order_release);

  void* const apply = nevr::ResolveVA_Checked(gameBase, nevr_render_config::kApplyVa);
  void* const selector = nevr::ResolveVA_Checked(gameBase, nevr_render_config::kSelectorVa);
  const bool applyOk = PrologueMatches("ApplyRuntimeGfxSettings", nevr_render_config::kApplyVa, apply,
                                       kApplyPrologue.data(), kApplyPrologue.size());
  const bool selectorOk = PrologueMatches("GfxSettingsAutoSelect", nevr_render_config::kSelectorVa, selector,
                                          kSelectorPrologue.data(), kSelectorPrologue.size());
  if (!applyOk || !selectorOk) return false;

  g_originalApply = reinterpret_cast<ApplyFn>(apply);
  g_originalSelector = reinterpret_cast<SelectorFn>(selector);
  const BOOL applyHooked = PatchDetour(&g_originalApply, reinterpret_cast<PVOID>(&ApplyHook), "ApplyRuntimeGfxSettings");
  const BOOL selectorHooked =
      PatchDetour(&g_originalSelector, reinterpret_cast<PVOID>(&SelectorHook), "GfxSettingsAutoSelect");
  Log(applyHooked && selectorHooked ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning,
      "[NEVR.RENDER] render_config hooks apply=%s selector=%s (settings source and values on the render_config line)",
      applyHooked ? "installed" : "failed", selectorHooked ? "installed" : "failed");
  return applyHooked && selectorHooked;
}

void Poll(std::uint64_t nowUs) {
  if (!g_active.load(std::memory_order_acquire)) return;
  try {
    const std::string line = g_collector->Poll(nowUs / 1000);
    if (!line.empty()) {
      Log(EchoVR::LogLevel::Info, "%s", line.c_str());
      // Once, with the first line: what the observers saw, so a `settings_source=unknown` or `menu_or_json` explains itself
      // (observers installed after the game's boot Apply call, or a boot return address that is not the one the game uses).
      if (!g_traceLogged.exchange(true)) {
        const SettingsObserver::View seen = g_observer.Snapshot();
        Log(EchoVR::LogLevel::Info,
            "[NEVR.RENDER] render_config settings_trace apply_calls=%llu read_failures=%llu last_apply_return=0x%llx "
            "boot_return=0x%llx",
            static_cast<unsigned long long>(seen.applyCalls), static_cast<unsigned long long>(seen.readFailures),
            static_cast<unsigned long long>(g_lastApplyReturn.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_bootReturn.load(std::memory_order_relaxed)));
      }
    }
  } catch (const std::exception& error) {
    // Out of memory while building the line: the frame goes on, and the failure is said once.
    if (!g_checkFailureLogged.exchange(true)) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.RENDER] render_config check failed reason=exception what=%s", error.what());
    }
  }
}

#ifdef NEVR_RENDER_CONFIG_TESTING
void SetReaderForTest(const MemoryReader* reader) {
  g_reader.store(reader != nullptr ? reader : &g_processReader, std::memory_order_release);
}
void SetExternalsForTest(Externals externals) { g_externals = std::move(externals); }
void SetBootReturnForTest(std::uintptr_t address) { g_bootReturn.store(address, std::memory_order_relaxed); }
std::uintptr_t LastApplyReturnForTest() { return g_lastApplyReturn.load(std::memory_order_relaxed); }
SettingsObserver::View ObservedForTest() { return g_observer.Snapshot(); }
bool ActiveForTest() { return g_active.load(std::memory_order_acquire); }

void ResetForTest() {
  g_active.store(false, std::memory_order_release);
  if (g_originalApply != nullptr) Hooking::Detach(reinterpret_cast<PVOID*>(&g_originalApply), reinterpret_cast<PVOID>(&ApplyHook));
  if (g_originalSelector != nullptr) {
    Hooking::Detach(reinterpret_cast<PVOID*>(&g_originalSelector), reinterpret_cast<PVOID>(&SelectorHook));
  }
  g_originalApply = nullptr;
  g_originalSelector = nullptr;
  g_collector.reset();
  g_reader.store(&g_processReader, std::memory_order_release);
  g_externals = {&QueryGpuDriver, &QueryRuntimeVersion};
  g_bootReturn.store(0);
  g_lastApplyReturn.store(0);
  g_swallowed.store(0);
  g_serverSkipLogged.store(false);
  g_checkFailureLogged.store(false);
  g_traceLogged.store(false);
  g_observer.Reset();
}
#endif

}  // namespace nevr_render_config_hook
