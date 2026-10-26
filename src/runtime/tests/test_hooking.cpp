#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "core/hooking.h"

TEST(Hooking, LastAttachErrorBelongsToTheCallingThread) {
  Hooking::LastAttachErrorRef() = "main failure";
  std::thread worker([] {
    Hooking::LastAttachErrorRef() = "worker failure";
    EXPECT_STREQ(Hooking::LastAttachError(), "worker failure");
  });
  worker.join();
  EXPECT_STREQ(Hooking::LastAttachError(), "main failure");
  Hooking::LastAttachErrorRef() = "";
}

// #800: the host (runtime/host) initializes MinHook at the wrapper rendezvous before the runtime's
// own initialization runs, in the same process and the same MinHook state, so the runtime must treat
// an already-initialized library as ready and fail on every other status.
TEST(Hooking, InitializeAcceptsAMinHookTheHostAlreadyInitialized) {
  EXPECT_TRUE(Hooking::InitializeMinHookWith([] { return MH_OK; }));
  EXPECT_TRUE(Hooking::InitializeMinHookWith([] { return MH_ERROR_ALREADY_INITIALIZED; }));
}

TEST(Hooking, InitializeStillFailsOnAnyOtherMinHookStatus) {
  EXPECT_FALSE(Hooking::InitializeMinHookWith([] { return MH_ERROR_MEMORY_ALLOC; }));
  EXPECT_FALSE(Hooking::InitializeMinHookWith([] { return MH_UNKNOWN; }));
  EXPECT_FALSE(Hooking::InitializeMinHookWith([] { return MH_ERROR_NOT_INITIALIZED; }));
}

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

// ---- #954: MinHook relocates a RIP-relative load at the start of a function ----
//
// The game's ovr_GetPerfStats wrapper (echovr.exe 0x1413662e0) starts with a seven-byte
// MOV RAX,[RIP+disp32]; MinHook has to copy that instruction into its trampoline and rewrite the
// displacement. This is the same 21 bytes in a page of its own, with the pointer the MOV loads in the same
// page: if the displacement were copied unchanged, the trampoline would read the wrong address.
namespace {

using PerfThunk = int32_t(__fastcall*)(void* session, void* out);
PerfThunk g_perfTrampoline = nullptr;
int g_perfDetourCalls = 0;

int32_t __fastcall RealPerfStats(void*, void* out) {
  *static_cast<int32_t*>(out) = 42;
  return 7;
}

int32_t __fastcall PerfStatsDetour(void* session, void* out) {
  ++g_perfDetourCalls;
  return g_perfTrampoline(session, out) + 100;
}

}  // namespace

TEST(Hooking, ARipRelativeLoadAtTheStartOfAFunctionRunsThroughTheTrampoline) {
  ASSERT_TRUE(Hooking::Initialize());
  auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  ASSERT_NE(nullptr, page);
  constexpr uint32_t kPointerOffset = 0x100;
  const uint8_t thunk[21] = {0x48, 0x8B, 0x05, 0xF9, 0x00, 0x00, 0x00,  // MOV RAX,[RIP+0xf9] -> page+0x100
                             0x48, 0x85, 0xC0,                          // TEST RAX,RAX
                             0x75, 0x06,                                // JNZ +6
                             0xB8, 0x14, 0xFC, 0xFF, 0xFF,              // MOV EAX,0xfffffc14
                             0xC3,                                      // RET
                             0x48, 0xFF, 0xE0};                         // JMP RAX
  ASSERT_EQ(kPointerOffset, 7u + 0xF9u);
  std::memcpy(page, thunk, sizeof(thunk));
  auto* pointer = reinterpret_cast<uintptr_t*>(page + kPointerOffset);
  *pointer = reinterpret_cast<uintptr_t>(&RealPerfStats);

  PerfThunk call = reinterpret_cast<PerfThunk>(page);
  int32_t out = 0;
  ASSERT_EQ(7, call(nullptr, &out));  // unhooked: the thunk jumps to the real function
  ASSERT_EQ(42, out);

  PVOID original = page;
  ASSERT_TRUE(Hooking::Attach(&original, reinterpret_cast<PVOID>(&PerfStatsDetour))) << Hooking::LastAttachError();
  g_perfTrampoline = reinterpret_cast<PerfThunk>(original);
  EXPECT_NE(static_cast<PVOID>(page), original);

  out = 0;
  g_perfDetourCalls = 0;
  EXPECT_EQ(107, call(nullptr, &out));  // detour -> trampoline -> the relocated MOV loads the pointer -> real
  EXPECT_EQ(42, out);
  EXPECT_EQ(1, g_perfDetourCalls);

  *pointer = 0;  // no LibOVR export: the thunk's own error path, still through the relocated MOV
  EXPECT_EQ(-1004 + 100, call(nullptr, &out));
  EXPECT_EQ(2, g_perfDetourCalls);

  EXPECT_TRUE(Hooking::Detach(&original, reinterpret_cast<PVOID>(&PerfStatsDetour)));
  VirtualFree(page, 0, MEM_RELEASE);
}
