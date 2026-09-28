/* SYNTHESIS -- custom tool code, not from binary */

#include "core/mic_capture_drain.h"

#include <exception>

namespace {

class ReleaseBufferOnce {
 public:
  ReleaseBufferOnce(const MicCaptureDrainOperations& operations, uint32_t frames)
      : operations_(operations), frames_(frames) {}

  ~ReleaseBufferOnce() {
    if (!attempted_) (void)Release();
  }

  bool Release() {
    if (attempted_) return releaseSucceeded_;
    attempted_ = true;
    releaseSucceeded_ = operations_.releaseBuffer(operations_.context, frames_);
    return releaseSucceeded_;
  }

 private:
  const MicCaptureDrainOperations& operations_;
  uint32_t frames_;
  bool attempted_ = false;
  bool releaseSucceeded_ = false;
};

}  // namespace

MicCaptureDrainStatus DrainMicCapturePackets(const MicCaptureDrainOperations& operations) {
  if (operations.cancelled == nullptr || operations.nextPacketSize == nullptr ||
      operations.acquireBuffer == nullptr || operations.processBuffer == nullptr ||
      operations.releaseBuffer == nullptr) {
    return MicCaptureDrainStatus::PacketQueryFailed;
  }

  for (;;) {
    if (operations.cancelled(operations.context)) return MicCaptureDrainStatus::Cancelled;
    uint32_t packetFrames = 0;
    if (!operations.nextPacketSize(operations.context, &packetFrames)) {
      return MicCaptureDrainStatus::PacketQueryFailed;
    }
    if (packetFrames == 0) return MicCaptureDrainStatus::Complete;
    if (operations.cancelled(operations.context)) return MicCaptureDrainStatus::Cancelled;

    const void* data = nullptr;
    uint32_t frames = 0;
    uint32_t flags = 0;
    if (!operations.acquireBuffer(operations.context, &data, &frames, &flags)) {
      return MicCaptureDrainStatus::BufferAcquireFailed;
    }
    ReleaseBufferOnce release(operations, frames);
    bool processSucceeded = true;
    try {
      operations.processBuffer(operations.context, data, frames, flags);
    } catch (const std::exception&) {
      processSucceeded = false;
    }
    const bool releaseSucceeded = release.Release();
    if (!releaseSucceeded) {
      return MicCaptureDrainStatus::BufferReleaseFailed;
    }
    if (!processSucceeded) return MicCaptureDrainStatus::BufferProcessFailed;
    if (operations.cancelled(operations.context)) return MicCaptureDrainStatus::Cancelled;
  }
}
