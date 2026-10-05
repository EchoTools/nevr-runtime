#pragma once

// Shared protocol for the attach-time MinHook proof-gate harness
// (docs/design/2026-10-05-bugsplat-nevr-split.md, Implementation step 2).
//
// Three binaries speak it:
//   - the test executable (attach_test.cpp) — stands in for echovr.exe. It owns
//     the event recorder and a synthetic game image whose PreprocessCommandLine
//     carries the REAL echovr.exe prologue bytes at the REAL RVA;
//   - the probe host (host_probe.cpp, built as BugSplat64.dll) — the proposed
//     DLL_PROCESS_ATTACH path: validate image, validate prologue, MinHook the
//     rendezvous, load nothing;
//   - runtime fixtures (runtime_probe.cpp, built as nevr.dll) — implement
//     core/runtime_abi.h and record what they are called with.

#include "core/runtime_abi.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace BootstrapProbe {

// echovr.exe facts, measured 2026-10-05 against sha256
// b6d08277e5846900c81004b64b298df6acba834b69700a640b758bda94a52043:
//   - PE TimeDateStamp 0x6452dff6 (file header read; equals the production
//     guard, src/runtime/lifecycle/game_image_guard.cpp:11);
//   - PreprocessCommandLine at RVA 0x116720 (src/abi/echovr_functions.cpp:112;
//     ReVault echovr.exe 0x140116720 "CR15Game::vfunction1");
//   - its first 18 bytes (ReVault disassembly == file bytes at offset 0x115b20),
//     every instruction before the first position-dependent one (CALL rel32 at
//     +0x12):  40 55 push rbp | 57 push rdi | 48 8d 6c 24 b1 lea rbp,[rsp-0x4f]
//             48 81 ec a8 00 00 00 sub rsp,0xa8 | 48 8b f9 mov rdi,rcx
//     tools/hook_identity_manifest.json pins the first 8 of these.
inline constexpr DWORD kSupportedTimestamp = 0x6452dff6;
inline constexpr std::uintptr_t kPreprocessRva = 0x116720;
inline constexpr unsigned char kPreprocessPrologue[18] = {
    0x40, 0x55, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xb1, 0x48,
    0x81, 0xec, 0xa8, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xf9};
/// The byte the bad-prologue image changes: the lea displacement (0xb1 -> 0xb2).
/// Still a valid, semantically harmless instruction for the synthetic body, so
/// an unhooked call through it is safe.
inline constexpr std::size_t kPrologueMutationOffset = 7;

/// What the synthetic original returns for `game`. Callers and the runtime check
/// every result against it, so an argument or result altered anywhere on the
/// path is visible.
inline constexpr std::uint64_t kResultMask = 0x5EED5EED5EED5EEDull;
inline std::uint64_t ExpectedResult(const void* game) {
  return reinterpret_cast<std::uintptr_t>(game) ^ kResultMask;
}

/// Game pointers the runtime fixture passes on its own nested calls.
inline constexpr std::uintptr_t kNestedGame = 0x7A11'0000;
inline constexpr std::uintptr_t kInitReentryGame = 0x7A12'0000;

enum Event : LONG {
  kHostAttachEnter = 1,
  kHostImageAccepted = 2,
  kHostImageRejected = 3,
  kHostPrologueAccepted = 4,
  kHostPrologueRejected = 5,
  kHostHookEnabled = 6,       // payload: first byte at the target after enable
  kHostHookFailed = 7,        // payload: MH_STATUS
  kHostAttachExit = 8,        // payload: 1 if nevr.dll was loaded at attach exit
  kHostLoadBegin = 9,
  kHostLoadEnd = 10,          // payload: LoadOutcome (| Win32 error << 32)
  kHostReentryBypass = 11,    // payload: game
  kRuntimeDllAttach = 12,
  kRuntimeGetApi = 13,
  kRuntimeInitialized = 14,   // payload: host-supplied original trampoline
  kRuntimeBefore = 15,        // payload: game
  kOriginal = 16,             // payload: game (recorded INSIDE the original body)
  kRuntimeAfter = 17,         // payload: result
  kCallIssued = 18,           // payload: game (recorded by every caller)
  kBadResult = 19,            // payload: game whose result was wrong
  kMainEntered = 20,
  kHostLoadReturned = 21,     // dynamic mode: LoadLibraryW(host) returned
  kRuntimeSecondHookEnabled = 22,  // runtime's own MinHook instance, disjoint target
  kSecondDetour = 23,
  kSecondOriginal = 24,
  kHostAttachHeld = 25,       // payload: ms the test asked DllMain to stay open
  kEventLimit = 26,
};

enum LoadOutcome : std::uint64_t {
  kLoadOk = 0,
  kLoadNoPath = 1,
  kLoadLibraryFailed = 2,
  kLoadNoEntry = 3,
  kLoadInitFailed = 4,
  /// kLoadAbiRejectedBase + NevrAbi::Status
  kLoadAbiRejectedBase = 0x100,
};

// Exports of the test executable, resolved with GetProcAddress(GetModuleHandleW(nullptr)).
inline constexpr char kRecordEventExport[] = "BootstrapProbeRecordEvent";
inline constexpr char kGameImageExport[] = "BootstrapProbeGameImage";
inline constexpr char kSecondTargetExport[] = "BootstrapProbeSecondTarget";
/// Test-only: milliseconds the host's DllMain stays open after enabling the hook,
/// so a racing thread is guaranteed to enter the detour under the loader lock.
inline constexpr char kAttachHoldExport[] = "BootstrapProbeAttachHoldMs";
using RecordEventFn = void (*)(LONG event, std::uint64_t payload);
using GameImageFn = HMODULE (*)();
using SecondTargetFn = std::uint64_t (*)(std::uint64_t);
using AttachHoldFn = DWORD (*)();

}  // namespace BootstrapProbe
