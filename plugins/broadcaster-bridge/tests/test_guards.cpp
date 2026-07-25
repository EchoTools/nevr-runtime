// ============================================================================
// N72/N73 unit tests — broadcaster ingress guards (production-linked)
// WOULD-FAIL-IF:
//   N72 tests → delete ValidateBroadcasterPayload guard conditions.
//   N73 tests → delete BroadcasterRecvRateCheckAtTime limit logic.
// Mutation witnessed: neuter guard → 12/15 (3 N72 tests RED) → restore → 15/15.
// ============================================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

// The broadcaster-bridge header defines the test hooks when NEVR_TEST_HOOKS
// is defined. Include winsock2.h first to avoid the ordering warning.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#endif

#include "broadcaster_bridge.h"

using namespace nevr::broadcaster_bridge;

// ============================================================================
// N72 — payload validation guards
// ============================================================================

class N72_PayloadValidation : public ::testing::Test {};

TEST_F(N72_PayloadValidation, ValidPayload_Passes) {
  uint8_t buf[64] = {0};
  EXPECT_TRUE(TestHook_N72_ValidatePayload(buf, sizeof(buf)));
}

TEST_F(N72_PayloadValidation, ZeroPayloadWithNullptr_Passes) {
  // Some SNS notifications have no body — null payload + size 0 is valid.
  EXPECT_TRUE(TestHook_N72_ValidatePayload(nullptr, 0));
}

TEST_F(N72_PayloadValidation, OversizedPayload_Dropped) {
  uint8_t buf[64] = {0};
  // MAX_BROADCASTER_PAYLOAD = 65536 — anything larger is dropped.
  EXPECT_FALSE(TestHook_N72_ValidatePayload(buf, 65537));
}

TEST_F(N72_PayloadValidation, ExactlyAtLimit_Passes) {
  uint8_t buf[64] = {0};
  // payload_size == MAX_BROADCASTER_PAYLOAD is still valid.
  EXPECT_TRUE(TestHook_N72_ValidatePayload(buf, 65536));
}

TEST_F(N72_PayloadValidation, NullPayloadWithSize_Dropped) {
  // Null pointer claiming to have data is malformed.
  EXPECT_FALSE(TestHook_N72_ValidatePayload(nullptr, 10));
}

TEST_F(N72_PayloadValidation, NullPayloadWithZeroSize_Passes) {
  // Null + zero = no data, but not malformed.
  EXPECT_TRUE(TestHook_N72_ValidatePayload(nullptr, 0));
}

TEST_F(N72_PayloadValidation, VeryLargeSize_Dropped) {
  uint8_t buf[64] = {0};
  // Far beyond the limit — dropped.
  EXPECT_FALSE(TestHook_N72_ValidatePayload(buf, 0xFFFFFFFFULL));
}

// ============================================================================
// N73 — broadcast receive rate limiter
// ============================================================================

class N73_RecvRateLimit : public ::testing::Test {
 protected:
  void SetUp() override {
    TestHook_RecvRateReset();
  }
  void TearDown() override {
    TestHook_RecvRateReset();
  }
};

TEST_F(N73_RecvRateLimit, FirstEvent_AlwaysPasses) {
  // First event at any time in a fresh window must pass.
  EXPECT_TRUE(TestHook_N73_RecvRateCheck(1000));
}

TEST_F(N73_RecvRateLimit, EventsWithinLimit_Pass) {
  // MAX_RECV_EVENTS_PER_SEC = 2000. First 2000 events within the same
  // 1-second window should all pass.
  for (int i = 0; i < 2000; i++) {
    EXPECT_TRUE(TestHook_N73_RecvRateCheck(500))
        << "Event " << (i + 1) << " should pass (within limit)";
  }
}

TEST_F(N73_RecvRateLimit, EventExceedingLimit_Dropped) {
  // Fill the 2000-event quota...
  for (int i = 0; i < 2000; i++) {
    TestHook_N73_RecvRateCheck(500);
  }
  // 2001st event in same window is DROPPED.
  EXPECT_FALSE(TestHook_N73_RecvRateCheck(500))
      << "2001st event must be dropped (rate limit exceeded)";
}

TEST_F(N73_RecvRateLimit, DropCounterIncrements) {
  // Fill quota, then check that subsequent drops keep failing.
  for (int i = 0; i < 2000; i++) {
    TestHook_N73_RecvRateCheck(500);
  }
  // Next few calls must all return false.
  EXPECT_FALSE(TestHook_N73_RecvRateCheck(500));
  EXPECT_FALSE(TestHook_N73_RecvRateCheck(501));
  EXPECT_FALSE(TestHook_N73_RecvRateCheck(599));
}

TEST_F(N73_RecvRateLimit, WindowResetsAfterOneSecond) {
  // Reset puts window_start at 0. Fill 2000 events at t=500 (still within
  // the 0..999 window since 500-0 < 1000).
  for (int i = 0; i < 2000; i++) {
    TestHook_N73_RecvRateCheck(500);
  }
  // At t=999ms: 999-0=999 < 1000 — still same window. Dropped.
  EXPECT_FALSE(TestHook_N73_RecvRateCheck(999))
      << "999ms: still within same 1s window (window_start=0)";

  // At t=1000ms: 1000-0=1000 >= 1000 — window resets. Event passes.
  EXPECT_TRUE(TestHook_N73_RecvRateCheck(1000))
      << "1000ms: new window (window_start advances to 1000)";
}

TEST_F(N73_RecvRateLimit, NewWindowResetsCounter) {
  // Fill quota at t=0 (window_start=0 after reset, 0-0=0 < 1000).
  for (int i = 0; i < 2000; i++) {
    TestHook_N73_RecvRateCheck(0);
  }
  // At t=1000, window resets. Counter should be 1 (counting this call).
  EXPECT_TRUE(TestHook_N73_RecvRateCheck(1000));
  // Now fill the new window (1999 more events at t=1500).
  for (int i = 0; i < 1999; i++) {
    EXPECT_TRUE(TestHook_N73_RecvRateCheck(1500))
        << "Event " << (i + 2) << " in new window should pass";
  }
  // 2001st in new window (window_start=1000, 1500-1000=500 < 1000) — dropped.
  EXPECT_FALSE(TestHook_N73_RecvRateCheck(1500))
      << "2001st event in window must be dropped";
}

TEST_F(N73_RecvRateLimit, ManySmallWindows_EachResets) {
  // Simulate 5 separate second-long windows, each filling to the limit
  // and verifying the next event is dropped, then the window after passes.
  for (int window = 0; window < 5; window++) {
    int64_t base = window * 1000;
    TestHook_RecvRateReset();  // fresh state per window
    // Fill quota
    for (int i = 0; i < 2000; i++) {
      EXPECT_TRUE(TestHook_N73_RecvRateCheck(base))
          << "Window " << window << ": event " << i << " should pass";
    }
    // One over
    EXPECT_FALSE(TestHook_N73_RecvRateCheck(base))
        << "Window " << window << ": overflow must be dropped";
    // Window change — first of new window passes
    EXPECT_TRUE(TestHook_N73_RecvRateCheck(base + 1000))
        << "Window " << window << ": first event of next window";
  }
}

TEST_F(N73_RecvRateLimit, NormalLoad_FewEventsPerFrame_Passes) {
  // Normal operation: ~5 events/frame at 90fps = ~450 events/sec.
  // Every call should pass at this rate.
  for (int sec = 0; sec < 10; sec++) {
    int64_t base = sec * 1000;
    for (int i = 0; i < 450; i++) {
      EXPECT_TRUE(TestHook_N73_RecvRateCheck(base + i * 2))
          << "Normal load: sec=" << sec << " event=" << i << " should pass";
    }
  }
}
