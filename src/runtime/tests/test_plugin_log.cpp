// #598: the host line a plugin's ctx->log call becomes: tag, plugin identity, message, fields.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "runtime/ext/plugin_log.h"

namespace {

using nevr_plugin_log::Format;

TEST(PluginLogFormat, LineCarriesTagIdentityMessageAndFields) {
    const NevrLogField fields[] = {{"answer", "42"}, {"mode", "arena"}};
    const auto out = Format("combat-mod", NEVR_LOG_WARNING, "hook skipped", fields, 2);
    EXPECT_EQ(out.line, "[NEVR.PLUGIN] plugin=combat-mod hook skipped answer=42 mode=arena");
    EXPECT_EQ(out.level, static_cast<uint32_t>(NEVR_LOG_WARNING));
}

TEST(PluginLogFormat, EveryLevelIsKept) {
    for (const uint32_t level : {NEVR_LOG_DEBUG, NEVR_LOG_INFO, NEVR_LOG_WARNING, NEVR_LOG_ERROR}) {
        EXPECT_EQ(Format("p", level, "m", nullptr, 0).level, level);
    }
}

TEST(PluginLogFormat, OutOfRangeLevelIsAWarningThatNamesIt) {
    const auto out = Format("p", 77, "m", nullptr, 0);
    EXPECT_EQ(out.level, static_cast<uint32_t>(NEVR_LOG_WARNING));
    EXPECT_EQ(out.line, "[NEVR.PLUGIN] plugin=p m level_invalid=77");
}

TEST(PluginLogFormat, ValuesWithSpacesQuotesOrEqualsAreQuoted) {
    const NevrLogField fields[] = {{"a", "two words"}, {"b", "x=y"}, {"c", "say \"hi\""}, {"d", ""}, {"e", nullptr}};
    const auto out = Format("p", NEVR_LOG_INFO, "m", fields, 5);
    EXPECT_EQ(out.line,
              "[NEVR.PLUGIN] plugin=p m a=\"two words\" b=\"x=y\" c=\"say \\\"hi\\\"\" d=\"\" e=\"\"");
}

TEST(PluginLogFormat, APluginCannotStartASecondLineOrSpoofAField) {
    const NevrLogField fields[] = {{"k", "v\nplugin=other"}, {"bad key\n", "1"}};
    const auto out = Format("p", NEVR_LOG_INFO, "first\r\n[NEVR.PLUGIN] plugin=other forged", fields, 2);
    EXPECT_EQ(out.line.find('\n'), std::string::npos);
    EXPECT_EQ(out.line.find('\r'), std::string::npos);
    EXPECT_EQ(out.line,
              "[NEVR.PLUGIN] plugin=p first  [NEVR.PLUGIN] plugin=other forged k=\"v plugin=other\" bad_key_=1");
}

TEST(PluginLogFormat, TheIdentityIsSanitisedAndNeverEmpty) {
    EXPECT_EQ(Format("my plugin\n", NEVR_LOG_INFO, "m", nullptr, 0).line, "[NEVR.PLUGIN] plugin=my_plugin_ m");
    EXPECT_EQ(Format(nullptr, NEVR_LOG_INFO, "m", nullptr, 0).line, "[NEVR.PLUGIN] plugin=unknown m");
    EXPECT_EQ(Format("", NEVR_LOG_INFO, "m", nullptr, 0).line, "[NEVR.PLUGIN] plugin=unknown m");
}

TEST(PluginLogFormat, NullMessageAndNullKeysAreTolerated) {
    const NevrLogField fields[] = {{nullptr, "orphan"}, {"ok", "1"}};
    const auto out = Format("p", NEVR_LOG_INFO, nullptr, fields, 2);
    EXPECT_EQ(out.line, "[NEVR.PLUGIN] plugin=p ok=1 fields_dropped=1");
    EXPECT_EQ(Format("p", NEVR_LOG_INFO, "m", nullptr, 5).line, "[NEVR.PLUGIN] plugin=p m");
}

TEST(PluginLogFormat, AMessageIsCappedAndMarked) {
    const std::string big(5000, 'x');
    const auto out = Format("p", NEVR_LOG_INFO, big.c_str(), nullptr, 0);
    EXPECT_EQ(out.line, "[NEVR.PLUGIN] plugin=p " + std::string(nevr_plugin_log::kMaxMessageBytes, 'x') + "...");
}

TEST(PluginLogFormat, TheWholeLineStaysUnderTheBootBufferAndDroppedFieldsAreCounted) {
    const std::string big(5000, 'v');
    std::vector<NevrLogField> fields(40, NevrLogField{"key", big.c_str()});
    const std::string msg(5000, 'm');
    const auto out = Format("p", NEVR_LOG_INFO, msg.c_str(), fields.data(), static_cast<uint32_t>(fields.size()));
    EXPECT_LT(out.line.size(), 2048u) << "Log() formats into 2048 bytes while the runtime boots";
    EXPECT_NE(out.line.find(" fields_dropped="), std::string::npos);
}

}  // namespace
