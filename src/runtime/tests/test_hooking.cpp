#include <gtest/gtest.h>

#include <vector>

#include "core/hooking.h"

TEST(Hooking, PublishesTrampolineBeforeEnable) {
  int targetValue = 0;
  int trampolineValue = 0;
  int detourValue = 0;
  PVOID original = &targetValue;
  const PVOID target = original;
  const PVOID trampoline = &trampolineValue;
  const PVOID detour = &detourValue;
  std::vector<const char*> calls;

  const BOOL ok = Hooking::AttachMinHookWith(
      &original, detour,
      [&](PVOID actualTarget, PVOID actualDetour, PVOID* out) {
        calls.push_back("create");
        EXPECT_EQ(actualTarget, target);
        EXPECT_EQ(actualDetour, detour);
        EXPECT_EQ(original, target);
        *out = trampoline;
        return MH_OK;
      },
      [&](PVOID actualTarget) {
        calls.push_back("enable");
        EXPECT_EQ(actualTarget, target);
        EXPECT_EQ(original, trampoline);
        return MH_OK;
      },
      [&](PVOID) {
        calls.push_back("remove");
        return MH_OK;
      });

  EXPECT_TRUE(ok);
  EXPECT_EQ(original, trampoline);
  EXPECT_EQ(calls, (std::vector<const char*>{"create", "enable"}));
  EXPECT_STREQ(Hooking::LastAttachError(), "");
}

TEST(Hooking, EnableFailureRemovesHookAndRestoresOriginal) {
  int targetValue = 0;
  int trampolineValue = 0;
  PVOID original = &targetValue;
  const PVOID target = original;
  const PVOID trampoline = &trampolineValue;
  std::vector<const char*> calls;

  const BOOL ok = Hooking::AttachMinHookWith(
      &original, nullptr,
      [&](PVOID, PVOID, PVOID* out) {
        calls.push_back("create");
        *out = trampoline;
        return MH_OK;
      },
      [&](PVOID) {
        calls.push_back("enable");
        EXPECT_EQ(original, trampoline);
        return MH_ERROR_MEMORY_PROTECT;
      },
      [&](PVOID actualTarget) {
        calls.push_back("remove");
        EXPECT_EQ(actualTarget, target);
        EXPECT_EQ(original, trampoline);
        return MH_OK;
      });

  EXPECT_FALSE(ok);
  EXPECT_EQ(original, target);
  EXPECT_EQ(calls, (std::vector<const char*>{"create", "enable", "remove"}));
  EXPECT_STREQ(Hooking::LastAttachError(), MH_StatusToString(MH_ERROR_MEMORY_PROTECT));
}

TEST(Hooking, CreateFailureLeavesOriginalAndNeverEnables) {
  int targetValue = 0;
  PVOID original = &targetValue;
  const PVOID target = original;
  int enables = 0;
  int removals = 0;
  const BOOL ok = Hooking::AttachMinHookWith(
      &original, nullptr,
      [](PVOID, PVOID, PVOID*) { return MH_ERROR_UNSUPPORTED_FUNCTION; },
      [&](PVOID) {
        ++enables;
        return MH_OK;
      },
      [&](PVOID) {
        ++removals;
        return MH_OK;
      });

  EXPECT_FALSE(ok);
  EXPECT_EQ(original, target);
  EXPECT_EQ(enables, 0);
  EXPECT_EQ(removals, 0);
  EXPECT_STREQ(Hooking::LastAttachError(), MH_StatusToString(MH_ERROR_UNSUPPORTED_FUNCTION));
}
