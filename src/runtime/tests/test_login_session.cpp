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
  LoginSession::Set(GUID{});
  EXPECT_EQ(LoginSession::Get().Data1, 0U);
  const GUID id = MakeGuid(0x5A);
  LoginSession::Set(id);
  const GUID read = LoginSession::Get();
  EXPECT_EQ(std::memcmp(&read, &id, sizeof(GUID)), 0);
  LoginSession::Set(GUID{});
}

// A reader on another thread must wait for the lock rather than copy 16 bytes a writer may be
// halfway through replacing. Holding the lock here makes that deterministic: Get and Set from another
// thread cannot finish until it is released.
TEST(LoginSession, AccessFromAnotherThreadWaitsForTheLock) {
  std::atomic<int> finished{0};
  std::unique_lock<std::mutex> held(LoginSession::Mutex());
  std::thread reader([&] {
    LoginSession::Get();
    ++finished;
  });
  std::thread writer([&] {
    LoginSession::Set(MakeGuid(0x33));
    ++finished;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(finished.load(), 0) << "Get/Set ran while the lock was held";
  held.unlock();
  reader.join();
  writer.join();
  EXPECT_EQ(finished.load(), 2);
  LoginSession::Set(GUID{});
}

}  // namespace
