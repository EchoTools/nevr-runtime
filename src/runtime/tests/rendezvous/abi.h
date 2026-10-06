#pragma once

// Shared protocol for the step-4 early-rendezvous harness
// (docs/design/2026-10-05-bugsplat-nevr-split.md, Implementation step 4).
//
// Three binaries speak it:
//   - the test executable (rendezvous_test.cpp) — stands in for echovr.exe. It
//     owns the event recorder and a synthetic game image whose
//     PreprocessCommandLine carries the REAL echovr.exe prologue at the REAL RVA;
//   - the host shim (host_shim.cpp, built as BugSplat64.dll) — drives the
//     production RuntimeRendezvous: validate the image, install ONE detour from
//     DLL_PROCESS_ATTACH, load nothing there; on the first call it loads the
//     sibling nevr.dll through RuntimeLoader::Load and dispatches;
//   - runtime fixtures (runtime_fixture.cpp, built as nevr.dll) — implement
//     core/runtime_abi.h and record what they are called with, emulating the
//     runtime-side server/client preflight + deferred-bootstrap semantics.
//
// The prologue and RVA are shared with the production host unit
// (runtime/host/runtime_rendezvous.h) rather than re-declared here.

#include "core/runtime_abi.h"
#include "runtime/host/runtime_rendezvous.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace RendezvousProbe {

/// The synthetic original returns this for `game`, so any argument or result
/// altered anywhere on the path is visible.
inline constexpr std::uint64_t kResultMask = 0x5EED5EED5EED5EEDull;
inline std::uint64_t ExpectedResult(const void* game) {
  return reinterpret_cast<std::uintptr_t>(game) ^ kResultMask;
}

/// Game pointer the runtime fixture passes on its nested same-thread re-entry call.
inline constexpr std::uintptr_t kReentryGame = 0x7A12'0000;

enum Event : LONG {
  kHostAttachEnter = 1,
  kHostHookEnabled = 2,   // payload: first byte at the target after enable
  kHostHookFailed = 3,
  kHostAttachExit = 4,    // payload: 1 if nevr.dll was loaded at attach exit
  kRuntimeDllAttach = 5,
  kRuntimeInitialized = 6,  // payload: host-supplied original trampoline
  kRuntimeBefore = 7,       // payload: game
  kOriginal = 8,            // payload: game (recorded INSIDE the original body)
  kRuntimeAfter = 9,        // payload: result
  kRuntimeServerDetected = 10,  // preflight parsed -server BEFORE the original
  kRuntimeClientDetected = 11,  // preflight parsed no -server BEFORE the original
  kRuntimeDeferred = 12,        // post phase ran the deferred server bootstrap
  kCallIssued = 13,             // payload: game
  kBadResult = 14,              // payload: game whose result was wrong
  kEventLimit = 15,
};

// Exports of the test executable, resolved with
// GetProcAddress(GetModuleHandleW(nullptr)).
inline constexpr char kRecordEventExport[] = "RendezvousProbeRecordEvent";
inline constexpr char kGameImageExport[] = "RendezvousProbeGameImage";
using RecordEventFn = void (*)(LONG event, std::uint64_t payload);
using GameImageFn = HMODULE (*)();

}  // namespace RendezvousProbe
