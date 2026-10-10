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

// ---- Detach disables the one hook it is asked about (#357) ----

namespace {
// Attach through the injected backend so the trampoline -> target record is made exactly as
// Hooking::Attach makes it.
void FakeAttach(PVOID* original, PVOID trampoline) {
  const BOOL ok = Hooking::AttachMinHookWith(
      original, nullptr,
      [&](PVOID, PVOID, PVOID* out) {
        *out = trampoline;
        return MH_OK;
      },
      [](PVOID) { return MH_OK; }, [](PVOID) { return MH_OK; });
  EXPECT_TRUE(ok);
}
}  // namespace

TEST(HookingDetach, DisablesOnlyTheTargetOfTheDetachedHook) {
  int targetA = 0, targetB = 0, trampA = 0, trampB = 0;
  PVOID origA = &targetA;
  PVOID origB = &targetB;
  FakeAttach(&origA, &trampA);
  FakeAttach(&origB, &trampB);

  std::vector<PVOID> disabled;
  EXPECT_TRUE(Hooking::DetachMinHookWith(&origA, [&](PVOID target) {
    disabled.push_back(target);
    return MH_OK;
  }));
  EXPECT_EQ(disabled, (std::vector<PVOID>{&targetA}));

  // B is still tracked: detaching it disables B's target and nothing else.
  disabled.clear();
  EXPECT_TRUE(Hooking::DetachMinHookWith(&origB, [&](PVOID target) {
    disabled.push_back(target);
    return MH_OK;
  }));
  EXPECT_EQ(disabled, (std::vector<PVOID>{&targetB}));
}

TEST(HookingDetach, NeverDisablesAllHooks) {
  int target = 0, tramp = 0;
  PVOID orig = &target;
  FakeAttach(&orig, &tramp);
  PVOID seen = &target;
  EXPECT_TRUE(Hooking::DetachMinHookWith(&orig, [&](PVOID t) {
    seen = t;
    return MH_OK;
  }));
  EXPECT_NE(seen, static_cast<PVOID>(MH_ALL_HOOKS));
  EXPECT_EQ(seen, &target);
}

TEST(HookingDetach, UnknownTrampolineIsRefusedWithoutTouchingMinHook) {
  int stranger = 0;
  PVOID orig = &stranger;
  int disables = 0;
  EXPECT_FALSE(Hooking::DetachMinHookWith(&orig, [&](PVOID) {
    ++disables;
    return MH_OK;
  }));
  EXPECT_EQ(disables, 0);
}

TEST(HookingDetach, FailedDisableKeepsTheHookTracked) {
  int target = 0, tramp = 0;
  PVOID orig = &target;
  FakeAttach(&orig, &tramp);
  EXPECT_FALSE(Hooking::DetachMinHookWith(&orig, [](PVOID) { return MH_ERROR_NOT_EXECUTABLE; }));
  PVOID seen = nullptr;
  EXPECT_TRUE(Hooking::DetachMinHookWith(&orig, [&](PVOID t) {
    seen = t;
    return MH_OK;
  }));
  EXPECT_EQ(seen, &target);
}

namespace {
using IntFn = int (*)(int);
// noinline + volatile traffic so each body is long enough for MinHook's 5-byte jump patch.
__attribute__((noinline)) int RealTargetA(int x) {
  volatile int a = x;
  a = a + 1;
  a = a * 3;
  a = a - 2;
  return a;
}
__attribute__((noinline)) int RealTargetB(int x) {
  volatile int b = x;
  b = b + 10;
  b = b * 5;
  b = b - 7;
  return b;
}
IntFn g_origA = nullptr;
IntFn g_origB = nullptr;
int g_detourACalls = 0;
int g_detourBCalls = 0;
int DetourA(int x) {
  ++g_detourACalls;
  return g_origA(x);
}
int DetourB(int x) {
  ++g_detourBCalls;
  return g_origB(x);
}
}  // namespace

TEST(HookingDetach, RealMinHookDetachingOneLeavesTheOtherFiring) {
  const MH_STATUS init = MH_Initialize();
  ASSERT_TRUE(init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED) << MH_StatusToString(init);

  const int expectA = RealTargetA(4);
  const int expectB = RealTargetB(4);
  g_origA = &RealTargetA;
  g_origB = &RealTargetB;
  g_detourACalls = g_detourBCalls = 0;

  ASSERT_TRUE(Hooking::Attach(reinterpret_cast<PVOID*>(&g_origA), reinterpret_cast<PVOID>(&DetourA)))
      << Hooking::LastAttachError();
  ASSERT_TRUE(Hooking::Attach(reinterpret_cast<PVOID*>(&g_origB), reinterpret_cast<PVOID>(&DetourB)))
      << Hooking::LastAttachError();

  EXPECT_EQ(RealTargetA(4), expectA);
  EXPECT_EQ(RealTargetB(4), expectB);
  EXPECT_EQ(g_detourACalls, 1);
  EXPECT_EQ(g_detourBCalls, 1);

  EXPECT_TRUE(Hooking::Detach(reinterpret_cast<PVOID*>(&g_origA), reinterpret_cast<PVOID>(&DetourA)));

  EXPECT_EQ(RealTargetA(4), expectA);
  EXPECT_EQ(g_detourACalls, 1) << "detached hook A still fires";
  EXPECT_EQ(RealTargetB(4), expectB);
  EXPECT_EQ(g_detourBCalls, 2) << "hook B stopped firing when A was detached";

  EXPECT_TRUE(Hooking::Detach(reinterpret_cast<PVOID*>(&g_origB), reinterpret_cast<PVOID>(&DetourB)));
  EXPECT_EQ(RealTargetB(4), expectB);
  EXPECT_EQ(g_detourBCalls, 2);

  MH_RemoveHook(reinterpret_cast<PVOID>(&RealTargetA));
  MH_RemoveHook(reinterpret_cast<PVOID>(&RealTargetB));
}

// ---- core/hook_lifecycle.h: the ordering contract shared with the Quest backend ----

TEST(HookLifecycle, AttachPublishedPublishesBeforeEnableAndKeepsTrampoline) {
  int target = 0, trampoline = 0;
  void* original = &target;
  std::vector<const char*> calls;
  const nevr::hook::AttachStage stage = nevr::hook::AttachPublished(
      &original,
      [&](void** out) {
        calls.push_back("create");
        *out = &trampoline;
        return true;
      },
      [&] {
        calls.push_back("enable");
        EXPECT_EQ(original, &trampoline);
        return true;
      },
      [&] { calls.push_back("remove"); });
  EXPECT_EQ(stage, nevr::hook::AttachStage::kAttached);
  EXPECT_EQ(original, &trampoline);
  EXPECT_EQ(calls, (std::vector<const char*>{"create", "enable"}));
}

TEST(HookLifecycle, EnableFailureRemovesAndRestoresThePriorPointer) {
  int target = 0, trampoline = 0;
  void* original = &target;
  std::vector<const char*> calls;
  const nevr::hook::AttachStage stage = nevr::hook::AttachPublished(
      &original,
      [&](void** out) {
        calls.push_back("create");
        *out = &trampoline;
        return true;
      },
      [&] {
        calls.push_back("enable");
        return false;
      },
      [&] { calls.push_back("remove"); });
  EXPECT_EQ(stage, nevr::hook::AttachStage::kEnableFailed);
  EXPECT_EQ(original, &target);
  EXPECT_EQ(calls, (std::vector<const char*>{"create", "enable", "remove"}));
}

TEST(HookLifecycle, CreateFailureNeverEnablesOrRemoves) {
  int target = 0;
  void* original = &target;
  int enables = 0, removes = 0;
  const nevr::hook::AttachStage stage = nevr::hook::AttachPublished(
      &original, [](void**) { return false; },
      [&] {
        ++enables;
        return true;
      },
      [&] { ++removes; });
  EXPECT_EQ(stage, nevr::hook::AttachStage::kCreateFailed);
  EXPECT_EQ(original, &target);
  EXPECT_EQ(enables, 0);
  EXPECT_EQ(removes, 0);
}

TEST(HookLifecycle, NullTrampolineIsRemovedAndNeverPublished) {
  int target = 0;
  void* original = &target;
  int enables = 0, removes = 0;
  const nevr::hook::AttachStage stage = nevr::hook::AttachPublished(
      &original,
      [](void** out) {
        *out = nullptr;
        return true;
      },
      [&] {
        ++enables;
        return true;
      },
      [&] { ++removes; });
  EXPECT_EQ(stage, nevr::hook::AttachStage::kNullTrampoline);
  EXPECT_EQ(original, &target);
  EXPECT_EQ(enables, 0);
  EXPECT_EQ(removes, 1);
}

TEST(HookLifecycle, StageNamesAreDistinctLogTokens) {
  EXPECT_STREQ(nevr::hook::AttachStageName(nevr::hook::AttachStage::kAttached), "attached");
  EXPECT_STREQ(nevr::hook::AttachStageName(nevr::hook::AttachStage::kCreateFailed), "create_failed");
  EXPECT_STREQ(nevr::hook::AttachStageName(nevr::hook::AttachStage::kNullTrampoline), "null_trampoline");
  EXPECT_STREQ(nevr::hook::AttachStageName(nevr::hook::AttachStage::kEnableFailed), "enable_failed");
}

TEST(HookLifecycle, KeepPublishedLeavesTheOriginalAfterAFailedEnable) {
  int target = 0, trampoline = 0;
  void* original = &target;
  int removes = 0;
  const nevr::hook::AttachStage stage = nevr::hook::AttachPublished(
      &original,
      [&](void** out) {
        *out = &trampoline;
        return true;
      },
      [] { return false; }, [&] { ++removes; }, [] { return true; });
  EXPECT_EQ(stage, nevr::hook::AttachStage::kEnableFailed);
  EXPECT_EQ(original, &trampoline);
  EXPECT_EQ(removes, 1);
}

TEST(HookLifecycle, KeepPublishedIsNotAskedWhenCreateFails) {
  int target = 0;
  void* original = &target;
  bool asked = false;
  const nevr::hook::AttachStage stage = nevr::hook::AttachPublished(
      &original, [](void**) { return false; }, [] { return true; }, [] {},
      [&] {
        asked = true;
        return true;
      });
  EXPECT_EQ(stage, nevr::hook::AttachStage::kCreateFailed);
  EXPECT_EQ(original, &target);
  EXPECT_FALSE(asked);
}
