#include "core/mic_dsp.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

constexpr uint16_t kMonoBlockAlign = sizeof(int16_t);

std::vector<int16_t> Reference(const std::vector<int16_t>& input,
                               uint32_t srcRate, uint32_t targetRate) {
  std::vector<int16_t> expected;
  if (input.empty()) return expected;
  const uint64_t lastOutputIndex =
      (static_cast<uint64_t>(input.size() - 1u) * targetRate) / srcRate;
  expected.reserve(static_cast<size_t>(lastOutputIndex + 1u));
  for (uint64_t outputIndex = 0; outputIndex <= lastOutputIndex; ++outputIndex) {
    const uint64_t numerator = outputIndex * srcRate;
    const uint64_t left = numerator / targetRate;
    const uint64_t remainder = numerator % targetRate;
    const double sample = static_cast<double>(input[static_cast<size_t>(left)]) +
        (static_cast<double>(input[static_cast<size_t>(left + (remainder != 0))]) -
         input[static_cast<size_t>(left)]) * static_cast<double>(remainder) / targetRate;
    const double rounded = sample >= 0.0 ? std::floor(sample + 0.5) : std::ceil(sample - 0.5);
    expected.push_back(static_cast<int16_t>(std::clamp(rounded, -32768.0, 32767.0)));
  }
  return expected;
}

void Append(std::vector<int16_t>& output, const int16_t* samples, uint32_t count) {
  output.insert(output.end(), samples, samples + count);
}

std::vector<int16_t> ConvertPartitioned(const std::vector<int16_t>& input,
                                        uint32_t srcRate, uint32_t targetRate,
                                        uint32_t packetSize, uint32_t outputCapacity) {
  MicDspResampler resampler;
  std::vector<int16_t> result;
  int16_t output[257];
  if (outputCapacity == 0 || outputCapacity > 257) {
    ADD_FAILURE() << "invalid test output capacity " << outputCapacity;
    return result;
  }
  size_t packetStart = 0;
  while (packetStart < input.size()) {
    const size_t requestedPacket = packetSize == 0
        ? 1u + ((packetStart * 1103515245u + 12345u) % 257u)
        : packetSize;
    const size_t packetFrames = std::min<size_t>(requestedPacket, input.size() - packetStart);
    uint32_t consumed = 0;
    for (;;) {
      const size_t offset = packetStart + consumed;
      const uint32_t remaining = static_cast<uint32_t>(packetFrames - consumed);
      const MicDspResult converted = resampler.Process(
          input.data() + offset, remaining, static_cast<size_t>(remaining) * sizeof(int16_t),
          1, kMonoBlockAlign, srcRate, targetRate, false, 16, false,
          output, outputCapacity);
      Append(result, output, converted.producedSamples);
      consumed += converted.consumedFrames;
      if (converted.status == MicDspStatus::InvalidInput || converted.status == MicDspStatus::RateChanged) {
        ADD_FAILURE() << "unexpected conversion error";
        return result;
      }
      if (converted.status == MicDspStatus::OutputFull) continue;
      if (consumed == packetFrames) break;
      if (converted.consumedFrames == 0 && converted.producedSamples == 0) {
        ADD_FAILURE() << "resampler stalled inside a packet";
        return result;
      }
    }
    packetStart += packetFrames;
  }

  for (;;) {
    const MicDspResult drained = resampler.Process(
        nullptr, 0, 0, 1, kMonoBlockAlign, srcRate, targetRate, false, 16, false,
        output, outputCapacity);
    Append(result, output, drained.producedSamples);
    if (drained.status == MicDspStatus::InvalidInput || drained.status == MicDspStatus::RateChanged) {
      ADD_FAILURE() << "unexpected drain error";
      return result;
    }
    if (drained.status != MicDspStatus::OutputFull) break;
  }
  return result;
}

void ExpectNearSamples(const std::vector<int16_t>& actual,
                       const std::vector<int16_t>& expected, int tolerance = 1) {
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < actual.size(); ++i) {
    EXPECT_LE(std::abs(static_cast<int>(actual[i]) - static_cast<int>(expected[i])), tolerance)
        << "sample " << i;
  }
}

std::vector<int16_t> MakeSignal(size_t frames) {
  std::vector<int16_t> signal(frames);
  for (size_t i = 0; i < frames; ++i) {
    const int32_t value = static_cast<int32_t>((i * 7919u) % 50001u) - 25000;
    signal[i] = static_cast<int16_t>(value);
  }
  return signal;
}

}  // namespace

TEST(MicRingBuffer, EmptyBufferHasNothingAvailable) {
  MicRingBuffer ring(8);
  EXPECT_EQ(ring.Available(), 0u);
  int16_t output[8] = {};
  EXPECT_EQ(ring.Pop(output, 8), 0u);
}

TEST(MicRingBuffer, PushThenPopReturnsSamplesInOrder) {
  MicRingBuffer ring(8);
  const int16_t samples[] = {1, 2, 3, 4};
  EXPECT_FALSE(ring.Push(samples, 4));
  int16_t output[4] = {};
  EXPECT_EQ(ring.Pop(output, 4), 4u);
  EXPECT_EQ(std::vector<int16_t>(output, output + 4), (std::vector<int16_t>{1, 2, 3, 4}));
}

TEST(MicRingBuffer, PopReturnsOnlyAvailableSamples) {
  MicRingBuffer ring(8);
  const int16_t samples[] = {10, 20, 30};
  ring.Push(samples, 3);
  int16_t output[8] = {};
  EXPECT_EQ(ring.Pop(output, 8), 3u);
  EXPECT_EQ(std::vector<int16_t>(output, output + 3), (std::vector<int16_t>{10, 20, 30}));
}

TEST(MicRingBuffer, WrapsAroundInOrder) {
  MicRingBuffer ring(4);
  const int16_t first[] = {1, 2, 3};
  ring.Push(first, 3);
  int16_t drained[2] = {};
  EXPECT_EQ(ring.Pop(drained, 2), 2u);
  const int16_t second[] = {4, 5, 6};
  ring.Push(second, 3);
  int16_t output[4] = {};
  EXPECT_EQ(ring.Pop(output, 4), 4u);
  EXPECT_EQ(std::vector<int16_t>(output, output + 4), (std::vector<int16_t>{3, 4, 5, 6}));
}

TEST(MicRingBuffer, PushPopOverflowAndResetPreserveOrder) {
  MicRingBuffer ring(4);
  const int16_t first[] = {1, 2, 3, 4};
  EXPECT_FALSE(ring.Push(first, 4));
  const int16_t second[] = {5, 6};
  EXPECT_TRUE(ring.Push(second, 2));
  ASSERT_EQ(ring.Available(), 4u);
  int16_t output[4] = {};
  EXPECT_EQ(ring.Pop(output, 4), 4u);
  EXPECT_EQ(std::vector<int16_t>(output, output + 4), std::vector<int16_t>({3, 4, 5, 6}));
  ring.Push(first, 4);
  ring.Reset();
  EXPECT_EQ(ring.Available(), 0u);
}

// #95: audio captured before the game's first read of a stream is stale and is dropped.
TEST(MicRingBuffer, FirstReaderCallDropsAudioCapturedBeforeItAndReportsHowMuch) {
  MicRingBuffer ring(8);
  const int16_t before[] = {1, 2, 3, 4, 5};
  ring.Push(before, 5);
  EXPECT_FALSE(ring.ReaderActive());
  EXPECT_EQ(ring.NoteReaderActive(), 5u);
  EXPECT_TRUE(ring.ReaderActive());
  EXPECT_EQ(ring.Available(), 0u);
  const int16_t fresh[] = {9, 8};
  ring.Push(fresh, 2);
  int16_t out[2] = {};
  EXPECT_EQ(ring.Pop(out, 2), 2u);
  EXPECT_EQ(std::vector<int16_t>(out, out + 2), (std::vector<int16_t>{9, 8}));
}

// The game polls MicAvailable once the moment capture starts, before any audio exists (nevr-2026-10-10T12-00-22.946,
// MicAvailable=1 MicRead=0 at +0.234 s). That poll must not make the ring look consumed.
TEST(MicRingBuffer, ACallOnAnEmptyRingIsNotAReader) {
  MicRingBuffer ring(8);
  EXPECT_EQ(ring.NoteReaderActive(), 0u);
  EXPECT_FALSE(ring.ReaderActive());
  const int16_t before[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  EXPECT_TRUE(ring.Push(before, 10));  // overflows while nobody consumes: the provider stays quiet
  EXPECT_FALSE(ring.ReaderActive());
  EXPECT_EQ(ring.NoteReaderActive(), 8u);  // the first call that finds audio drops it and arms the warning
  EXPECT_TRUE(ring.ReaderActive());
}

TEST(MicRingBuffer, LaterReaderCallsDropNothing) {
  MicRingBuffer ring(8);
  const int16_t first[] = {9};
  ring.Push(first, 1);
  EXPECT_EQ(ring.NoteReaderActive(), 1u);
  const int16_t s[] = {1, 2, 3};
  ring.Push(s, 3);
  EXPECT_EQ(ring.NoteReaderActive(), 0u);
  EXPECT_EQ(ring.Available(), 3u);
}

TEST(MicRingBuffer, ResetStartsANewStreamTheNextReaderCallDropsAgain) {
  MicRingBuffer ring(8);
  ring.NoteReaderActive();
  ring.Reset();
  EXPECT_FALSE(ring.ReaderActive());
  const int16_t s[] = {1, 2, 3};
  ring.Push(s, 3);
  EXPECT_EQ(ring.NoteReaderActive(), 3u);
}

TEST(MicRingBuffer, StalledReaderIsBoundedByTheCapacityKeepingTheNewest) {
  MicRingBuffer ring(4);
  const int16_t first[] = {0};
  ring.Push(first, 1);
  ring.NoteReaderActive();
  const int16_t s[] = {1, 2, 3, 4, 5, 6};
  EXPECT_TRUE(ring.Push(s, 6));
  int16_t out[4] = {};
  EXPECT_EQ(ring.Pop(out, 4), 4u);
  EXPECT_EQ(std::vector<int16_t>(out, out + 4), (std::vector<int16_t>{3, 4, 5, 6}));
}

TEST(MicDspResampler, MatchesIndependentRationalReferenceAcrossRatesAndPartitions) {
  struct RatePair { uint32_t source; uint32_t target; size_t frames; };
  const RatePair rates[] = {
      {48000, 48000, 31}, {44100, 48000, 3073}, {48000, 44100, 4097},
      {8000, 48000, 5003}, {96000, 48000, 2051}, {44100, 32003, 8197},
  };
  for (const RatePair& pair : rates) {
    const std::vector<int16_t> input = MakeSignal(pair.frames);
    const std::vector<int16_t> expected = Reference(input, pair.source, pair.target);
    const uint32_t packetSizes[] = {1, 7, 113, 8192, 0};
    const uint32_t capacities[] = {1, 2, 31, 257};
    for (const uint32_t packet : packetSizes) {
      for (const uint32_t capacity : capacities) {
        SCOPED_TRACE(::testing::Message() << pair.source << "->" << pair.target
                                         << " packet=" << packet
                                         << " capacity=" << capacity);
        ExpectNearSamples(ConvertPartitioned(input, pair.source, pair.target, packet, capacity), expected);
      }
    }
  }
}

TEST(MicDspResampler, LongCoprimeRateStreamMatchesReference) {
  const std::vector<int16_t> input = MakeSignal(100003);
  ExpectNearSamples(ConvertPartitioned(input, 44100, 48000, 0, 97), Reference(input, 44100, 48000));
}

TEST(MicDspResampler, SingleFramePacketsEmitEachAvailableExactSampleOnce) {
  const std::vector<int16_t> input = {10, 20, 40, 80, 160, 320};
  ExpectNearSamples(ConvertPartitioned(input, 48000, 48000, 1, 1), input);
  ExpectNearSamples(ConvertPartitioned(input, 44100, 48000, 1, 1), Reference(input, 44100, 48000));
}

TEST(MicDspResampler, ZeroCapacityConsumesNothingAndZeroInputDrainsPendingSegment) {
  MicDspResampler resampler;
  int16_t output[1] = {};
  const int16_t input[] = {1000, 2000};
  const MicDspResult noCapacity = resampler.Process(input, 2, sizeof(input), 1, 2,
      8000, 48000, false, 16, false, output, 0);
  EXPECT_EQ(noCapacity.status, MicDspStatus::OutputFull);
  EXPECT_EQ(noCapacity.consumedFrames, 0u);
  EXPECT_EQ(noCapacity.producedSamples, 0u);

  const MicDspResult first = resampler.Process(input, 2, sizeof(input), 1, 2,
      8000, 48000, false, 16, false, output, 1);
  ASSERT_EQ(first.consumedFrames, 1u);
  ASSERT_EQ(first.producedSamples, 1u);
  EXPECT_EQ(output[0], 1000);

  const MicDspResult segment = resampler.Process(input + 1, 1, sizeof(int16_t), 1, 2,
      8000, 48000, false, 16, false, output, 1);
  ASSERT_EQ(segment.consumedFrames, 1u);
  ASSERT_EQ(segment.producedSamples, 1u);
  EXPECT_EQ(segment.status, MicDspStatus::OutputFull);

  const MicDspResult noCapacityWhilePending = resampler.Process(nullptr, 0, 0, 1, 2,
      8000, 48000, false, 16, false, output, 0);
  EXPECT_EQ(noCapacityWhilePending.status, MicDspStatus::OutputFull);
  EXPECT_EQ(noCapacityWhilePending.consumedFrames, 0u);
  EXPECT_EQ(noCapacityWhilePending.producedSamples, 0u);

  uint32_t drained = 0;
  for (;;) {
    const MicDspResult next = resampler.Process(nullptr, 0, 0, 1, 2,
        8000, 48000, false, 16, false, output, 1);
    drained += next.producedSamples;
    if (next.status != MicDspStatus::OutputFull) break;
  }
  EXPECT_EQ(drained, 5u);
}

TEST(MicDspResampler, EmptyCallDoesNotConfigureOrChangeStreamRate) {
  MicDspResampler resampler;
  int16_t output[4] = {};
  const MicDspResult empty = resampler.Process(nullptr, 0, 0, 1, 2,
      44100, 48000, false, 16, false, output, 4);
  EXPECT_EQ(empty.status, MicDspStatus::NeedInput);
  const int16_t first = 10;
  const MicDspResult configured = resampler.Process(&first, 1, sizeof(first), 1, 2,
      48000, 48000, false, 16, false, output, 4);
  EXPECT_EQ(configured.consumedFrames, 1u);
  const MicDspResult changed = resampler.Process(&first, 1, sizeof(first), 1, 2,
      44100, 48000, false, 16, false, output, 4);
  EXPECT_EQ(changed.status, MicDspStatus::RateChanged);
}

TEST(MicDspResampler, RejectsTruncatedDataAndInvalidStrideWithoutConsuming) {
  MicDspResampler resampler;
  int16_t output[8] = {};
  const int16_t samples[] = {1, 2};
  const MicDspResult truncated = resampler.Process(samples, 2, sizeof(int16_t), 1, 2,
      48000, 48000, false, 16, false, output, 8);
  EXPECT_EQ(truncated.status, MicDspStatus::InvalidInput);
  EXPECT_EQ(truncated.consumedFrames, 0u);
  const MicDspResult badStride = resampler.Process(samples, 2, sizeof(samples), 2, 2,
      48000, 48000, false, 16, false, output, 8);
  EXPECT_EQ(badStride.status, MicDspStatus::InvalidInput);
  EXPECT_EQ(badStride.consumedFrames, 0u);
}

TEST(MicDspResampler, DefinesFloatScalingClippingAndHalfwayRounding) {
  MicDspResampler resampler;
  int16_t output[8] = {};
  const float input[] = {1.0F, -1.0F, 0.5F, 2.0F};
  const MicDspResult floats = resampler.Process(input, 4, sizeof(input), 1, sizeof(float),
      48000, 48000, true, 32, false, output, 8);
  ASSERT_EQ(floats.producedSamples, 4u);
  EXPECT_EQ(output[0], 32767);
  EXPECT_EQ(output[1], -32768);
  EXPECT_EQ(output[2], 16384);
  EXPECT_EQ(output[3], 32767);

  resampler.Reset();
  const int16_t stereo[] = {0, 1, 0, -1};
  const MicDspResult rounded = resampler.Process(stereo, 2, sizeof(stereo), 2, 4,
      48000, 48000, false, 16, false, output, 8);
  ASSERT_EQ(rounded.producedSamples, 2u);
  EXPECT_EQ(output[0], 1);
  EXPECT_EQ(output[1], -1);
}

TEST(MicDspResampler, SilentFramesAdvanceTimeWithoutDereferencingInput) {
  MicDspResampler resampler;
  int16_t output[32] = {};
  const int16_t initial[] = {1200, 2400};
  const MicDspResult initialResult = resampler.Process(initial, 2, sizeof(initial), 1, 2,
      48000, 48000, false, 16, false, output, 32);
  ASSERT_EQ(initialResult.consumedFrames, 2u);
  std::vector<int16_t> actual(output, output + initialResult.producedSamples);
  const MicDspResult silent = resampler.Process(reinterpret_cast<const void*>(1), 3, 0, 1, 2,
      48000, 48000, false, 16, true, output, 32);
  ASSERT_EQ(silent.consumedFrames, 3u);
  actual.insert(actual.end(), output, output + silent.producedSamples);
  const int16_t afterSilence[] = {3000};
  const MicDspResult resumed = resampler.Process(afterSilence, 1, sizeof(afterSilence), 1, 2,
      48000, 48000, false, 16, false, output, 32);
  ASSERT_EQ(resumed.consumedFrames, 1u);
  actual.insert(actual.end(), output, output + resumed.producedSamples);
  const std::vector<int16_t> expected = {1200, 2400, 0, 0, 0, 3000};
  ExpectNearSamples(actual, expected);
}

TEST(MicCapturePacketAdapter, HandlesLargePacketsAndInterleavedStrideWithoutTruncation) {
  constexpr uint32_t kFrames = 9001;
  constexpr uint16_t kChannels = 2;
  constexpr uint16_t kBlockAlign = kChannels * sizeof(int16_t);
  std::vector<int16_t> interleaved(static_cast<size_t>(kFrames) * kChannels);
  std::vector<int16_t> mono(kFrames);
  for (uint32_t frame = 0; frame < kFrames; ++frame) {
    mono[frame] = static_cast<int16_t>(static_cast<int32_t>(frame % 4096u) * 7 - 14000);
    interleaved[frame * 2] = mono[frame];
    interleaved[frame * 2 + 1] = mono[frame];
  }
  MicRingBuffer ring(65536);
  MicCapturePacketAdapter adapter;
  const auto converted = adapter.Process(interleaved.data(), kFrames, interleaved.size() * sizeof(int16_t),
      kChannels, kBlockAlign, 44100, 48000, false, 16, false, ring);
  ASSERT_EQ(converted.status, MicCapturePacketStatus::Complete);
  EXPECT_EQ(converted.consumedFrames, kFrames);
  const std::vector<int16_t> expected = Reference(mono, 44100, 48000);
  EXPECT_EQ(converted.producedSamples, expected.size());
  std::vector<int16_t> actual(expected.size());
  ASSERT_EQ(ring.Pop(actual.data(), static_cast<uint32_t>(actual.size())), actual.size());
  ExpectNearSamples(actual, expected);
}

TEST(MicCapturePacketAdapter, SilentPacketUsesNoInputPointerButAdvancesClock) {
  MicRingBuffer ring(128);
  MicCapturePacketAdapter adapter;
  const auto result = adapter.Process(reinterpret_cast<const void*>(1), 20, 0, 1, 2,
      8000, 48000, false, 16, true, ring);
  EXPECT_EQ(result.status, MicCapturePacketStatus::Complete);
  EXPECT_EQ(result.consumedFrames, 20u);
  EXPECT_EQ(result.producedSamples, 115u);
  std::vector<int16_t> actual(115);
  EXPECT_EQ(ring.Pop(actual.data(), 115), 115u);
  EXPECT_TRUE(std::all_of(actual.begin(), actual.end(), [](int16_t sample) { return sample == 0; }));
}

TEST(MicCapturePacketAdapter, RejectsMalformedPacketBoundsBeforeReading) {
  MicRingBuffer ring(16);
  MicCapturePacketAdapter adapter;
  const int16_t sample = 5;
  const auto result = adapter.Process(&sample, 2, sizeof(sample), 1, 2,
      48000, 48000, false, 16, false, ring);
  EXPECT_EQ(result.status, MicCapturePacketStatus::InvalidInput);
  EXPECT_EQ(result.consumedFrames, 0u);
}

TEST(MicCapturePacketAdapter, ResetStartsANewStreamAtTimeZero) {
  MicRingBuffer ring(32);
  MicCapturePacketAdapter adapter;
  const int16_t first[] = {100, 200, 300};
  ASSERT_EQ(adapter.Process(first, 3, sizeof(first), 1, 2,
      48000, 48000, false, 16, false, ring).status, MicCapturePacketStatus::Complete);
  int16_t oldOutput[3] = {};
  ring.Pop(oldOutput, 3);
  adapter.Reset();
  const int16_t second[] = {700, 800};
  const auto result = adapter.Process(second, 2, sizeof(second), 1, 2,
      48000, 48000, false, 16, false, ring);
  EXPECT_EQ(result.producedSamples, 2u);
  int16_t newOutput[2] = {};
  EXPECT_EQ(ring.Pop(newOutput, 2), 2u);
  EXPECT_EQ(newOutput[0], 700);
  EXPECT_EQ(newOutput[1], 800);
}
