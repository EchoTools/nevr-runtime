// NevrConfig::Set — the first config.yaml writer (#2060). Every test goes through the interface (bind a file, Set, read the
// file's bytes back, read the value back through the getter); none calls the splice. Format preservation is the point: a
// commented file keeps every byte but the key's value.
//
// The same cases run on the Quest build in src/quest/tests/nevr_config_set_test.cpp.

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

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
  // The same file spelled another way (up one level and back down): one file, a different string.
  std::string OtherSpelling() const { return (path_ / ".." / path_.filename() / "config.yaml").string(); }

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
      "# nevr config\n"
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
      "# nevr config\n"
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
  for (const char* word : {"", "high: 1", "a b", "\"high\"", "#high", "true", "No", "NULL", "-high", "high\n", "x\ty",
                           "123", "1.5", "1e3", "0x1f", "1_000", ".5", ".inf"}) {
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

// ---- two callers in one process ------------------------------------------------------------------------------------------------

namespace {

// Two threads each edit their own key of one config.yaml, many times at once, each through a config object of its own bound to the
// file (the way two pollers do). One sets `graphics.hand_flip` and flips it every time, the other sets `graphics.texture_quality`
// and cycles four words, so every call writes. Neither key may be lost, every call must succeed, and at the end each key holds the
// last value its thread set. `fileForSecond` is the path the second thread binds: the same file, perhaps spelled another way.
struct RoundOutcome {
  int failures = 0;
  std::string firstFailure;
  std::string text;
  std::optional<bool> handFlip;
  std::optional<std::string> textureQuality;
  bool tmpLeft = false;
};

constexpr int kSetsPerThread = 120;
const char* const kWords[] = {"low", "medium", "high", "very_high"};

RoundOutcome RunRound(const Dir& dir, const std::string& fileForSecond) {
  Write(dir.File(), "social:\n  facade: true\n");
  std::atomic<int> failures{0};
  std::atomic<bool> go{false};
  std::string firstFailure;
  std::atomic_flag firstFailureTaken = ATOMIC_FLAG_INIT;
  auto note = [&](const nevr::ConfigSetResult& r) {
    if (r.ok) return;
    ++failures;
    if (!firstFailureTaken.test_and_set()) firstFailure = std::string(r.reason) + " err=" + std::to_string(r.err);
  };
  std::thread flipper([&] {
    while (!go.load()) std::this_thread::yield();
    for (int i = 0; i < kSetsPerThread; ++i) {
      nevr::NevrConfig cfg;
      cfg.BindFile(dir.File());
      note(cfg.Set("graphics.hand_flip", i % 2 == 0));
    }
  });
  std::thread leveller([&] {
    while (!go.load()) std::this_thread::yield();
    for (int i = 0; i < kSetsPerThread; ++i) {
      nevr::NevrConfig cfg;
      cfg.BindFile(fileForSecond);
      note(cfg.Set("graphics.texture_quality", std::string(kWords[i % 4])));
    }
  });
  go.store(true);
  flipper.join();
  leveller.join();
  RoundOutcome out;
  out.failures = failures.load();
  out.firstFailure = firstFailure;
  out.text = Read(dir.File());
  out.tmpLeft = Exists(dir.Tmp());
  try {
    const nevr::NevrConfig loaded = nevr::NevrConfig::LoadFromFile(dir.File());
    out.handFlip = loaded.GetBool("graphics.hand_flip");
    out.textureQuality = loaded.GetString("graphics.texture_quality");
  } catch (const nevr::NevrConfigError&) {
    // the file is unreadable: the outcome keeps no values and the checks below say so
  }
  return out;
}

void ExpectBothKeysSurvive(const RoundOutcome& out, int round) {
  EXPECT_EQ(out.failures, 0) << "round " << round << ": a Set failed (" << out.firstFailure << ")";
  ASSERT_TRUE(out.handFlip.has_value()) << "round " << round << ": graphics.hand_flip was lost; the file reads:\n" << out.text;
  EXPECT_EQ(*out.handFlip, (kSetsPerThread - 1) % 2 == 0) << "round " << round << ": hand_flip is not the last value set";
  ASSERT_TRUE(out.textureQuality.has_value()) << "round " << round << ": graphics.texture_quality was lost; the file reads:\n" << out.text;
  EXPECT_EQ(*out.textureQuality, kWords[(kSetsPerThread - 1) % 4]) << "round " << round << ": texture_quality is not the last value set";
  EXPECT_FALSE(out.tmpLeft) << "round " << round << ": a temporary file was left";
}

}  // namespace

TEST(NevrConfigSetConcurrent, TwoThreadsEachSettingTheirOwnKeyNeverLoseEachOther) {
  for (int round = 0; round < 6; ++round) {
    Dir dir;
    ExpectBothKeysSurvive(RunRound(dir, dir.File()), round);
    if (HasFatalFailure()) return;
  }
}

// One file has many spellings; the callers' lock is the file's, not the string's.
TEST(NevrConfigSetConcurrent, TheSameFileSpelledTwoWaysIsStillOneFile) {
  for (int round = 0; round < 6; ++round) {
    Dir dir;
    ExpectBothKeysSurvive(RunRound(dir, dir.OtherSpelling()), round);
    if (HasFatalFailure()) return;
  }
}

// Two threads on ONE config object: the in-memory tree they both update is inside the lock too, so the object ends up reading both keys.
TEST(NevrConfigSetConcurrent, TwoThreadsOnOneObjectBothKeysReadBack) {
  for (int round = 0; round < 6; ++round) {
    Dir dir;
    Write(dir.File(), "social:\n  facade: true\n");
    nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromFile(dir.File());
    std::atomic<int> failures{0};
    std::atomic<bool> go{false};
    std::thread flipper([&] {
      while (!go.load()) std::this_thread::yield();
      for (int i = 0; i < kSetsPerThread; ++i) {
        if (!cfg.Set("graphics.hand_flip", i % 2 == 0).ok) ++failures;
      }
    });
    std::thread leveller([&] {
      while (!go.load()) std::this_thread::yield();
      for (int i = 0; i < kSetsPerThread; ++i) {
        if (!cfg.Set("graphics.texture_quality", std::string(kWords[i % 4])).ok) ++failures;
      }
    });
    go.store(true);
    flipper.join();
    leveller.join();
    EXPECT_EQ(failures.load(), 0) << "round " << round;
    ASSERT_TRUE(cfg.GetBool("graphics.hand_flip").has_value()) << "round " << round << ": the object lost hand_flip";
    EXPECT_EQ(*cfg.GetBool("graphics.hand_flip"), (kSetsPerThread - 1) % 2 == 0) << "round " << round;
    EXPECT_EQ(cfg.GetString("graphics.texture_quality").value_or(""), kWords[(kSetsPerThread - 1) % 4]) << "round " << round;
    const nevr::NevrConfig reloaded = nevr::NevrConfig::LoadFromFile(dir.File());
    EXPECT_EQ(reloaded.GetBool("graphics.hand_flip").value_or(!((kSetsPerThread - 1) % 2 == 0)), (kSetsPerThread - 1) % 2 == 0) << "round " << round;
    EXPECT_EQ(reloaded.GetString("graphics.texture_quality").value_or(""), kWords[(kSetsPerThread - 1) % 4]) << "round " << round;
  }
}

// The lock is held through the in-memory update. The first Set is held at the seam after the object is updated and before it returns;
// a second Set of the same key on the same object starts meanwhile and must wait for the lock, so it cannot finish before the first
// returns, and the file and the object end with the second value. With the lock released at or before the update, the second Set
// finishes while the first is held (and may even be overwritten in the object by the first's older value).
namespace {
std::atomic<int> g_hookCalls{0};
std::atomic<bool> g_firstIsHeld{false};
std::atomic<bool> g_secondDone{false};
void HoldTheFirstSet() {
  if (g_hookCalls.fetch_add(1) != 0) return;  // only the first Set is held
  g_firstIsHeld.store(true);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
  while (!g_secondDone.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
}
}  // namespace

TEST(NevrConfigSetConcurrent, TheLockIsHeldThroughTheObjectUpdate) {
  Dir dir;
  Write(dir.File(), "graphics:\n  hand_flip: false\n");
  nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromFile(dir.File());
  g_hookCalls.store(0);
  g_firstIsHeld.store(false);
  g_secondDone.store(false);
  std::atomic<bool> firstReturned{false};
  std::atomic<bool> secondFinishedBeforeFirstReturned{false};
  nevr::config_testing::SetAfterMemoryUpdateForTest(&HoldTheFirstSet);
  std::thread first([&] {
    EXPECT_TRUE(cfg.Set("graphics.hand_flip", true).ok);
    firstReturned.store(true);
  });
  while (!g_firstIsHeld.load()) std::this_thread::yield();
  std::thread second([&] {
    EXPECT_TRUE(cfg.Set("graphics.hand_flip", false).ok);  // changes the file back: a real write
    secondFinishedBeforeFirstReturned.store(!firstReturned.load());
    g_secondDone.store(true);
  });
  first.join();
  second.join();
  nevr::config_testing::SetAfterMemoryUpdateForTest(nullptr);
  EXPECT_FALSE(secondFinishedBeforeFirstReturned.load()) << "a second Set ran to the end while the first still held the object update (the lock was released too early)";
  const nevr::NevrConfig onDisk = nevr::NevrConfig::LoadFromFile(dir.File());
  ASSERT_TRUE(onDisk.GetBool("graphics.hand_flip").has_value());
  ASSERT_TRUE(cfg.GetBool("graphics.hand_flip").has_value());
  EXPECT_EQ(*cfg.GetBool("graphics.hand_flip"), *onDisk.GetBool("graphics.hand_flip")) << "the object disagrees with the file";
  EXPECT_FALSE(*onDisk.GetBool("graphics.hand_flip"));  // the second Set came last
}
