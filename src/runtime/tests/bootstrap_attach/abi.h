#pragma once

// Shared protocol for the attach-time MinHook proof-gate harness
// (docs/adr/0008-bugsplat-host-and-nevr-runtime.md, "Proof gate").
//
// Three binaries speak it:
//   - the test executable (attach_test.cpp) — stands in for echovr.exe. It owns
//     the event recorder and a synthetic game image whose GameMainWrapper carries
//     the REAL echovr.exe prologue bytes at the REAL RVA;
//   - the host shim (host_shim.cpp, built as BugSplat64.dll) — the PRODUCTION host
//     units (runtime/host/) behind a DllMain: validate the image, validate the
//     wrapper's prologue, MinHook the rendezvous, load nothing. The harness measures
//     the code the game runs, not a copy of it;
//   - runtime fixtures (runtime_probe.cpp, built as nevr-runtime.dll) — implement
//     core/runtime_abi.h and record what they are called with.

#include "core/runtime_abi.h"
#include "runtime/host/game_targets.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace nevr_bootstrap_probe {

// echovr.exe facts, measured against sha256
// b6d08277e5846900c81004b64b298df6acba834b69700a640b758bda94a52043:
//   - PE TimeDateStamp 0x6452dff6 (file header read; equals the production
//     guard, kSupportedTimestamp in src/runtime/lifecycle/game_image_guard.cpp);
//   - GameMainWrapper at RVA 0xCD510 (GAME_MAIN_WRAPPER in src/runtime/hook/addresses.h),
//     its first 13 bytes in runtime/host/game_targets.h: `sub rsp,0x38` and
//     `mov qword [rsp+0x20],0`, every instruction before the CALL rel32 to GameMain.
inline constexpr DWORD kSupportedTimestamp = 0x6452dff6;
/// The offset the bad-prologue image changes: the store's displacement (0x20 -> 0x28).
/// Still a valid, harmless store inside the wrapper's frame, so an unhooked call through
/// the image is safe.
inline constexpr std::size_t kPrologueMutationOffset = 8;

/// What the synthetic original returns for `command_line`. Callers and the runtime
/// check every result against it, so an argument or result altered anywhere on the
/// path is visible. Full 64-bit values on purpose.
inline constexpr std::uint64_t kResultMask = 0x5EED5EED5EED5EEDull;
inline std::int64_t ExpectedResult(std::int64_t command_line) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(command_line) ^ kResultMask);
}

/// Arguments the runtime fixture passes on its own nested calls.
inline constexpr std::int64_t kNestedArgument = 0x7A11'0000'0000'0042;
inline constexpr std::int64_t kInitReentryArgument = 0x7A12'0000'0000'0043;

enum Event : LONG {
  kHostAttachEnter = 1,
  kHostHookEnabled = 2,       // payload: first byte at the target after the host attach
  kHostHookFailed = 3,        // the host left the game unhooked
  kHostAttachExit = 4,        // payload: 1 if nevr-runtime.dll was loaded at attach exit
  kRuntimeDllAttach = 5,
  kRuntimeGetApi = 6,
  kRuntimeInitialized = 7,    // payload: host-supplied original wrapper trampoline
  kRuntimeGameMainEnter = 8,  // payload: command_line
  kOriginal = 9,              // payload: command_line (recorded INSIDE the original body)
  kRuntimeGameMainExit = 10,  // payload: result
  kCallIssued = 11,           // payload: command_line (recorded by every caller)
  kBadResult = 12,            // payload: command_line whose result was wrong
  kMainEntered = 13,
  kHostLoadReturned = 14,     // dynamic mode: LoadLibraryW(host) returned
  kRuntimeSecondHookEnabled = 15,  // runtime's own MinHook instance, disjoint target
  kSecondDetour = 16,
  kSecondOriginal = 17,
  kHostAttachHeld = 18,       // payload: ms the test asked DllMain to stay open
  kRuntimeHookTarget = 19,    // payload: the host context's wrapper_hook_target
  kRuntimeHookBytes = 20,     // payload: the first 8 bytes of wrapper_hook_bytes
  kRuntimeHookMismatch = 21,  // the host's record differs from the live bytes at the target
  kEventLimit = 22,
};

// Exports of the test executable, resolved with GetProcAddress(GetModuleHandleW(nullptr)).
inline constexpr char kRecordEventExport[] = "BootstrapProbeRecordEvent";
inline constexpr char kGameImageExport[] = "BootstrapProbeGameImage";
inline constexpr char kSecondTargetExport[] = "BootstrapProbeSecondTarget";
/// Test-only: milliseconds the host's DllMain stays open after the hook is live,
/// so a racing thread is guaranteed to enter the detour under the loader lock.
inline constexpr char kAttachHoldExport[] = "BootstrapProbeAttachHoldMs";
/// Test-only: 1 makes the host shim make CreateEventW "fail" for the install event, so the load
/// proceeds without waiting for the install (install_wait: unavailable).
inline constexpr char kNoInstallEventExport[] = "BootstrapProbeNoInstallEvent";
using NoInstallEventFn = DWORD (*)();
using RecordEventFn = void (*)(LONG event, std::uint64_t payload);
using GameImageFn = HMODULE (*)();
using SecondTargetFn = std::uint64_t (*)(std::uint64_t);
using AttachHoldFn = DWORD (*)();

}  // namespace nevr_bootstrap_probe
