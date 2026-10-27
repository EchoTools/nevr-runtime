/* Inline (detour) hook for an arm64 function that the GOT cannot reach.
 *
 * GotHook replaces a pointer a module calls through. A call made directly, through a
 * vtable, or through a pointer the module stored itself has no such pointer, so it
 * needs a patch at the function's entry (ADR 0003, "Internal hook"). This header is that
 * backend, arm64 only: it overwrites the first four instructions of the target with
 *
 *     LDR x16, #8        ; x16 = the detour address stored after the branch
 *     BR  x16            ; tail-jump into the detour
 *     .quad detour       ; 64-bit absolute address
 *
 * and builds, in a fresh page, a trampoline that runs the four displaced instructions and
 * then branches to target+16. The detour receives the trampoline as its original, so it
 * can call the real function. x16 (IP0) is the one register the patch uses; the AAPCS64
 * lets a call site treat it as clobbered by any call.
 *
 * What is checked before any byte is written:
 *   - the target is 4-byte aligned and its 16 patched bytes stay inside one page (the page
 *     size is sysconf(_SC_PAGESIZE)),
 *   - the four words at the target equal the caller's expected prologue, which the caller
 *     got from the pinned build and the decompilation (never a guess),
 *   - none of the four words is PC-relative (IsPcRelative). A displaced PC-relative
 *     instruction would read a different address in the trampoline, and this backend does
 *     not rewrite them. Such a target is refused, not relocated,
 *   - the target's page is mapped r-xp in /proc/self/maps; a page that is not is refused.
 *
 * Write order and atomicity. Under one process-wide lock the page is made writable once and
 * the words are stored in this order: the detour address (words 2-3), the BR (word 1), then
 * the LDR (word 0). Word 0 is the commit: until it is stored the target still starts with its
 * original instructions, so the detour is not reachable. A failure before the commit restores
 * words 1-3 and frees the trampoline. The page is made executable again and the instruction
 * cache is flushed after the commit.
 *
 * Limits, stated so nobody mistakes the checks for a proof:
 *   - Install must run before any thread can be executing the target's first 16 bytes. A
 *     thread that has executed the original word 0 but not yet word 1 would reach the new BR
 *     with x16 unset and jump to a stale address. The sentinel installs at process start,
 *     before the game's code that calls these functions runs (the caller's obligation).
 *   - While the page is writable (one mprotect window, microseconds) no thread can execute
 *     any code on that page. The same obligation as above covers it.
 *   - No caller may branch to target+4..target+15 (a jump into the middle of the patched
 *     window). The backend cannot see branches; the pinned decompilation's xrefs must show
 *     none. This is checked by the human who pins the address, not by the code.
 *   - There is no Remove. A hook is process-lifetime state, as with GotHook. Restoring the
 *     prologue while threads may be inside the detour is the same hazard as un-hooking a GOT
 *     slot from a destructor.
 *   - A failed restore of page protection after the commit leaves the page writable and not
 *     executable. That status (kRestoreProtectFailed) is fatal to the caller; the code cannot
 *     recover from it. It is reported, not hidden.
 *
 * Install is reached only through InstallInlineThunk (hook_install.h), the same rule as
 * GotHook: the entry the detour jumps to is a CallbackThunk entry, so the frame sensor walks it.
 *
 * Each install logs one structured line through hook_log.h, success or failure.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace sentinel {

enum class InlineStatus : std::uint8_t {
  kOk,
  kBadArgument,
  kNotWordAligned,         // the target is not 4-byte aligned
  kWindowStraddlesPage,    // the 16 patched bytes cross a page boundary
  kPrologueMismatch,       // the four words at the target are not the expected prologue
  kPcRelativeInstruction,  // a displaced word depends on its own address (see IsPcRelative)
  kPageNotExecutable,      // the target's page is not mapped r-xp in /proc/self/maps
  kAlreadyInstalled,       // this target already has a detour in this process
  kRegistryFull,           // more inline hooks than kMaxInlineHooks in one process
  kTrampolineFailed,       // mmap or mprotect of the trampoline page failed
  kProtectFailed,          // the target's page could not be made writable
  kWriteVerifyFailed,      // a read-back before the commit differs from what was written; rolled back
  kRestoreProtectFailed,   // the page could not be made executable after the commit; fatal (see above)
};

// Stable tokens for log lines and tests.
const char* InlineStatusName(InlineStatus status);

// The four instructions the patch displaces, as the caller pinned them.
struct InlinePrologue {
  std::uint32_t words[4];
};

// LDR x16, #8 (A64 literal load, Rt = x16, offset +8).
inline constexpr std::uint32_t kLdrX16Literal8 = 0x58000050u;
// BR x16.
inline constexpr std::uint32_t kBrX16 = 0xD61F0200u;
// The patch length in bytes: three words for the LDR and BR and the 64-bit address.
inline constexpr std::size_t kInlinePatchBytes = 16;
// More than this many inline hooks in one process is refused (kRegistryFull).
inline constexpr std::size_t kMaxInlineHooks = 16;

// True when the A64 instruction encodes a dependency on its own address: B, BL, B.cond,
// CBZ/CBNZ, TBZ/TBNZ, ADR, ADRP, and LDR/LDRSW/PRFM (literal). Such a word cannot be displaced
// unchanged into a trampoline at another address, so the backend refuses it. Pure: the
// table is exercised by the host test on fixed encodings.
bool IsPcRelative(std::uint32_t insn) noexcept;

// Read-only: the target's address and prologue checks, without touching the page. Install
// runs the same checks under its lock. A non-kOk result changes nothing.
InlineStatus CheckInlineTarget(const void* target, const InlinePrologue& expected) noexcept;

// The one way to install an inline hook is through InstallInlineThunk (hook_install.h).
// InlineHook::Install is private: a detour that is not a CallbackThunk entry would not be a
// root of the frame sensor.
struct InlineThunkInstaller;
struct InlineHookTestAccess;

class InlineHook {
 public:
  InlineHook() = delete;

 private:
  friend struct InlineThunkInstaller;
  friend struct InlineHookTestAccess;

  // Patches `target` so that calling it runs `detour`. Stores the trampoline (the real
  // function, runnable) in *originalOut before the commit, and restores *originalOut to its
  // entry value on any failure. Returns kOk only when the hook is live and verified.
  static InlineStatus Install(const void* target, const InlinePrologue& expected, void* detour,
                              void** originalOut);
};

}  // namespace sentinel
