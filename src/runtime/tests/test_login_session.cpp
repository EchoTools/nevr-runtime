// #49: the login session GUID is written on the game thread and read on ixwebsocket's thread.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <cstring>
#include <thread>

#include "core/login_session.h"

namespace {

GUID MakeGuid(unsigned char fill) {
  GUID guid = {};
  std::memset(&guid, fill, sizeof(guid));
  return guid;
}

TEST(LoginSession, StartsEmptyAndReturnsWhatWasSet) {
  nevr_login_session::Set(GUID{});
  EXPECT_EQ(nevr_login_session::Get().Data1, 0U);
  const GUID id = MakeGuid(0x5A);
  nevr_login_session::Set(id);
  const GUID read = nevr_login_session::Get();
  EXPECT_EQ(std::memcmp(&read, &id, sizeof(GUID)), 0);
  nevr_login_session::Set(GUID{});
}

// A reader on another thread must wait for the lock rather than copy 16 bytes a writer may be
// halfway through replacing. Holding the lock here makes that deterministic: Get and Set from another
// thread cannot finish until it is released.
TEST(LoginSession, AccessFromAnotherThreadWaitsForTheLock) {
  std::atomic<int> finished{0};
  std::unique_lock<std::mutex> held(nevr_login_session::Mutex());
  std::thread reader([&] {
    nevr_login_session::Get();
    ++finished;
  });
  std::thread writer([&] {
    nevr_login_session::Set(MakeGuid(0x33));
    ++finished;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(finished.load(), 0) << "Get/Set ran while the lock was held";
  held.unlock();
  reader.join();
  writer.join();
  EXPECT_EQ(finished.load(), 2);
  nevr_login_session::Set(GUID{});
}

}  // namespace
