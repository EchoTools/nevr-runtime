/* SYNTHESIS -- custom tool code, not from binary */

#include "core/mic_dsp.h"

#include <algorithm>
#include <cstring>

MicRingBuffer::MicRingBuffer(uint32_t capacity)
    : capacity_(capacity), data_(capacity > 0 ? capacity : 1) {}

bool MicRingBuffer::Push(const int16_t* samples, uint32_t count) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (capacity_ == 0) return count > 0;
  bool dropped = false;
  for (uint32_t i = 0; i < count; i++) {
    data_[head_] = samples[i];
    head_ = (head_ + 1) % capacity_;
    if (count_ < capacity_) {
      count_++;
    } else {
      dropped = true;  // ring already full: this write displaced the oldest sample
    }
  }
  return dropped;
}

uint32_t MicRingBuffer::Available() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return count_;
}

uint32_t MicRingBuffer::Pop(int16_t* out, uint32_t maxCount) {
  std::lock_guard<std::mutex> lock(mutex_);
  uint32_t n = std::min(maxCount, count_);
  uint32_t tail = (head_ + capacity_ - count_) % capacity_;
  for (uint32_t i = 0; i < n; i++) {
    out[i] = data_[(tail + i) % capacity_];
  }
  count_ -= n;
  return n;
}

void MicRingBuffer::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  head_ = 0;
  count_ = 0;
}

uint32_t DownmixResampleToMonoInt16(const void* interleaved, uint32_t frameCount, uint16_t channels,
                                    uint32_t srcRate, bool isFloat, uint16_t bitsPerSample,
                                    uint32_t targetRate, double* phase, int16_t* out,
                                    uint32_t outCapacity) {
  if (frameCount == 0 || channels == 0 || outCapacity == 0 || srcRate == 0 || targetRate == 0) {
    return 0;
  }

  const uint8_t* data = static_cast<const uint8_t*>(interleaved);

  // Downmix to mono float first (simple channel average). Bounded stack
  // buffer avoids a heap allocation on the capture hot path; frames beyond
  // this are dropped (never observed at any common device rate on a 10ms
  // WASAPI shared-mode buffer).
  constexpr uint32_t kMaxFrames = 8192;
  if (frameCount > kMaxFrames) frameCount = kMaxFrames;

  float monoFloat[kMaxFrames];
  for (uint32_t f = 0; f < frameCount; f++) {
    float sum = 0.0f;
    for (uint16_t c = 0; c < channels; c++) {
      size_t sampleIndex = static_cast<size_t>(f) * channels + c;
      if (isFloat) {
        float sample;
        std::memcpy(&sample, data + sampleIndex * sizeof(float), sizeof(float));
        sum += sample;
      } else if (bitsPerSample == 16) {
        int16_t sample;
        std::memcpy(&sample, data + sampleIndex * sizeof(int16_t), sizeof(int16_t));
        sum += static_cast<float>(sample) / 32768.0f;
      }
      // Other bit depths (24/32-bit int) are not expected in shared-mode
      // WASAPI mix format; treated as silence rather than guessed.
    }
    monoFloat[f] = sum / static_cast<float>(channels);
  }

  // Linear-interpolation resample to targetRate, carrying fractional phase
  // across packets so boundaries don't click. Sufficient for voice — Opus
  // at 24kbps is the actual fidelity ceiling downstream — not intended as a
  // general-purpose resampler.
  uint32_t outCount = 0;
  double ratio = static_cast<double>(srcRate) / static_cast<double>(targetRate);
  double pos = *phase;
  while (pos < static_cast<double>(frameCount) - 1.0 && outCount < outCapacity) {
    uint32_t i0 = static_cast<uint32_t>(pos);
    double frac = pos - static_cast<double>(i0);
    float s0 = monoFloat[i0];
    float s1 = monoFloat[i0 + 1];
    float s = static_cast<float>(s0 + (s1 - s0) * frac);
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    out[outCount++] = static_cast<int16_t>(s * 32767.0f);
    pos += ratio;
  }
  *phase = pos - static_cast<double>(frameCount);
  if (*phase < 0.0) *phase = 0.0;

  return outCount;
}
