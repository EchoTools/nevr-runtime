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
namespace nevr_hook_guard {

/// Snapshot the first bytes at `target` under `name`. Called automatically by
/// PatchDetour (gamepatches_internal.h) after a successful install, so every
/// PatchDetour call site is covered without touching the site.
/// Once the fixed table is full the call is counted (DroppedCount) and reported at ERROR by the
/// next VerifyAll — never silent — but it never fails the boot: this is a diagnostic. It does
/// not log itself, because it runs in the boot phase, before the game's log exists.
void Record(const void* target, const char* name);

/// Record `target` under `name` with the first bytes the owner of the hook read there after enabling it
/// (16 bytes), instead of reading them now. The wrapper hook belongs to the host (runtime/host), which
/// hands the runtime what it wrote; a hook that changed the bytes since is then a mismatch at the first
/// VerifyAll. Same table, same never-fails contract as Record.
void RecordBytes(const void* target, const char* name, const unsigned char (&bytes)[16]);

/// Re-read every recorded address and compare. Logs one ERROR per mismatch with
/// the address, the name, expected vs actual bytes, and `context` (what just
/// happened — normally the plugin filename). Returns the number of mismatches.
/// Cheap: a handful of 16-byte memcmps.
int VerifyAll(const char* context);

/// Whether `target` is an address this runtime detoured and still holds: recorded, and its first bytes
/// equal the snapshot taken after install. Code that calls a game function the runtime may have
/// detoured (whose original prologue is then gone) uses this to call through our own detour, and to
/// refuse anything else.
bool IsOurDetour(const void* target);

/// How many addresses are currently recorded.
int RecordedCount();

/// How many Record() calls found the table full: detours nothing is guarding.
int DroppedCount();

/// The fixed number of addresses the table holds.
int Capacity();

#ifdef NEVR_TEST_HOOKS
/// Pause a writer after it fills a reserved slot but before readers may see it.
void SetBeforePublishForTest(void (*callback)());

/// Test-only: drop every recorded site. The guard is deliberately process-global
/// (hooked addresses live in loaded modules and are never freed in production),
/// which makes tests order-dependent without this — a freed-and-reallocated
/// scratch page can collide with a previous test's record and be counted twice.
void ResetForTest();
#endif

}  // namespace nevr_hook_guard
