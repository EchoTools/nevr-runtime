/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>

enum class MicCaptureDrainStatus {
  Complete,
  Cancelled,
  PacketQueryFailed,
  BufferAcquireFailed,
  BufferProcessFailed,
  BufferReleaseFailed,
};

struct MicCaptureDrainOperations {
  void* context = nullptr;
  bool (*cancelled)(void*) = nullptr;
  bool (*nextPacketSize)(void*, uint32_t*) = nullptr;
  bool (*acquireBuffer)(void*, const void**, uint32_t*, uint32_t*) = nullptr;
  // Process may throw std::exception; drain translates it after releasing.
  void (*processBuffer)(void*, const void*, uint32_t, uint32_t) = nullptr;
  // Release callback must not throw; production wraps the HRESULT API.
  bool (*releaseBuffer)(void*, uint32_t) = nullptr;
};

/// Drain one event's available packets. Cancellation is checked inside the
/// drain; every successfully acquired packet gets one ReleaseBuffer attempt.
MicCaptureDrainStatus DrainMicCapturePackets(const MicCaptureDrainOperations& operations);
