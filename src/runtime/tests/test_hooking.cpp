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
