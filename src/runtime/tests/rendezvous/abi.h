#pragma once

// Shared protocol for the early-rendezvous harness
// (docs/adr/0008-bugsplat-host-and-nevr-runtime.md, "Decision: the rendezvous is the
// game's main wrapper").
//
// Three binaries speak it:
//   - the test executable (rendezvous_test.cpp) — stands in for echovr.exe. It
//     owns the event recorder and a synthetic game image whose GameMainWrapper
//     carries the REAL echovr.exe prologue at the REAL RVA;
//   - the host shim (host_shim.cpp, built as BugSplat64.dll) — calls the production
//     host entry points (runtime/host/host_entry.h): validate the image, install ONE
//     detour from DLL_PROCESS_ATTACH, load nothing there; on the wrapper's first call
//     the host loads the sibling nevr.dll through RuntimeLoader::Load, then calls the
//     runtime's initialize and game_main;
//   - runtime fixtures (runtime_fixture.cpp, built as nevr.dll) — implement
//     core/runtime_abi.h and record what they are called with. Their game_main runs
//     the original wrapper through the host's trampoline, standing in for GameMain.
//
// The prologue and RVA are shared with the production host
// (runtime/host/game_targets.h) rather than re-declared here.

#include "core/runtime_abi.h"
#include "runtime/host/game_targets.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace nevr_rendezvous_probe {

/// The synthetic original returns this for `command_line`, so any argument or result
/// altered anywhere on the path is visible. Full 64-bit values on purpose.
inline constexpr std::uint64_t kResultMask = 0x5EED5EED5EED5EEDull;
inline std::int64_t ExpectedResult(std::int64_t command_line) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(command_line) ^ kResultMask);
}

/// Argument the runtime fixture passes on its nested same-thread re-entry call.
inline constexpr std::int64_t kReentryArgument = 0x7A12'0000'0000'1234;

enum Event : LONG {
  kHostAttachEnter = 1,
  kHostHookEnabled = 2,   // payload: first byte at the target after enable
  kHostHookFailed = 3,
  kHostAttachExit = 4,    // payload: 1 if nevr.dll was loaded at attach exit
  kRuntimeDllAttach = 5,
  kRuntimeInitialized = 6,      // payload: host-supplied original wrapper trampoline
  kRuntimeGameMainEnter = 7,    // payload: command_line
  kOriginal = 8,                // payload: command_line (recorded INSIDE the original body)
  kRuntimeGameMainExit = 9,     // payload: result
  kRuntimeHookTarget = 10,      // payload: the host context's wrapper_hook_target
  kRuntimeHookBytes = 11,       // payload: the first 8 bytes of wrapper_hook_bytes
  kRuntimeHookMismatch = 12,    // the record's 16 bytes differ from the live bytes at the target
  kCallIssued = 13,             // payload: command_line
  kBadResult = 14,              // payload: command_line whose result was wrong
  kEventLimit = 15,
};

// Exports of the test executable, resolved with
// GetProcAddress(GetModuleHandleW(nullptr)).
inline constexpr char kRecordEventExport[] = "RendezvousProbeRecordEvent";
inline constexpr char kGameImageExport[] = "RendezvousProbeGameImage";
inline constexpr char kLauncherModeExport[] = "RendezvousProbeLauncherMode";
inline constexpr char kFailHookCreateExport[] = "RendezvousProbeFailHookCreate";
using RecordEventFn = void (*)(LONG event, std::uint64_t payload);
using GameImageFn = HMODULE (*)();
// Returns 1 when the child is exercising the external NEVR_SetGameModule launcher
// contract: the host shim's DllMain then skips its own direct-import detour install
// so the test can prove NEVR_SetGameModule does the Prepare -> Install -> dispatch.
using LauncherModeFn = int (*)();
using FailHookCreateFn = int (*)();

}  // namespace nevr_rendezvous_probe
