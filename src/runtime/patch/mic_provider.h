/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>

/// MicProvider — WASAPI-backed replacement for pnsrad.dll's stubbed Mic*
/// exports (GH nevr-runtime#15). Design record and full trace:
/// docs/design/2026-09-21-mic-provider-voip-fix.md.
///
/// pnsrad.dll's MicAvailable/MicCreate/MicDetected/MicRead are identical-
/// code-folded onto one address, and MicDestroy/MicStart/MicStop onto a
/// second — hooking those addresses cannot give each export name distinct
/// behavior (only one detour lands per address; see the design doc's N128
/// precedent). Instead, CSysDLL_GetSymbolHook (lifecycle/initialize.cpp)
/// resolves these names against pnsrad.dll's module handle to the
/// functions below directly. pnsrad's own (unmodified, correct)
/// MicBufferSize/MicCaptureSize/MicSampleRate keep answering from pnsrad
/// itself — only the seven functions below are overridden.
namespace nevr_mic_provider {

/// Samples currently buffered and ready for MicRead. Cheap; the game calls
/// this every tick from CaptureAndEncodeLocalVoice (echovr.exe 0x140d7bd90).
uint64_t MicAvailable();

/// Opens the default capture device via WASAPI and starts the background
/// capture thread's setup (not yet capturing — see MicStart). Idempotent:
/// returns 0 immediately if already created. Returns 0 on success, nonzero
/// on failure (matches the pnsrad convention of a nonzero error code).
uint64_t MicCreate();

/// Whether a capture device is present. Cheap presence probe; does not
/// require MicCreate to have run first.
uint64_t MicDetected();

/// Copies up to sampleCount mono int16 samples at 48kHz into buffer.
/// Returns the number of samples actually copied (may be less than
/// requested, or 0) — CaptureAndEncodeLocalVoice tolerates any count and
/// carries the remainder to the next tick, so no fixed chunk size is
/// required here.
uint64_t MicRead(void* buffer, uint64_t sampleCount);

/// Threading (GH #51): MicCreate/MicStart/MicStop/MicDestroy may be called
/// from any thread — the game issues them from a component-system job
/// (CR15NetVoipBroadcasterCS::UpdateGlobal, echovr.exe 0x140d7cfc0) that
/// lands on different threads across frames. Each call is marshalled onto a
/// single capture owner thread (core/mic_owner_thread.h) and the caller blocks
/// until it has run, so COM initialization and the WASAPI objects never leave
/// that thread. MicAvailable/MicRead/MicDetected are not marshalled.

/// Starts capture after successful MicCreate. A retained worker or pending
/// stop recovery blocks restart and is logged; Closed is a no-op.
void MicStart();

/// Requests capture cancellation and waits for the worker to exit. Retries
/// joining a retained worker even if capture is already marked not running;
/// a timeout or failed wait keeps worker and WASAPI resources retained for a
/// later Stop/Destroy retry.
void MicStop();

/// Stops capture and releases WASAPI resources after the worker has exited,
/// then stops the capture owner thread. A timeout or failed wait fails closed
/// and retains resources (and the owner thread) so a later Destroy can retry
/// the CoUninitialize on the thread that initialized COM.
void MicDestroy();

} // namespace nevr_mic_provider
