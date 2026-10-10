#pragma once
// Runs the device-code flow off the bootstrap thread while that thread keeps its window responsive
// (#37, beta gate G7). The bootstrap thread still waits: the game must not reach its login before the
// token exists (the bridge reads it when the game connects). What changes is that the waiting thread
// keeps pumping, so Windows does not mark the game window "Not Responding".

#include <chrono>
#include <functional>

namespace nevr_token_auth {

struct OffThreadWaitResult {
  bool flowResult = false;        // what the flow returned (false when it threw)
  bool flowThrew = false;         // the flow threw a std::exception
  bool ranOnOtherThread = false;  // the flow ran on a thread other than the caller's
  bool ranInline = false;         // no worker thread could be started; the flow ran on the caller
  unsigned pumpCalls = 0;         // how many times the caller pumped while waiting
};

// Runs `flow` on a new thread; until it returns, calls `pump` on the calling thread every `interval`.
// If the worker thread cannot be started the flow runs inline (ranInline) rather than not at all.
OffThreadWaitResult RunWhilePumping(const std::function<bool()>& flow, const std::function<void()>& pump,
                                    std::chrono::milliseconds interval);

}  // namespace nevr_token_auth
