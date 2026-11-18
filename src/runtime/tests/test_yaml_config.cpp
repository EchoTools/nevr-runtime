// The plugin SDK's YAML subset parser (sdk/include/nevr/yaml_config.h): header-only, shared by every PC and Quest plugin
// and by the built-in log filter. Each test names the input shape it pins. The cases that pin a limit of the subset
// (nested mappings, tab indentation, unindented list items, an unterminated quote) say so in their name: they are what the parser does with
// input outside the subset, so a change to them is a change to every plugin's config reading.
//
// NOT shown by these tests: a plugin's config in a run.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "nevr/yaml_config.h"

namespace {

constexpr int kFallbackInt = 7;
constexpr int kNegativeValue = -42;
constexpr int kPositiveValue = 80;
constexpr uint32_t kFallbackUint = 9;
constexpr size_t kTwoItems = 2;
constexpr size_t kThreeItems = 3;
constexpr size_t kOneItem = 1;
constexpr int kTruncatedFraction = 3;
constexpr int kThreeSpaces = 3;

const YamlValue* Find(const YamlConfig& cfg, const std::string& key) {
  const auto it = cfg.find(key);
  return it == cfg.end() ? nullptr : &it->second;
}

}  // namespace

TEST(YamlConfigScalars, EmptyTextGivesEmptyConfig) { EXPECT_TRUE(ParseYamlConfig("").empty()); }

TEST(YamlConfigScalars, OnlyCommentsAndBlankLinesGiveEmptyConfig) {
  EXPECT_TRUE(ParseYamlConfig("# a\n\n   \n  # b\n\t\n").empty());
}

TEST(YamlConfigScalars, PlainScalarIsTrimmed) {
  const YamlConfig cfg = ParseYamlConfig("name:   hello world  \n");
  ASSERT_NE(Find(cfg, "name"), nullptr);
  EXPECT_EQ(Find(cfg, "name")->str, "hello world");
  EXPECT_FALSE(Find(cfg, "name")->is_list);
}

TEST(YamlConfigScalars, LastLineWithoutNewlineIsRead) {
  const YamlConfig cfg = ParseYamlConfig("a: 1\nb: 2");
  ASSERT_NE(Find(cfg, "b"), nullptr);
  EXPECT_EQ(Find(cfg, "b")->str, "2");
}

TEST(YamlConfigScalars, CarriageReturnLineEndingsAreTrimmed) {
  const YamlConfig cfg = ParseYamlConfig("a: 1\r\nb: two\r\n");
  ASSERT_NE(Find(cfg, "a"), nullptr);
  ASSERT_NE(Find(cfg, "b"), nullptr);
  EXPECT_EQ(Find(cfg, "a")->str, "1");
  EXPECT_EQ(Find(cfg, "b")->str, "two");
}

TEST(YamlConfigScalars, DoubleAndSingleQuotesAreRemoved) {
  const YamlConfig cfg = ParseYamlConfig("d: \"x y\"\ns: 'p q'\n");
  EXPECT_EQ(Find(cfg, "d")->str, "x y");
  EXPECT_EQ(Find(cfg, "s")->str, "p q");
}

TEST(YamlConfigScalars, MismatchedQuotesAreKept) {
  const YamlConfig cfg = ParseYamlConfig("m: \"abc'\n");
  EXPECT_EQ(Find(cfg, "m")->str, "\"abc'");
}

TEST(YamlConfigScalars, LoneQuoteCharacterIsKept) {
  const YamlConfig cfg = ParseYamlConfig("q: \"\n");
  EXPECT_EQ(Find(cfg, "q")->str, "\"");
}

TEST(YamlConfigScalars, EmptyQuotesGiveEmptyString) {
  const YamlConfig cfg = ParseYamlConfig("e: \"\"\n");
  ASSERT_NE(Find(cfg, "e"), nullptr);
  EXPECT_TRUE(Find(cfg, "e")->str.empty());
}

TEST(YamlConfigScalars, ValueKeepsLaterColons) {
  const YamlConfig cfg = ParseYamlConfig("uri: http://host:8080/path\n");
  EXPECT_EQ(Find(cfg, "uri")->str, "http://host:8080/path");
}

TEST(YamlConfigScalars, LineWithoutColonIsIgnored) {
  const YamlConfig cfg = ParseYamlConfig("junk\na: 1\n");
  EXPECT_EQ(cfg.size(), kOneItem);
  EXPECT_NE(Find(cfg, "a"), nullptr);
}

TEST(YamlConfigScalars, DuplicateKeyLastWins) {
  const YamlConfig cfg = ParseYamlConfig("a: 1\na: 2\n");
  EXPECT_EQ(cfg.size(), kOneItem);
  EXPECT_EQ(Find(cfg, "a")->str, "2");
}

TEST(YamlConfigScalars, KeyWithNoValueIsPresentAndEmpty) {
  const YamlConfig cfg = ParseYamlConfig("k:\n");
  ASSERT_NE(Find(cfg, "k"), nullptr);
  EXPECT_TRUE(Find(cfg, "k")->str.empty());
  EXPECT_FALSE(Find(cfg, "k")->is_list);
  EXPECT_FALSE(Find(cfg, "k")->is_object_list);
}

TEST(YamlConfigComments, FullLineAndInlineCommentsAreStripped) {
  const YamlConfig cfg = ParseYamlConfig("# top\na: 1  # trailing\n   # indented comment\nb: 2\n");
  EXPECT_EQ(Find(cfg, "a")->str, "1");
  EXPECT_EQ(Find(cfg, "b")->str, "2");
  EXPECT_EQ(cfg.size(), kTwoItems);
}

TEST(YamlConfigComments, HashInsideDoubleQuotesIsKept) {
  const YamlConfig cfg = ParseYamlConfig("a: \"x # y\"  # real comment\n");
  EXPECT_EQ(Find(cfg, "a")->str, "x # y");
}

TEST(YamlConfigComments, HashInsideSingleQuotesIsKept) {
  const YamlConfig cfg = ParseYamlConfig("a: 'x # y'\n");
  EXPECT_EQ(Find(cfg, "a")->str, "x # y");
}

TEST(YamlConfigComments, SingleQuotesEndOnASingleQuoteAndALaterCommentIsStripped) {
  const YamlConfig cfg = ParseYamlConfig("a: 'x' # c\n");
  EXPECT_EQ(Find(cfg, "a")->str, "x");
}

TEST(YamlConfigComments, DoubleQuoteInsideSingleQuotesDoesNotEndThem) {
  const YamlConfig cfg = ParseYamlConfig("a: 'say \"# hi\"' # c\n");
  EXPECT_EQ(Find(cfg, "a")->str, "say \"# hi\"");
}

TEST(YamlConfigComments, OtherQuoteCharacterInsideQuotesDoesNotEndThem) {
  const YamlConfig cfg = ParseYamlConfig("a: \"it's # kept\"\n");
  EXPECT_EQ(Find(cfg, "a")->str, "it's # kept");
}

TEST(YamlConfigComments, UnterminatedApostropheSwallowsTheComment_LimitOfTheSubset) {
  const YamlConfig cfg = ParseYamlConfig("a: it's # c\n");
  EXPECT_EQ(Find(cfg, "a")->str, "it's # c");
}

TEST(YamlConfigComments, HashWithNoSpaceBeforeItStillStartsAComment) {
  const YamlConfig cfg = ParseYamlConfig("a: v#c\n");
  EXPECT_EQ(Find(cfg, "a")->str, "v");
}

TEST(YamlConfigLists, BlockListCollectsItemsInOrder) {
  const YamlConfig cfg = ParseYamlConfig("items:\n  - one\n  - \"two\"\n  - 'three'\n");
  const YamlValue* v = Find(cfg, "items");
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(v->is_list);
  EXPECT_FALSE(v->is_object_list);
  ASSERT_EQ(v->list.size(), kThreeItems);
  EXPECT_EQ(v->list[0], "one");
  EXPECT_EQ(v->list[1], "two");
  EXPECT_EQ(v->list[kThreeItems - 1], "three");
}

TEST(YamlConfigLists, ItemCommentsAreStripped) {
  const YamlConfig cfg = ParseYamlConfig("l:\n  - a # first\n  # between\n  - b\n");
  ASSERT_EQ(Find(cfg, "l")->list.size(), kTwoItems);
  EXPECT_EQ(Find(cfg, "l")->list[0], "a");
  EXPECT_EQ(Find(cfg, "l")->list[1], "b");
}

TEST(YamlConfigLists, ItemsBelongToTheMostRecentKey) {
  const YamlConfig cfg = ParseYamlConfig("a:\n  - 1\nb:\n  - 2\n  - 3\n");
  EXPECT_EQ(Find(cfg, "a")->list.size(), kOneItem);
  EXPECT_EQ(Find(cfg, "b")->list.size(), kTwoItems);
}

TEST(YamlConfigLists, ItemAfterAScalarKeyIsDropped) {
  const YamlConfig cfg = ParseYamlConfig("a: 1\n  - stray\n");
  EXPECT_TRUE(Find(cfg, "a")->list.empty());
  EXPECT_FALSE(Find(cfg, "a")->is_list);
}

TEST(YamlConfigLists, ItemBeforeAnyKeyIsDropped) {
  EXPECT_TRUE(ParseYamlConfig("  - stray\n").empty());
}

TEST(YamlConfigLists, BareDashIsNotAnItem) {
  const YamlConfig cfg = ParseYamlConfig("l:\n  -\n  - x\n");
  ASSERT_EQ(Find(cfg, "l")->list.size(), kOneItem);
  EXPECT_EQ(Find(cfg, "l")->list[0], "x");
}

TEST(YamlConfigLists, DashWithOnlySpacesIsNotAnItem) {
  const YamlConfig cfg = ParseYamlConfig("l:\n  -   \n");
  EXPECT_TRUE(Find(cfg, "l")->list.empty());
  EXPECT_FALSE(Find(cfg, "l")->is_list);
}

TEST(YamlConfigLists, UnindentedItemIsNotAListItem_LimitOfTheSubset) {
  const YamlConfig cfg = ParseYamlConfig("l:\n- x\n");
  EXPECT_TRUE(Find(cfg, "l")->list.empty());
  EXPECT_EQ(cfg.size(), kOneItem);
}

TEST(YamlConfigLists, TabIndentedItemIsNotAListItem_LimitOfTheSubset) {
  const YamlConfig cfg = ParseYamlConfig("l:\n\t- x\n");
  EXPECT_TRUE(Find(cfg, "l")->list.empty());
}

TEST(YamlConfigLists, NestedMappingLineUnderAKeyIsIgnored_LimitOfTheSubset) {
  const YamlConfig cfg = ParseYamlConfig("parent:\n  child: 1\nother: 2\n");
  EXPECT_EQ(cfg.size(), kTwoItems);
  EXPECT_EQ(Find(cfg, "child"), nullptr);
  EXPECT_TRUE(Find(cfg, "parent")->str.empty());
}

TEST(YamlConfigLists, RedefiningAKeyReplacesItsList) {
  const YamlConfig cfg = ParseYamlConfig("l:\n  - a\n  - b\nl:\n  - c\n");
  ASSERT_EQ(Find(cfg, "l")->list.size(), kOneItem);
  EXPECT_EQ(Find(cfg, "l")->list[0], "c");
}

TEST(YamlConfigObjectLists, InlineObjectsBecomeAnObjectList) {
  const YamlConfig cfg = ParseYamlConfig(
      "rules:\n  - {prefix: \"a\", max_length: 80}\n  - {prefix: b, max_length: 5}\n");
  const YamlValue* v = Find(cfg, "rules");
  ASSERT_NE(v, nullptr);
  EXPECT_TRUE(v->is_list);
  EXPECT_TRUE(v->is_object_list);
  EXPECT_TRUE(v->list.empty());
  ASSERT_EQ(v->object_list.size(), kTwoItems);
  EXPECT_EQ(v->object_list[0].at("prefix"), "a");
  EXPECT_EQ(v->object_list[0].at("max_length"), "80");
  EXPECT_EQ(v->object_list[1].at("prefix"), "b");
}

TEST(YamlConfigObjectLists, CommaInsideQuotesDoesNotSplitThePair) {
  const YamlConfig cfg = ParseYamlConfig("r:\n  - {k: \"a, b\", n: 1}\n");
  const auto& obj = Find(cfg, "r")->object_list.at(0);
  EXPECT_EQ(obj.at("k"), "a, b");
  EXPECT_EQ(obj.at("n"), "1");
}

TEST(YamlConfigObjectLists, QuotedKeysAreUnquoted) {
  const YamlConfig cfg = ParseYamlConfig("r:\n  - {\"k\": 'v'}\n");
  EXPECT_EQ(Find(cfg, "r")->object_list.at(0).at("k"), "v");
}

TEST(YamlConfigObjectLists, EmptyObjectIsDropped) {
  const YamlConfig cfg = ParseYamlConfig("r:\n  - {}\n");
  EXPECT_TRUE(Find(cfg, "r")->object_list.empty());
  EXPECT_FALSE(Find(cfg, "r")->is_object_list);
}

TEST(YamlConfigObjectLists, ObjectWithoutAColonIsDropped) {
  const YamlConfig cfg = ParseYamlConfig("r:\n  - {nokey}\n");
  EXPECT_TRUE(Find(cfg, "r")->object_list.empty());
}

TEST(YamlConfigObjectLists, UnclosedBraceIsDropped) {
  const YamlConfig cfg = ParseYamlConfig("r:\n  - {a: 1\n");
  EXPECT_TRUE(Find(cfg, "r")->object_list.empty());
  EXPECT_FALSE(Find(cfg, "r")->is_object_list);
}

TEST(YamlConfigObjectLists, PairWithoutColonIsSkippedOthersKept) {
  const YamlConfig cfg = ParseYamlConfig("r:\n  - {junk, a: 1}\n");
  const auto& obj = Find(cfg, "r")->object_list.at(0);
  EXPECT_EQ(obj.size(), kOneItem);
  EXPECT_EQ(obj.at("a"), "1");
}

TEST(YamlConfigObjectLists, MixedPlainAndObjectItemsKeepBothLists) {
  const YamlConfig cfg = ParseYamlConfig("r:\n  - plain\n  - {a: 1}\n");
  const YamlValue* v = Find(cfg, "r");
  EXPECT_TRUE(v->is_list);
  EXPECT_TRUE(v->is_object_list);
  EXPECT_EQ(v->list.size(), kOneItem);
  EXPECT_EQ(v->object_list.size(), kOneItem);
}

TEST(YamlConfigAsBool, AcceptsTheSixWords) {
  for (const char* word : {"true", "yes", "on"}) {
    YamlValue v;
    v.str = word;
    EXPECT_TRUE(v.as_bool(false)) << word;
  }
  for (const char* word : {"false", "no", "off"}) {
    YamlValue v;
    v.str = word;
    EXPECT_FALSE(v.as_bool(true)) << word;
  }
}

TEST(YamlConfigAsBool, OtherTextGivesTheFallback) {
  for (const char* word : {"", "True", "YES", "1", "0", "maybe"}) {
    YamlValue v;
    v.str = word;
    EXPECT_TRUE(v.as_bool(true)) << word;
    EXPECT_FALSE(v.as_bool(false)) << word;
  }
}

TEST(YamlConfigAsInt, ParsesDecimalIncludingNegative) {
  YamlValue v;
  v.str = "-42";
  EXPECT_EQ(v.as_int(kFallbackInt), kNegativeValue);
  v.str = "80";
  EXPECT_EQ(v.as_int(kFallbackInt), kPositiveValue);
}

TEST(YamlConfigAsInt, EmptyAndNonNumericGiveTheFallback) {
  YamlValue v;
  EXPECT_EQ(v.as_int(kFallbackInt), kFallbackInt);
  v.str = "abc";
  EXPECT_EQ(v.as_int(kFallbackInt), kFallbackInt);
  v.str = "true";
  EXPECT_EQ(v.as_int(kFallbackInt), kFallbackInt);
}

TEST(YamlConfigAsInt, LeadingDigitsWinOverTrailingText) {
  YamlValue v;
  v.str = "80px";
  EXPECT_EQ(v.as_int(kFallbackInt), kPositiveValue);
}

TEST(YamlConfigAsInt, AFractionReadsItsIntegerPart) {
  YamlValue v;
  v.str = "3.9";
  EXPECT_EQ(v.as_int(kFallbackInt), kTruncatedFraction);
}

TEST(YamlConfigAsUint32, ParsesAndFallsBack) {
  YamlValue v;
  v.str = "80";
  EXPECT_EQ(v.as_uint32(kFallbackUint), static_cast<uint32_t>(kPositiveValue));
  v.str = "x";
  EXPECT_EQ(v.as_uint32(kFallbackUint), kFallbackUint);
  v.str.clear();
  EXPECT_EQ(v.as_uint32(kFallbackUint), kFallbackUint);
}

TEST(YamlConfigHelpers, TrimUnquoteIndent) {
  EXPECT_EQ(yaml_trim(" \t\r\n"), "");
  EXPECT_EQ(yaml_trim("  a b \r\n"), "a b");
  EXPECT_EQ(yaml_unquote("\"a\""), "a");
  EXPECT_EQ(yaml_unquote("'a'"), "a");
  EXPECT_EQ(yaml_unquote("\"a'"), "\"a'");
  EXPECT_EQ(yaml_unquote("a"), "a");
  EXPECT_EQ(yaml_indent("   x"), kThreeSpaces);
  EXPECT_EQ(yaml_indent("\tx"), 0);
  EXPECT_EQ(yaml_indent(""), 0);
}
