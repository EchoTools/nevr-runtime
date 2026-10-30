// #34: plugins log through plugin_logger.h; every line carries a level so it can be filtered.
// #598: once bound to a v6+ host's context, the same calls go to the host's structured log.

#include <gtest/gtest.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "nevr/plugin_logger.h"

namespace {

std::string Format(const char* level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::string line = nevr::FormatPluginLog("[p]", level, fmt, args);
    va_end(args);
    return line;
}

using nevr::kPluginLogHeldLines;

NEVR_DEFINE_PLUGIN_LOG("[test_plugin]")

TEST(PluginLogger, LineIsPrefixLevelMessage) {
    EXPECT_EQ(Format("INFO", "hello %d %s", 7, "x"), "[p] INFO hello 7 x");
    EXPECT_EQ(Format("WARNING", "no args"), "[p] WARNING no args");
    EXPECT_EQ(Format("ERROR", "%s", ""), "[p] ERROR ");
}

TEST(PluginLogger, LongMessagesAreNotTruncated) {
    const std::string big(5000, 'x');
    EXPECT_EQ(Format("ERROR", "%s", big.c_str()), "[p] ERROR " + big);
}

// --- #598: bound to the host's log ---

struct HostCall {
    std::string id;
    uint32_t level = 99;
    std::string message;
    std::vector<std::pair<std::string, std::string>> fields;
};
std::vector<HostCall> g_hostCalls;

void FakeHostLog(const char* id, uint32_t level, const char* message, const NevrLogField* fields, uint32_t count) {
    HostCall call;
    call.id = id;
    call.level = level;
    call.message = message;
    for (uint32_t i = 0; i < count; ++i) call.fields.emplace_back(fields[i].key, fields[i].value);
    g_hostCalls.push_back(call);
}

NvrGameContext V6Context() {
    NvrGameContext ctx = {};
    ctx.ctx_size = sizeof(NvrGameContext);
    ctx.plugin_id = "bound-plugin";
    ctx.log = FakeHostLog;
    return ctx;
}

// Reads stderr by redirecting it to a file for the duration of `body`.
template <typename Body>
std::string CaptureStderr(Body body) {
    const char* temp = std::getenv("TEMP");
    const std::string path = std::string(temp != nullptr ? temp : ".") + "\\plugin_logger_bound_test.txt";
    EXPECT_NE(std::freopen(path.c_str(), "w", stderr), nullptr);
    body();
    std::fflush(stderr);
    std::FILE* in = std::fopen(path.c_str(), "r");
    EXPECT_NE(in, nullptr);
    char buf[512] = {};
    const std::size_t n = in != nullptr ? std::fread(buf, 1, sizeof(buf) - 1, in) : 0;
    if (in != nullptr) std::fclose(in);
    std::remove(path.c_str());
    return std::string(buf, n);
}

TEST(PluginLoggerHost, BoundCallsGoToTheHostWithIdLevelAndFields) {
    g_hostCalls.clear();
    const NvrGameContext ctx = V6Context();
    ASSERT_TRUE(PluginLogBind(&ctx));
    const std::string err = CaptureStderr([] {
        PluginLogDebug("d %d", 0);
        PluginLog("i %d", 1);
        PluginLogWarning("w %d", 2);
        PluginLogError("e %s", "three");
        PluginLogFields(NEVR_LOG_INFO, "with fields", {{"k", "v"}, {"n", "7"}});
    });
    EXPECT_EQ(err, "") << "a bound plugin must not also write to stderr";
    ASSERT_EQ(g_hostCalls.size(), 5u);
    const uint32_t levels[] = {NEVR_LOG_DEBUG, NEVR_LOG_INFO, NEVR_LOG_WARNING, NEVR_LOG_ERROR, NEVR_LOG_INFO};
    const char* messages[] = {"d 0", "i 1", "w 2", "e three", "with fields"};
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(g_hostCalls[i].id, "bound-plugin") << i;
        EXPECT_EQ(g_hostCalls[i].level, levels[i]) << i;
        EXPECT_EQ(g_hostCalls[i].message, messages[i]) << i;
    }
    ASSERT_EQ(g_hostCalls[4].fields.size(), 2u);
    EXPECT_EQ(g_hostCalls[4].fields[0], (std::pair<std::string, std::string>("k", "v")));
    EXPECT_EQ(g_hostCalls[4].fields[1], (std::pair<std::string, std::string>("n", "7")));
    PluginLogState() = {};  // leave the shared binding as the other tests expect it
}

TEST(PluginLoggerHost, APercentInAnArgumentIsNotAFormat) {
    g_hostCalls.clear();
    const NvrGameContext ctx = V6Context();
    ASSERT_TRUE(PluginLogBind(&ctx));
    PluginLog("%s", "100%s done %n");
    ASSERT_EQ(g_hostCalls.size(), 1u);
    EXPECT_EQ(g_hostCalls[0].message, "100%s done %n");
    PluginLogState() = {};
}

TEST(PluginLoggerHost, AHostThatPredatesV6IsNotCalledThroughFieldsItDoesNotHave) {
    g_hostCalls.clear();
    NvrGameContext ctx = V6Context();
    ctx.ctx_size = static_cast<uint32_t>(offsetof(NvrGameContext, plugin_id));  // a v5 host's size
    EXPECT_FALSE(PluginLogBind(&ctx));
    const std::string err = CaptureStderr([] { PluginLogWarning("fallback %d", 5); });
    EXPECT_EQ(err, "[test_plugin] WARNING fallback 5\n");
    EXPECT_TRUE(g_hostCalls.empty());
}

TEST(PluginLoggerHost, AContextWithNoLogFunctionStaysOnStderr) {
    g_hostCalls.clear();
    NvrGameContext ctx = V6Context();
    ctx.log = nullptr;
    EXPECT_FALSE(PluginLogBind(&ctx));
    ctx = V6Context();
    ctx.plugin_id = nullptr;
    EXPECT_FALSE(PluginLogBind(&ctx));
    EXPECT_FALSE(PluginLogBind(nullptr));
    const std::string err = CaptureStderr([] { PluginLogFields(NEVR_LOG_ERROR, "unbound", {{"k", "v"}}); });
    EXPECT_EQ(err, "[test_plugin] ERROR unbound k=v\n");
}

TEST(PluginLoggerHost, RebindingToAnotherContextFollowsIt) {
    g_hostCalls.clear();
    NvrGameContext first = V6Context();
    ASSERT_TRUE(PluginLogBind(&first));
    NvrGameContext none = V6Context();
    none.log = nullptr;
    EXPECT_FALSE(PluginLogBind(&none));
    const std::string err = CaptureStderr([] { PluginLog("after unbind"); });
    EXPECT_EQ(err, "[test_plugin] INFO after unbind\n");
    EXPECT_TRUE(g_hostCalls.empty());
}

// --- lines logged before the bind are held, then written when the bind decides where they go ---

// Before a bind the lines are held (nothing reaches stderr); the bind writes them through the host, in order.
TEST(PluginLoggerHeld, LinesLoggedBeforeABindReachTheHostInOrderWhenItBinds) {
    PluginLogState() = {};
    g_hostCalls.clear();
    const std::string err = CaptureStderr([] {
        PluginLog("early %d", 1);
        PluginLogFields(NEVR_LOG_WARNING, "early fields", {{"k", "v"}});
        const NvrGameContext ctx = V6Context();
        EXPECT_TRUE(PluginLogBind(&ctx));
    });
    EXPECT_EQ(err, "") << "a bind with a host log writes the held lines through the host, not to stderr";
    ASSERT_EQ(g_hostCalls.size(), 2u);
    EXPECT_EQ(g_hostCalls[0].id, "bound-plugin");
    EXPECT_EQ(g_hostCalls[0].level, NEVR_LOG_INFO);
    EXPECT_EQ(g_hostCalls[0].message, "early 1");
    EXPECT_EQ(g_hostCalls[1].level, NEVR_LOG_WARNING);
    EXPECT_EQ(g_hostCalls[1].message, "early fields");
    ASSERT_EQ(g_hostCalls[1].fields.size(), 1u);
    EXPECT_EQ(g_hostCalls[1].fields[0], (std::pair<std::string, std::string>("k", "v")));
    PluginLog("after");
    EXPECT_EQ(g_hostCalls.size(), 3u) << "after the bind a line goes straight to the host";
    PluginLogState() = {};
}

// A held line keeps its own copy of the text: the caller's buffer may change after the call.
TEST(PluginLoggerHeld, AHeldLineKeepsItsOwnCopyOfItsText) {
    PluginLogState() = {};
    g_hostCalls.clear();
    std::string value = "first";
    PluginLogFields(NEVR_LOG_INFO, "held", {{"k", value.c_str()}});
    value[0] = 'X';
    const NvrGameContext ctx = V6Context();
    ASSERT_TRUE(PluginLogBind(&ctx));
    ASSERT_EQ(g_hostCalls.size(), 1u);
    ASSERT_EQ(g_hostCalls[0].fields.size(), 1u);
    EXPECT_EQ(g_hostCalls[0].fields[0], (std::pair<std::string, std::string>("k", "first")));
    PluginLogState() = {};
}

// Past the cap a held line is counted, not kept; the bind writes the kept lines and then the count.
TEST(PluginLoggerHeld, LinesPastTheCapAreCountedAndTheBindReportsTheCount) {
    PluginLogState() = {};
    g_hostCalls.clear();
    const std::size_t dropped = 10;
    for (std::size_t i = 0; i < kPluginLogHeldLines + dropped; ++i) PluginLog("line %zu", i);
    const NvrGameContext ctx = V6Context();
    ASSERT_TRUE(PluginLogBind(&ctx));
    ASSERT_EQ(g_hostCalls.size(), kPluginLogHeldLines + 1);
    EXPECT_EQ(g_hostCalls[kPluginLogHeldLines - 1].message, "line 255");
    EXPECT_EQ(g_hostCalls.back().level, NEVR_LOG_WARNING);
    EXPECT_EQ(g_hostCalls.back().message, "lines logged before bind were dropped (buffer full)");
    ASSERT_EQ(g_hostCalls.back().fields.size(), 1u);
    EXPECT_EQ(g_hostCalls.back().fields[0], (std::pair<std::string, std::string>("count", "10")));
    PluginLogState() = {};
}

// A bind with no host log writes the held lines to stderr, in order, behind a line that says why.
TEST(PluginLoggerHeld, WithNoHostLogTheHeldLinesGoToStderrBehindANoteThatSaysSo) {
    PluginLogState() = {};
    const std::string err = CaptureStderr([] {
        PluginLog("a %d", 1);
        PluginLogError("b");
        NvrGameContext ctx = V6Context();
        ctx.log = nullptr;
        EXPECT_FALSE(PluginLogBind(&ctx));
    });
    EXPECT_EQ(err,
              "[test_plugin] WARNING no host log function; the lines logged before bind follow on stderr\n"
              "[test_plugin] INFO a 1\n"
              "[test_plugin] ERROR b\n");
    PluginLogState() = {};
}

}  // namespace
