/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

/// A fixed-capacity, thread-safe ring of int16 samples. One writer (capture),
/// one reader (game thread).
class MicRingBuffer {
 public:
  explicit MicRingBuffer(uint32_t capacity);
  MicRingBuffer(const MicRingBuffer&) = delete;
  MicRingBuffer& operator=(const MicRingBuffer&) = delete;
  bool Push(const int16_t* samples, uint32_t count);
  uint32_t Available() const;
  uint32_t Pop(int16_t* out, uint32_t maxCount);
  void Reset();
  uint32_t capacity() const { return capacity_; }

 private:
  mutable std::mutex mutex_;
  uint32_t capacity_;
  std::vector<int16_t> data_;
  uint32_t head_ = 0;
  uint32_t count_ = 0;
};

enum class MicDspStatus {
  Progress,
  NeedInput,
  OutputFull,
  InvalidInput,
  RateChanged,
};

struct MicDspResult {
  uint32_t consumedFrames = 0;
  uint32_t producedSamples = 0;
  MicDspStatus status = MicDspStatus::NeedInput;
};

/// Stateful rational-time mono converter. Output sample k is evaluated at
/// source time k*srcRate/targetRate. It emits exact source points directly
/// and interpolates only when both bracketing frames are available. A call
/// reports input frames accepted and output samples written independently.
class MicDspResampler {
 public:
  void Reset();

  MicDspResult Process(const void* interleaved, uint32_t frameCount, size_t dataBytes,
                       uint16_t channels, uint16_t blockAlign, uint32_t srcRate,
                       uint32_t targetRate, bool isFloat, uint16_t bitsPerSample,
                       bool silent, int16_t* output, uint32_t outputCapacity);

 private:
  bool configured_ = false;
  bool hasPrevious_ = false;
  bool segmentPending_ = false;
  uint32_t srcRate_ = 0;
  uint32_t targetRate_ = 0;
  uint64_t nextNumerator_ = 0;
  int16_t previousSample_ = 0;
  int16_t pendingSample_ = 0;
};

enum class MicCapturePacketStatus {
  Complete,
  InvalidInput,
  RateChanged,
  NoProgress,
};

struct MicCapturePacketResult {
  uint32_t consumedFrames = 0;
  uint64_t producedSamples = 0;
  MicCapturePacketStatus status = MicCapturePacketStatus::Complete;
  bool ringOverflow = false;
};

/// Adapts one complete capture packet without retaining its input pointer.
/// Output is streamed through a bounded scratch buffer into the ring; frame
/// offsets always use the validated block alignment.
class MicCapturePacketAdapter {
 public:
  void Reset() { resampler_.Reset(); }
  MicCapturePacketResult Process(const void* data, uint32_t frameCount, size_t dataBytes,
                                uint16_t channels, uint16_t blockAlign,
                                uint32_t srcRate, uint32_t targetRate,
                                bool isFloat, uint16_t bitsPerSample,
                                bool silent, MicRingBuffer& ring);

 private:
  MicDspResampler resampler_;
};
