#pragma once

#include <cstdint>

/// HookGuard — runtime detection of a second detour on an address we already own.
///
/// N84. The static checker (tools/verify_hook_invariants.py) finds double-detours
/// by scanning source, so it only sees plugins in this tree. A third-party plugin
/// is a DLL we never compile — no source-level check can reach it.
///
/// So this detects the EFFECT rather than the SOURCE: snapshot the bytes at every
/// address we detour, immediately after installing, then re-verify after each
/// plugin loads. If the bytes changed, something re-hooked our target. That works
/// against code we have never seen, which is the whole point.
///
/// Why it matters here specifically: gamepatches links MinHook from
/// extern/minhook, plugins link the vcpkg minhook::minhook port. Separate static
/// copies, separate private hook tables — MinHook cannot tell you that another
/// MinHook already owns the address. The second installer reads our JMP stub and
/// trampolines off it as if it were the original prologue.
namespace HookGuard {

/// Snapshot the first bytes at `target` under `name`. Called automatically by
/// PatchDetour (gamepatches_internal.h) after a successful install, so every
/// PatchDetour call site is covered without touching the site.
/// Silently ignored once the fixed table is full — this is a diagnostic, and it
/// must never be the reason a boot fails.
void Record(const void* target, const char* name);

/// Re-read every recorded address and compare. Logs one ERROR per mismatch with
/// the address, the name, expected vs actual bytes, and `context` (what just
/// happened — normally the plugin filename). Returns the number of mismatches.
/// Cheap: a handful of 16-byte memcmps.
int VerifyAll(const char* context);

/// How many addresses are currently recorded.
int RecordedCount();

#ifdef NEVR_TEST_HOOKS
/// Test-only: drop every recorded site. The guard is deliberately process-global
/// (hooked addresses live in loaded modules and are never freed in production),
/// which makes tests order-dependent without this — a freed-and-reallocated
/// scratch page can collide with a previous test's record and be counted twice.
void ResetForTest();
#endif

}  // namespace HookGuard
