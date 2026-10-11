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
  /// Samples overwritten before the game read them, over the life of the process (Reset() keeps it).
  uint64_t DroppedSamples() const;

  /// Called by the game's MicRead before it pops. The first call after the stream (re)started that
  /// finds audio waiting drops that backlog, so the first read gets only fresh audio (#95), and
  /// returns how many samples that was. A call that finds the ring empty changes nothing. Later calls
  /// return 0 until Reset(). It does not make the game a consumer by itself (see ReaderActive).
  uint32_t NoteReaderActive();
  /// True once a Pop has returned audio since the last Reset(): the game is demonstrably consuming.
  /// Before that a full ring is the normal state, not a game that stopped draining.
  bool ReaderActive() const;

 private:
  mutable std::mutex mutex_;
  bool backlogDropped_ = false;
  bool consumerSeen_ = false;
  uint32_t capacity_;
  std::vector<int16_t> data_;
  uint32_t head_ = 0;
  uint32_t count_ = 0;
  uint64_t droppedSamples_ = 0;
};

/// Decides when ring overflow is worth a log line: the first overflow of a stream the game is reading,
/// then at most one line per interval carrying how many samples were dropped since the last line (#95).
/// Used from the capture thread only.
class MicOverflowLogGate {
 public:
  explicit MicOverflowLogGate(uint64_t intervalMs) : intervalMs_(intervalMs) {}
  /// `droppedTotal` is MicRingBuffer::DroppedSamples(); `nowMs` a monotonic clock. Returns the samples
  /// dropped since the last reported line when one should be logged now, otherwise 0.
  uint64_t Poll(uint64_t droppedTotal, uint64_t nowMs);
  /// Treats everything dropped so far as already accounted for (the game is not reading yet, so a full
  /// ring is the normal state and not a stall).
  void Rebase(uint64_t droppedTotal) { reported_ = droppedTotal; }
  /// A new stream starts: its first overflow logs at once. Keeps what was already accounted for.
  void Reset() { hasLogged_ = false; }

 private:
  uint64_t intervalMs_;
  uint64_t reported_ = 0;
  uint64_t lastLogMs_ = 0;
  bool hasLogged_ = false;
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
