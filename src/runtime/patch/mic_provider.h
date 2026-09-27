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

/// Starts the capture thread. No-op if already running or MicCreate has
/// not succeeded.
void MicStart();

/// Stops the capture thread and the audio client. No-op if not running.
void MicStop();

/// Stops capture (if running) and releases all WASAPI resources.
void MicDestroy();

} // namespace MicProvider
