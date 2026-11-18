// The Detours and __try shims every EchoCombat port runs through (runtime/combat/cat_compat.cpp): the transaction a
// feature group installs its detours in (all of them or none, newest rolled back first), the fault handler behind
// EC_TRY / EC_EXCEPT, and the settings reads that fall back from config.yaml to his shipped values to the code's literal.
//
// cat_compat.cpp is the production code, with the real HookGuard, write gate, PatchDetour and MinHook. The targets are
// hand-assembled functions on a page the test allocates; the game is not involved. config.yaml is stood in for by
// NevrCfgCombatValue below.
//
// NOT shown here: the same transactions against echovr.exe's own functions in a live process.

#include <winsock2.h>
#include <windows.h>

#include <gtest/gtest.h>

#include <MinHook.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/hooking.h"
#include "core/logging.h"
#include "runtime/combat/cat_compat.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/lifecycle/service_config.h"

namespace {

// config.yaml's `combat` section, as the test sets it. Stands in for service_config.cpp's reader.
std::map<std::string, std::string> g_combatConfig;

}  // namespace

const char* NevrCfgCombatValue(const char* path) {
  const auto it = g_combatConfig.find(path);
  return it == g_combatConfig.end() ? nullptr : it->second.c_str();
}

namespace {

namespace cat = nevr_combat::cat;

constexpr std::size_t kPageBytes = 0x2000;
constexpr std::size_t kStubStride = 32;  // beyond the 16 bytes HookGuard records for each hook
constexpr int kStubCount = 128;
constexpr uint32_t kOriginalBase = 1000;
constexpr uint32_t kDetourResult = 7777;
constexpr int kMaxPendingHooks = 32;  // kMaxPending in cat_compat.cpp
constexpr int kMaxHookTable = 96;     // kMaxHooks in cat_compat.cpp
constexpr uint8_t kMovEaxImm32 = 0xB8;
constexpr uint8_t kRet = 0xC3;
constexpr uint8_t kInt3 = 0xCC;
constexpr std::size_t kNoAccessPageBytes = 0x1000;  // one page reserved PAGE_NOACCESS to fault on
constexpr DWORD kTestCode = 0xE0000042;  // a C++-style exception code, not on Cat's list
constexpr std::size_t kLoggedLineCount = 5;  // lines HostLog writes in the levels test
constexpr std::size_t kWarningLineIndex = 2;
constexpr std::size_t kErrorLineIndex = 3;
constexpr std::size_t kNullModuleLineIndex = 4;
constexpr int kConfiguredValue = 42;  // the leading digits of "42abc"
constexpr int kCodeFallback = 9;
constexpr int kShippedSelfDamage = 5;  // OVERCHARGE self damage in his PowerBoost.ini
constexpr int kCodeSelfDamage = 2;     // the code's literal for it
constexpr int kNullArgFallback = 3;
constexpr std::size_t kOutBufferBytes = 16;
constexpr std::size_t kSmallBufferBytes = 4;
constexpr std::size_t kHelloLength = 5;  // strlen("hello")
constexpr std::size_t kTruncatedLength = 3;  // kSmallBufferBytes minus the NUL
constexpr std::size_t kFallbackLength = 8;  // strlen("fallback")
constexpr int kNeverHookedStub = 2;  // a stub index no test attaches in that test
constexpr int kDetachedStub = 3;     // the stub the detach test hooks then removes
constexpr int kMismatchStub = 4;     // the stub the detach-mismatch test hooks
constexpr int kUnhookedPointerStub = 5;  // a stub whose pointer was never hooked

uint32_t Detour() { return kDetourResult; }

std::vector<std::pair<EchoVR::LogLevel, std::string>> g_logged;
void CaptureLog(EchoVR::LogLevel level, const CHAR* message) { g_logged.emplace_back(level, message); }

int Count(const std::string& needle) {
  int n = 0;
  for (const auto& line : g_logged) n += line.second.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

int IndexOf(const std::string& needle) {
  for (size_t i = 0; i < g_logged.size(); ++i) {
    if (g_logged[i].second.find(needle) != std::string::npos) return static_cast<int>(i);
  }
  return -1;
}

using StubFn = uint32_t (*)();

// Runs before any test installs the shim's handler (gtest runs tests in definition order, and nothing above this calls
// InstallFaultHandler): a handler added earlier, as the runtime's crash recovery is, must still see a fault second.
DWORD g_earlierSaw = 0;
LONG CALLBACK EarlierHandler(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO) ++g_earlierSaw;
  return EXCEPTION_CONTINUE_SEARCH;
}

TEST(CatFaultOrder, TheShimsHandlerRunsBeforeOneAddedEarlier) {
  PVOID earlier = AddVectoredExceptionHandler(1, &EarlierHandler);
  ASSERT_NE(earlier, nullptr);
  cat::InstallFaultHandler();
  volatile bool excepted = false;
  EC_TRY { RaiseException(EXCEPTION_INT_DIVIDE_BY_ZERO, 0, 0, nullptr); }
  EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { excepted = true; }
  EXPECT_TRUE(excepted);
  EXPECT_EQ(g_earlierSaw, 0u);
  RemoveVectoredExceptionHandler(earlier);
}

class CatCompat : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    g_logged.clear();
    g_combatConfig.clear();
    SetBootLogSink(&CaptureLog);
    page_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kPageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(page_, nullptr) << "VirtualAlloc error=" << GetLastError();
    std::memset(page_, kInt3, kPageBytes);
    // Stub i: `mov eax, kOriginalBase + i; ret`. Five bytes, the least MinHook can patch; no callee-saved register.
    for (int i = 0; i < kStubCount; ++i) {
      uint8_t* at = page_ + static_cast<size_t>(i) * kStubStride;
      const uint32_t result = kOriginalBase + static_cast<uint32_t>(i);
      at[0] = kMovEaxImm32;
      std::memcpy(at + 1, &result, sizeof(result));
      at[1 + sizeof(result)] = kRet;
      real_[i] = at;
    }
    // The same stub on a page that is not executable: MinHook refuses to hook it.
    dataPage_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kPageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    ASSERT_NE(dataPage_, nullptr) << "VirtualAlloc error=" << GetLastError();
    std::memcpy(dataPage_, page_, kStubStride);
    dataReal_ = dataPage_;
    cat::TransactionAbort();  // a transaction left open on this thread by an earlier test
    g_logged.clear();
  }

  void TearDown() override {
    cat::TransactionAbort();
    for (int i = 0; i < kStubCount; ++i) cat::Detach(&real_[i], reinterpret_cast<PVOID>(&Detour));
    nevr_hook_guard::ResetForTest();
    SetBootLogSink(nullptr);
    VirtualFree(page_, 0, MEM_RELEASE);
    VirtualFree(dataPage_, 0, MEM_RELEASE);
  }

  uint8_t* Target(int i) const { return page_ + static_cast<size_t>(i) * kStubStride; }
  uint32_t CallTarget(int i) const { return reinterpret_cast<StubFn>(Target(i))(); }
  uint32_t CallReal(int i) const { return reinterpret_cast<StubFn>(real_[i])(); }
  bool IsHooked(int i) const { return CallTarget(i) == kDetourResult; }
  LONG Attach(int i, const char* name) {
    return cat::Attach(&real_[i], reinterpret_cast<PVOID>(&Detour), name, "test");
  }

  LONG AttachData(const char* name) { return cat::Attach(&dataReal_, reinterpret_cast<PVOID>(&Detour), name, "test"); }

  uint8_t* page_ = nullptr;
  uint8_t* dataPage_ = nullptr;
  PVOID dataReal_ = nullptr;
  PVOID real_[kStubCount] = {};
};

// ---- the transaction ----

TEST_F(CatCompat, ACommittedTransactionHooksEveryTargetItAttached) {
  EXPECT_EQ(cat::TransactionBegin(), NO_ERROR);
  EXPECT_EQ(Attach(0, "first"), NO_ERROR);
  EXPECT_EQ(Attach(1, "second"), NO_ERROR);
  EXPECT_EQ(cat::TransactionCommit(), NO_ERROR);

  EXPECT_TRUE(IsHooked(0));
  EXPECT_TRUE(IsHooked(1));
  EXPECT_FALSE(IsHooked(kNeverHookedStub));
  // The caller's pointer is the trampoline now: it runs the original.
  EXPECT_NE(real_[0], Target(0));
  EXPECT_EQ(CallReal(0), kOriginalBase);
  EXPECT_EQ(CallReal(1), kOriginalBase + 1);
  EXPECT_EQ(Count("hook applied module=test name=first"), 1);
  EXPECT_EQ(Count("hook applied module=test name=second"), 1);
  EXPECT_EQ(Count("rolled back"), 0);
}

TEST_F(CatCompat, AnAttachThatFailsFailsTheCommitAndRollsBackEarlierHooksNewestFirst) {
  cat::TransactionBegin();
  ASSERT_EQ(Attach(0, "first"), NO_ERROR);
  ASSERT_EQ(Attach(1, "second"), NO_ERROR);
  PVOID none = nullptr;
  EXPECT_EQ(cat::Attach(&none, reinterpret_cast<PVOID>(&Detour), "third", "test"), ERROR_INVALID_PARAMETER);
  EXPECT_EQ(cat::TransactionCommit(), ERROR_INVALID_PARAMETER);

  // Nothing of the transaction is left: both targets run their own code, and the caller's pointers are the targets again.
  EXPECT_FALSE(IsHooked(0));
  EXPECT_FALSE(IsHooked(1));
  EXPECT_EQ(real_[0], Target(0));
  EXPECT_EQ(real_[1], Target(1));
  EXPECT_EQ(CallTarget(0), kOriginalBase);
  const int second = IndexOf("hook rolled back name=second");
  const int first = IndexOf("hook rolled back name=first");
  ASSERT_GE(second, 0);
  ASSERT_GE(first, 0);
  EXPECT_LT(second, first);
  // The rolled-back hooks are not guarded any more.
  EXPECT_EQ(nevr_hook_guard::RecordedCount(), 0);
}

TEST_F(CatCompat, ARefusedWriteIsAccessDeniedAndRollsBackTheTransaction) {
  nevr_hook_guard::Record(Target(1), "OurHook");
  cat::TransactionBegin();
  ASSERT_EQ(Attach(0, "first"), NO_ERROR);
  EXPECT_EQ(Attach(1, "second"), ERROR_ACCESS_DENIED);
  EXPECT_EQ(cat::TransactionCommit(), ERROR_ACCESS_DENIED);
  EXPECT_FALSE(IsHooked(0));
  EXPECT_FALSE(IsHooked(1));
  EXPECT_EQ(Count("hook refused module=test name=second"), 1);
  EXPECT_EQ(Count("reason=overlap"), 1);
}

TEST_F(CatCompat, ATargetThatCannotBeHookedIsInvalidOperationAndLeavesNoRecord) {
  cat::TransactionBegin();
  EXPECT_EQ(AttachData("data"), ERROR_INVALID_OPERATION);
  EXPECT_EQ(cat::TransactionCommit(), ERROR_INVALID_OPERATION);
  EXPECT_EQ(nevr_hook_guard::RecordedCount(), 0);
  EXPECT_EQ(Count("hook refused module=test name=data"), 1);
}

TEST_F(CatCompat, ANullPointerOrDetourIsInvalidParameter) {
  cat::TransactionBegin();
  EXPECT_EQ(cat::Attach(nullptr, reinterpret_cast<PVOID>(&Detour), "a", "test"), ERROR_INVALID_PARAMETER);
  PVOID none = nullptr;
  EXPECT_EQ(cat::Attach(&none, reinterpret_cast<PVOID>(&Detour), "b", "test"), ERROR_INVALID_PARAMETER);
  EXPECT_EQ(cat::Attach(&real_[0], nullptr, "c", "test"), ERROR_INVALID_PARAMETER);
  EXPECT_EQ(cat::TransactionCommit(), ERROR_INVALID_PARAMETER);
  EXPECT_FALSE(IsHooked(0));
}

TEST_F(CatCompat, TheFirstFailureOfATransactionIsTheOneCommitReturns) {
  PVOID none = nullptr;
  cat::TransactionBegin();
  EXPECT_EQ(cat::Attach(&none, reinterpret_cast<PVOID>(&Detour), "invalid", "test"), ERROR_INVALID_PARAMETER);
  nevr_hook_guard::Record(Target(1), "OurHook");
  EXPECT_EQ(Attach(1, "refused"), ERROR_ACCESS_DENIED);
  EXPECT_EQ(cat::TransactionCommit(), ERROR_INVALID_PARAMETER);
}

// Each way an Attach fails records its failure only when none is recorded yet, so a transaction reports its first one.
TEST_F(CatCompat, EachKindOfFailureIsKeptOnlyWhenItIsTheFirst) {
  constexpr int kDenied = kMaxPendingHooks + 2;
  nevr_hook_guard::Record(Target(kDenied), "OurHook");
  PVOID none = nullptr;
  // A refusal first, then an invalid argument and an unhookable target: commit says "refused".
  cat::TransactionBegin();
  EXPECT_EQ(Attach(kDenied, "refused"), ERROR_ACCESS_DENIED);
  EXPECT_EQ(cat::Attach(&none, reinterpret_cast<PVOID>(&Detour), "invalid", "test"), ERROR_INVALID_PARAMETER);
  EXPECT_EQ(AttachData("data"), ERROR_INVALID_OPERATION);
  EXPECT_EQ(cat::TransactionCommit(), ERROR_ACCESS_DENIED);
  // An unhookable target first: commit says that, not the refusal after it.
  cat::TransactionBegin();
  EXPECT_EQ(AttachData("data"), ERROR_INVALID_OPERATION);
  EXPECT_EQ(Attach(kDenied, "refused"), ERROR_ACCESS_DENIED);
  EXPECT_EQ(cat::TransactionCommit(), ERROR_INVALID_OPERATION);
  // A refusal, then the transaction filling up: commit says "refused".
  cat::TransactionBegin();
  for (int i = 0; i < kMaxPendingHooks; ++i) ASSERT_EQ(Attach(i, "pending"), NO_ERROR) << i;
  EXPECT_EQ(Attach(kDenied, "refused"), ERROR_ACCESS_DENIED);
  EXPECT_EQ(Attach(kMaxPendingHooks, "full"), ERROR_NOT_ENOUGH_MEMORY);
  EXPECT_EQ(cat::TransactionCommit(), ERROR_ACCESS_DENIED);
}

TEST_F(CatCompat, AFailureOutsideATransactionDoesNotFailTheNextOne) {
  PVOID none = nullptr;
  EXPECT_EQ(cat::Attach(&none, reinterpret_cast<PVOID>(&Detour), "invalid", "test"), ERROR_INVALID_PARAMETER);
  cat::TransactionBegin();
  EXPECT_EQ(Attach(0, "fine"), NO_ERROR);
  EXPECT_EQ(cat::TransactionCommit(), NO_ERROR);
  EXPECT_TRUE(IsHooked(0));
}

TEST_F(CatCompat, ACommitReportsAFailureOnceAndTheNextCommitStartsClean) {
  PVOID none = nullptr;
  cat::TransactionBegin();
  cat::Attach(&none, reinterpret_cast<PVOID>(&Detour), "invalid", "test");
  EXPECT_EQ(cat::TransactionCommit(), ERROR_INVALID_PARAMETER);
  EXPECT_EQ(cat::TransactionCommit(), NO_ERROR);
}

TEST_F(CatCompat, ABeginStartsFromNoFailureAfterAFailedTransaction) {
  PVOID none = nullptr;
  cat::TransactionBegin();
  cat::Attach(&none, reinterpret_cast<PVOID>(&Detour), "invalid", "test");
  ASSERT_EQ(cat::TransactionCommit(), ERROR_INVALID_PARAMETER);

  cat::TransactionBegin();
  EXPECT_EQ(Attach(0, "again"), NO_ERROR);
  EXPECT_EQ(cat::TransactionCommit(), NO_ERROR);
  EXPECT_TRUE(IsHooked(0));
}

TEST_F(CatCompat, AnAbortTakesOutWhatTheTransactionAttached) {
  cat::TransactionBegin();
  ASSERT_EQ(Attach(0, "first"), NO_ERROR);
  ASSERT_TRUE(IsHooked(0));
  EXPECT_EQ(cat::TransactionAbort(), NO_ERROR);
  EXPECT_FALSE(IsHooked(0));
  EXPECT_EQ(real_[0], Target(0));
  EXPECT_EQ(Count("hook rolled back name=first"), 1);
  // The transaction is closed: a commit after it has nothing to fail or undo.
  EXPECT_EQ(cat::TransactionCommit(), NO_ERROR);
}

TEST_F(CatCompat, AnAbortLeavesAHookMadeOutsideATransaction) {
  ASSERT_EQ(Attach(0, "standing"), NO_ERROR);
  cat::TransactionBegin();
  ASSERT_EQ(Attach(1, "pending"), NO_ERROR);
  cat::TransactionAbort();
  EXPECT_TRUE(IsHooked(0));
  EXPECT_FALSE(IsHooked(1));
}

TEST_F(CatCompat, ATransactionHoldsAtMostThirtyTwoHooks) {
  cat::TransactionBegin();
  for (int i = 0; i < kMaxPendingHooks; ++i) ASSERT_EQ(Attach(i, "pending"), NO_ERROR) << i;
  EXPECT_EQ(Attach(kMaxPendingHooks, "one too many"), ERROR_NOT_ENOUGH_MEMORY);
  EXPECT_EQ(Count("reason=hook_table_full"), 1);
  EXPECT_EQ(cat::TransactionCommit(), ERROR_NOT_ENOUGH_MEMORY);
  for (int i = 0; i <= kMaxPendingHooks; ++i) EXPECT_FALSE(IsHooked(i)) << i;
}

TEST_F(CatCompat, TheTableHoldsAtMostNinetySixHooks) {
  for (int i = 0; i < kMaxHookTable; ++i) ASSERT_EQ(Attach(i, "standing"), NO_ERROR) << i;
  EXPECT_EQ(Attach(kMaxHookTable, "one too many"), ERROR_NOT_ENOUGH_MEMORY);
  EXPECT_FALSE(IsHooked(kMaxHookTable));
  // A removal makes room again.
  EXPECT_EQ(cat::Detach(&real_[0], reinterpret_cast<PVOID>(&Detour)), NO_ERROR);
  EXPECT_EQ(Attach(kMaxHookTable, "fits now"), NO_ERROR);
  EXPECT_TRUE(IsHooked(kMaxHookTable));
}

// ---- Detach ----

TEST_F(CatCompat, DetachRemovesTheHookAndPutsTheTargetBackInThePointer) {
  ASSERT_EQ(Attach(kDetachedStub, "standing"), NO_ERROR);
  ASSERT_TRUE(IsHooked(kDetachedStub));
  EXPECT_EQ(cat::Detach(&real_[kDetachedStub], reinterpret_cast<PVOID>(&Detour)), NO_ERROR);
  EXPECT_FALSE(IsHooked(kDetachedStub));
  EXPECT_EQ(real_[kDetachedStub], Target(kDetachedStub));
  EXPECT_EQ(Count("hook removed name=standing"), 1);
  EXPECT_EQ(nevr_hook_guard::RecordedCount(), 0);
}

TEST_F(CatCompat, DetachOfAHookItDidNotMakeIsInvalidHandle) {
  EXPECT_EQ(cat::Detach(&real_[kMismatchStub], reinterpret_cast<PVOID>(&Detour)), ERROR_INVALID_HANDLE);
  ASSERT_EQ(Attach(kMismatchStub, "standing"), NO_ERROR);
  int other = 0;
  EXPECT_EQ(cat::Detach(&real_[kMismatchStub], &other), ERROR_INVALID_HANDLE);  // right pointer, another detour
  EXPECT_EQ(cat::Detach(&real_[kUnhookedPointerStub], reinterpret_cast<PVOID>(&Detour)), ERROR_INVALID_HANDLE);  // another pointer
  EXPECT_TRUE(IsHooked(kMismatchStub));
}

// ---- the fault handler behind EC_TRY ----

DWORD g_passedCode = 0;
LONG CALLBACK LastResort(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (code != kTestCode && code != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
  g_passedCode = code;
  return EXCEPTION_CONTINUE_EXECUTION;
}

class CatFaults : public ::testing::Test {
 protected:
  void SetUp() override {
    cat::InstallFaultHandler();
    cat::InstallFaultHandler();  // idempotent
    lastResort_ = AddVectoredExceptionHandler(0, &LastResort);  // after the shim's, which is first in the chain
    ASSERT_NE(lastResort_, nullptr);
    g_passedCode = 0;
  }
  void TearDown() override { RemoveVectoredExceptionHandler(lastResort_); }
  PVOID lastResort_ = nullptr;
};

TEST_F(CatFaults, AnAccessViolationInATryBlockRunsTheExceptBlockWithItsCode) {
  uint8_t* bad = static_cast<uint8_t*>(VirtualAlloc(nullptr, kNoAccessPageBytes, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS));
  ASSERT_NE(bad, nullptr);
  volatile bool bodyFinished = false;
  volatile bool excepted = false;
  volatile DWORD code = 0;
  EC_TRY {
    volatile uint8_t sink = *static_cast<volatile uint8_t*>(bad);
    static_cast<void>(sink);
    bodyFinished = true;
  }
  EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
    excepted = true;
    code = EC_EXCEPTION_CODE();
  }
  EXPECT_FALSE(bodyFinished);
  EXPECT_TRUE(excepted);
  EXPECT_EQ(code, static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION));
  EXPECT_EQ(g_passedCode, 0u);  // contained: the rest of the chain never saw it
  VirtualFree(bad, 0, MEM_RELEASE);
}

TEST_F(CatFaults, ATryBlockThatDoesNotFaultRunsToTheEndAndSkipsTheExceptBlock) {
  volatile bool bodyFinished = false;
  volatile bool excepted = false;
  EC_TRY { bodyFinished = true; }
  EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { excepted = true; }
  EXPECT_TRUE(bodyFinished);
  EXPECT_FALSE(excepted);
}

TEST_F(CatFaults, EveryListedFaultCodeIsCaught) {
  const DWORD codes[] = {EXCEPTION_ACCESS_VIOLATION,   EXCEPTION_IN_PAGE_ERROR,   EXCEPTION_ARRAY_BOUNDS_EXCEEDED,
                         EXCEPTION_DATATYPE_MISALIGNMENT, EXCEPTION_ILLEGAL_INSTRUCTION, EXCEPTION_PRIV_INSTRUCTION,
                         EXCEPTION_INT_DIVIDE_BY_ZERO, EXCEPTION_INT_OVERFLOW};
  for (const DWORD raised : codes) {
    volatile DWORD seen = 0;
    volatile bool bodyFinished = false;
    EC_TRY {
      RaiseException(raised, 0, 0, nullptr);
      bodyFinished = true;
    }
    EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { seen = EC_EXCEPTION_CODE(); }
    EXPECT_FALSE(bodyFinished) << std::hex << raised;
    EXPECT_EQ(seen, raised);
  }
  EXPECT_EQ(g_passedCode, 0u);
}

TEST_F(CatFaults, AnExceptionNotOnTheListPassesToTheRestOfTheChain) {
  volatile bool bodyFinished = false;
  volatile bool excepted = false;
  EC_TRY {
    RaiseException(kTestCode, 0, 0, nullptr);  // a C++-style exception code: not Cat's to catch
    bodyFinished = true;
  }
  EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { excepted = true; }
  EXPECT_TRUE(bodyFinished);
  EXPECT_FALSE(excepted);
  EXPECT_EQ(g_passedCode, kTestCode);
}

TEST_F(CatFaults, WithNoTryBlockOpenAFaultPassesToTheRestOfTheChain) {
  RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
  EXPECT_EQ(g_passedCode, static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION));
}

TEST_F(CatFaults, AfterAFaultWasCaughtTheBlockIsClosedAndALaterFaultPassesOn) {
  volatile bool excepted = false;
  EC_TRY { RaiseException(EXCEPTION_INT_DIVIDE_BY_ZERO, 0, 0, nullptr); }
  EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { excepted = true; }
  ASSERT_TRUE(excepted);
  RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
  EXPECT_EQ(g_passedCode, static_cast<DWORD>(EXCEPTION_ACCESS_VIOLATION));
}

TEST_F(CatFaults, TheInnermostBlockTakesTheFaultAndTheOuterOneStaysOpen) {
  volatile bool innerExcepted = false;
  volatile bool outerExcepted = false;
  volatile bool outerBodyFinished = false;
  EC_TRY {
    EC_TRY { RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr); }
    EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { innerExcepted = true; }
    // The outer block is still open: a second fault is its own.
    RaiseException(EXCEPTION_INT_DIVIDE_BY_ZERO, 0, 0, nullptr);
    outerBodyFinished = true;
  }
  EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { outerExcepted = true; }
  EXPECT_TRUE(innerExcepted);
  EXPECT_TRUE(outerExcepted);
  EXPECT_FALSE(outerBodyFinished);
}

TEST_F(CatFaults, AFinishedBlockIsPoppedSoALaterOuterFaultIsNotLostToIt) {
  volatile bool outerExcepted = false;
  EC_TRY {
    EC_TRY { /* falls off its end */ }
    EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {}
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
  }
  EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { outerExcepted = true; }
  EXPECT_TRUE(outerExcepted);
}

// ---- the plugin logger and the settings reads ----

TEST_F(CatCompat, HostLogMapsCatsLevelsAndTagsTheModule) {
  cat::HostLog("dronefix", NEVR_LOG_DEBUG, "d");
  cat::HostLog("dronefix", NEVR_LOG_INFO, "i");
  cat::HostLog("dronefix", NEVR_LOG_WARNING, "w");
  cat::HostLog("dronefix", NEVR_LOG_ERROR, "e");
  cat::HostLog(nullptr, NEVR_LOG_INFO, nullptr);
  ASSERT_EQ(g_logged.size(), kLoggedLineCount);
  EXPECT_EQ(g_logged[0].first, EchoVR::LogLevel::Debug);
  EXPECT_EQ(g_logged[1].first, EchoVR::LogLevel::Info);
  EXPECT_EQ(g_logged[kWarningLineIndex].first, EchoVR::LogLevel::Warning);
  EXPECT_EQ(g_logged[kErrorLineIndex].first, EchoVR::LogLevel::Error);
  EXPECT_NE(g_logged[0].second.find("[NEVR.COMBAT] module=dronefix d"), std::string::npos);
  EXPECT_NE(g_logged[kNullModuleLineIndex].second.find("module=?"), std::string::npos);
}

TEST_F(CatCompat, ASettingComesFromConfigThenHisShippedValueThenTheCodesLiteral) {
  // config.yaml wins; the leading digits are read, as GetPrivateProfileInt does.
  g_combatConfig["settings.testsec.configured"] = "42abc";
  EXPECT_EQ(cat::IniInt("testsec", "configured", -1), kConfiguredValue);
  // Present but empty or not a number: the fallback.
  g_combatConfig["settings.testsec.empty"] = "";
  g_combatConfig["settings.testsec.word"] = "abc";
  EXPECT_EQ(cat::IniInt("testsec", "empty", -1), -1);
  EXPECT_EQ(cat::IniInt("testsec", "word", -1), -1);
  // Absent from config.yaml and from his PowerBoost.ini: the code's literal.
  EXPECT_EQ(cat::IniInt("testsec", "absent", kCodeFallback), kCodeFallback);
  // Absent from config.yaml, in his PowerBoost.ini: his value (OVERCHARGE self damage is 5, the code's literal is 2).
  EXPECT_EQ(cat::IniInt("buff", "self_damage", kCodeSelfDamage), kShippedSelfDamage);
  // A null section or key is no setting.
  EXPECT_EQ(cat::IniInt(nullptr, "x", kNullArgFallback), kNullArgFallback);
  EXPECT_EQ(cat::IniInt("buff", nullptr, kNullArgFallback), kNullArgFallback);
}

TEST_F(CatCompat, IniSourcesCountsEachKeyOnceBySource) {
  const cat::IniSourceCounts before = cat::IniSources();
  g_combatConfig["settings.countsec.a"] = "1";
  cat::IniInt("countsec", "a", 0);
  cat::IniInt("countsec", "a", 0);  // the same key again is not counted again
  cat::IniInt("countsec", "b", 0);  // code literal
  cat::IniInt("buff", "head_bonus", 0);  // shipped
  const cat::IniSourceCounts after = cat::IniSources();
  EXPECT_EQ(after.config - before.config, 1);
  EXPECT_EQ(after.code - before.code, 1);
  EXPECT_EQ(after.shipped - before.shipped, 1);
}

TEST_F(CatCompat, IniStringCopiesTheValueTruncatesToTheBufferAndReportsTheLength) {
  g_combatConfig["settings.strsec.name"] = "hello";
  char out[kOutBufferBytes];
  EXPECT_EQ(cat::IniString("strsec", "name", "fallback", out, sizeof(out)), kHelloLength);
  EXPECT_STREQ(out, "hello");
  char small[kSmallBufferBytes];
  EXPECT_EQ(cat::IniString("strsec", "name", "fallback", small, sizeof(small)), kTruncatedLength);
  EXPECT_STREQ(small, "hel");
  EXPECT_EQ(cat::IniString("strsec", "missing", "fallback", out, sizeof(out)), kFallbackLength);
  EXPECT_STREQ(out, "fallback");
  EXPECT_EQ(cat::IniString("strsec", "missing", nullptr, out, sizeof(out)), 0u);
  EXPECT_STREQ(out, "");
  EXPECT_EQ(cat::IniString("strsec", "name", "x", nullptr, sizeof(out)), 0u);
  EXPECT_EQ(cat::IniString("strsec", "name", "x", out, 0), 0u);
}

}  // namespace
