/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

/// mic_dsp — the pure logic behind the WASAPI mic provider
/// (runtime/patch/mic_provider.cpp, GH nevr-runtime#15).
///
/// Deliberately platform-independent (no windows.h, no WASAPI types) so it
/// can be unit-tested on the build host without a capture device or Wine —
/// see tests/test_mic_dsp.cpp. mic_provider.cpp adapts real WAVEFORMATEX
/// packets into the plain description DownmixResampleToMonoInt16 takes.

/// A fixed-capacity, thread-safe ring of int16 samples. One writer (a
/// capture thread), one reader — Push/Pop/Available may be called
/// concurrently from different threads.
class MicRingBuffer {
 public:
  explicit MicRingBuffer(uint32_t capacity);

  MicRingBuffer(const MicRingBuffer&) = delete;
  MicRingBuffer& operator=(const MicRingBuffer&) = delete;

  /// Appends up to `count` samples. Never blocks and never grows: if the
  /// ring is already full, the oldest buffered samples are dropped to make
  /// room. Returns true if any sample was dropped this call.
  bool Push(const int16_t* samples, uint32_t count);

  /// Samples currently buffered and ready for Pop.
  uint32_t Available() const;

  /// Copies up to maxCount samples (oldest first) into out, removing them
  /// from the ring. Returns the number of samples actually copied — may be
  /// less than maxCount, or 0.
  uint32_t Pop(int16_t* out, uint32_t maxCount);

  /// Discards all buffered samples.
  void Reset();

  uint32_t capacity() const { return capacity_; }

 private:
  mutable std::mutex mutex_;
  uint32_t capacity_;
  std::vector<int16_t> data_;
  uint32_t head_ = 0;   // next write position
  uint32_t count_ = 0;  // samples currently buffered
};

/// Downmixes one packet of interleaved PCM to mono, then resamples to
/// targetRate via linear interpolation, appending the result to `out`
/// (caller-allocated, at least outCapacity samples). `phase` carries the
/// fractional resample position across consecutive packets from the same
/// stream — initialize to 0.0 before the first packet and pass the same
/// variable to every subsequent call.
///
/// Supported source formats: 16-bit signed PCM, and 32-bit IEEE float
/// (the only two WASAPI shared-mode mix formats). Any other bitsPerSample
/// with isFloat=false is treated as silence rather than misinterpreted.
///
/// Returns the number of samples written to `out` (0 if frameCount,
/// channels, or outCapacity is 0).
uint32_t DownmixResampleToMonoInt16(const void* interleaved, uint32_t frameCount, uint16_t channels,
                                    uint32_t srcRate, bool isFloat, uint16_t bitsPerSample,
                                    uint32_t targetRate, double* phase, int16_t* out,
                                    uint32_t outCapacity);
