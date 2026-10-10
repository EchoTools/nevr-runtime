// The export tracer (#20): the thunk's transparency (arguments, stack arguments, float arguments, results,
// nesting, exceptions, threads), the lock-free ring, the policy decisions, and that it is off by default.
// The thunk is assembly, so the transparency tests run it: under Wine in `just test-auth-unit`.

#include <windows.h>
#include <emmintrin.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/call_ring.h"
#include "core/logging.h"
#include "runtime/hook/export_trace_policy.h"
#include "runtime/hook/export_trace_thunk.h"
#include "runtime/hook/export_tracer.h"

// The tracer logs through the runtime's Log; the test supplies it and keeps the lines.
static std::vector<std::string>& LogLines() {
  static std::vector<std::string> lines;
  return lines;
}
static std::mutex& LogMutex() {
  static std::mutex m;
  return m;
}

VOID Log(EchoVR::LogLevel, const CHAR* format, ...) {
  char buffer[512];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  std::lock_guard<std::mutex> lock(LogMutex());
  LogLines().emplace_back(buffer);
}

namespace {

namespace Policy = nevr_export_trace_policy;

// ---- functions to trace -------------------------------------------------------------------------

extern "C" {
// Three of the seven arguments are on the stack: a thunk that loses them changes the sum.
__attribute__((noinline)) std::uint64_t SevenArgs(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                                  std::uint64_t d, std::uint64_t e, std::uint64_t f,
                                                  std::uint64_t g) {
  return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g;
}

// x in xmm0, k in rdx, y in xmm2: the argument slots are positional on Windows x64.
__attribute__((noinline)) double MixFloat(double x, std::uint64_t k, double y) {
  return x * static_cast<double>(k) + y;
}

std::atomic<std::uint64_t> g_voidSeen{0};
__attribute__((noinline)) void VoidCall(std::uint64_t v) { g_voidSeen.store(v); }

__attribute__((noinline)) std::uint64_t Add(std::uint64_t a, std::uint64_t b) { return a + b; }

__attribute__((noinline)) std::uint64_t Throws(std::uint64_t) { throw std::runtime_error("through the thunk"); }
}

using SevenFn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                  std::uint64_t, std::uint64_t);
using MixFn = double (*)(double, std::uint64_t, double);
using VoidFn = void (*)(std::uint64_t);
using AddFn = std::uint64_t (*)(std::uint64_t, std::uint64_t);
using ThrowFn = std::uint64_t (*)(std::uint64_t);

std::uint64_t (*g_nestedInner)(std::uint64_t, std::uint64_t) = nullptr;
extern "C" __attribute__((noinline)) std::uint64_t Outer(std::uint64_t a, std::uint64_t b) {
  return g_nestedInner(a, b) + 100;
}

std::vector<nevr::CallRecord> DrainAll() {
  std::vector<nevr::CallRecord> out;
  nevr::CallRecord r;
  while (nevr_export_trace::Pop(&r)) out.push_back(r);
  return out;
}

class ExportTraceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    nevr_export_trace::Reset();
    LogLines().clear();
  }
};

// ---- policy -------------------------------------------------------------------------------------

TEST(ExportTracePolicy, ParsesModuleLists) {
  bool unknown = true;
  EXPECT_EQ(Policy::ParseModules("pnsrad", &unknown), Policy::kPnsrad);
  EXPECT_FALSE(unknown);
  EXPECT_EQ(Policy::ParseModules("PNSRAD,pnsovr.dll", &unknown), Policy::kPnsrad | Policy::kPnsovr);
  EXPECT_EQ(Policy::ParseModules("pnsrad pnsdemo;pnsovr", &unknown), Policy::kAllModules);
  EXPECT_EQ(Policy::ParseModules("all", &unknown), Policy::kAllModules);
  EXPECT_EQ(Policy::ParseModules("pnsrad,bogus", &unknown), Policy::kPnsrad);
  EXPECT_TRUE(unknown);
  EXPECT_EQ(Policy::ParseModules("", &unknown), 0u) << "an empty list is the tracer off";
  EXPECT_FALSE(unknown);
  EXPECT_EQ(Policy::ParseModules(nullptr, &unknown), 0u);
}

TEST(ExportTracePolicy, IdentifiesPlatformDllsByFileName) {
  EXPECT_EQ(Policy::ModuleOfPath("C:\\EchoVR\\bin\\win10\\pnsrad.dll"), Policy::kPnsrad);
  EXPECT_EQ(Policy::ModuleOfPath("/x/PNSOVR.DLL"), Policy::kPnsovr);
  EXPECT_EQ(Policy::ModuleOfPath("pnsdemo.dll"), Policy::kPnsdemo);
  EXPECT_EQ(Policy::ModuleOfPath("C:\\EchoVR\\bin\\win10\\pnsradmatchmaking.dll"), 0u);
  EXPECT_EQ(Policy::ModuleOfPath("C:\\Windows\\kernel32.dll"), 0u);
  EXPECT_EQ(Policy::ModuleOfPath(nullptr), 0u);
}

// ---- the ring -----------------------------------------------------------------------------------

TEST(CallRing, KeepsOrderAndCountsDropsWhenFull) {
  static nevr::CallRing<8> ring;
  ring.Seed();
  nevr::CallRecord r{};
  for (std::uint64_t i = 0; i < 8; ++i) {
    r.ret = i;
    EXPECT_TRUE(ring.Push(r));
  }
  r.ret = 99;
  EXPECT_FALSE(ring.Push(r)) << "a full ring drops the new record";
  EXPECT_EQ(ring.Dropped(), 1u);
  nevr::CallRecord out;
  for (std::uint64_t i = 0; i < 8; ++i) {
    ASSERT_TRUE(ring.Pop(&out));
    EXPECT_EQ(out.ret, i);
  }
  EXPECT_FALSE(ring.Pop(&out));
  r.ret = 7;
  EXPECT_TRUE(ring.Push(r)) << "space is reusable after a drain";
}

// ---- the thunk ----------------------------------------------------------------------------------

TEST_F(ExportTraceTest, ForwardsRegisterAndStackArgumentsAndTheResult) {
  void* const thunk = nevr_export_trace::MakeThunk(reinterpret_cast<void*>(&SevenArgs), 7);
  ASSERT_NE(thunk, nullptr);
  const std::uint64_t direct = SevenArgs(1, 2, 3, 4, 5, 6, 7);
  const std::uint64_t traced = reinterpret_cast<SevenFn>(thunk)(1, 2, 3, 4, 5, 6, 7);
  EXPECT_EQ(traced, direct) << "the three stack arguments reached the original";
  const auto records = DrainAll();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].exportId, 7u);
  EXPECT_EQ(records[0].args[0], 1u);
  EXPECT_EQ(records[0].args[1], 2u);
  EXPECT_EQ(records[0].args[2], 3u);
  EXPECT_EQ(records[0].args[3], 4u);
  EXPECT_EQ(records[0].ret, direct);
  EXPECT_LE(records[0].enterTicks, records[0].exitTicks);
  EXPECT_EQ(records[0].threadId, GetCurrentThreadId());
}

TEST_F(ExportTraceTest, KeepsFloatArgumentsAndAFloatResult) {
  void* const thunk = nevr_export_trace::MakeThunk(reinterpret_cast<void*>(&MixFloat), 1);
  ASSERT_NE(thunk, nullptr);
  const double traced = reinterpret_cast<MixFn>(thunk)(1.5, 4, 0.25);
  EXPECT_DOUBLE_EQ(traced, 6.25);
  const auto records = DrainAll();
  ASSERT_EQ(records.size(), 1u);
  double recorded;
  std::memcpy(&recorded, &records[0].retXmm0, sizeof(recorded));
  EXPECT_DOUBLE_EQ(recorded, 6.25) << "xmm0 is recorded as it came back";
  EXPECT_EQ(records[0].args[1], 4u) << "the integer argument in rdx";
}

extern "C" __attribute__((noinline)) __m128 VectorResult(std::uint64_t k) {
  return _mm_set_ps(4.0f, 3.0f, 2.0f, static_cast<float>(k));
}

TEST_F(ExportTraceTest, HandsBackAllOf128BitsOfAVectorResult) {
  void* const thunk = nevr_export_trace::MakeThunk(reinterpret_cast<void*>(&VectorResult), 3);
  ASSERT_NE(thunk, nullptr);
  const __m128 got = reinterpret_cast<__m128 (*)(std::uint64_t)>(thunk)(9);
  alignas(16) float lanes[4];
  _mm_store_ps(lanes, got);
  EXPECT_FLOAT_EQ(lanes[0], 9.0f);
  EXPECT_FLOAT_EQ(lanes[1], 2.0f);
  EXPECT_FLOAT_EQ(lanes[2], 3.0f) << "the high half of xmm0";
  EXPECT_FLOAT_EQ(lanes[3], 4.0f);
}

TEST_F(ExportTraceTest, ForwardsAVoidCall) {
  void* const thunk = nevr_export_trace::MakeThunk(reinterpret_cast<void*>(&VoidCall), 2);
  ASSERT_NE(thunk, nullptr);
  g_voidSeen.store(0);
  reinterpret_cast<VoidFn>(thunk)(0xABCD);
  EXPECT_EQ(g_voidSeen.load(), 0xABCDu);
  EXPECT_EQ(DrainAll().size(), 1u);
}

TEST_F(ExportTraceTest, OneBodyBehindTwoNamesGivesTwoIds) {
  // Identical-code folding: two exports, one address. The game asks for them by name, so each gets a thunk.
  void* const a = nevr_export_trace::MakeThunk(reinterpret_cast<void*>(&Add), 10);
  void* const b = nevr_export_trace::MakeThunk(reinterpret_cast<void*>(&Add), 11);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_NE(a, b);
  EXPECT_EQ(reinterpret_cast<AddFn>(a)(2, 3), 5u);
  EXPECT_EQ(reinterpret_cast<AddFn>(b)(4, 5), 9u);
  const auto records = DrainAll();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].exportId, 10u);
  EXPECT_EQ(records[1].exportId, 11u);
}

TEST_F(ExportTraceTest, NestedTracedCallsRecordInnerFirst) {
  void* const inner = nevr_export_trace::MakeThunk(reinterpret_cast<void*>(&Add), 20);
  void* const outer = nevr_export_trace::MakeThunk(reinterpret_cast<void*>(&Outer), 21);
  ASSERT_NE(inner, nullptr);
  ASSERT_NE(outer, nullptr);
  g_nestedInner = reinterpret_cast<AddFn>(inner);
  EXPECT_EQ(reinterpret_cast<AddFn>(outer)(1, 2), 103u);
  const auto records = DrainAll();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].exportId, 20u) << "the inner call returns, and is recorded, first";
  EXPECT_EQ(records[1].exportId, 21u);
  EXPECT_LE(records[1].enterTicks, records[0].enterTicks);
  EXPECT_GE(records[1].exitTicks, records[0].exitTicks);
}

TEST_F(ExportTraceTest, AnExceptionUnwindsThroughTheThunk) {
  void* const thunk = nevr_export_trace::MakeThunk(reinterpret_cast<void*>(&Throws), 30);
  ASSERT_NE(thunk, nullptr);
  bool caught = false;
  try {
    reinterpret_cast<ThrowFn>(thunk)(1);
  } catch (const std::runtime_error&) {
    caught = true;
  }
  EXPECT_TRUE(caught) << "the thunk frame has unwind data";
  // The call did not return, so nothing was recorded for it.
  EXPECT_TRUE(DrainAll().empty());
}

TEST_F(ExportTraceTest, ManyThreadsEveryCallIsAccountedFor) {
  void* const thunk = nevr_export_trace::MakeThunk(reinterpret_cast<void*>(&Add), 40);
  ASSERT_NE(thunk, nullptr);
  constexpr int kThreads = 6;
  constexpr int kCalls = 3000;
  std::atomic<bool> wrong{false};
  std::atomic<int> running{kThreads};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kCalls; ++i) {
        if (reinterpret_cast<AddFn>(thunk)(static_cast<std::uint64_t>(t), static_cast<std::uint64_t>(i)) !=
            static_cast<std::uint64_t>(t + i)) {
          wrong = true;
        }
      }
      --running;
    });
  }
  std::uint64_t popped = 0;
  nevr::CallRecord r;
  while (running.load() > 0) {
    while (nevr_export_trace::Pop(&r)) ++popped;
  }
  for (auto& th : threads) th.join();
  while (nevr_export_trace::Pop(&r)) ++popped;
  EXPECT_FALSE(wrong.load());
  EXPECT_EQ(popped, nevr_export_trace::Pushed());
  EXPECT_EQ(nevr_export_trace::Pushed() + nevr_export_trace::Dropped(), static_cast<std::uint64_t>(kThreads) * kCalls)
      << "a call is recorded or counted as dropped, never lost";
}

TEST_F(ExportTraceTest, ATableThatIsFullHandsBackNull) {
  for (std::uint32_t i = 0; i < nevr_export_trace::kMaxThunks; ++i) {
    ASSERT_NE(nevr_export_trace::MakeThunk(reinterpret_cast<void*>(&Add), i), nullptr);
  }
  EXPECT_EQ(nevr_export_trace::MakeThunk(reinterpret_cast<void*>(&Add), 999), nullptr);
}

// ---- the command line, code or data, and the whole path against a real module ------------------

TEST(ExportTracePolicy, ReadsTheFlagValueFromACommandLine) {
  char out[64];
  EXPECT_TRUE(Policy::ExtractFlagValue(L"C:\\g\\echovr.exe -windowed -traceexports pnsrad,pnsovr -noconsole", L"-traceexports",
                                       out, sizeof(out)));
  EXPECT_STREQ(out, "pnsrad,pnsovr");
  EXPECT_TRUE(Policy::ExtractFlagValue(L"\"C:\\Program Files\\echovr.exe\" -TraceExports \"all\"", L"-traceexports", out,
                                       sizeof(out)));
  EXPECT_STREQ(out, "all");
  EXPECT_FALSE(Policy::ExtractFlagValue(L"echovr.exe -windowed", L"-traceexports", out, sizeof(out)));
  EXPECT_FALSE(Policy::ExtractFlagValue(L"echovr.exe -traceexports", L"-traceexports", out, sizeof(out)))
      << "a flag with no value";
  EXPECT_FALSE(Policy::ExtractFlagValue(L"echovr.exe -traceexportsx pnsrad", L"-traceexports", out, sizeof(out)))
      << "a longer flag is a different flag";
  EXPECT_FALSE(Policy::ExtractFlagValue(L"echovr.exe -traceexports aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", L"-traceexports",
                                        out, 8))
      << "a value that does not fit";
  EXPECT_FALSE(Policy::ExtractFlagValue(nullptr, L"-traceexports", out, sizeof(out)));
}

TEST(ExportTracer, OnlyCodeIsAThunkTarget) {
  static int data = 5;
  EXPECT_TRUE(nevr_export_tracer::PointsToCode(reinterpret_cast<const void*>(&Add)));
  EXPECT_FALSE(nevr_export_tracer::PointsToCode(&data));
  int onStack = 0;
  EXPECT_FALSE(nevr_export_tracer::PointsToCode(&onStack));
  EXPECT_FALSE(nevr_export_tracer::PointsToCode(nullptr));
}

bool LogHas(const char* needle) {
  std::lock_guard<std::mutex> lock(LogMutex());
  for (const std::string& line : LogLines()) {
    if (line.find(needle) != std::string::npos) return true;
  }
  return false;
}

bool WaitForLog(const char* needle, int ms = 3000) {
  for (int waited = 0; waited < ms; waited += 10) {
    if (LogHas(needle)) return true;
    Sleep(10);
  }
  return LogHas(needle);
}

// The whole path against a real module named pnsrad.dll, with one code export and one data export. This is the
// only test that configures the tracer (it is configured once per process, from the command line text).
TEST(ExportTracer, EndToEndAgainstAModuleNamedPnsrad) {
  char exePath[MAX_PATH];
  ASSERT_GT(GetModuleFileNameA(nullptr, exePath, sizeof(exePath)), 0u);
  std::string dir(exePath);
  dir.resize(dir.find_last_of("\\/") + 1);
  const std::string fixture = dir + "export-trace-fixture\\pnsrad.dll";
  HMODULE const module = LoadLibraryA(fixture.c_str());
  ASSERT_NE(module, nullptr) << "the fixture " << fixture;
  void* const code = reinterpret_cast<void*>(GetProcAddress(module, "FixtureCode"));
  void* const data = reinterpret_cast<void*>(GetProcAddress(module, "FixtureData"));
  ASSERT_NE(code, nullptr);
  ASSERT_NE(data, nullptr);

  // Off until configured: the pointer is what the module exports, whatever the module.
  EXPECT_FALSE(nevr_export_tracer::Enabled());
  EXPECT_EQ(nevr_export_tracer::WrapSymbol(module, "FixtureCode", code), code);

  // A lookup before the flag is known must not decide anything: the flag is read from the command line,
  // which was complete from process start.
  nevr_export_tracer::ConfigureFromCommandLineText(L"echovr.exe -windowed -TraceExports pnsrad -noconsole");
  ASSERT_TRUE(nevr_export_tracer::Enabled());

  void* const wrapped = nevr_export_tracer::WrapSymbol(module, "FixtureCode", code);
  ASSERT_NE(wrapped, code) << "a code export of a selected module is a thunk";
  EXPECT_EQ(nevr_export_tracer::WrapSymbol(module, "FixtureCode", code), wrapped) << "one thunk per name";
  EXPECT_EQ(reinterpret_cast<int (*)(int)>(wrapped)(41), 42);

  // A data export is handed back as it is: the game reads a value through that pointer.
  EXPECT_EQ(nevr_export_tracer::WrapSymbol(module, "FixtureData", data), data);
  EXPECT_EQ(*static_cast<int*>(data), 1234);

  // A module that is not selected is left alone (the test process's own functions are not pnsrad).
  EXPECT_EQ(nevr_export_tracer::WrapSymbol(GetModuleHandleA("kernel32.dll"), "Sleep", reinterpret_cast<void*>(&Sleep)),
            reinterpret_cast<void*>(&Sleep));

  EXPECT_TRUE(WaitForLog("[NEVR.TRACE] call #1 module=pnsrad export=FixtureCode"));
  EXPECT_TRUE(LogHas("[NEVR.TRACE] resolve module=pnsrad export=FixtureCode"));
  EXPECT_TRUE(LogHas("[NEVR.TRACE] skip module=pnsrad export=FixtureData"));

  // Shutdown makes a last pass and the summary, and returns.
  nevr_export_tracer::Shutdown();
  EXPECT_TRUE(LogHas("[NEVR.TRACE] summary module=pnsrad export=FixtureCode calls=1"));
  nevr_export_tracer::Shutdown();  // twice is harmless
}

}  // namespace
