// Host test for NevrConfig::Set on the Quest build (#1986): the first config.yaml writer, the same source the PC compiles
// (src/core/nevr_config.cpp). Every case goes through the interface (bind a file, Set, read the bytes back, read the getter);
// the PC GTest (src/runtime/tests/test_nevr_config_set.cpp) runs the wider set.
//
// Built and run by `just test-quest-shared`.

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "core/nevr_config.h"
#include "core/nevr_config_testing.h"
#include "quest/tests/test_check.h"

namespace {

struct Dir {
  std::string root;
  Dir() {
    char path[] = "/tmp/nevrcfgsetXXXXXX";
    root = mkdtemp(path) != nullptr ? path : "";
  }
  ~Dir() {
    std::remove(File().c_str());
    std::remove(Tmp().c_str());
    rmdir(root.c_str());
  }
  std::string File() const { return root + "/config.yaml"; }
  std::string Tmp() const { return File() + ".tmp"; }
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
bool Exists(const std::string& path) { return access(path.c_str(), F_OK) == 0; }

nevr::ConfigSetResult SetOn(const std::string& file, bool value) {
  nevr::NevrConfig cfg;
  cfg.BindFile(file);
  return cfg.Set("graphics.hand_flip", value);
}

void ACommentedFileKeepsEveryByteButTheValue() {
  const std::string before =
      "# nevr config\nversion: 1   # schema\n\nservices:\n  socket_uri: wss://example.invalid/ws   # c\n\n"
      "graphics:\n  # the level\n  texture_quality: high\n  hand_flip: false   # set by the menu\n  # note\n\nsocial:\n  facade: true\n";
  Dir dir;
  Write(dir.File(), before);
  const nevr::ConfigSetResult on = SetOn(dir.File(), true);
  QCHECK(on.ok && on.wrote);
  std::string expected = before;
  expected.replace(expected.find("hand_flip: false") + 11, 5, "true");
  QCHECK(Read(dir.File()) == expected);
  QCHECK(SetOn(dir.File(), false).ok);
  QCHECK(Read(dir.File()) == before);  // back, byte for byte
  QCHECK(!Exists(dir.Tmp()));
}

void AnInsertIntoAGraphicsBlockThatLacksTheKey() {
  Dir dir;
  Write(dir.File(), "graphics:\n    texture_quality: high   # four spaces\n\n# about social\nsocial:\n  facade: true\n");
  QCHECK(SetOn(dir.File(), true).ok);
  QCHECK(Read(dir.File()) ==
         "graphics:\n    texture_quality: high   # four spaces\n    hand_flip: true\n\n# about social\nsocial:\n  facade: true\n");
}

void AMissingFileIsCreatedAndNoGraphicsBlockIsAppended() {
  Dir dir;
  QCHECK(SetOn(dir.File(), true).ok);
  QCHECK(Read(dir.File()) == "graphics:\n  hand_flip: true\n");
  Write(dir.File(), "social:\n  facade: true\n");
  QCHECK(SetOn(dir.File(), false).ok);
  QCHECK(Read(dir.File()) == "social:\n  facade: true\ngraphics:\n  hand_flip: false\n");
}

void QuotedAndUnquotedValues() {
  struct Case {
    const char* before;
    bool value;
    const char* after;
  };
  const Case cases[] = {
      {"graphics:\n  hand_flip: false\n", true, "graphics:\n  hand_flip: true\n"},
      {"graphics:\n  hand_flip: no   # off\n", true, "graphics:\n  hand_flip: true   # off\n"},
      {"graphics:\n  hand_flip: \"false\"\n", true, "graphics:\n  hand_flip: \"true\"\n"},
      {"graphics:\n  hand_flip: 'true'   # q\n", false, "graphics:\n  hand_flip: 'false'   # q\n"},
  };
  for (const Case& c : cases) {
    Dir dir;
    Write(dir.File(), c.before);
    QCHECK(SetOn(dir.File(), c.value).ok);
    QCHECK(Read(dir.File()) == c.after);
  }
}

void ARefusalWritesNothing(const std::string& text, const char* reason) {
  Dir dir;
  Write(dir.File(), text);
  const nevr::ConfigSetResult r = SetOn(dir.File(), true);
  QCHECK(!r.ok && !r.wrote && std::string(r.reason) == reason);
  QCHECK(Read(dir.File()) == text);
  QCHECK(!Exists(dir.Tmp()));
}

void AFlowStyleGraphicsBlockAndATruncatedFileAreRefused() {
  ARefusalWritesNothing("graphics: {texture_quality: high}\n", "section_flow_style");
  ARefusalWritesNothing("graphics:\n  texture_quality: \"hig", "unparseable");
  ARefusalWritesNothing("graphics: {texture_quality: high\n", "unparseable");
  ARefusalWritesNothing("graphics:\n  hand_flip: [true]\n", "value_not_a_scalar");
}

int FailingRename(const char*, const char*) { return EXDEV; }

void AFailedRenameLeavesThePreviousFileIntact() {
  Dir dir;
  const std::string before = "# keep\ngraphics:\n  hand_flip: false\n";
  Write(dir.File(), before);
  nevr::config_testing::SetRenameForTest(&FailingRename);
  const nevr::ConfigSetResult r = SetOn(dir.File(), true);
  nevr::config_testing::SetRenameForTest(nullptr);
  QCHECK(!r.ok && std::string(r.reason) == "rename_failed" && r.err == EXDEV);
  QCHECK(Read(dir.File()) == before && !Exists(dir.Tmp()));
}

void SetThenReadRoundTrip() {
  Dir dir;
  Write(dir.File(), "graphics:\n  texture_quality: high\n");
  nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromFile(dir.File());
  QCHECK(!cfg.GetBool("graphics.hand_flip").has_value());
  QCHECK(cfg.Set("graphics.hand_flip", true).ok);
  QCHECK(cfg.GetBool("graphics.hand_flip").has_value() && *cfg.GetBool("graphics.hand_flip"));
  const nevr::NevrConfig reloaded = nevr::NevrConfig::LoadFromFile(dir.File());
  QCHECK(reloaded.GetBool("graphics.hand_flip").has_value() && *reloaded.GetBool("graphics.hand_flip"));
  QCHECK(reloaded.UnknownKeyCount() == 0);
}

nevr::ConfigSetResult SetWord(const std::string& file, const std::string& word) {
  nevr::NevrConfig cfg;
  cfg.BindFile(file);
  return cfg.Set("graphics.texture_quality", word);
}

// The texture level the player picks (a plain word) replaces the old one in place; every other byte stays. The same cases run on
// PC in src/runtime/tests/test_nevr_config_set.cpp (NevrConfigSetWord): one setter, both platforms.
void AWordReplacesTheOldOneInPlaceAndGoesBack() {
  const std::string before =
      "# nevr config\ngraphics:\n  # the level\n  texture_quality: high   # the player's pick\n  hand_flip: true\n\nsocial:\n  facade: true\n";
  Dir dir;
  Write(dir.File(), before);
  const nevr::ConfigSetResult r = SetWord(dir.File(), "very_high");
  QCHECK(r.ok && r.wrote);
  std::string expected = before;
  expected.replace(expected.find("high   # the player's pick"), 4, "very_high");
  QCHECK(Read(dir.File()) == expected);
  QCHECK(SetWord(dir.File(), "high").ok);
  QCHECK(Read(dir.File()) == before);
  QCHECK(!Exists(dir.Tmp()));
}

void AWordThatIsAlreadyThereIsNotRewrittenAndQuotesStay() {
  Dir same;
  Write(same.File(), "graphics:\n  texture_quality: \"ultra\"\n");
  const nevr::ConfigSetResult r = SetWord(same.File(), "ultra");
  QCHECK(r.ok && !r.wrote);
  QCHECK(Read(same.File()) == "graphics:\n  texture_quality: \"ultra\"\n");
  Dir quoted;
  Write(quoted.File(), "graphics:\n  texture_quality: 'low'\n");
  QCHECK(SetWord(quoted.File(), "medium").ok);
  QCHECK(Read(quoted.File()) == "graphics:\n  texture_quality: 'medium'\n");
}

void ATextThatIsNotAPlainWordIsRefusedAndNothingIsWritten() {
  const char* const kinds[] = {"", "two words", "a: b", "# c", "true", "No", "null", "-lead", "line\nbreak", "quote\"d"};
  for (const char* kind : kinds) {
    Dir dir;
    const std::string before = "graphics:\n  texture_quality: low\n";
    Write(dir.File(), before);
    const nevr::ConfigSetResult r = SetWord(dir.File(), kind);
    QCHECK(!r.ok && !r.wrote && std::string(r.reason) == "value_not_a_plain_word");
    QCHECK(Read(dir.File()) == before);
  }
}

}  // namespace

int main() {
  ACommentedFileKeepsEveryByteButTheValue();
  AnInsertIntoAGraphicsBlockThatLacksTheKey();
  AMissingFileIsCreatedAndNoGraphicsBlockIsAppended();
  QuotedAndUnquotedValues();
  AFlowStyleGraphicsBlockAndATruncatedFileAreRefused();
  AFailedRenameLeavesThePreviousFileIntact();
  SetThenReadRoundTrip();
  AWordReplacesTheOldOneInPlaceAndGoesBack();
  AWordThatIsAlreadyThereIsNotRewrittenAndQuotesStay();
  ATextThatIsNotAPlainWordIsRefusedAndNothingIsWritten();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "nevr_config_set_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("nevr_config_set_test: all checks pass\n");
  return 0;
}
