// dllmain.cpp : the entry point of nevr-runtime.dll.
//
// The host, BugSplat64.dll (runtime/host/host_dllmain.cpp), is the DLL the game imports and the one that
// installs the rendezvous detour. It loads this DLL on the game's first call into the main wrapper and
// calls the descriptor it exports (NEVR_GetRuntimeApi, runtime/lifecycle/runtime_entry.cpp); the runtime
// initializes from there, on the game's main thread, outside the loader lock. DllMain therefore does nothing at
// attach: no initialization, no file I/O, no thread.
#include "core/pch.h"
#include "runtime/patch/asset_cdn.h"
#include "runtime/patch/invite_service.h"
#include "runtime/patch/broadcaster_guard.h"
#include "runtime/ext/module_loader.h"
#include "runtime/compat/ws_bridge.h"
#include "runtime/patch/binary_bug_fixes.h"
#include "runtime/log/builtin_filter.h"
#include "runtime/patch/resource_override.h"
#include "runtime/patch/plugin_assets.h"

// --- DLL entry point ---

BOOL APIENTRY DllMain(HMODULE, DWORD ul_reason_for_call, LPVOID lpReserved) {
  switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
      break;
    case DLL_PROCESS_DETACH:
      // Only do clean shutdown on dynamic unload (lpReserved == NULL).
      // During process termination (lpReserved != NULL), threads are already
      // dead and joining would deadlock under the loader lock.
      //
      // Nothing frees this DLL in production: the host loads it once and keeps it mapped for the life of the
      // process (a failed runtime stays mapped too), so this list does not run there. It stays, in its order,
      // for a host that unloads the runtime and for the test hosts that do.
      if (lpReserved == NULL) {
        UnloadModules();
        nevr_asset_cdn::Shutdown();
        nevr_invite_service::Shutdown();
        nevr_binary_bug_fixes::Shutdown();
        nevr_broadcaster_guard::Shutdown();
        nevr_builtin_log_filter::Shutdown();
        nevr::assets::ShutdownPluginAssets();
        ShutdownResourceOverride();
        ShutdownWebSocketBridge();
      }
      break;
  }
  return TRUE;
}
