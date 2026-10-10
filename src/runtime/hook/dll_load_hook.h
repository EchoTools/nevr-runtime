/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif

namespace DllLoadHook {

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
 * Applies to every LoadLibrary variant and every dwFlags value. */
void AddLoadFilter(const char* name, LoadFilter filter);

/* True when a registered filter refuses `lower_path` (what the hooks call; exposed for tests). */
bool IsLoadBlocked(const wchar_t* lower_path, const char** blocked_by);

/* Register a callback to fire when a DLL matching `dll_name` loads.
 * dll_name is matched case-insensitively against the filename only (not path).
 * The callback fires once per load. Multiple callbacks per DLL are supported. */
void OnLoad(const char* dll_name, PatchCallback callback);

/* Trigger callbacks for a DLL that was loaded through a non-LoadLibrary path
 * (e.g., the game's CSysDLL_Load). Only fires each callback once per DLL. */
void FireCallbacksForModule(const char* lower_name, HMODULE module);

} // namespace DllLoadHook
