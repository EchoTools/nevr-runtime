/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef NEVR_DLL_LOAD_HOOK_TESTING
#include <MinHook.h>
#endif

namespace nevr_dll_load_hook {

/* Callback signature: called after a DLL loads successfully.
 * dll_name: lowercase filename (e.g., "pnsdemo.dll")
 * module: the loaded HMODULE
 * Return: ignored. */
typedef void (*PatchCallback)(const char* dll_name, HMODULE module);

void Install();
void Shutdown();

/* Predicate for AddLoadFilter(): `lower_path` is the full path or name passed to LoadLibrary*,
 * lowercased, as wide characters. Return true to refuse the load. */
typedef bool (*LoadFilter)(const wchar_t* lower_path);

/* Refuse loads before they happen. This is the one place LoadLibraryA/W/ExA/ExW are hooked, so a
 * feature that must veto a load registers here instead of installing its own detour on the same
 * targets (MinHook allows one per target: the second install fails with MH_ERROR_ALREADY_CREATED,
 * #361). A refused load returns NULL with ERROR_MOD_NOT_FOUND. `name` is for the log line.
 * Applies to every LoadLibrary variant and every dwFlags value. `name` is truncated to 47 bytes
 * (the truncation is logged); IsLoadBlocked reports the truncated name. */
void AddLoadFilter(const char* name, LoadFilter filter);

/* The Oculus Platform SDK predicate: true when the FILE NAME of `lower_path` (the part after the
 * last '\\' or '/') contains "ovrplatform", e.g. libovrplatform64_1.dll. A folder named like that
 * does not match, so a library under such a directory is not refused. */
bool IsOculusPlatformPath(const wchar_t* lower_path);

/* True when a registered filter refuses `lower_path` (what the hooks call; exposed for tests).
 * `blocked_by` remains valid until the next IsLoadBlocked call on the same thread. */
bool IsLoadBlocked(const wchar_t* lower_path, const char** blocked_by);

/* Register a callback to fire when a DLL matching `dll_name` loads.
 * dll_name is matched case-insensitively against the filename only (not path).
 * The callback fires at most once per registration. Multiple callbacks per DLL are supported.
 * A DLL that is already loaded fires its callback before OnLoad returns, during the runtime's boot too
 * (an injector such as Revive's loads dxgi/d3d12 before the runtime starts, #495): a callback that
 * logs then reports through the boot tee, because Log() routes by boot phase (core/logging.h). */
void OnLoad(const char* dll_name, PatchCallback callback);

/* Trigger callbacks for a DLL that was loaded through a non-LoadLibrary path
 * (e.g., the game's CSysDLL_Load). Only fires each callback once per DLL. */
void FireCallbacksForModule(const char* lower_name, HMODULE module);

#ifdef NEVR_DLL_LOAD_HOOK_TESTING
using ModuleProbe = HMODULE(WINAPI*)(LPCSTR);
void SetModuleProbeForTest(ModuleProbe probe);

/* The three MinHook calls Install() makes, replaceable so a test can make creation or enabling
 * fail and watch the cleanup. A null member keeps the real function. */
struct MinHookOps {
  MH_STATUS(WINAPI* create)(LPVOID target, LPVOID detour, LPVOID* original);
  MH_STATUS(WINAPI* enable)(LPVOID target);
  MH_STATUS(WINAPI* remove)(LPVOID target);
};
void SetMinHookOpsForTest(const MinHookOps* ops);

/* The trampoline Install() holds for "LoadLibraryA"/"LoadLibraryW"/"LoadLibraryExA"/"LoadLibraryExW",
 * or nullptr when it holds none. */
void* OriginalForTest(const char* name);
#endif

} // namespace nevr_dll_load_hook
