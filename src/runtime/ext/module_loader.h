#pragma once

#include "extension/module_interface.h"

/// Load a module DLL from the modules/ subdirectory next to echovr.exe.
/// Calls NvrModuleInit — on failure, calls FatalError (game exits).
/// Prefer RegisterStaticModule for modules compiled directly into BugSplat64.dll.
void LoadModule(const char* name, const NvrModuleContext* ctx);

/// Register a statically-linked module that was initialized directly (no DLL).
/// The module's init was already called; this records it so TickModules,
/// NotifyModulesStateChange, and UnloadModules find it. hModule must be nullptr
/// for static modules — UnloadModules skips FreeLibrary on them.
void RegisterStaticModule(const char* name, uint32_t api_version,
                          NvrModuleOnFrame_fn on_frame,
                          NvrModuleOnGameStateChange_fn on_state,
                          NvrModuleShutdown_fn shutdown);

/// Unload all modules in reverse order, calling NvrModuleShutdown on each.
void UnloadModules();

/// Tick all modules that export NvrModuleOnFrame.
void TickModules(const NvrModuleContext* ctx);

/// Notify all modules of a game state change.
void NotifyModulesStateChange(const NvrModuleContext* ctx, uint32_t old_state, uint32_t new_state);

/// Store the module context globally so GetModuleContext() works.
void SetModuleContext(const NvrModuleContext* ctx);

/// Get the stored module context.
const NvrModuleContext* GetModuleContext();

/// Register a named proc for cross-module resolution.
void RegisterModuleProc(const char* name, void* proc);

/// Resolve a named proc registered by another module.
void* ResolveModuleProc(const char* name);

// ============================================================================
// Test hooks — enabled only when NEVR_TEST_HOOKS is defined.
// Allow unit tests to inject mock callbacks into the module registry
// so TickModules / NotifyModulesStateChange behavior can be verified.
// ============================================================================

#ifdef NEVR_TEST_HOOKS
void TestHook_RegisterModuleOnFrame(NvrModuleOnFrame_fn fn);
void TestHook_RegisterModuleOnStateChange(NvrModuleOnGameStateChange_fn fn);
void TestHook_ClearModules();
#endif
