/* The pinned slots and code words of the EC client (ec.h).
 *
 * Every address is an ELF virtual address; the disassembly's address is the ELF address plus 0x100000. libr15: the store file (build ID in
 * pinned_targets.h, `LibR15PatchedBuildId` at run time). libpnsovr.so and libpnsrad.so: the store APK's copies (their build IDs are
 * pinned_targets.h's kPnsovrBuildId / kPnsradBuildId). src/quest/tests/got_pinned_test.cpp checks every relocation and every word against the
 * real files (`just test-quest-hooks-pinned`).
 *
 *   libr15  recvfrom@LIBC                   JUMP_SLOT 0x36d3e88  (libc's function: no libr15-relative value to compare)
 *   libr15  sendto@LIBC                     JUMP_SLOT 0x36db3d0
 *   libr15  NWriteLog::WriteLogV            JUMP_SLOT 0x36cb1b0 -> 0xf88720
 *   libr15  NR15::ReloadEquipmentConfig     JUMP_SLOT 0x36ea1b0 -> 0x20feda8
 *   libr15  NR15::UnloadEquipmentConfig     JUMP_SLOT 0x36f1878 -> 0x20ff7b4
 *   libr15  the tac-mod list pointer        .bss word 0x37af5f8 (read only, never hooked)
 *   pnsovr  NWriteLog::WriteLogV            JUMP_SLOT 0x6e1238  -> 0x1ca79c
 *   pnsrad  NWriteLog::WriteLogV            JUMP_SLOT 0x731ee8  -> 0x1be7b0
 *
 * Why WriteLogV and not WriteLog. `NWriteLog::WriteLog(ELogLevel, unsigned long long, const char*, ...)` is variadic: a thunk forwards only
 * its named arguments, so a hook on its slot would cut the arguments off every log line of the game. Each library's WriteLog is 25 words that
 * store x3..x7 and q0..q7 in a save area, build the va_list at sp+0xb0, put its address in x3 and call WriteLogV through that library's own
 * PLT stub, with x0..x2 untouched, and return what WriteLogV left in x0 (kWords below pin all 25 and the stub). The stub reads the WriteLogV
 * slot pinned here. So a thunk of `R(int, unsigned long long, const char*, va_list)` on the WriteLogV slot sees every WriteLog call with its
 * own arguments and passes the va_list on: an AAPCS64 composite over 16 bytes (the 32-byte __va_list) is passed by reference, so the callee
 * gets a pointer in x3 as it does unhooked. In libr15 the GOT page of the slot is read only by PLT stubs (`adrp x16`: every one of the 512
 * `adrp` of page 0x36cb000 is the PLT's), and the WriteLogV stub at 0xee7590 is called from exactly the two WriteLog overloads (0xf88710 in
 * `WriteLog(ELogLevel, const char*, ...)`, 0xf88c74 in `WriteLog(ELogLevel, unsigned long long, const char*, ...)`), so nothing a WriteLog-slot
 * hook would see is missed, and the one more caller, the two-argument overload, is a log line too.
 *
 * Origins (docs/audits/2026-11-05-q15t-origin-citations.md): see ec.h.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <sys/socket.h>
#include <sys/types.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>

#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::ec {

inline constexpr const char* kRecvfromSymbol = "recvfrom";
inline constexpr const char* kSendtoSymbol = "sendto";
inline constexpr const char* kWriteLogSymbol = "_ZN10NRadEngine9NWriteLog8WriteLogENS0_9ELogLevelEyPKcz";
inline constexpr const char* kWriteLogVSymbol = "_ZN10NRadEngine9NWriteLog9WriteLogVENS0_9ELogLevelEyPKcSt9__va_list";
inline constexpr const char* kReloadSymbol = "_ZN10NRadEngine8NRadGame4NR1521ReloadEquipmentConfigEv";
inline constexpr const char* kUnloadSymbol = "_ZN10NRadEngine8NRadGame4NR1521UnloadEquipmentConfigEv";

// libr15's slots, functions and words (its WriteLogV hook is kLogLib[0]).
struct Pins {
  std::uint64_t recvfromSlot, sendtoSlot;
  std::uint64_t reloadSlot, reloadFn;
  std::uint64_t unloadSlot, unloadFn;
  std::uint64_t tacModList;  // .bss word: pointer to the tac-mod list { allocator, entries, u64 count, u64 capacity }
};
inline constexpr Pins kPins = {0x36d3e88ULL, 0x36db3d0ULL, 0x36ea1b0ULL, 0x20feda8ULL, 0x36f1878ULL, 0x20ff7b4ULL, 0x37af5f8ULL};

// WriteLog's 25 words, the same in the three libraries but for the `bl` (word 21), which each library's pins give.
inline constexpr std::size_t kWriteLogWords = 25;
inline constexpr std::uint32_t kWriteLogBody[kWriteLogWords] = {
    0xd10403ff, 0x928004e8, 0x910003e9, 0x910223ea, 0xf2dff008, 0x910403eb, 0x91020129, 0x9100a14a, 0xa90efbe8,
    0xa90da7ea, 0xf9006beb, 0xad0007e0, 0xad4683e1, 0xa90893e3, 0x9102c3e3, 0xa9099be5, 0xf90057e7, 0xad010fe2,
    0xad0217e4, 0xad031fe6, 0xad0583e1, 0x00000000 /* bl: per library */, 0xf9407bfe, 0x910403ff, 0xd65f03c0};
inline constexpr std::size_t kWriteLogBlIndex = 21;

// One library's WriteLogV hook: where the slot is, what it may hold, and the WriteLog body and stub that prove the slot is the one
// WriteLog calls through. `stub` is the four words `adrp x16; ldr x17,[x16,#lo12]; add x16,x16,#lo12; br x17` that name the slot.
struct LogLib {
  const char* module;
  const char* buildId;  // nullptr: libr15's patched ID (LibR15PatchedBuildId), read at run time
  std::uint64_t slot, fn;
  std::uint64_t writeLogFn, blWord;
  std::uint64_t stubAt;
  std::uint32_t stub[4];
};
inline constexpr std::size_t kLogLibs = 3;
inline constexpr LogLib kLogLib[kLogLibs] = {
    {sentinel::pinned::kLibR15, nullptr, 0x36cb1b0ULL, 0xf88720ULL, 0xf88c20ULL, 0x97fd7a47ULL, 0xee7590ULL,
     {0x90013f30, 0xf940da11, 0x9106c210, 0xd61f0220}},
    {sentinel::pinned::kPnsovr, sentinel::pinned::kPnsovrBuildId, 0x6e1238ULL, 0x1ca79cULL, 0x1cac9cULL, 0x97ffa1e0ULL, 0x1b3470ULL,
     {0xd0002970, 0xf9411e11, 0x9108e210, 0xd61f0220}},
    {sentinel::pinned::kPnsrad, sentinel::pinned::kPnsradBuildId, 0x731ee8ULL, 0x1be7b0ULL, 0x1becb0ULL, 0x97ffdb87ULL, 0x1b5b20ULL,
     {0x90002bf0, 0xf9477611, 0x913ba210, 0xd61f0220}},
};

// The signatures. WriteLogV's result is whatever it leaves in x0: WriteLog returns it untouched.
using RecvfromSig = ssize_t(int, void*, size_t, int, sockaddr*, socklen_t*);
using SendtoSig = ssize_t(int, const void*, size_t, int, const sockaddr*, socklen_t);
using WriteLogVSig = std::uintptr_t(int, unsigned long long, const char*, va_list);
using GearSig = void();

// The slot as a manager target. recvfrom / sendto hold libc's function (plausibility only); every libr15 function slot holds
// `base + fn`; a platform library's WriteLogV slot is checked by the install itself (its own function or libr15's), so it asks
// plausibility only.
inline sentinel::GotTarget LibcTarget(const char* symbol, std::uint64_t slot) {
  return sentinel::GotTarget(sentinel::pinned::kLibR15, symbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(), slot)
      .Owned("combat_ec");
}
inline sentinel::GotTarget LibR15FnTarget(const char* symbol, std::uint64_t slot, std::uintptr_t base, std::uint64_t fn) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, symbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(), slot);
  target.expectedOriginal = reinterpret_cast<const void*>(base + fn);
  return target.Owned("combat_ec");
}
inline sentinel::GotTarget LogLibTarget(const LogLib& lib, std::uintptr_t base) {
  if (lib.buildId == nullptr) return LibR15FnTarget(kWriteLogVSymbol, lib.slot, base, lib.fn);
  return sentinel::GotTarget(lib.module, kWriteLogVSymbol, sentinel::RelocKind::kJumpSlot, lib.buildId, lib.slot).Owned("combat_ec");
}

}  // namespace nevr_quest::combat::ec
