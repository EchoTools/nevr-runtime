// NevrConfig::Set — the first config.yaml writer (#1986). Every test goes through the interface (bind a file, Set, read the
// file's bytes back, read the value back through the getter); none calls the splice. Format preservation is the point: a
// commented file keeps every byte but the key's value.
//
// The same cases run on the Quest build in src/quest/tests/nevr_config_set_test.cpp.

#include <gtest/gtest.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "core/nevr_config.h"
#include "core/nevr_config_testing.h"

namespace {

namespace fs = std::filesystem;

class Dir {
 public:
  Dir() {
    static int counter = 0;
    path_ = fs::temp_directory_path() / ("nevr_config_set_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                                         std::to_string(counter++));
    fs::create_directories(path_);
  }
  ~Dir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  std::string File() const { return (path_ / "config.yaml").string(); }
  std::string Tmp() const { return File() + ".tmp"; }

 private:
  fs::path path_;
};

void Write(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}
std::string Read(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Exists(const std::string& path) { return fs::exists(path); }

nevr::ConfigSetResult SetOn(const std::string& file, bool value, const char* key = "graphics.hand_flip") {
  nevr::NevrConfig cfg;
  cfg.BindFile(file);
  return cfg.Set(key, value);
}

// A commented file keeps every byte except the key's value, in both directions.
TEST(NevrConfigSet, ACommentedFileKeepsEveryByteButTheValue) {
  const std::string before =
      "# nEVR config\n"
      "version: 1   # schema\n"
      "\n"
      "services:\n"
      "  socket_uri: wss://example.invalid/ws   # comment\n"
      "\n"
      "graphics:\n"
      "  # the level\n"
      "  texture_quality: high\n"
      "  hand_flip: false   # set by the menu\n"
      "  # trailing note\n"
      "\n"
      "social:\n"
      "  facade: true\n";
  Dir dir;
  Write(dir.File(), before);
  const nevr::ConfigSetResult on = SetOn(dir.File(), true);
  ASSERT_TRUE(on.ok) << on.reason;
  EXPECT_TRUE(on.wrote);
  std::string expected = before;
  expected.replace(expected.find("hand_flip: false") + 11, 5, "true");
  EXPECT_EQ(Read(dir.File()), expected);
  const nevr::ConfigSetResult off = SetOn(dir.File(), false);
  ASSERT_TRUE(off.ok) << off.reason;
  EXPECT_EQ(Read(dir.File()), before);  // and back: the original, byte for byte
  EXPECT_FALSE(Exists(dir.Tmp()));
}

// A graphics block without the key gets it at the block's child indentation, after the last content line and before the
// comments and blank lines that separate it from the next block.
TEST(NevrConfigSet, AnInsertGoesAtTheEndOfTheGraphicsBlockAtItsIndentation) {
  Dir dir;
  Write(dir.File(), "graphics:\n    texture_quality: high   # four spaces here\n\n# about social\nsocial:\n  facade: true\n");
  ASSERT_TRUE(SetOn(dir.File(), true).ok);
  EXPECT_EQ(Read(dir.File()),
            "graphics:\n    texture_quality: high   # four spaces here\n    hand_flip: true\n\n# about social\nsocial:\n  facade: true\n");
}

TEST(NevrConfigSet, AnInsertIntoABlockAtTheEndOfAFileWithoutATrailingNewline) {
  Dir dir;
  Write(dir.File(), "graphics:\n  texture_quality: low");
  ASSERT_TRUE(SetOn(dir.File(), true).ok);
  EXPECT_EQ(Read(dir.File()), "graphics:\n  texture_quality: low\n  hand_flip: true\n");
}

TEST(NevrConfigSet, ASectionWithNothingUnderItTakesTheKeyOnTheNextLine) {
  Dir dir;
  Write(dir.File(), "graphics:   # nothing yet\nsocial:\n  facade: true\n");
  ASSERT_TRUE(SetOn(dir.File(), false).ok);
  EXPECT_EQ(Read(dir.File()), "graphics:   # nothing yet\n  hand_flip: false\nsocial:\n  facade: true\n");
}

TEST(NevrConfigSet, NoGraphicsBlockAppendsOne) {
  Dir dir;
  Write(dir.File(), "social:\n  facade: true\n");
  ASSERT_TRUE(SetOn(dir.File(), true).ok);
  EXPECT_EQ(Read(dir.File()), "social:\n  facade: true\ngraphics:\n  hand_flip: true\n");
}

TEST(NevrConfigSet, AMissingFileIsCreatedHoldingJustTheKey) {
  Dir dir;
  ASSERT_FALSE(Exists(dir.File()));
  const nevr::ConfigSetResult r = SetOn(dir.File(), true);
  ASSERT_TRUE(r.ok) << r.reason;
  EXPECT_TRUE(r.wrote);
  EXPECT_EQ(Read(dir.File()), "graphics:\n  hand_flip: true\n");
  EXPECT_FALSE(Exists(dir.Tmp()));
}

// Quoted and unquoted values: the quotes stay, only what is inside changes; an unquoted synonym is replaced by the word.
TEST(NevrConfigSet, QuotedAndUnquotedValues) {
  struct Case {
    const char* before;
    bool value;
    const char* after;
  };
  const Case cases[] = {
      {"graphics:\n  hand_flip: false\n", true, "graphics:\n  hand_flip: true\n"},
      {"graphics:\n  hand_flip: true\n", false, "graphics:\n  hand_flip: false\n"},
      {"graphics:\n  hand_flip: no   # off\n", true, "graphics:\n  hand_flip: true   # off\n"},
      {"graphics:\n  hand_flip: \"false\"\n", true, "graphics:\n  hand_flip: \"true\"\n"},
      {"graphics:\n  hand_flip: 'true'   # q\n", false, "graphics:\n  hand_flip: 'false'   # q\n"},
  };
  for (const Case& c : cases) {
    Dir dir;
    Write(dir.File(), c.before);
    const nevr::ConfigSetResult r = SetOn(dir.File(), c.value);
    ASSERT_TRUE(r.ok) << c.before << " -> " << r.reason;
    EXPECT_EQ(Read(dir.File()), c.after) << c.before;
  }
}

// The value already reads as asked: nothing is written, whatever its spelling.
TEST(NevrConfigSet, AValueThatAlreadyReadsAsAskedIsLeftAlone) {
  Dir dir;
  const std::string before = "graphics:\n  hand_flip: yes   # on\n";
  Write(dir.File(), before);
  const nevr::ConfigSetResult r = SetOn(dir.File(), true);
  ASSERT_TRUE(r.ok);
  EXPECT_FALSE(r.wrote);
  EXPECT_EQ(Read(dir.File()), before);
  EXPECT_FALSE(Exists(dir.Tmp()));
}

// Only the key under `graphics` is the target: the same name elsewhere, or deeper, is not touched.
TEST(NevrConfigSet, ASameNamedKeyElsewhereIsNotTouched) {
  Dir dir;
  Write(dir.File(),
        "network:\n  hand_flip: true\n"
        "graphics:\n  texture_quality: low\n  extra:\n    hand_flip: true\n"
        "plugins: []\n");
  ASSERT_TRUE(SetOn(dir.File(), false).ok);
  EXPECT_EQ(Read(dir.File()),
            "network:\n  hand_flip: true\n"
            "graphics:\n  texture_quality: low\n  extra:\n    hand_flip: true\n  hand_flip: false\n"
            "plugins: []\n");
}

TEST(NevrConfigSet, ACrLfFileKeepsItsLineEndings) {
  Dir dir;
  Write(dir.File(), "graphics:\r\n  texture_quality: high\r\nsocial:\r\n  facade: true\r\n");
  ASSERT_TRUE(SetOn(dir.File(), true).ok);
  EXPECT_EQ(Read(dir.File()), "graphics:\r\n  texture_quality: high\r\n  hand_flip: true\r\nsocial:\r\n  facade: true\r\n");
}

TEST(NevrConfigSet, AKeyWithNoValueIsFilledIn) {
  Dir dir;
  Write(dir.File(), "graphics:\n  hand_flip:   # empty\n  texture_quality: low\n");
  ASSERT_TRUE(SetOn(dir.File(), true).ok);
  EXPECT_EQ(Read(dir.File()), "graphics:\n  hand_flip: true   # empty\n  texture_quality: low\n");
}

// Refusals write nothing: the bytes are the same and no temporary file is left.
void ExpectRefused(const std::string& text, const char* reason) {
  Dir dir;
  Write(dir.File(), text);
  const nevr::ConfigSetResult r = SetOn(dir.File(), true);
  EXPECT_FALSE(r.ok) << text;
  EXPECT_FALSE(r.wrote) << text;
  EXPECT_STREQ(r.reason, reason) << text;
  EXPECT_EQ(Read(dir.File()), text);
  EXPECT_FALSE(Exists(dir.Tmp()));
}

TEST(NevrConfigSet, AFlowStyleGraphicsBlockIsRefused) {
  ExpectRefused("graphics: {texture_quality: high}\n", "section_flow_style");
  ExpectRefused("graphics: { texture_quality: high, hand_flip: false }\n", "section_flow_style");
}

TEST(NevrConfigSet, ATruncatedOrBrokenFileIsRefused) {
  ExpectRefused("graphics:\n  texture_quality: \"hig", "unparseable");   // cut inside a quoted value
  ExpectRefused("graphics: {texture_quality: high\n", "unparseable");    // cut inside a flow mapping
  ExpectRefused("graphics:\n  texture_quality: [high,\n", "unparseable");
}

TEST(NevrConfigSet, ShapesThatAreNotASafePlaceAreRefused) {
  ExpectRefused("graphics: high\n", "section_not_a_mapping");
  ExpectRefused("graphics: ~\n", "section_not_a_mapping");
  ExpectRefused("graphics:\n  - a\n  - b\n", "section_not_a_mapping");
  ExpectRefused("graphics:\n  hand_flip: [true]\n", "value_not_a_scalar");
  ExpectRefused("graphics:\n  hand_flip: {a: 1}\n", "value_not_a_scalar");
  ExpectRefused("graphics:\n  hand_flip: !!str false\n", "value_not_plain");
  ExpectRefused("graphics:\n  hand_flip: false\n  hand_flip: false\n", "duplicate_key");
  ExpectRefused("graphics:\n  texture_quality: low\ngraphics:\n  hand_flip: false\n", "duplicate_section");
  ExpectRefused("graphics:\n  hand_flip: false\n---\nsocial:\n  facade: true\n", "multi_document");
  ExpectRefused("- a\n- b\n", "not_a_mapping");
  ExpectRefused("{graphics: {hand_flip: false}}\n", "root_flow_style");
}

TEST(NevrConfigSet, NoBoundFileAndAnUnsupportedPathAreRefused) {
  nevr::NevrConfig unbound;
  EXPECT_STREQ(unbound.Set("graphics.hand_flip", true).reason, "no_file");
  Dir dir;
  nevr::NevrConfig cfg;
  cfg.BindFile(dir.File());
  for (const char* path : {"hand_flip", "a.b.c", ".x", "x.", ""}) {
    EXPECT_STREQ(cfg.Set(path, true).reason, "unsupported_path") << path;
  }
  EXPECT_FALSE(Exists(dir.File()));
}

TEST(NevrConfigSet, ADirectoryAtThePathIsNotAFile) {
  Dir dir;
  fs::create_directories(dir.File());
  const nevr::ConfigSetResult r = SetOn(dir.File(), true);
  EXPECT_FALSE(r.ok);
  EXPECT_STREQ(r.reason, "not_a_regular_file");
}

int FailingRename(const char*, const char*) { return EXDEV; }

// A rename that fails leaves the previous file intact and no temporary file behind; the failure carries the code.
TEST(NevrConfigSet, AFailedRenameLeavesThePreviousFileIntact) {
  Dir dir;
  const std::string before = "# keep me\ngraphics:\n  hand_flip: false\n";
  Write(dir.File(), before);
  nevr::config_testing::SetRenameForTest(&FailingRename);
  const nevr::ConfigSetResult r = SetOn(dir.File(), true);
  nevr::config_testing::SetRenameForTest(nullptr);
  EXPECT_FALSE(r.ok);
  EXPECT_FALSE(r.wrote);
  EXPECT_STREQ(r.reason, "rename_failed");
  EXPECT_EQ(r.err, EXDEV);
  EXPECT_EQ(Read(dir.File()), before);
  EXPECT_FALSE(Exists(dir.Tmp()));
  EXPECT_TRUE(SetOn(dir.File(), true).ok);  // and with the real rename it goes through
}

// Set then read, through the interface: the same object's getter, and a fresh load of the file, both read the value.
TEST(NevrConfigSet, SetThenReadRoundTrip) {
  Dir dir;
  Write(dir.File(), "graphics:\n  texture_quality: high\n");
  nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromFile(dir.File());
  EXPECT_EQ(cfg.BoundFile(), dir.File());
  EXPECT_FALSE(cfg.GetBool("graphics.hand_flip").has_value());
  ASSERT_TRUE(cfg.Set("graphics.hand_flip", true).ok);
  ASSERT_TRUE(cfg.GetBool("graphics.hand_flip").has_value());
  EXPECT_TRUE(*cfg.GetBool("graphics.hand_flip"));
  EXPECT_EQ(*cfg.GetString("graphics.texture_quality"), "high");
  const nevr::NevrConfig reloaded = nevr::NevrConfig::LoadFromFile(dir.File());
  ASSERT_TRUE(reloaded.GetBool("graphics.hand_flip").has_value());
  EXPECT_TRUE(*reloaded.GetBool("graphics.hand_flip"));
  EXPECT_EQ(reloaded.UnknownKeyCount(), 0U);  // hand_flip is on the schema
  ASSERT_TRUE(cfg.Set("graphics.hand_flip", false).ok);
  EXPECT_FALSE(*cfg.GetBool("graphics.hand_flip"));
  EXPECT_FALSE(*nevr::NevrConfig::LoadFromFile(dir.File()).GetBool("graphics.hand_flip"));
}

}  // namespace

// ---- a plain word: the player's texture level (graphics.texture_quality) ------------------------------------------------------

namespace {

nevr::ConfigSetResult SetWord(const std::string& file, const std::string& word, const char* key = "graphics.texture_quality") {
  nevr::NevrConfig cfg;
  cfg.BindFile(file);
  return cfg.Set(key, word);
}

}  // namespace

// The level the player picks replaces the old one in place; every other byte of a commented file stays.
TEST(NevrConfigSetWord, ACommentedFileKeepsEveryByteButTheWord) {
  const std::string before =
      "# nEVR config\n"
      "graphics:\n"
      "  # the level the headset renders at\n"
      "  texture_quality: high   # the player's pick\n"
      "  hand_flip: true\n"
      "\n"
      "social:\n"
      "  facade: true\n";
  Dir dir;
  Write(dir.File(), before);
  const nevr::ConfigSetResult r = SetWord(dir.File(), "very_high");
  ASSERT_TRUE(r.ok) << r.reason;
  EXPECT_TRUE(r.wrote);
  std::string expected = before;
  expected.replace(expected.find("high   # the player's pick"), 4, "very_high");
  EXPECT_EQ(Read(dir.File()), expected);
  ASSERT_TRUE(SetWord(dir.File(), "high").ok);
  EXPECT_EQ(Read(dir.File()), before);  // and back, byte for byte
  EXPECT_FALSE(Exists(dir.Tmp()));
}

TEST(NevrConfigSetWord, AWordThatIsAlreadyThereIsNotRewritten) {
  Dir dir;
  Write(dir.File(), "graphics:\n  texture_quality: \"ultra\"\n");
  const nevr::ConfigSetResult r = SetWord(dir.File(), "ultra");
  ASSERT_TRUE(r.ok) << r.reason;
  EXPECT_FALSE(r.wrote);
  EXPECT_EQ(Read(dir.File()), "graphics:\n  texture_quality: \"ultra\"\n");
}

TEST(NevrConfigSetWord, QuotedAndUnquotedValuesKeepTheirQuotes) {
  Dir quoted;
  Write(quoted.File(), "graphics:\n  texture_quality: 'low'\n");
  ASSERT_TRUE(SetWord(quoted.File(), "medium").ok);
  EXPECT_EQ(Read(quoted.File()), "graphics:\n  texture_quality: 'medium'\n");
  Dir bare;
  Write(bare.File(), "graphics:\n  texture_quality: low\n");
  ASSERT_TRUE(SetWord(bare.File(), "medium").ok);
  EXPECT_EQ(Read(bare.File()), "graphics:\n  texture_quality: medium\n");
}

// A block without the key gets it at the block's indentation; a missing file is created holding just the key.
TEST(NevrConfigSetWord, AnInsertAndACreatedFile) {
  Dir block;
  Write(block.File(), "graphics:\n    hand_flip: true   # four spaces\n\nsocial:\n  facade: true\n");
  ASSERT_TRUE(SetWord(block.File(), "high").ok);
  EXPECT_EQ(Read(block.File()), "graphics:\n    hand_flip: true   # four spaces\n    texture_quality: high\n\nsocial:\n  facade: true\n");
  Dir missing;
  ASSERT_TRUE(SetWord(missing.File(), "low").ok);
  EXPECT_EQ(Read(missing.File()), "graphics:\n  texture_quality: low\n");
}

TEST(NevrConfigSetWord, FlowStyleAndBrokenFilesAreRefusedAndNothingIsWritten) {
  const struct {
    const char* text;
    const char* reason;
  } cases[] = {
      {"graphics: {texture_quality: low}\n", "section_flow_style"},
      {"graphics:\n  texture_quality: [low, high]\n", "value_not_a_scalar"},
      {"graphics:\n  texture_quality: low\n  broken: [unclosed\n", "unparseable"},
  };
  for (const auto& c : cases) {
    Dir dir;
    Write(dir.File(), c.text);
    const nevr::ConfigSetResult r = SetWord(dir.File(), "high");
    EXPECT_FALSE(r.ok) << c.text;
    EXPECT_STREQ(r.reason, c.reason) << c.text;
    EXPECT_EQ(Read(dir.File()), c.text) << "a refusal changed the file";
    EXPECT_FALSE(Exists(dir.Tmp()));
  }
}

// Only a plain word is written: text that YAML would read as another type, or that needs quoting, is refused before the file is read.
TEST(NevrConfigSetWord, ATextThatIsNotAPlainWordIsRefused) {
  for (const char* word : {"", "high: 1", "a b", "\"high\"", "#high", "true", "No", "NULL", "-high", "high\n", "x\ty"}) {
    Dir dir;
    Write(dir.File(), "graphics:\n  texture_quality: low\n");
    const nevr::ConfigSetResult r = SetWord(dir.File(), word);
    EXPECT_FALSE(r.ok) << "[" << word << "]";
    EXPECT_STREQ(r.reason, "value_not_a_plain_word") << "[" << word << "]";
    EXPECT_EQ(Read(dir.File()), "graphics:\n  texture_quality: low\n");
  }
}

// The getter reads what was written, so the Quest's next read of the key in this process sees the choice.
TEST(NevrConfigSetWord, SetThenReadRoundTrip) {
  Dir dir;
  Write(dir.File(), "graphics:\n  texture_quality: low\n");
  nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromFile(dir.File());
  ASSERT_TRUE(cfg.Set("graphics.texture_quality", std::string("ultra")).ok);
  EXPECT_EQ(cfg.GetString("graphics.texture_quality").value_or(""), "ultra");
  const nevr::NevrConfig again = nevr::NevrConfig::LoadFromFile(dir.File());
  EXPECT_EQ(again.GetString("graphics.texture_quality").value_or(""), "ultra");
}
