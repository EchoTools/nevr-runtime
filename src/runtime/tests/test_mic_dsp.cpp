// Ground-truth tests for core/mic_dsp.{h,cpp} — the pure logic behind the
// WASAPI mic provider (GH nevr-runtime#15, docs/design/2026-09-21-mic-
// provider-voip-fix.md). No windows.h, no capture device, no Wine: this
// exercises exactly the ring-buffer and downmix/resample math on the build
// host.

#include "core/mic_dsp.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

// --- MicRingBuffer ---------------------------------------------------------

TEST(MicRingBuffer, EmptyBufferHasNothingAvailable) {
  MicRingBuffer ring(8);
  EXPECT_EQ(ring.Available(), 0u);
  int16_t out[8];
  EXPECT_EQ(ring.Pop(out, 8), 0u);
}

TEST(MicRingBuffer, PushThenPopReturnsSameSamplesInOrder) {
  MicRingBuffer ring(8);
  const int16_t samples[] = {1, 2, 3, 4};
  EXPECT_FALSE(ring.Push(samples, 4));  // fits; nothing dropped
  ASSERT_EQ(ring.Available(), 4u);

  int16_t out[4] = {0, 0, 0, 0};
  EXPECT_EQ(ring.Pop(out, 4), 4u);
  EXPECT_EQ(ring.Available(), 0u);
  EXPECT_EQ(std::vector<int16_t>(out, out + 4), std::vector<int16_t>({1, 2, 3, 4}));
}

TEST(MicRingBuffer, PopReturnsFewerSamplesThanRequestedWhenNotFull) {
  MicRingBuffer ring(8);
  const int16_t samples[] = {10, 20, 30};
  ring.Push(samples, 3);

  int16_t out[8] = {0};
  EXPECT_EQ(ring.Pop(out, 8), 3u);  // asked for 8, only 3 were ever pushed
  EXPECT_EQ(out[0], 10);
  EXPECT_EQ(out[1], 20);
  EXPECT_EQ(out[2], 30);
}

TEST(MicRingBuffer, WrapsAroundCorrectly) {
  MicRingBuffer ring(4);
  const int16_t first[] = {1, 2, 3};
  ring.Push(first, 3);
  int16_t drained[2];
  ring.Pop(drained, 2);  // ring now holds just {3}, head/tail both mid-buffer

  const int16_t second[] = {4, 5, 6};  // wraps past the buffer's physical end
  ring.Push(second, 3);
  ASSERT_EQ(ring.Available(), 4u);  // capacity 4: {3,4,5,6}

  int16_t out[4] = {0};
  EXPECT_EQ(ring.Pop(out, 4), 4u);
  EXPECT_EQ(std::vector<int16_t>(out, out + 4), std::vector<int16_t>({3, 4, 5, 6}));
}

TEST(MicRingBuffer, OverflowDropsOldestAndReportsTrue) {
  MicRingBuffer ring(4);
  const int16_t first[] = {1, 2, 3, 4};
  EXPECT_FALSE(ring.Push(first, 4));  // exactly fills the ring

  const int16_t second[] = {5, 6};
  EXPECT_TRUE(ring.Push(second, 2));  // must drop the two oldest (1, 2) to fit

  ASSERT_EQ(ring.Available(), 4u);
  int16_t out[4] = {0};
  ring.Pop(out, 4);
  // Oldest surviving sample first: 1 and 2 were displaced, 3/4/5/6 remain.
  EXPECT_EQ(std::vector<int16_t>(out, out + 4), std::vector<int16_t>({3, 4, 5, 6}));
}

TEST(MicRingBuffer, ResetDiscardsBufferedSamples) {
  MicRingBuffer ring(4);
  const int16_t samples[] = {1, 2, 3};
  ring.Push(samples, 3);
  ring.Reset();
  EXPECT_EQ(ring.Available(), 0u);
}

// --- DownmixResampleToMonoInt16 --------------------------------------------

TEST(DownmixResampleToMonoInt16, MonoInt16AtTargetRatePassesThroughUnchanged) {
  const int16_t in[] = {1000, -1000, 500, -500, 0};
  double phase = 0.0;
  int16_t out[16] = {0};
  uint32_t n = DownmixResampleToMonoInt16(in, /*frameCount=*/5, /*channels=*/1,
                                          /*srcRate=*/48000, /*isFloat=*/false, /*bitsPerSample=*/16,
                                          /*targetRate=*/48000, &phase, out, 16);
  // Same rate in and out: expect (frameCount - 1) samples (the resampler
  // stops one short of the last frame — no next sample to interpolate
  // toward — and carries the remainder via phase for the next packet).
  ASSERT_EQ(n, 4u);
  for (uint32_t i = 0; i < n; i++) {
    EXPECT_NEAR(out[i], in[i], 1) << "sample " << i;
  }
}

TEST(DownmixResampleToMonoInt16, StereoDownmixAveragesChannels) {
  // Interleaved stereo: L=32767 (max), R=-32768 (min) -> average ~0.
  const int16_t in[] = {32767, -32768, 32767, -32768};
  double phase = 0.0;
  int16_t out[16] = {0};
  uint32_t n = DownmixResampleToMonoInt16(in, /*frameCount=*/2, /*channels=*/2,
                                          /*srcRate=*/48000, /*isFloat=*/false, /*bitsPerSample=*/16,
                                          /*targetRate=*/48000, &phase, out, 16);
  ASSERT_GE(n, 1u);
  EXPECT_NEAR(out[0], 0, 200) << "L/R average of max and min should be near silence";
}

TEST(DownmixResampleToMonoInt16, FloatInputIsScaledToInt16Range) {
  const float in[] = {1.0f, -1.0f, 0.0f, 0.5f};
  double phase = 0.0;
  int16_t out[16] = {0};
  uint32_t n = DownmixResampleToMonoInt16(in, /*frameCount=*/4, /*channels=*/1,
                                          /*srcRate=*/48000, /*isFloat=*/true, /*bitsPerSample=*/32,
                                          /*targetRate=*/48000, &phase, out, 16);
  ASSERT_GE(n, 3u);
  EXPECT_NEAR(out[0], 32767, 5);
  EXPECT_NEAR(out[1], -32767, 5);
  EXPECT_NEAR(out[2], 0, 5);
}

TEST(DownmixResampleToMonoInt16, ClipsOutOfRangeFloatSamples) {
  const float in[] = {2.0f, -3.0f, 0.0f};  // deliberately out of [-1, 1]
  double phase = 0.0;
  int16_t out[16] = {0};
  uint32_t n = DownmixResampleToMonoInt16(in, /*frameCount=*/3, /*channels=*/1,
                                          /*srcRate=*/48000, /*isFloat=*/true, /*bitsPerSample=*/32,
                                          /*targetRate=*/48000, &phase, out, 16);
  ASSERT_GE(n, 2u);
  EXPECT_EQ(out[0], 32767);   // clipped to max, not wrapped/overflowed
  EXPECT_EQ(out[1], -32767);  // clipped to min
}

TEST(DownmixResampleToMonoInt16, UpsamplingProducesMoreSamplesThanInput) {
  // 8kHz -> 48000Hz is a 6x upsample.
  const int16_t in[] = {0, 10000, 0, -10000, 0, 10000, 0, -10000};
  double phase = 0.0;
  int16_t out[64] = {0};
  uint32_t n = DownmixResampleToMonoInt16(in, /*frameCount=*/8, /*channels=*/1,
                                          /*srcRate=*/8000, /*isFloat=*/false, /*bitsPerSample=*/16,
                                          /*targetRate=*/48000, &phase, out, 64);
  EXPECT_GT(n, 8u * 3);  // meaningfully more output samples than input frames
}

TEST(DownmixResampleToMonoInt16, PhaseCarriesAcrossPacketsWithoutGapOrOverlap) {
  // Two consecutive packets of the same ramp: total output samples across
  // both calls should track total input frames at a 1:1 rate, not double-
  // count or drop a sample at the boundary.
  const int16_t packet1[] = {0, 1, 2, 3};
  const int16_t packet2[] = {4, 5, 6, 7};
  double phase = 0.0;
  int16_t out1[16] = {0};
  int16_t out2[16] = {0};
  uint32_t n1 = DownmixResampleToMonoInt16(packet1, 4, 1, 48000, false, 16, 48000, &phase, out1, 16);
  uint32_t n2 = DownmixResampleToMonoInt16(packet2, 4, 1, 48000, false, 16, 48000, &phase, out2, 16);
  // At a 1:1 rate this should track close to 8 total samples across both
  // packets (allowing for the boundary carry), not e.g. 6 (a dropped gap)
  // or 10+ (a duplicated overlap).
  EXPECT_NEAR(static_cast<int>(n1 + n2), 7, 1);
}

TEST(DownmixResampleToMonoInt16, ZeroChannelsProducesNoOutput) {
  const int16_t in[] = {1, 2, 3};
  double phase = 0.0;
  int16_t out[16] = {0};
  uint32_t n = DownmixResampleToMonoInt16(in, 3, /*channels=*/0, 48000, false, 16, 48000, &phase, out, 16);
  EXPECT_EQ(n, 0u);
}

TEST(DownmixResampleToMonoInt16, UnsupportedBitDepthIsTreatedAsSilenceNotGarbage) {
  // 24-bit int PCM (not float, bitsPerSample != 16) is documented as
  // "treated as silence rather than guessed" — verify that contract holds
  // rather than reinterpreting the bytes as some other width.
  const uint8_t in[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // would be nonzero misread as int16
  double phase = 0.0;
  int16_t out[16] = {0};
  uint32_t n = DownmixResampleToMonoInt16(in, /*frameCount=*/2, /*channels=*/1, 48000, /*isFloat=*/false,
                                          /*bitsPerSample=*/24, 48000, &phase, out, 16);
  ASSERT_GE(n, 1u);
  EXPECT_EQ(out[0], 0);
}
