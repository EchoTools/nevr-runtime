// #34: plugins log through plugin_logger.h; every line carries a level so it can be filtered.

#include <gtest/gtest.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "plugin_logger.h"

namespace {

std::string Format(const char* level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::string line = nevr::FormatPluginLog("[p]", level, fmt, args);
    va_end(args);
    return line;
}

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

// The macro-generated functions write one line per call to stderr, with the level their name says.
TEST(PluginLogger, GeneratedFunctionsWriteTheirLevelToStderr) {
    const char* temp = std::getenv("TEMP");
    const std::string path = std::string(temp != nullptr ? temp : ".") + "\\plugin_logger_test.txt";
    ASSERT_NE(std::freopen(path.c_str(), "w", stderr), nullptr);
    PluginLog("a %d", 1);
    PluginLogWarning("b %d", 2);
    PluginLogError("c %d", 3);
    std::fflush(stderr);
    std::FILE* in = std::fopen(path.c_str(), "r");
    ASSERT_NE(in, nullptr);
    char buf[256] = {};
    const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, in);
    std::fclose(in);
    std::remove(path.c_str());
    EXPECT_EQ(std::string(buf, n), "[test_plugin] INFO a 1\n[test_plugin] WARNING b 2\n[test_plugin] ERROR c 3\n");
}

}  // namespace
