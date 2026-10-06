#pragma once

// Host-side runtime-export surface for the BugSplat64.dll / nevr.dll split
// (docs/design/2026-10-05-bugsplat-nevr-split.md step 5).
//
// The stable host keeps the game-facing 26-symbol export set, but the
// implementations of the 14 runtime-side symbols (NEVR_ScheduleReturnToLobby,
// NEVR_GetUPnPConfig, the three resource-override wrappers, the three TokenAuth
// accessors, and the platform_compat/token_auth module entry points) now live in
// nevr.dll. The host therefore exports thin, null-safe forwarding wrappers that
// resolve the runtime's symbol from the lazily-loaded nevr.dll module at call
// time — never through an eager import or a PE forward, which would stop the
// host loading when the runtime is absent.
//
// The host does NOT link nevr_core/nevr_abi/the modules, so none of their static
// state (globals, MinHook, singletons) is duplicated into BugSplat64.dll; the
// runtime keeps the single copy. The one piece of state here — the published
// runtime module handle — exists only in the host and is a pointer, not a copy
// of any runtime-owned object.

#include <windows.h>

namespace NevrHost {

/// Publish the nevr.dll module handle once RuntimeLoader::Load succeeds, so the
/// forwarding wrappers below can resolve the runtime's implementations. Called
/// from RuntimeRendezvous after a successful load. The handle is process-lifetime
/// (a failed/loaded runtime is never freed), so a raw pointer is sufficient.
void PublishRuntimeModule(HMODULE module);

/// Null-safe, cached resolution of a runtime export by name. Returns nullptr
/// when the runtime is absent, the symbol is missing, or the load failed —
/// every forwarding wrapper degrades to a no-op / default value in that case.
FARPROC ResolveRuntimeExport(const char* name);

}  // namespace NevrHost
