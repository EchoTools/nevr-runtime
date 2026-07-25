#include "module_loader.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "common/globals.h"
#include "common/logging.h"

struct LoadedModule {
  HMODULE                       hModule;
  const char*                   name;
  NvrModuleInit_fn              init;
  NvrModuleShutdown_fn          shutdown;
  NvrModuleOnFrame_fn           on_frame;
  NvrModuleOnGameStateChange_fn on_state;
  std::string                   path;
};

static std::vector<LoadedModule> g_modules;
static std::unordered_map<std::string, void*> g_moduleProcs;
static const NvrModuleContext* g_storedCtx = nullptr;

void SetModuleContext(const NvrModuleContext* ctx) {
  g_storedCtx = ctx;
}

const NvrModuleContext* GetModuleContext() {
  return g_storedCtx;
}

void RegisterModuleProc(const char* name, void* proc) {
  g_moduleProcs[name] = proc;
}

void* ResolveModuleProc(const char* name) {
  auto it = g_moduleProcs.find(name);
  if (it != g_moduleProcs.end()) return it->second;
  return NULL;
}

void LoadModule(const char* name, const NvrModuleContext* ctx) {
  // Resolve modules/ directory relative to echovr.exe
  CHAR moduleDir[MAX_PATH] = {0};
  GetModuleFileNameA((HMODULE)EchoVR::g_GameBaseAddress, moduleDir, MAX_PATH);
  CHAR* lastSlash = strrchr(moduleDir, '\\');
  if (lastSlash) *(lastSlash + 1) = '\0';

  std::string dllPath = std::string(moduleDir) + "modules\\" + name + ".dll";

  Log(EchoVR::LogLevel::Info, "[NEVR.MODULE] Loading: %s", name);

  HMODULE hModule = LoadLibraryA(dllPath.c_str());
  if (!hModule) {
    DWORD err = GetLastError();
    Log(EchoVR::LogLevel::Error, "[NEVR.MODULE] Failed to load %s: error %lu (path: %s)", name, err, dllPath.c_str());
    FatalError("Required module missing", name);
    return;
  }

  auto initFn = (NvrModuleInit_fn)GetProcAddress(hModule, "NvrModuleInit");
  if (!initFn) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MODULE] %s: missing NvrModuleInit export", name);
    FreeLibrary(hModule);
    FatalError("Module missing NvrModuleInit", name);
    return;
  }

  int result = initFn(ctx);
  if (result != 0) {
    Log(EchoVR::LogLevel::Error, "[NEVR.MODULE] %s: init failed with code %d", name, result);
    FreeLibrary(hModule);
    FatalError("Module init failed", name);
    return;
  }

  auto shutdownFn = (NvrModuleShutdown_fn)GetProcAddress(hModule, "NvrModuleShutdown");
  auto onFrameFn = (NvrModuleOnFrame_fn)GetProcAddress(hModule, "NvrModuleOnFrame");
  auto onStateFn = (NvrModuleOnGameStateChange_fn)GetProcAddress(hModule, "NvrModuleOnGameStateChange");

  g_modules.push_back({hModule, name, initFn, shutdownFn, onFrameFn, onStateFn, dllPath});
  Log(EchoVR::LogLevel::Info, "[NEVR.MODULE] Loaded: %s", name);
}

void UnloadModules() {
  for (auto it = g_modules.rbegin(); it != g_modules.rend(); ++it) {
    if (it->shutdown) {
      it->shutdown();
    }
    FreeLibrary(it->hModule);
  }
  g_modules.clear();
  g_moduleProcs.clear();
}

void TickModules(const NvrModuleContext* ctx) {
  for (auto& m : g_modules) {
    if (m.on_frame) {
      m.on_frame(ctx);
    }
  }
}

void NotifyModulesStateChange(const NvrModuleContext* ctx, uint32_t old_state, uint32_t new_state) {
  for (auto& m : g_modules) {
    if (m.on_state) {
      m.on_state(ctx, old_state, new_state);
    }
  }
}

// ============================================================================
// Test hooks — NEVR_TEST_HOOKS enables unit-test injection of mock callbacks
// into the static module registry, so behavioral tests can verify that
// TickModules / NotifyModulesStateChange actually fire registered callbacks.
// These are NEVER compiled into production builds (gamepatches.dll).
// ============================================================================

#ifdef NEVR_TEST_HOOKS

void TestHook_RegisterModuleOnFrame(NvrModuleOnFrame_fn fn) {
  LoadedModule m = {};
  m.hModule = nullptr;
  m.name = "test_mock";
  m.init = nullptr;
  m.shutdown = nullptr;
  m.on_frame = fn;
  m.on_state = nullptr;
  g_modules.push_back(m);
}

void TestHook_RegisterModuleOnStateChange(NvrModuleOnGameStateChange_fn fn) {
  LoadedModule m = {};
  m.hModule = nullptr;
  m.name = "test_mock";
  m.init = nullptr;
  m.shutdown = nullptr;
  m.on_frame = nullptr;
  m.on_state = fn;
  g_modules.push_back(m);
}

void TestHook_ClearModules() {
  g_modules.clear();
}

#endif  // NEVR_TEST_HOOKS
