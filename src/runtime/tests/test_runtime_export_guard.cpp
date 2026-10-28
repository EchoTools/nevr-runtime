// The guard behind every runtime export entry (lifecycle/export_guard.h): whatever a function throws, the caller gets
// the entry's failure value and one report, and nothing propagates.

#include "runtime/lifecycle/export_guard.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

using nevr_runtime_exports::Guarded;
using nevr_runtime_exports::GuardedInitialize;
using nevr_runtime_exports::GuardedVoid;
using nevr_runtime_exports::kInitializeThrew;

struct Report {
  int count = 0;
  std::string entry;
  bool had_text = false;
  std::string text;
};
Report g_report;

void Record(const char* entry, const char* what) noexcept {
  ++g_report.count;
  g_report.entry = entry;
  g_report.had_text = what != nullptr;
  g_report.text = what != nullptr ? what : "";
}

struct ForeignError {};

class ExportGuard : public ::testing::Test {
 protected:
  void SetUp() override { g_report = Report{}; }
};

TEST_F(ExportGuard, ACallThatReturnsGivesItsResultAndReportsNothing) {
  EXPECT_EQ(Guarded("e", &Record, -1, [] { return 5; }), 5);
  GuardedVoid("v", &Record, [] {});
  EXPECT_EQ(g_report.count, 0);
}

TEST_F(ExportGuard, AStdExceptionGivesTheFailureValueAndOneReportWithItsText) {
  EXPECT_EQ(Guarded("register_asset", &Record, -1, []() -> int { throw std::runtime_error("boom"); }), -1);
  EXPECT_EQ(g_report.count, 1);
  EXPECT_EQ(g_report.entry, "register_asset");
  EXPECT_TRUE(g_report.had_text);
  EXPECT_EQ(g_report.text, "boom");
}

TEST_F(ExportGuard, AThrowOfAnyOtherTypeIsStoppedToo) {
  EXPECT_EQ(Guarded("a", &Record, std::uint64_t{7}, []() -> std::uint64_t { throw ForeignError{}; }), 7u);
  EXPECT_EQ(g_report.count, 1);
  EXPECT_FALSE(g_report.had_text);
  EXPECT_EQ(Guarded("b", &Record, 3, []() -> int { throw 42; }), 3);
  EXPECT_EQ(g_report.count, 2);
  EXPECT_EQ(g_report.entry, "b");
}

TEST_F(ExportGuard, AVoidEntryThatThrowsAnyTypeIsStoppedAndReported) {
  EXPECT_NO_THROW(GuardedVoid("v1", &Record, [] { throw std::logic_error("bad"); }));
  EXPECT_EQ(g_report.count, 1);
  EXPECT_EQ(g_report.text, "bad");
  EXPECT_NO_THROW(GuardedVoid("v2", &Record, [] { throw ForeignError{}; }));
  EXPECT_EQ(g_report.count, 2);
  EXPECT_FALSE(g_report.had_text);
}

TEST_F(ExportGuard, ThePointerReturningEntriesGiveTheirFailureValueUnchanged) {
  static const char kEmpty[] = "";
  const char* got = Guarded("token", &Record, static_cast<const char*>(kEmpty), []() -> const char* { throw 1; });
  EXPECT_EQ(got, kEmpty);
}

// The runtime's initialize (lifecycle/runtime_entry.cpp) is called by the host from its detour; a throw out of it
// must reach the host as the nonzero code that stops the game, with one report, never as an unwinding exception.
TEST_F(ExportGuard, AnInitializeThatThrowsAnyTypeGivesTheHostANonzeroCodeAndOneReport) {
  EXPECT_NE(kInitializeThrew, 0);
  EXPECT_EQ(GuardedInitialize(&Record, []() -> std::int32_t { throw std::runtime_error("no init"); }), kInitializeThrew);
  EXPECT_EQ(g_report.count, 1);
  EXPECT_EQ(g_report.entry, "initialize");
  EXPECT_EQ(g_report.text, "no init");
  EXPECT_EQ(GuardedInitialize(&Record, []() -> std::int32_t { throw ForeignError{}; }), kInitializeThrew);
  EXPECT_EQ(g_report.count, 2);
  EXPECT_FALSE(g_report.had_text);
}

TEST_F(ExportGuard, AnInitializeThatReturnsKeepsItsOwnCode) {
  EXPECT_EQ(GuardedInitialize(&Record, [] { return std::int32_t{0}; }), 0);
  EXPECT_EQ(GuardedInitialize(&Record, [] { return std::int32_t{3}; }), 3);
  EXPECT_EQ(g_report.count, 0);
}

}  // namespace
