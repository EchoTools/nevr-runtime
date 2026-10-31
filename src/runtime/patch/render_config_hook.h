#pragma once

#include <cstdint>

#include "runtime/patch/gpu_score.h"
#include "runtime/patch/render_config.h"

// #1139, docs/adr/0022 "PCVR": the process half of the render_config line (the pure half is patch/render_config.h).
//
// Two detours, both on a client only, both prologue-validated and installed with PatchDetour:
//   ApplyRuntimeGfxSettings (echovr.exe 0x140c31870) - every change to the game's graphics settings passes through it.
//   The GPU auto-selector   (echovr.exe 0x140c31e00) - the boot code runs it on the settings struct when no saved
//                                                       profile is applied.
// The Apply detour calls the original first and returns its result unchanged; it copies a few words into mutex-guarded
// state and never logs, throws or waits on the game's path. The selector detour is an observer too, with one write: when
// the game's GPU score is zero it fills the score the selector reads before the original runs (#1419, patch/gpu_score.h),
// and it logs that decision once a run. Frequency: ApplyRuntimeGfxSettings has two callers in the whole image (the boot
// code and the menu/JSON settings translator; the callers of 0x140c31870 in the disassembly), so it runs at boot and when a setting
// changes; the selector has one caller, at boot. Neither is per frame.
//
// The check itself is Poll(), called from nevr_frame::DispatchPerFrameWork (N86): no thread and no Tick* call site.
namespace nevr_render_config_hook {

/// Installs the two observers on a client and starts the check. On a server it installs nothing and logs
/// `[NEVR.RENDER] render_config skipped reason=server_mode` once. If either prologue does not validate it installs
/// neither, logs a Warning naming the cause, and the line reports the settings fields as unavailable. Returns true when
/// both observers are installed.
bool Install(std::uintptr_t gameBase, bool isServer);

/// The per-frame call. `nowUs` is the dispatcher's microsecond clock. A no-op until Install ran on a client; then at most
/// one evaluation a second, and a log line only at the first frame and when the configuration changed.
void Poll(std::uint64_t nowUs);

#ifdef NEVR_RENDER_CONFIG_TESTING
/// Test seams: the reader the hook and the check use (set before Install), the boot return address the hook compares
/// with, the last return address the Apply hook saw, and a full reset (hooks off, state cleared).
void SetReaderForTest(const nevr_render_config::MemoryReader* reader);
void SetExternalsForTest(nevr_render_config::Externals externals);
/// The NVIDIA query the selector hook asks for a score (#1419); nullptr restores the NVAPI query.
void SetNvidiaQueryForTest(void (*query)(nevr_gpu_score::NvidiaMeasure* out));
void SetBootReturnForTest(std::uintptr_t address);
std::uintptr_t LastApplyReturnForTest();
nevr_render_config::SettingsObserver::View ObservedForTest();
bool ActiveForTest();
void ResetForTest();
#endif

}  // namespace nevr_render_config_hook
