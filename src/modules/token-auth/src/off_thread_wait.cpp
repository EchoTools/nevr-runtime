#include "off_thread_wait.h"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <system_error>
#include <thread>

namespace nevr_token_auth {
namespace {

// Runs the flow, turning a thrown std::exception into a false result.
void RunFlow(const std::function<bool()>& flow, bool& result, bool& threw) {
  try {
    result = flow ? flow() : false;
  } catch (const std::exception&) {
    result = false;
    threw = true;
  }
}

}  // namespace

OffThreadWaitResult RunWhilePumping(const std::function<bool()>& flow, const std::function<void()>& pump,
                                    std::chrono::milliseconds interval) {
  OffThreadWaitResult r;
  std::mutex mutex;
  std::condition_variable condition;
  bool done = false;
  bool result = false;
  bool threw = false;
  bool otherThread = false;
  const std::thread::id caller = std::this_thread::get_id();

  std::thread worker;
  try {
    worker = std::thread([&] {
      bool localResult = false;
      bool localThrew = false;
      const bool isOther = std::this_thread::get_id() != caller;
      RunFlow(flow, localResult, localThrew);
      std::lock_guard<std::mutex> lock(mutex);
      result = localResult;
      threw = localThrew;
      otherThread = isOther;
      done = true;
      condition.notify_all();
    });
  } catch (const std::system_error&) {
    r.ranInline = true;
    RunFlow(flow, r.flowResult, r.flowThrew);
    return r;
  }

  unsigned pumps = 0;
  {
    std::unique_lock<std::mutex> lock(mutex);
    while (!condition.wait_for(lock, interval, [&] { return done; })) {
      lock.unlock();
      if (pump) pump();
      ++pumps;
      lock.lock();
    }
  }
  worker.join();
  r.flowResult = result;
  r.flowThrew = threw;
  r.ranOnOtherThread = otherThread;
  r.pumpCalls = pumps;
  return r;
}

}  // namespace nevr_token_auth
