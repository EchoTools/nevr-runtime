/* SYNTHESIS -- custom tool code, not from binary */

#include "core/mic_dsp.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

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
      dropped = true;
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
  if (capacity_ == 0) return 0;
  const uint32_t n = std::min(maxCount, count_);
  const uint32_t tail = (head_ + capacity_ - count_) % capacity_;
  for (uint32_t i = 0; i < n; i++) out[i] = data_[(tail + i) % capacity_];
  count_ -= n;
  if (n > 0) consumerSeen_ = true;
  return n;
}

void MicRingBuffer::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  head_ = 0;
  count_ = 0;
  backlogDropped_ = false;
  consumerSeen_ = false;
}

uint32_t MicRingBuffer::NoteReaderActive() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (backlogDropped_ || count_ == 0) return 0;
  backlogDropped_ = true;
  const uint32_t dropped = count_;
  count_ = 0;
  return dropped;
}

bool MicRingBuffer::ReaderActive() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return consumerSeen_;
}

namespace {

int16_t ReadMonoFrame(const uint8_t* frame, uint16_t channels, bool isFloat,
                      uint16_t bitsPerSample, bool silent) {
  if (silent) return 0;
  double sum = 0.0;
  if (isFloat && bitsPerSample == 32) {
    for (uint16_t channel = 0; channel < channels; ++channel) {
      float value = 0.0F;
      std::memcpy(&value, frame + static_cast<size_t>(channel) * sizeof(value), sizeof(value));
      if (std::isfinite(value)) sum += std::clamp(static_cast<double>(value), -1.0, 1.0);
    }
    sum /= channels;
    const double scaled = sum * 32768.0;
    const double rounded = scaled >= 0.0 ? std::floor(scaled + 0.5) : std::ceil(scaled - 0.5);
    return static_cast<int16_t>(std::clamp(rounded, -32768.0, 32767.0));
  }
  if (!isFloat && bitsPerSample == 16) {
    for (uint16_t channel = 0; channel < channels; ++channel) {
      int16_t value = 0;
      std::memcpy(&value, frame + static_cast<size_t>(channel) * sizeof(value), sizeof(value));
      sum += value;
    }
    sum /= channels;
    const double rounded = sum >= 0.0 ? std::floor(sum + 0.5) : std::ceil(sum - 0.5);
    return static_cast<int16_t>(std::clamp(rounded, -32768.0, 32767.0));
  }
  // Unknown but structurally valid formats preserve the source clock as
  // silence; the adapter validates block alignment before reaching this path.
  return 0;
}

MicDspResult MakeResult(uint32_t consumed, uint32_t produced, MicDspStatus status) {
  return {consumed, produced, status};
}

}  // namespace

void MicDspResampler::Reset() {
  configured_ = false;
  hasPrevious_ = false;
  segmentPending_ = false;
  srcRate_ = 0;
  targetRate_ = 0;
  nextNumerator_ = 0;
  previousSample_ = 0;
  pendingSample_ = 0;
}

MicDspResult MicDspResampler::Process(const void* interleaved, uint32_t frameCount,
                                      size_t dataBytes, uint16_t channels,
                                      uint16_t blockAlign, uint32_t srcRate,
                                      uint32_t targetRate, bool isFloat,
                                      uint16_t bitsPerSample, bool silent,
                                      int16_t* output, uint32_t outputCapacity) {
  if (frameCount == 0 && !segmentPending_) return MakeResult(0, 0, MicDspStatus::NeedInput);
  if (channels == 0 || blockAlign == 0 || srcRate == 0 || targetRate == 0 ||
      (outputCapacity > 0 && output == nullptr)) {
    return MakeResult(0, 0, MicDspStatus::InvalidInput);
  }
  const uint32_t bytesPerSample = (static_cast<uint32_t>(bitsPerSample) + 7u) / 8u;
  if (bytesPerSample == 0 || static_cast<uint32_t>(channels) * bytesPerSample > blockAlign) {
    return MakeResult(0, 0, MicDspStatus::InvalidInput);
  }
  if (!silent && frameCount > 0) {
    if (interleaved == nullptr || frameCount > std::numeric_limits<size_t>::max() / blockAlign ||
        dataBytes < static_cast<size_t>(frameCount) * blockAlign) {
      return MakeResult(0, 0, MicDspStatus::InvalidInput);
    }
  }
  if (outputCapacity == 0) return MakeResult(0, 0, MicDspStatus::OutputFull);
  if (configured_ && (srcRate != srcRate_ || targetRate != targetRate_)) {
    return MakeResult(0, 0, MicDspStatus::RateChanged);
  }
  if (!configured_) {
    configured_ = true;
    srcRate_ = srcRate;
    targetRate_ = targetRate;
  }

  uint32_t consumed = 0;
  uint32_t produced = 0;
  const auto* bytes = static_cast<const uint8_t*>(interleaved);
  for (;;) {
    if (segmentPending_) {
      while (nextNumerator_ <= targetRate_) {
        if (produced == outputCapacity) return MakeResult(consumed, produced, MicDspStatus::OutputFull);
        const double fraction = static_cast<double>(nextNumerator_) / targetRate_;
        const double sample = static_cast<double>(previousSample_) +
            (static_cast<double>(pendingSample_) - previousSample_) * fraction;
        const double rounded = sample >= 0.0 ? std::floor(sample + 0.5) : std::ceil(sample - 0.5);
        output[produced++] = static_cast<int16_t>(std::clamp(rounded, -32768.0, 32767.0));
        nextNumerator_ += srcRate_;
      }
      nextNumerator_ -= targetRate_;
      previousSample_ = pendingSample_;
      segmentPending_ = false;
      if (produced == outputCapacity) return MakeResult(consumed, produced, MicDspStatus::OutputFull);
      continue;
    }

    if (consumed == frameCount) {
      const MicDspStatus status = (consumed != 0 || produced != 0)
          ? MicDspStatus::Progress : MicDspStatus::NeedInput;
      return MakeResult(consumed, produced, status);
    }

    const uint8_t* frame = nullptr;
    if (!silent) frame = bytes + static_cast<size_t>(consumed) * blockAlign;
    const int16_t current = ReadMonoFrame(frame, channels, isFloat, bitsPerSample, silent);
    ++consumed;
    if (!hasPrevious_) {
      previousSample_ = current;
      hasPrevious_ = true;
      nextNumerator_ = srcRate_;
      output[produced++] = current;
      if (produced == outputCapacity) return MakeResult(consumed, produced, MicDspStatus::OutputFull);
      continue;
    }

    pendingSample_ = current;
    segmentPending_ = true;
  }
}

MicCapturePacketResult MicCapturePacketAdapter::Process(
    const void* data, uint32_t frameCount, size_t dataBytes, uint16_t channels,
    uint16_t blockAlign, uint32_t srcRate, uint32_t targetRate, bool isFloat,
    uint16_t bitsPerSample, bool silent, MicRingBuffer& ring) {
    if (blockAlign == 0 || frameCount > std::numeric_limits<size_t>::max() / blockAlign ||
      (!silent && dataBytes < static_cast<size_t>(frameCount) * blockAlign) ||
      (!silent && frameCount > 0 && data == nullptr)) {
    return {0, 0, MicCapturePacketStatus::InvalidInput, false};
  }

  constexpr uint32_t kOutputChunkSamples = 2048;
  int16_t output[kOutputChunkSamples];
  uint32_t consumed = 0;
  uint64_t producedTotal = 0;
  bool ringOverflow = false;
  bool drainPending = false;
  for (;;) {
    const uint32_t remaining = frameCount - consumed;
    const size_t byteOffset = static_cast<size_t>(consumed) * blockAlign;
    const void* input = (!silent && remaining > 0)
        ? static_cast<const uint8_t*>(data) + byteOffset : nullptr;
    const size_t remainingBytes = silent ? 0 : dataBytes - byteOffset;
    const MicDspResult result = resampler_.Process(
        input, remaining, remainingBytes, channels, blockAlign, srcRate, targetRate,
        isFloat, bitsPerSample, silent, output, kOutputChunkSamples);
    if (result.producedSamples > 0) {
      ringOverflow = ring.Push(output, result.producedSamples) || ringOverflow;
      producedTotal += result.producedSamples;
    }
    consumed += result.consumedFrames;
    if (result.status == MicDspStatus::InvalidInput) {
      return {consumed, producedTotal, MicCapturePacketStatus::InvalidInput, ringOverflow};
    }
    if (result.status == MicDspStatus::RateChanged) {
      return {consumed, producedTotal, MicCapturePacketStatus::RateChanged, ringOverflow};
    }
    if (result.status == MicDspStatus::OutputFull) {
      drainPending = consumed == frameCount;
      continue;
    }
    if (consumed == frameCount) {
      if (drainPending) {
        drainPending = false;
        continue;
      }
      return {consumed, producedTotal, MicCapturePacketStatus::Complete, ringOverflow};
    }
    if (result.consumedFrames == 0 && result.producedSamples == 0) {
      return {consumed, producedTotal, MicCapturePacketStatus::NoProgress, ringOverflow};
    }
  }
}
