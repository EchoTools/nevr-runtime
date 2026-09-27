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
namespace MicProvider {

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

/// Starts capture after successful MicCreate. A retained worker or pending
/// stop recovery blocks restart and is logged; Closed is a no-op.
void MicStart();

/// Requests capture cancellation and waits for the worker to exit. Retries
/// joining a retained worker even if capture is already marked not running;
/// a timeout or failed wait keeps worker and WASAPI resources retained for a
/// later Stop/Destroy retry.
void MicStop();

/// Stops capture and releases WASAPI resources after the worker has exited.
/// A timeout, failed wait, or non-owner-thread call fails closed and retains
/// resources so a later owner-thread Destroy can retry safely.
void MicDestroy();

} // namespace MicProvider
