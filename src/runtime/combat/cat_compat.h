#pragma once

// What he_is_the_cat's EchoCombat sources (dronefix.cpp, powerboost.cpp, veilnet.cpp in this directory, ported from
// heisthecat31/EchoCombat nevr-pack at c3ff4b54 nearly as he wrote them) need to build inside nevr-runtime.dll with
// MinGW instead of as three MSVC plugins:
//
//  * Microsoft Detours' calls become the runtime's PatchDetour (MinHook, recorded by HookGuard), and every detour first
//    passes the combat write gate (write_gate.h): a target that overlaps anything the runtime hooked or wrote is refused,
//    and the transaction it is in fails, as a Detours commit does.
//  * MSVC's __try / __except (spelled EC_TRY / EC_EXCEPT in the ported files) become a frame on a thread-local stack and one vectored handler that jumps back to the
//    innermost frame on a fault (setjmp / longjmp, as crash_recovery.cpp's game-loop recovery does). The filter
//    expression is not evaluated: the handler takes the faults listed in cat_compat.cpp, whatever the filter said.
//  * PowerBoost.ini / CombatBots.ini reads become config.yaml reads (`combat.settings.<section>.<key>`); no file beside
//    the DLL is read.
//  * The plugin logger becomes Log() with the tag [NEVR.COMBAT] and `module=<file>`.
//
// Include it first in each of those files, in place of detours.h, nevr_plugin.h and nevr/plugin_logger.h.

#include <winsock2.h>
#include <windows.h>
#include <intrin.h>
#include <setjmp.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <utility>

#include "nevr/plugin_interface.h"  // NEVR_LOG_* levels, as Cat's logger names them

namespace nevr_combat::cat {

// --- __try / __except ---------------------------------------------------------------------------------------------

/// One __try block. Enter() pushes it on the thread's stack the first time (setjmp returned 0) and says "run the
/// block"; after a fault the handler has already popped it and longjmp'd here, setjmp returns 1, and Enter() says "run
/// the __except block". The destructor pops a frame the block left by falling off its end, a return or a break.
struct SehFrame {
  jmp_buf buf;
  SehFrame* prev = nullptr;
  bool armed = false;
  bool Enter(int jumped);
  ~SehFrame();
};

/// The code of the fault the innermost __except block was entered for (GetExceptionCode in that block).
DWORD LastExceptionCode();

/// Adds the vectored handler (first in the chain). Idempotent. Called before any of Cat's code runs.
void InstallFaultHandler();

// --- Detours -----------------------------------------------------------------------------------------------------

LONG TransactionBegin();
LONG TransactionCommit();
LONG TransactionAbort();
/// Detours' DetourAttach: `*real` is the target; on success it is the trampoline. `name` is the detour's name (a string
/// literal), which HookGuard and the log carry. The target's first 16 bytes pass the write gate first.
LONG Attach(PVOID* real, PVOID detour, const char* name, const char* module);
/// Detours' DetourDetach: the hook made through `real` is removed and `*real` is the target again.
LONG Detach(PVOID* real, PVOID detour);

// --- the plugin logger -------------------------------------------------------------------------------------------

/// One line through Log(): `[NEVR.COMBAT] module=<module> <text>`, at the level Cat named.
void HostLog(const char* module, std::uint32_t level, const char* text);

// --- PowerBoost.ini -> config.yaml ------------------------------------------------------------------------------

/// GetPrivateProfileIntA / GetPrivateProfileStringA over `combat.settings.<section>.<key>` in config.yaml.
int IniInt(const char* section, const char* key, int fallback);
DWORD IniString(const char* section, const char* key, const char* fallback, char* out, DWORD size);

}  // namespace nevr_combat::cat

// Cat's `__try`, `__except (filter)` and `GetExceptionCode()` are spelled EC_TRY, EC_EXCEPT(filter) and
// EC_EXCEPTION_CODE() in the ported files: libstdc++ defines `__try` itself, and MinGW's excpt.h GetExceptionCode.
#define EC_TRY                                                        \
  if (::nevr_combat::cat::SehFrame nevr_combat_seh_frame_{};          \
      nevr_combat_seh_frame_.Enter(setjmp(nevr_combat_seh_frame_.buf)))
#define EC_EXCEPT(...) else
#define EC_EXCEPTION_CODE() ::nevr_combat::cat::LastExceptionCode()
#define _ReturnAddress() __builtin_return_address(0)

#define DetourTransactionBegin() ::nevr_combat::cat::TransactionBegin()
#define DetourTransactionCommit() ::nevr_combat::cat::TransactionCommit()
#define DetourTransactionAbort() ::nevr_combat::cat::TransactionAbort()
#define DetourUpdateThread(thread) ((void)(thread), static_cast<LONG>(NO_ERROR))
#define DetourAttach(real, detour) \
  ::nevr_combat::cat::Attach(reinterpret_cast<PVOID*>(real), (PVOID)(detour), #detour, kCatModule)
#define DetourDetach(real, detour) ::nevr_combat::cat::Detach(reinterpret_cast<PVOID*>(real), (PVOID)(detour))

#define GetPrivateProfileIntA(section, key, fallback, file) \
  ((void)(file), ::nevr_combat::cat::IniInt((section), (key), (fallback)))
#define GetPrivateProfileStringA(section, key, fallback, out, size, file) \
  ((void)(file), ::nevr_combat::cat::IniString((section), (key), (fallback), (out), (size)))

// Cat's NEVR_DEFINE_PLUGIN_LOG("[Name]"): the module's name for its log lines, and the logger calls his code makes.
#define NEVR_DEFINE_PLUGIN_LOG(prefix)                                                                         \
  [[maybe_unused]] static void PluginLogFields(std::uint32_t level, const char* text,                          \
                                               std::initializer_list<std::pair<const char*, const char*>>) {   \
    ::nevr_combat::cat::HostLog(kCatModule, level, text);                                                      \
  }

// nevr_plugin.h's byte check, which Cat's code calls before each write to the game's code: true only if `len` bytes at
// `addr` are exactly `expected` (a null pointer or a length of 0 is no match).
inline bool EchoCombatBytesMatch(const void* addr, const std::uint8_t* expected, std::size_t len) {
  if (addr == nullptr || expected == nullptr || len == 0) return false;
  return std::memcmp(addr, expected, len) == 0;
}
