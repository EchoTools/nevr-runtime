// #963: the guild picker's pure logic (patch/guild_picker.h): what the eligible-guilds answer means, the default
// choice, and the refusal of a guild the answer did not list. No game, no socket, no hook.

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/patch/guild_picker.h"

namespace {

using nevr_guild::Guild;

std::string Answer(const nlohmann::json& guilds) {
  return nlohmann::json{{"guilds", guilds}}.dump();
}

TEST(GuildPicker, ReadsIdLabelAndBorrowedFlag) {
  const auto eligible = nevr_guild::ParseEligible(Answer({
      {{"id", "g-own"}, {"label", "Home Guild"}, {"borrowed", false}},
      {{"id", "g-lend"}, {"label", "Lent Guild"}, {"borrowed", true}},
  }));
  ASSERT_TRUE(eligible.has_value());
  ASSERT_EQ(eligible->size(), 2u);
  EXPECT_EQ((*eligible)[0].id, "g-own");
  EXPECT_EQ((*eligible)[0].label, "Home Guild");
  EXPECT_FALSE((*eligible)[0].borrowed);
  EXPECT_EQ((*eligible)[1].id, "g-lend");
  EXPECT_TRUE((*eligible)[1].borrowed);
}

TEST(GuildPicker, BorrowedDefaultsToFalseWhenAbsent) {
  const auto eligible = nevr_guild::ParseEligible(Answer({{{"id", "g1"}, {"label", "One"}}}));
  ASSERT_TRUE(eligible.has_value());
  ASSERT_EQ(eligible->size(), 1u);
  EXPECT_FALSE((*eligible)[0].borrowed);
}

TEST(GuildPicker, AnAnswerThatIsNotTheShapeIsRefused) {
  EXPECT_FALSE(nevr_guild::ParseEligible("").has_value());
  EXPECT_FALSE(nevr_guild::ParseEligible("not json").has_value());
  EXPECT_FALSE(nevr_guild::ParseEligible("[1,2,3]").has_value());
  EXPECT_FALSE(nevr_guild::ParseEligible("{}").has_value());
  EXPECT_FALSE(nevr_guild::ParseEligible(R"({"guilds":"g1"})").has_value());
}

TEST(GuildPicker, AnOversizedAnswerIsRefused) {
  const std::string huge(nevr_guild::kAnswerMaxBytes + 1, ' ');
  EXPECT_FALSE(nevr_guild::ParseEligible(huge).has_value());
}

TEST(GuildPicker, MalformedEntriesAreSkippedNotGuessedAt) {
  const auto eligible = nevr_guild::ParseEligible(Answer({
      {{"label", "No id"}},                                    // no id
      {{"id", 7}, {"label", "Numeric id"}},                    // id not a string
      {{"id", "g-nolabel"}},                                   // no label
      {{"id", ""}, {"label", "Empty id"}},                     // empty id
      {{"id", "g-bad-borrow"}, {"label", "X"}, {"borrowed", "yes"}},  // borrowed not a boolean
      {{"id", "g-ok"}, {"label", "Kept"}},
  }));
  ASSERT_TRUE(eligible.has_value());
  ASSERT_EQ(eligible->size(), 1u);
  EXPECT_EQ((*eligible)[0].id, "g-ok");
}

TEST(GuildPicker, NonPrintableIdOrLabelIsSkipped) {
  const auto eligible = nevr_guild::ParseEligible(Answer({
      {{"id", "g\x01"}, {"label", "Ctrl id"}},
      {{"id", "g-utf8"}, {"label", "Gilde \xc3\xbc"}},  // non-ASCII label: the font draws none of it
      {{"id", "g-fine"}, {"label", "Fine"}},
  }));
  ASSERT_TRUE(eligible.has_value());
  ASSERT_EQ(eligible->size(), 1u);
  EXPECT_EQ((*eligible)[0].id, "g-fine");
}

TEST(GuildPicker, AnIdTooLongIsSkipped) {
  const std::string long_id(nevr_guild::kIdMaxChars + 1, 'a');
  const auto eligible = nevr_guild::ParseEligible(Answer({
      {{"id", long_id}, {"label", "Too long"}},
      {{"id", "g-ok"}, {"label", "Ok"}},
  }));
  ASSERT_TRUE(eligible.has_value());
  ASSERT_EQ(eligible->size(), 1u);
  EXPECT_EQ((*eligible)[0].id, "g-ok");
}

TEST(GuildPicker, ALongLabelIsCutToThePickerWidth) {
  const std::string long_label(100, 'L');
  const auto eligible = nevr_guild::ParseEligible(Answer({{{"id", "g1"}, {"label", long_label}}}));
  ASSERT_TRUE(eligible.has_value());
  ASSERT_EQ(eligible->size(), 1u);
  EXPECT_EQ((*eligible)[0].label.size(), nevr_guild::kLabelMaxChars);
}

TEST(GuildPicker, ARepeatedIdKeepsItsFirstEntry) {
  const auto eligible = nevr_guild::ParseEligible(Answer({
      {{"id", "g1"}, {"label", "First"}, {"borrowed", false}},
      {{"id", "g1"}, {"label", "Second"}, {"borrowed", true}},
  }));
  ASSERT_TRUE(eligible.has_value());
  ASSERT_EQ(eligible->size(), 1u);
  EXPECT_EQ((*eligible)[0].label, "First");
  EXPECT_FALSE((*eligible)[0].borrowed);
}

TEST(GuildPicker, AnEmptyListOffersNoCreate) {
  const auto eligible = nevr_guild::ParseEligible(Answer(nlohmann::json::array()));
  ASSERT_TRUE(eligible.has_value());
  EXPECT_TRUE(eligible->empty());
  EXPECT_FALSE(nevr_guild::DefaultChoice(*eligible).has_value());
}

TEST(GuildPicker, DefaultIsTheFirstGuildTheCallerBelongsTo) {
  const std::vector<Guild> eligible = {
      {"g-lend", "Lent", true},
      {"g-own", "Own", false},
      {"g-own2", "Own two", false},
  };
  const auto choice = nevr_guild::DefaultChoice(eligible);
  ASSERT_TRUE(choice.has_value());
  EXPECT_EQ(choice->id, "g-own");
}

TEST(GuildPicker, DefaultFallsBackToABorrowableGuildWhenNoneIsOwn) {
  const std::vector<Guild> eligible = {{"g-lend", "Lent", true}};
  const auto choice = nevr_guild::DefaultChoice(eligible);
  ASSERT_TRUE(choice.has_value());
  EXPECT_EQ(choice->id, "g-lend");
  EXPECT_TRUE(choice->borrowed);
}

TEST(GuildPicker, ChooseAcceptsOnlyAGuildTheAnswerListed) {
  const std::vector<Guild> eligible = {{"g-own", "Own", false}, {"g-lend", "Lent", true}};
  const auto listed = nevr_guild::Choose(eligible, "g-lend");
  ASSERT_TRUE(listed.has_value());
  EXPECT_TRUE(listed->borrowed);
  EXPECT_FALSE(nevr_guild::Choose(eligible, "g-other").has_value());
  EXPECT_FALSE(nevr_guild::Choose(eligible, "").has_value());
}

TEST(GuildPicker, TheCreateFieldCarriesTheChosenIdOnly) {
  const Guild guild{"g-own", "Own", false};
  const nlohmann::json field = nevr_guild::CreateGuildField(guild);
  ASSERT_TRUE(field.is_object());
  ASSERT_EQ(field.size(), 1u);
  EXPECT_EQ(field.at(std::string(nevr_guild::kCreateGuildKey)), "g-own");
}

}  // namespace
