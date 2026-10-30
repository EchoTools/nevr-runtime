/* The Quest frame dispatcher (#966, S7): work from other threads, run on the game thread once per NetGame::Update.
 *
 * The hook that calls Tick() is the one installed on libr15's CR15NetGame::Update through its GOT slot
 * (src/quest/sentinel/login_prompt_hook.cpp, HookedNetGameUpdate). Its install is unconditional: the
 * slot is taken on every launch, so the dispatcher runs whether or not the login prompt is on.
 *
 * Contract:
 *   - RegisterFrameWork(fn, ctx) adds a function that runs on every Tick, in registration order, before the
 *     posted work of that Tick. At most kMaxFrameWork.
 *   - PostFrameWork(fn, ctx) queues one unit. It runs on a later Tick, once, in the order it was posted.
 *     A Tick takes at most kPerTickLimit posted units; the rest wait for the next Tick.
 *   - Tick() never blocks. It try-locks the drain and the queue; when either is held it counts a skip and
 *     returns, and the work stays queued. A unit posted while a Tick runs waits for the next Tick.
 *   - A unit reports failure by returning false. The dispatcher counts it, logs the first failure of each
 *     heartbeat window and goes on with the next unit. Units are noexcept: nothing may unwind out of a
 *     Tick (the sentinel is built with -fno-exceptions).
 *   - Units must not call Tick(): the drain lock is held while they run.
 *   - Heartbeat: once per kHeartbeatNs (a minute) the dispatcher logs one INFO line with the Tick calls in the
 *     window (frames) and what happened to the work. The sentinel has no Debug level (quest_config.h LogLevel).
 *
 * Built with -fno-exceptions and called from a hook frame, so it contains no throw path and no allocation.
 */
#pragma once

#include <pthread.h>

#include <cstddef>
#include <cstdint>

namespace nevr_quest::invite::frame {

using WorkFn = bool (*)(void* context) noexcept;

inline constexpr std::size_t kQueueCapacity = 256;
inline constexpr std::size_t kPerTickLimit = 64;
inline constexpr std::size_t kMaxFrameWork = 8;
inline constexpr std::uint64_t kHeartbeatNs = 60ULL * 1000ULL * 1000ULL * 1000ULL;

// Adds a function that runs on every Tick. Any thread. False when kMaxFrameWork are registered or fn is null.
bool RegisterFrameWork(WorkFn fn, void* context) noexcept;

// Queues one unit for a later Tick. Any thread. False when fn is null or the queue is full (counted as a drop).
bool PostFrameWork(WorkFn fn, void* context) noexcept;

// Runs the registered work and up to kPerTickLimit posted units. Called once per NetGame::Update, before the original.
void Tick() noexcept;

// The netgame the frame hook passes before each Tick (#966 S4): the join reads the game object through it. SetNetGame is
// called on the game thread; NetGame returns the last one set, null before the first.
void SetNetGame(void* netGame) noexcept;
void* NetGame() noexcept;
// True when the calling thread is the one that last ran Tick, i.e. the game thread. False before the first Tick.
bool OnTickThread() noexcept;

// One line for the install outcome: installed (`reason` ignored), or off with `reason`.
void LogInstall(bool installed, const char* reason) noexcept;

struct Stats {
  std::uint64_t frames;   // Tick calls, including skipped ones
  std::uint64_t ran;      // units that returned true
  std::uint64_t failed;   // units that returned false
  std::uint64_t dropped;  // posts refused because the queue was full
  std::uint64_t skipped;  // Tick calls that found the drain or the queue held
};
Stats CurrentStats() noexcept;

// Test support. ResetForTest clears the queue, the registrations, the counters and the heartbeat window; call it
// only while no Tick runs. SetClockForTest replaces the monotonic clock (nanoseconds); null restores it.
void ResetForTest() noexcept;
void SetClockForTest(std::uint64_t (*clock)() noexcept) noexcept;
// The lock a Tick takes first. A test holds it from another thread to show that Tick does not block on it.
pthread_mutex_t& DrainMutexForTest() noexcept;
// The lock a Tick takes second, for the queue. A test holds it from another thread the same way.
pthread_mutex_t& QueueMutexForTest() noexcept;

}  // namespace nevr_quest::invite::frame
