/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
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

/* The LoadLibraryA/W/ExA/ExW variants Install() hooks. */
constexpr int kLoadLibraryVariantCount = 4;

/* Hooks the four LoadLibrary variants and reports each outcome through the boot tee. Returns how many
 * variants are hooked: kLoadLibraryVariantCount on full success, less on a partial install (loads
 * through an unhooked variant run no filter, observer or callback). 0 on a non-Windows build. */
int Install();
void Shutdown();

/* Writes the lines a load made under the loader lock left waiting (#688), unless this thread holds the
 * lock. Called at graceful shutdown, before the process is ended, so a held line is not lost with it. */
void FlushPendingLines();

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

/* Called after every LoadLibraryA/W/ExA/ExW call the hooks see returns from the real loader, whatever it
 * loaded and whether it succeeded, on the calling thread, before the call returns to its caller. The loader lock
 * is released by then, so an observer may patch a module the call mapped before the caller can resolve any of
 * its exports. Observers are skipped when the calling thread holds the loader lock (a LoadLibrary call from
 * inside a DllMain): the next call that returns outside the lock runs them. */
typedef void (*PostLoadObserver)();
void AddPostLoadObserver(PostLoadObserver observer);

/* True when the calling thread owns the process's loader lock (PEB LoaderLock). */
bool LoaderLockHeldByThisThread();

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

/* Sets the four trampoline pointers OriginalForTest() reads to nullptr. Shutdown() only disables the
 * hooks; after MH_Uninitialize() the pointers would otherwise still name freed trampolines. */
void ResetTrampolinesForTest();
#endif

} // namespace nevr_dll_load_hook
