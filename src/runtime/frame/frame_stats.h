#pragma once

// #954: the PC frame-time probe. Quest has a metrics exporter; the PC release can't measure itself, so the
// runtime measures two things and logs one summary line per 10 s window:
//
//  1. The interval between iterations of the game's main loop. CncaGame::RunMainLoop (echovr.exe 0x14050bd00)
//     calls GetTimeMicroseconds (0x1400d00c0) once per iteration at 0x14050bd47, and the instruction after
//     that call is 0x14050bd4c. The runtime already hooks GetTimeMicroseconds (binary_bug_fixes.cpp); when
//     that hook's return address is 0x14050bd4c the caller is the loop head, and the hook pushes its own QPC
//     reading into FrameRing. Every other caller (about 5k calls a second on the server fixture) costs one
//     compare.
//  2. The compositor stats of a PCVR session. The game reaches ovr_GetPerfStats through a thunk at
//     0x1413662e0; a detour on the thunk calls the original and copies the per-frame entries out of the
//     caller's buffer into PerfRing. It never makes a second call, so it can't race the session.
//
// The two writers (the game's render/loop threads) never block and never log. Poll() runs on the runtime's
// per-frame dispatcher, drains both rings into a Collector and, every kWindowSeconds, writes one line.
//
// Everything in this header is pure (std only, no windows.h, no game memory) so the host GTest can drive it.
// frame_stats.cpp owns the hooks, the config and the log call.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#include "runtime/hook/addresses.h"

namespace nevr_frame_stats {

/// One summary line per this many seconds.
constexpr uint64_t kWindowSeconds = 10;
constexpr uint64_t kWindowUs = kWindowSeconds * 1000000ULL;

/// FrameRing capacity: one slot per loop iteration. Poll drains every 8 ms, so 1024 slots hold more than
/// eight seconds at 120 Hz.
constexpr std::size_t kFrameRingSlots = 1024;
/// PerfRing capacity: up to five entries per ovr_GetPerfStats call.
constexpr std::size_t kPerfRingSlots = 256;
/// Intervals one window can keep (1,600 fps for 10 s). Further intervals are counted as overruns.
constexpr std::size_t kMaxWindowIntervals = 16384;

// ---------------------------------------------------------------------------------------------------------
// The switch
// ---------------------------------------------------------------------------------------------------------

/// `diagnostics.frame_stats` in config.yaml. Off is the default: shipped builds and servers log nothing.
///   Full     the loop-interval ring and its line, with the compositor stats when a VR session supplies them
///   PerfOnly the line with the compositor stats only (frames=0): the ring is off, so a PCVR run can compare
///            the game's own app_cpu with the ring off against the ring on
enum class Mode : int { Off = 0, Full = 1, PerfOnly = 2 };

inline const char* ModeName(Mode mode) {
  switch (mode) {
    case Mode::Full: return "full";
    case Mode::PerfOnly: return "perf";
    case Mode::Off: break;
  }
  return "off";
}

namespace detail {
inline std::string Lower(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}
}  // namespace detail

/// Reads the config value. An absent key is Off (and recognised). true/yes/on/1 is Full, false/no/off/0 is
/// Off, "perf" is PerfOnly, anything else is Off with *recognised = false so the boot line can say so.
inline Mode ParseMode(const std::optional<std::string>& value, bool* recognised = nullptr) {
  if (recognised != nullptr) *recognised = true;
  if (!value) return Mode::Off;
  const std::string v = detail::Lower(*value);
  if (v == "true" || v == "yes" || v == "on" || v == "1") return Mode::Full;
  if (v == "false" || v == "no" || v == "off" || v == "0") return Mode::Off;
  if (v == "perf") return Mode::PerfOnly;
  if (recognised != nullptr) *recognised = false;
  return Mode::Off;
}

// ---------------------------------------------------------------------------------------------------------
// The loop head
// ---------------------------------------------------------------------------------------------------------

/// True when `returnAddress` is the instruction after the loop head's call to GetTimeMicroseconds. A zero
/// base never matches, so a hook that runs before the base is known pushes nothing.
inline bool IsLoopHeadReturn(uintptr_t returnAddress, uintptr_t gameBase) {
  return gameBase != 0 && returnAddress == gameBase + nevr_patch_addresses::FRAME_LOOP_HEAD_RETURN;
}

/// echovr.exe 0x14050bd47 (CncaGame::RunMainLoop): CALL rel32 0x1400d00c0. The five bytes name the callee
/// through the displacement (0x14050bd4c + 0xffbc4374 = 0x1400d00c0), so a match proves the return address
/// above follows a call to GetTimeMicroseconds.
constexpr uint8_t kLoopHeadCallBytes[5] = {0xE8, 0x74, 0x43, 0xBC, 0xFF};
constexpr uintptr_t kLoopHeadCallRva = nevr_patch_addresses::FRAME_LOOP_HEAD_RETURN - sizeof(kLoopHeadCallBytes);

inline bool LoopHeadCallMatches(const void* site) {
  return site != nullptr && std::memcmp(site, kLoopHeadCallBytes, sizeof(kLoopHeadCallBytes)) == 0;
}

// ---------------------------------------------------------------------------------------------------------
// The ring
// ---------------------------------------------------------------------------------------------------------

/// A bounded queue for one writer and one reader. The writer never blocks: a full ring drops the NEW entry and
/// counts it (the oldest entries are the ones a reader is about to use). A second writer that arrives while
/// one is inside Push is dropped and counted too, so a caller that turns out not to be single-threaded loses
/// samples instead of corrupting the ring.
template <typename T, std::size_t N>
class SpscRing {
  static_assert(N != 0 && (N & (N - 1)) == 0, "N must be a power of two");

 public:
  /// Writer. False when the entry was dropped.
  bool Push(const T& value) noexcept {
    if (writing_.test_and_set(std::memory_order_acquire)) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    bool pushed = false;
    if (head - tail < N) {
      slots_[head & (N - 1)] = value;
      head_.store(head + 1, std::memory_order_release);
      pushed = true;
    } else {
      dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    writing_.clear(std::memory_order_release);
    return pushed;
  }

  /// Reader. False when the ring is empty.
  bool Pop(T& out) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    if (tail == head) return false;
    out = slots_[tail & (N - 1)];
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  /// Entries dropped since the process started (monotonic).
  uint64_t Dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

 private:
  std::array<T, N> slots_{};
  std::atomic<std::size_t> head_{0};
  std::atomic<std::size_t> tail_{0};
  std::atomic<uint64_t> dropped_{0};
  std::atomic_flag writing_ = ATOMIC_FLAG_INIT;
};

/// One loop-head reading. `dropped` is the ring's drop count when the reading was pushed: if it differs from
/// the previous reading's, entries were lost in between and the interval across them is not a frame time.
struct FrameSlot {
  uint64_t us = 0;
  uint64_t dropped = 0;
};

// ---------------------------------------------------------------------------------------------------------
// ovrPerfStats
// ---------------------------------------------------------------------------------------------------------

/// The fields the probe keeps from one ovrPerfStatsPerCompositorFrame.
struct PerfSample {
  int32_t appDropped = 0;         // AppDroppedFrameCount (cumulative)
  int32_t compositorDropped = 0;  // CompositorDroppedFrameCount (cumulative)
  float appCpuS = 0.0f;           // AppCpuElapsedTime, seconds
  float appGpuS = 0.0f;           // AppGpuElapsedTime, seconds
};

// Layout of ovrPerfStats, from the Oculus PC SDK 1.35.0 OVR_CAPI.h (ovrPerfStatsPerCompositorFrame_, then
// ovrPerfStats_). Every field is 4 bytes except ovrBool (a char, padded to 4 by OVR_ALIGNAS(4)), so an entry
// is 18 x 4 = 0x48 bytes and the structure is 5 x 0x48 + 0x18 = 0x17c:
//   +0x00 HmdVsyncIndex          +0x04 AppFrameIndex          +0x08 AppDroppedFrameCount
//   +0x0c AppMotionToPhotonLatency   +0x10 AppQueueAheadTime  +0x14 AppCpuElapsedTime
//   +0x18 AppGpuElapsedTime      +0x1c CompositorFrameIndex   +0x20 CompositorDroppedFrameCount
//   +0x24 CompositorLatency      ... +0x44 AswFailedFrameCount
// FrameStats[5] starts the structure (newest entry first) and FrameStatsCount follows it at 0x168.
// The game's own use confirms the shape: it clears 0x17c bytes before the call (echovr.exe 0x14072af60), reads
// the count at +0x168 and walks entries at stride 0x48; AppDroppedFrameCount (+8) is the field it reads.
// The other three offsets (+0x14, +0x18, +0x20) are read from the SDK header, not from the game: the game
// reads none of them. The SDK version the game's LibOVR is built against was not verified (1.35.0 is the
// version whose header was read).
constexpr std::size_t kPerfEntryStride = 0x48;
constexpr std::size_t kPerfMaxEntries = 5;
constexpr std::size_t kPerfCountOffset = 0x168;
constexpr std::size_t kPerfAppDroppedOffset = 0x08;
constexpr std::size_t kPerfAppCpuOffset = 0x14;
constexpr std::size_t kPerfAppGpuOffset = 0x18;
constexpr std::size_t kPerfCompositorDroppedOffset = 0x20;
constexpr std::size_t kPerfStatsSize = 0x17C;

namespace detail {
template <typename T>
T ReadField(const uint8_t* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, base + offset, sizeof(T));
  return value;
}
}  // namespace detail

/// Copies the entries of one ovr_GetPerfStats result into `out` (room for kPerfMaxEntries), oldest first.
/// Returns how many were copied. A count outside 0..5, or an entry whose times are negative or not finite,
/// is not copied; *rejected counts those. The reads stay inside the kPerfStatsSize bytes the game gave.
inline std::size_t ExtractPerfSamples(const void* stats, PerfSample* out, std::size_t* rejected) {
  if (stats == nullptr || out == nullptr) return 0;
  const auto* bytes = static_cast<const uint8_t*>(stats);
  const int32_t count = detail::ReadField<int32_t>(bytes, kPerfCountOffset);
  if (count < 0 || count > static_cast<int32_t>(kPerfMaxEntries)) {
    if (rejected != nullptr) *rejected += 1;
    return 0;
  }
  std::size_t n = 0;
  for (int32_t i = count - 1; i >= 0; --i) {  // entry 0 is the newest: walk back to front for oldest first
    const uint8_t* entry = bytes + static_cast<std::size_t>(i) * kPerfEntryStride;
    PerfSample s;
    s.appDropped = detail::ReadField<int32_t>(entry, kPerfAppDroppedOffset);
    s.compositorDropped = detail::ReadField<int32_t>(entry, kPerfCompositorDroppedOffset);
    s.appCpuS = detail::ReadField<float>(entry, kPerfAppCpuOffset);
    s.appGpuS = detail::ReadField<float>(entry, kPerfAppGpuOffset);
    if (!std::isfinite(s.appCpuS) || !std::isfinite(s.appGpuS) || s.appCpuS < 0.0f || s.appGpuS < 0.0f) {
      if (rejected != nullptr) *rejected += 1;
      continue;
    }
    out[n++] = s;
  }
  return n;
}

// ---------------------------------------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------------------------------------

/// One window's figures. Intervals are loop-head to loop-head, in microseconds.
struct WindowStats {
  uint64_t frames = 0;      // intervals kept
  uint32_t medianUs = 0;
  uint32_t p95Us = 0;
  uint32_t maxUs = 0;
  uint64_t over1p5x = 0;    // intervals longer than 1.5 x the window's median
  uint64_t overruns = 0;    // intervals the probe lost (ring full, or window full)
  uint64_t elapsedMs = 0;   // the window's real length, set by the caller

  bool hasPerf = false;     // a VR session supplied compositor stats in this window
  uint64_t perfSamples = 0;
  uint64_t appDropped = 0;          // window delta of the cumulative AppDroppedFrameCount
  uint64_t compositorDropped = 0;   // window delta of the cumulative CompositorDroppedFrameCount
  uint32_t appCpuMeanUs = 0;
  uint32_t appCpuMaxUs = 0;
  uint32_t appGpuMeanUs = 0;
  uint32_t appGpuMaxUs = 0;
};

/// Nearest-rank percentile of an ascending array: the value at rank ceil(pct/100 x n), 1-based.
inline uint32_t Percentile(const uint32_t* sorted, std::size_t n, unsigned pct) {
  if (n == 0) return 0;
  std::size_t rank = (static_cast<std::size_t>(pct) * n + 99) / 100;
  if (rank < 1) rank = 1;
  if (rank > n) rank = n;
  return sorted[rank - 1];
}

/// Accumulates one window. Not thread-safe: the dispatcher's Poll is its only user.
class Collector {
 public:
  /// A loop-head reading. `dropped` is FrameSlot::dropped. The first reading, and the first after a gap,
  /// only set the baseline. A drop count below the one already seen belongs to a reading pushed before drops
  /// the collector has counted, so it neither counts nor breaks the baseline again.
  void AddFrame(uint64_t us, uint64_t dropped) {
    if (dropped > seenDropped_) {
      overruns_ += dropped - seenDropped_;
      seenDropped_ = dropped;
      haveFrame_ = false;
    }
    if (haveFrame_) {
      const uint64_t delta = us >= lastUs_ ? us - lastUs_ : 0;
      const uint32_t interval = delta > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(delta);
      if (count_ < intervals_.size()) {
        intervals_[count_++] = interval;
      } else {
        overruns_ += 1;
      }
    }
    lastUs_ = us;
    haveFrame_ = true;
  }

  void AddPerf(const PerfSample& s) {
    perfSamples_ += 1;
    if (havePerf_) {
      if (s.appDropped >= prevAppDropped_) appDelta_ += static_cast<uint64_t>(s.appDropped - prevAppDropped_);
      if (s.compositorDropped >= prevCompDropped_)
        compDelta_ += static_cast<uint64_t>(s.compositorDropped - prevCompDropped_);
    }
    havePerf_ = true;
    prevAppDropped_ = s.appDropped;
    prevCompDropped_ = s.compositorDropped;
    const double cpuUs = static_cast<double>(s.appCpuS) * 1e6;
    const double gpuUs = static_cast<double>(s.appGpuS) * 1e6;
    cpuSumUs_ += cpuUs;
    gpuSumUs_ += gpuUs;
    if (cpuUs > cpuMaxUs_) cpuMaxUs_ = cpuUs;
    if (gpuUs > gpuMaxUs_) gpuMaxUs_ = gpuUs;
  }

  /// Compositor-stat entries the probe could not keep (PerfRing full, or an entry rejected).
  void AddPerfLost(uint64_t n) { overruns_ += n; }

  /// Closes the window and starts the next. The drop baselines and the previous cumulative counters carry
  /// over, so a drop or a counter increment is counted in exactly one window.
  WindowStats Finish(uint64_t elapsedMs) {
    WindowStats w;
    w.elapsedMs = elapsedMs;
    w.frames = count_;
    if (count_ != 0) {
      std::sort(intervals_.begin(), intervals_.begin() + static_cast<std::ptrdiff_t>(count_));
      w.medianUs = Percentile(intervals_.data(), count_, 50);
      w.p95Us = Percentile(intervals_.data(), count_, 95);
      w.maxUs = intervals_[count_ - 1];
      // Strictly longer than 1.5 x the median, in integers: us x 2 > median x 3.
      for (std::size_t i = 0; i < count_; ++i) {
        if (static_cast<uint64_t>(intervals_[i]) * 2 > static_cast<uint64_t>(w.medianUs) * 3) ++w.over1p5x;
      }
    }
    w.overruns = overruns_;
    if (perfSamples_ != 0) {
      w.hasPerf = true;
      w.perfSamples = perfSamples_;
      w.appDropped = appDelta_;
      w.compositorDropped = compDelta_;
      w.appCpuMeanUs = RoundedUs(cpuSumUs_ / static_cast<double>(perfSamples_));
      w.appCpuMaxUs = RoundedUs(cpuMaxUs_);
      w.appGpuMeanUs = RoundedUs(gpuSumUs_ / static_cast<double>(perfSamples_));
      w.appGpuMaxUs = RoundedUs(gpuMaxUs_);
    }
    count_ = 0;
    overruns_ = 0;
    perfSamples_ = 0;
    appDelta_ = 0;
    compDelta_ = 0;
    cpuSumUs_ = gpuSumUs_ = cpuMaxUs_ = gpuMaxUs_ = 0.0;
    return w;
  }

 private:
  static uint32_t RoundedUs(double us) {
    if (!(us > 0.0)) return 0;
    if (us >= 4294967295.0) return UINT32_MAX;
    return static_cast<uint32_t>(us + 0.5);
  }

  std::array<uint32_t, kMaxWindowIntervals> intervals_{};
  std::size_t count_ = 0;
  uint64_t overruns_ = 0;
  uint64_t seenDropped_ = 0;
  uint64_t lastUs_ = 0;
  bool haveFrame_ = false;

  uint64_t perfSamples_ = 0;
  bool havePerf_ = false;
  int32_t prevAppDropped_ = 0;
  int32_t prevCompDropped_ = 0;
  uint64_t appDelta_ = 0;
  uint64_t compDelta_ = 0;
  double cpuSumUs_ = 0.0;
  double gpuSumUs_ = 0.0;
  double cpuMaxUs_ = 0.0;
  double gpuMaxUs_ = 0.0;
};

// ---------------------------------------------------------------------------------------------------------
// The lines
// ---------------------------------------------------------------------------------------------------------

/// `[NEVR.PERF] frame_stats window_s=10 frames= median_us= p95_us= max_us= over_1p5x= overruns=`, then, when
/// a VR session supplied compositor stats, ` app_dropped= compositor_dropped= app_cpu_us_mean= app_cpu_us_max=
/// app_gpu_us_mean= app_gpu_us_max= perf_samples=`, then ` elapsed_ms=` (the window's real length, so a rate
/// divides by it and not by the nominal 10 s). Returns snprintf's result.
inline int FormatWindowLine(char* buf, std::size_t cap, const WindowStats& w) {
  int n = std::snprintf(buf, cap,
                        "[NEVR.PERF] frame_stats window_s=%llu frames=%llu median_us=%u p95_us=%u max_us=%u "
                        "over_1p5x=%llu overruns=%llu",
                        static_cast<unsigned long long>(kWindowSeconds), static_cast<unsigned long long>(w.frames),
                        w.medianUs, w.p95Us, w.maxUs, static_cast<unsigned long long>(w.over1p5x),
                        static_cast<unsigned long long>(w.overruns));
  if (n < 0 || static_cast<std::size_t>(n) >= cap) return n;
  if (w.hasPerf) {
    const int m = std::snprintf(buf + n, cap - static_cast<std::size_t>(n),
                                " app_dropped=%llu compositor_dropped=%llu app_cpu_us_mean=%u app_cpu_us_max=%u "
                                "app_gpu_us_mean=%u app_gpu_us_max=%u perf_samples=%llu",
                                static_cast<unsigned long long>(w.appDropped),
                                static_cast<unsigned long long>(w.compositorDropped), w.appCpuMeanUs,
                                w.appCpuMaxUs, w.appGpuMeanUs, w.appGpuMaxUs,
                                static_cast<unsigned long long>(w.perfSamples));
    if (m < 0) return m;
    n += m;
    if (static_cast<std::size_t>(n) >= cap) return n;
  }
  const int m = std::snprintf(buf + n, cap - static_cast<std::size_t>(n), " elapsed_ms=%llu",
                              static_cast<unsigned long long>(w.elapsedMs));
  return m < 0 ? m : n + m;
}

/// The one line at boot, written whether the probe is on or off:
/// `[NEVR.PERF] frame_stats start mode= recognised= host= build= frame_ring= perf_hook=`.
inline int FormatStartLine(char* buf, std::size_t cap, Mode mode, bool recognised, const char* host,
                           const char* build, const char* frameRing, const char* perfHook) {
  return std::snprintf(buf, cap,
                       "[NEVR.PERF] frame_stats start mode=%s recognised=%s host=%s build=%s frame_ring=%s "
                       "perf_hook=%s",
                       ModeName(mode), recognised ? "yes" : "no", host, build, frameRing, perfHook);
}

// ---------------------------------------------------------------------------------------------------------
// The runtime side (frame_stats.cpp; the tests do not link it)
// ---------------------------------------------------------------------------------------------------------

/// Resolves the switch, arms the loop-head ring (mode Full, and only when the bytes before the loop head's
/// return address are the call to GetTimeMicroseconds), installs the ovr_GetPerfStats copy on a client, and
/// writes the one start line (whether the probe is on or off). Call once, after the CLI is parsed.
void Configure(uintptr_t gameBase, bool isServer, Mode mode, bool recognised);

/// The GetTimeMicroseconds hook calls this when its return address is the loop head (IsLoopHeadReturn). It
/// pushes one QPC reading and logs nothing. nowUs is the hook's own reading, 0 meaning no usable clock.
void OnLoopHead(uint64_t nowUs);

/// Called from nevr_frame::DispatchPerFrameWork (N86/N110) on every tick: drains both rings and, every
/// kWindowSeconds, writes the window line. Returns at once when the switch is off.
void Poll(uint64_t nowUs);

}  // namespace nevr_frame_stats
