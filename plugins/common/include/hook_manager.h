/*
 * hook_manager.h — Safe MinHook lifecycle management for plugins.
 *
 * Tracks hook targets and provides scoped cleanup. Prevents the
 * catastrophic MH_DisableHook(MH_ALL_HOOKS) pattern that disables
 * every hook in the process (crash handler, log filter, etc).
 *
 * Plugins should still call MH_Initialize() themselves in NvrPluginInit
 * before using HookManager (returns MH_ERROR_ALREADY_INITIALIZED
 * harmlessly if gamepatches already called it).
 *
 * Call RemoveAll() explicitly in NvrPluginShutdown — the destructor
 * does NOT auto-remove hooks to avoid order-of-destruction issues.
 */
#pragma once

#include <MinHook.h>
#include <vector>

namespace nevr {

class HookManager {
    std::vector<void*> m_hooks;

public:
    /* Install and enable a hook, tracking the target for cleanup.
     * On enable failure, the created hook is removed to avoid leaks. */
    MH_STATUS CreateAndEnable(void* target, void* detour, void** original) {
        MH_STATUS s = MH_CreateHook(target, detour, original);
        if (s != MH_OK) return s;
        s = MH_EnableHook(target);
        if (s != MH_OK) { MH_RemoveHook(target); return s; }
        m_hooks.push_back(target);
        return MH_OK;
    }

    /* Disable and remove only this manager's hooks. */
    void RemoveAll() {
        for (void* t : m_hooks) {
            MH_DisableHook(t);
            MH_RemoveHook(t);
        }
        m_hooks.clear();
    }

    /* Track an already-installed hook for cleanup.
     * Use when code already called MH_CreateHook + MH_EnableHook directly. */
    void Track(void* target) { m_hooks.push_back(target); }

    size_t count() const { return m_hooks.size(); }
};

} // namespace nevr
