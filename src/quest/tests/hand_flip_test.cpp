// Host test for the Controller Hand Flip toggle's pure half (#1986, src/quest/sentinel/hand_flip.h): the roll, the persisted choice,
// the start resolver, the save and the golden log lines. The handlers that use it are driven by hand_pose_log_hook_test.cpp (the
// tracking roll) and texture_menu_hook_test.cpp (the menu delivery); the menu document by texture_menu_test.cpp.
//
// KNOWN-WRONG #2043 Everything under test is the sentinel interim, dropped when the roll moves into the VrApi layer.
//
// Built and run by `just test-quest-hooks`.

#include <sys/stat.h>
#include <unistd.h>

#include <pthread.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hand_flip.h"
#include "hook_log.h"
#include "quest/tests/test_check.h"

namespace {

namespace hf = nevr_quest::hand_flip;

std::vector<std::string> g_lines;
void CaptureLine(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }

// A line from the event on, without the time stamp and level in front of it.
std::string Golden(const std::string& line) {
  const std::size_t at = line.find("\"event\"");
  return at == std::string::npos ? line : line.substr(at);
}

struct Capture {
  sentinel::LogSink previous;
  Capture() : previous(sentinel::SetLogSink(&CaptureLine)) { g_lines.clear(); }
  ~Capture() { sentinel::SetLogSink(previous); }
};

// A game directory with RAD/LoneEcho in it and the toggle's file with `contents`, when given.
std::string MakeDirectory(const char* contents) {
  char dir[] = "/tmp/handflipXXXXXX";
  if (mkdtemp(dir) == nullptr) return "";
  const std::string root = dir;
  const std::string rad = root + "/RAD";
  const std::string lone = rad + "/LoneEcho";
  if (mkdir(rad.c_str(), 0700) != 0 || mkdir(lone.c_str(), 0700) != 0) return "";
  if (contents != nullptr) {
    std::FILE* f = std::fopen((lone + "/" + hf::kFileName).c_str(), "w");
    if (f == nullptr) return "";
    std::fputs(contents, f);
    std::fclose(f);
  }
  return root;
}

std::string ReadFile(const std::string& root) {
  std::FILE* f = std::fopen((root + "/RAD/LoneEcho/" + hf::kFileName).c_str(), "r");
  if (f == nullptr) return "";
  char buffer[256] = {};
  const std::size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, f);
  std::fclose(f);
  return std::string(buffer, n);
}

bool FileExists(const std::string& root) { return access((root + "/RAD/LoneEcho/" + hf::kFileName).c_str(), F_OK) == 0; }

void RemoveDirectory(const std::string& root) {
  std::remove((root + "/RAD/LoneEcho/" + hf::kFileName).c_str());
  std::remove((root + "/RAD/LoneEcho/" + hf::kFileName + ".tmp").c_str());
  rmdir((root + "/RAD/LoneEcho").c_str());
  rmdir((root + "/RAD").c_str());
  rmdir(root.c_str());
}

// ---- the roll ------------------------------------------------------------------------------------------------------------

// q * (0, 0, 1, 0) = (y, -x, w, -z) for q = (x, y, z, w).
void TheRollIsTheProductWithAHalfTurnAboutZ() {
  float q[4] = {0.5F, -0.25F, 0.125F, 0.75F};
  hf::Roll(q);
  QCHECK(q[0] == -0.25F && q[1] == -0.5F && q[2] == 0.75F && q[3] == -0.125F);
  // The Hamilton product, written out: it is the same four numbers.
  const float x = 0.5F, y = -0.25F, z = 0.125F, w = 0.75F;
  const float rx = 0.0F, ry = 0.0F, rz = 1.0F, rw = 0.0F;
  const float px = w * rx + x * rw + y * rz - z * ry;
  const float py = w * ry - x * rz + y * rw + z * rx;
  const float pz = w * rz + x * ry - y * rx + z * rw;
  const float pw = w * rw - x * rx - y * ry - z * rz;
  QCHECK(q[0] == px && q[1] == py && q[2] == pz && q[3] == pw);
}

// A half turn twice is a full turn: the quaternion comes back negated, which is the same rotation.
void TheRollAppliedTwiceGivesTheNegatedQuaternion() {
  const float original[4] = {0.5F, -0.25F, 0.125F, 0.75F};
  float q[4] = {original[0], original[1], original[2], original[3]};
  hf::Roll(q);
  hf::Roll(q);
  for (int i = 0; i < 4; ++i) QCHECK(q[i] == -original[i]);
}

// The roll keeps a unit quaternion a unit quaternion.
void TheRollKeepsTheLength() {
  float q[4] = {0.5F, 0.5F, 0.5F, 0.5F};
  hf::Roll(q);
  const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  QCHECK(std::fabs(length - 1.0F) < 1e-6F);
}

// ---- the file ------------------------------------------------------------------------------------------------------------

int Parse(const char* text, hf::FileStatus* status) {
  int flip = -9;
  *status = hf::ParseFile(text, std::strlen(text), &flip);
  return flip;
}

void TheFileTextIsTheVersionedChoice() {
  char out[64];
  QCHECK(hf::FileText(1, out, sizeof(out)) == std::strlen(R"({"version":1,"hand_flip":1})"));
  QCHECK(std::string(out) == R"({"version":1,"hand_flip":1})");
  QCHECK(hf::FileText(0, out, sizeof(out)) > 0 && std::string(out) == R"({"version":1,"hand_flip":0})");
  QCHECK(hf::FileText(7, out, sizeof(out)) > 0 && std::string(out) == R"({"version":1,"hand_flip":1})");  // any non-zero is On
  QCHECK(hf::FileText(1, out, 5) == 0);  // too small: nothing is claimed
}

void OnlyAVersionedFileWithAZeroOrAOneIsRead() {
  hf::FileStatus status;
  QCHECK(Parse(R"({"version":1,"hand_flip":1})", &status) == 1 && status == hf::FileStatus::kValid);
  QCHECK(Parse(R"({"version":1,"hand_flip":0})", &status) == 0 && status == hf::FileStatus::kValid);
  QCHECK(Parse("{ \"hand_flip\" : 1 , \"version\" :\t1 }", &status) == 1 && status == hf::FileStatus::kValid);  // key order and spacing are free
  // Not ours to read: no version (a file of some other build), another version, a value outside 0 and 1.
  QCHECK(Parse(R"({"hand_flip":1})", &status) == -9 && status == hf::FileStatus::kIgnored);
  QCHECK(Parse(R"({"version":0,"hand_flip":1})", &status) == -9 && status == hf::FileStatus::kIgnored);
  QCHECK(Parse(R"({"version":2,"hand_flip":1})", &status) == -9 && status == hf::FileStatus::kIgnored);
  QCHECK(Parse(R"({"version":1,"hand_flip":2})", &status) == -9 && status == hf::FileStatus::kIgnored);
  QCHECK(Parse(R"({"version":1,"hand_flip":-1})", &status) == -9 && status == hf::FileStatus::kIgnored);
  QCHECK(Parse(R"({"version":1,"hand_flip":true})", &status) == -9 && status == hf::FileStatus::kAbsent);
  // No choice in it at all.
  QCHECK(Parse("", &status) == -9 && status == hf::FileStatus::kAbsent);
  QCHECK(Parse("not json", &status) == -9 && status == hf::FileStatus::kAbsent);
  QCHECK(Parse(R"({"version":1})", &status) == -9 && status == hf::FileStatus::kAbsent);
  int unused = 0;
  QCHECK(hf::ParseFile(nullptr, 0, &unused) == hf::FileStatus::kAbsent);
}

// ---- the start -----------------------------------------------------------------------------------------------------------

void NoFileMeansOffAndNothingIsWritten() {
  hf::ResetForTest();
  const std::string dir = MakeDirectory(nullptr);
  QCHECK(!dir.empty());
  Capture log;
  hf::StartFromDirectory(dir.c_str());
  QCHECK(hf::Started() && !hf::Enabled() && hf::Current() == 0 && hf::CurrentBy() == hf::kByDefault);
  QCHECK(hf::Poll() == 1);  // the start line only
  QCHECK(g_lines.size() == 1);
  if (g_lines.size() == 1) {
    QCHECK(Golden(g_lines[0]) ==
           "\"event\":\"hand_flip_start\",\"source\":\"default\",\"flip\":0,\"unversioned_marker_ignored\":0,"
           "\"consequence\":\"this start is not saved; only a change in the menu is remembered for the next launch\"}");
  }
  QCHECK(hf::Poll() == 0);                 // once
  QCHECK(!FileExists(dir));                // a start, derived or default, is never saved
  RemoveDirectory(dir);
}

void AVersionedFileIsThePlayersChoice() {
  for (const int stored : {0, 1}) {
    hf::ResetForTest();
    char text[64];
    QCHECK(hf::FileText(stored, text, sizeof(text)) > 0);
    const std::string dir = MakeDirectory(text);
    QCHECK(!dir.empty());
    Capture log;
    hf::StartFromDirectory(dir.c_str());
    QCHECK(hf::Started() && hf::Current() == stored && hf::Enabled() == (stored == 1));
    QCHECK(hf::CurrentBy() == hf::kByPlayerMarker);
    QCHECK(hf::Poll() == 1);
    QCHECK(g_lines.size() == 1);
    if (g_lines.size() == 1) {
      const std::string want = std::string("\"event\":\"hand_flip_start\",\"source\":\"player_marker\",\"flip\":") +
                               (stored == 1 ? "1" : "0") +
                               ",\"unversioned_marker_ignored\":0,\"consequence\":\"this start is not saved; only a change in the menu "
                               "is remembered for the next launch\"}";
      QCHECK(Golden(g_lines[0]) == want);
    }
    QCHECK(ReadFile(dir) == text);  // read, not rewritten
    RemoveDirectory(dir);
  }
}

void AnUnversionedOrOtherVersionFileIsIgnoredAndLogged() {
  for (const char* contents : {R"({"hand_flip":1})", R"({"version":2,"hand_flip":1})", R"({"version":1,"hand_flip":5})"}) {
    hf::ResetForTest();
    const std::string dir = MakeDirectory(contents);
    QCHECK(!dir.empty());
    Capture log;
    hf::StartFromDirectory(dir.c_str());
    QCHECK(hf::Started() && !hf::Enabled() && hf::CurrentBy() == hf::kByDefault);
    QCHECK(hf::Poll() == 1);
    QCHECK(g_lines.size() == 1);
    if (g_lines.size() == 1) {
      QCHECK(Golden(g_lines[0]) ==
             "\"event\":\"hand_flip_start\",\"source\":\"default\",\"flip\":0,\"unversioned_marker_ignored\":1,"
             "\"consequence\":\"this start is not saved; only a change in the menu is remembered for the next launch\"}");
    }
    QCHECK(ReadFile(dir) == contents);  // left alone until a real change
    RemoveDirectory(dir);
  }
}

void NoDirectoryMeansOff() {
  for (const char* directory : {static_cast<const char*>(nullptr), "", "/nonexistent/handflip"}) {
    hf::ResetForTest();
    hf::StartFromDirectory(directory);
    QCHECK(hf::Started() && !hf::Enabled() && hf::CurrentBy() == hf::kByDefault);
  }
}

// The first resolver wins; a second one changes nothing.
void TheStartIsResolvedOnce() {
  hf::ResetForTest();
  const std::string first = MakeDirectory(R"({"version":1,"hand_flip":1})");
  const std::string second = MakeDirectory(R"({"version":1,"hand_flip":0})");
  hf::StartFromDirectory(first.c_str());
  hf::StartFromDirectory(second.c_str());
  QCHECK(hf::Current() == 1);
  RemoveDirectory(first);
  RemoveDirectory(second);
}

int g_provided = 0;
std::string g_providedDirectory;
void Provider() noexcept {
  ++g_provided;
  hf::StartFromDirectory(g_providedDirectory.c_str());
}

// The tracking handler's way in: with a provider it asks the provider once, with none it starts at the default.
void EnsureStartedAsksTheProviderOnceOrDefaults() {
  hf::ResetForTest();
  hf::EnsureStarted();
  QCHECK(hf::Started() && !hf::Enabled());

  hf::ResetForTest();
  const std::string dir = MakeDirectory(R"({"version":1,"hand_flip":1})");
  g_provided = 0;
  g_providedDirectory = dir;
  hf::SetStartProvider(&Provider);
  hf::EnsureStarted();
  hf::EnsureStarted();
  QCHECK(g_provided == 1 && hf::Started() && hf::Enabled());
  RemoveDirectory(dir);
  hf::ResetForTest();
}

// ---- the choice ----------------------------------------------------------------------------------------------------------

// Only a change in the menu writes the file, with the versioned text, and says so.
void AMenuChangeWritesTheVersionedFile() {
  hf::ResetForTest();
  const std::string dir = MakeDirectory(nullptr);
  Capture log;
  hf::StartFromDirectory(dir.c_str());
  QCHECK(hf::Poll() == 1);  // the start
  g_lines.clear();

  hf::SetFromMenu(1);
  QCHECK(hf::Enabled() && hf::CurrentBy() == hf::kByMenu);  // live: the flag is set at once, before any poll
  QCHECK(!FileExists(dir));                                 // and the file waits for the poller
  QCHECK(hf::Poll() == 1);
  QCHECK(ReadFile(dir) == R"({"version":1,"hand_flip":1})");
  QCHECK(g_lines.size() == 1);
  if (g_lines.size() == 1) {
    QCHECK(Golden(g_lines[0]) ==
           "\"event\":\"hand_flip_saved\",\"result\":\"written\",\"flip\":1,\"errno\":0,"
           "\"consequence\":\"the next launch starts with this choice\"}");
  }
  QCHECK(hf::Poll() == 0);  // once per change

  g_lines.clear();
  hf::SetFromMenu(0);
  QCHECK(!hf::Enabled());
  QCHECK(hf::Poll() == 1);
  QCHECK(ReadFile(dir) == R"({"version":1,"hand_flip":0})");

  // The next launch reads it.
  hf::ResetForTest();
  hf::StartFromDirectory(dir.c_str());
  QCHECK(hf::Current() == 0 && hf::CurrentBy() == hf::kByPlayerMarker);
  RemoveDirectory(dir);
}

// A file that cannot be written is loud, with the errno and what it costs; the choice still holds for this run.
void AChoiceThatCannotBeWrittenIsLoud() {
  hf::ResetForTest();
  const std::string dir = MakeDirectory(nullptr);
  Capture log;
  hf::StartFromDirectory(dir.c_str());
  hf::Poll();
  g_lines.clear();
  RemoveDirectory(dir);  // the directory is gone: the temporary file cannot be made
  hf::SetFromMenu(1);
  QCHECK(hf::Poll() == 1);
  QCHECK(hf::Enabled());
  QCHECK(g_lines.size() == 1);
  if (g_lines.size() == 1) {
    QCHECK(Golden(g_lines[0]) ==
           "\"event\":\"hand_flip_saved\",\"result\":\"write_failed\",\"flip\":1,\"errno\":2,"
           "\"consequence\":\"this choice is not remembered yet; the previous choice comes back at the next launch unless a later "
           "attempt (one every poll) writes it\"}");
    QCHECK(g_lines[0].find("\"level\":\"warn\"") != std::string::npos);  // a feature that keeps running: a warning, not an error
  }
}

void AChoiceWithNoDirectoryIsLoud() {
  hf::ResetForTest();
  Capture log;
  hf::StartFromDirectory(nullptr);
  hf::Poll();
  g_lines.clear();
  hf::SetFromMenu(1);
  QCHECK(hf::Poll() == 1);
  QCHECK(g_lines.size() == 1);
  if (g_lines.size() == 1) {
    QCHECK(Golden(g_lines[0]) ==
           "\"event\":\"hand_flip_saved\",\"result\":\"no_directory\",\"flip\":1,\"errno\":0,"
           "\"consequence\":\"this choice is not remembered; the previous choice comes back at the next launch\"}");
    QCHECK(g_lines[0].find("\"level\":\"warn\"") != std::string::npos);
  }
  QCHECK(hf::Poll() == 0);  // no directory cannot be retried: said once
}

// A save that failed is kept and tried again at the next poll, without repeating its line, and the line of the write that finally
// works says so. A newer choice replaces the one waiting.
void AFailedSaveIsRetriedAtTheNextPollAndSaidOnce() {
  hf::ResetForTest();
  const std::string dir = MakeDirectory(nullptr);
  Capture log;
  hf::StartFromDirectory(dir.c_str());
  hf::Poll();
  g_lines.clear();
  const std::string lone = dir + "/RAD/LoneEcho";
  const std::string moved = dir + "/RAD/Elsewhere";
  QCHECK(std::rename(lone.c_str(), moved.c_str()) == 0);  // the directory is gone: the file cannot be made
  hf::SetFromMenu(1);
  QCHECK(hf::Poll() == 1 && g_lines.size() == 1 && g_lines[0].find("\"result\":\"write_failed\"") != std::string::npos);
  QCHECK(hf::Poll() == 0 && g_lines.size() == 1);  // retried, still failing, not said again
  QCHECK(hf::Poll() == 0 && g_lines.size() == 1);
  QCHECK(std::rename(moved.c_str(), lone.c_str()) == 0);  // the disk is back
  QCHECK(hf::Poll() == 1 && g_lines.size() == 2);
  QCHECK(g_lines.size() == 2 && g_lines[1].find("\"result\":\"written\"") != std::string::npos && g_lines[1].find("\"flip\":1") != std::string::npos);
  QCHECK(ReadFile(dir) == R"({"version":1,"hand_flip":1})");
  QCHECK(hf::Poll() == 0);  // and no further attempt
  // A later choice is the one that is written, not the one that failed.
  QCHECK(std::rename(lone.c_str(), moved.c_str()) == 0);
  hf::SetFromMenu(0);
  QCHECK(hf::Poll() == 1);  // fails
  hf::SetFromMenu(1);       // the player chose again while it was failing
  QCHECK(std::rename(moved.c_str(), lone.c_str()) == 0);
  g_lines.clear();
  QCHECK(hf::Poll() == 1);
  QCHECK(g_lines.size() == 1 && g_lines[0].find("\"result\":\"written\"") != std::string::npos &&
         g_lines[0].find("\"flip\":1") != std::string::npos);  // the newer choice, not the one that failed
  QCHECK(ReadFile(dir) == R"({"version":1,"hand_flip":1})");
  RemoveDirectory(dir);
}

// The row is offered only when the hooks that roll, save and log it say so: the product starts not offered, and a start that is not
// offered ignores the saved choice (the roll stays Off) and says so once, where the hand pose poller is not running.
void ANotOfferedStartIgnoresTheSavedChoiceAndSaysSoOnce() {
  hf::ResetForTest();
  hf::SetOffered(false);
  const std::string dir = MakeDirectory(R"({"version":1,"hand_flip":1})");
  Capture log;
  hf::StartFromDirectory(dir.c_str());
  QCHECK(hf::Started() && !hf::Enabled() && hf::CurrentBy() == hf::kByDefault);
  QCHECK(hf::PollRow() == 1 && g_lines.size() == 1);
  if (g_lines.size() == 1) {
    QCHECK(Golden(g_lines[0]) ==
           "\"event\":\"hand_flip_row\",\"offered\":0,\"reason\":\"the hand pose hooks or their poller are not installed\","
           "\"consequence\":\"the Controller Hand Flip row is not in the menu, a saved choice is ignored and the roll stays Off\"}");
    QCHECK(g_lines[0].find("\"level\":\"warn\"") != std::string::npos);
  }
  QCHECK(hf::PollRow() == 0);  // once
  QCHECK(ReadFile(dir) == R"({"version":1,"hand_flip":1})");  // and the file is left alone
  // Offered, the same file is the player's choice and nothing is said.
  hf::ResetForTest();
  g_lines.clear();
  hf::StartFromDirectory(dir.c_str());
  QCHECK(hf::Enabled() && hf::CurrentBy() == hf::kByPlayerMarker && hf::PollRow() == 0 && g_lines.empty());
  RemoveDirectory(dir);
}

// ---- the start and the threads that trust it ------------------------------------------------------------------------

std::atomic<int> g_seamEntered{0};
std::atomic<int> g_seamReleased{0};
std::atomic<int> g_awaitResult{-1};
std::atomic<int> g_flagSeenByAwait{-1};

bool WaitFor(const std::atomic<int>& word, int want) {  // bounded: a test that never reaches it fails, it does not hang
  for (int i = 0; i < 5000 && word.load() != want; ++i) usleep(1000);
  return word.load() == want;
}
void BlockingSeam() noexcept {
  g_seamEntered.store(1);
  for (int i = 0; i < 10000 && g_seamReleased.load() == 0; ++i) usleep(1000);
}
void* StartThread(void* directory) {
  hf::StartFromDirectory(static_cast<const char*>(directory));
  return nullptr;
}
void* AwaitThread(void*) {
  g_awaitResult.store(hf::AwaitStart() ? 1 : 0);
  g_flagSeenByAwait.store(hf::Current());
  return nullptr;
}

// A thread that asks for the start while another is still resolving it waits until it is resolved, and then sees the flag the
// resolution stored: it does not return with the flag still Off.
void AnAwaitingThreadSeesTheFinishedStart() {
  hf::ResetForTest();
  const std::string dir = MakeDirectory(R"({"version":1,"hand_flip":1})");
  g_seamEntered.store(0);
  g_seamReleased.store(0);
  g_awaitResult.store(-1);
  g_flagSeenByAwait.store(-1);
  hf::g_startSeam.store(&BlockingSeam);
  pthread_t starter, waiter;
  QCHECK(pthread_create(&starter, nullptr, &StartThread, const_cast<char*>(dir.c_str())) == 0);
  QCHECK(WaitFor(g_seamEntered, 1));
  QCHECK(!hf::Started() && hf::Current() == 0);  // mid-resolve: the file is read, the flag is not stored
  QCHECK(pthread_create(&waiter, nullptr, &AwaitThread, nullptr) == 0);
  QCHECK(WaitFor(hf::g_startWaiters, 1));         // it is waiting inside AwaitStart
  QCHECK(g_awaitResult.load() == -1);
  g_seamReleased.store(1);
  pthread_join(starter, nullptr);
  pthread_join(waiter, nullptr);
  QCHECK(g_awaitResult.load() == 1 && g_flagSeenByAwait.load() == 1);
  QCHECK(hf::g_startWaiters.load() == 0);
  hf::g_startSeam.store(nullptr);
  RemoveDirectory(dir);
}

// A wait that gives up says so once, leaves the flag as it was and counts; the next ask, after the start, succeeds.
void AWaitThatTimesOutSaysSoAndTheNextAskSucceeds() {
  hf::ResetForTest();
  const std::string dir = MakeDirectory(R"({"version":1,"hand_flip":1})");
  Capture log;
  g_seamEntered.store(0);
  g_seamReleased.store(0);
  hf::g_startSeam.store(&BlockingSeam);
  hf::g_startWaitMs.store(50);
  pthread_t starter;
  QCHECK(pthread_create(&starter, nullptr, &StartThread, const_cast<char*>(dir.c_str())) == 0);
  QCHECK(WaitFor(g_seamEntered, 1));
  QCHECK(!hf::AwaitStart());
  QCHECK(!hf::Started() && !hf::Enabled());
  QCHECK(hf::Poll() == 1 && g_lines.size() == 1);
  if (g_lines.size() == 1) {
    QCHECK(Golden(g_lines[0]) ==
           "\"event\":\"hand_flip_start_wait\",\"result\":\"timed_out\",\"wait_ms\":50,\"consequence\":\"a settings delivery did "
           "not wait longer for the start; the toggle's load waits for the next delivery\"}");
  }
  QCHECK(hf::Poll() == 0);  // once
  g_seamReleased.store(1);
  pthread_join(starter, nullptr);
  QCHECK(hf::AwaitStart() && hf::Enabled());
  hf::g_startSeam.store(nullptr);
  RemoveDirectory(dir);
}

void TheMenuRowIsBoundToTheCarrierBitAndNamedForThePlayer() {
  QCHECK(std::strcmp(hf::kMenuPath, "graphics|fullscreen") == 0);
  QCHECK(hf::kCarrierOffset == 0 && hf::kCarrierBit == 1U);  // SR15GfxSettings flags, bit 0
  QCHECK(std::strcmp(hf::kMenuName, "Controller Hand Flip") == 0);
  QCHECK(hf::kMenuValueCount == 2 && std::strcmp(hf::kMenuLabels[0], "On") == 0 && std::strcmp(hf::kMenuLabels[1], "Off") == 0);
  QCHECK(std::strcmp(hf::kFileName, "nevr_hand_flip.json") == 0 && hf::kFileVersion == 1);
  QCHECK(std::strcmp(hf::ByName(hf::kByDefault), "default") == 0 && std::strcmp(hf::ByName(hf::kByPlayerMarker), "player_marker") == 0 &&
         std::strcmp(hf::ByName(hf::kByMenu), "menu") == 0);
}

// The game's own value of the bit is 1 and that is Off: the bit and the choice are each other's inverse, and the row lists On first.
void TheBitIsTheInverseOfTheChoice() {
  QCHECK(hf::BitFor(0) == 1 && hf::BitFor(1) == 0);  // Off is the bit set, the game's own value
  QCHECK(hf::FlipFor(1) == 0 && hf::FlipFor(0) == 1);
  QCHECK(hf::MenuIndexFor(1) == 0 && hf::MenuIndexFor(0) == 1);  // "On" is index 0
  for (const int flip : {0, 1}) QCHECK(hf::FlipFor(hf::BitFor(flip)) == flip);
}

// The first delivery's write into the game's bit is logged once, by the poller, with the bit as the game held it and the bit it was
// set to.
void TheCarrierWriteIsLoggedOnce() {
  hf::ResetForTest();
  Capture log;
  hf::StartFromDirectory(nullptr);
  QCHECK(hf::Poll() == 1);  // the start line; no delivery has written yet, so no carrier line
  QCHECK(g_lines.size() == 1 && g_lines[0].find("hand_flip_carrier") == std::string::npos);
  hf::RecordCarrier(1, 0);
  QCHECK(hf::Poll() == 1);
  QCHECK(g_lines.size() == 2);
  if (g_lines.size() == 2) {
    QCHECK(Golden(g_lines[1]) ==
           "\"event\":\"hand_flip_carrier\",\"before\":1,\"after\":0,\"consequence\":\"the game's fullscreen setting bit carries the "
           "toggle: 1 is Off (the game's own value), 0 is On\"}");
  }
  QCHECK(hf::Poll() == 0);  // once
  hf::RecordCarrier(0, 1);
  QCHECK(hf::Poll() == 0);  // and never again, whatever a later record holds
  hf::ResetForTest();
}

}  // namespace

int main() {
  QCHECK(!hf::Offered());  // the product starts with no row: only the hooks' install says otherwise (before any ResetForTest)
  TheRollIsTheProductWithAHalfTurnAboutZ();
  TheRollAppliedTwiceGivesTheNegatedQuaternion();
  TheRollKeepsTheLength();
  TheFileTextIsTheVersionedChoice();
  OnlyAVersionedFileWithAZeroOrAOneIsRead();
  NoFileMeansOffAndNothingIsWritten();
  AVersionedFileIsThePlayersChoice();
  AnUnversionedOrOtherVersionFileIsIgnoredAndLogged();
  NoDirectoryMeansOff();
  TheStartIsResolvedOnce();
  EnsureStartedAsksTheProviderOnceOrDefaults();
  AMenuChangeWritesTheVersionedFile();
  AChoiceThatCannotBeWrittenIsLoud();
  AChoiceWithNoDirectoryIsLoud();
  AFailedSaveIsRetriedAtTheNextPollAndSaidOnce();
  ANotOfferedStartIgnoresTheSavedChoiceAndSaysSoOnce();
  AnAwaitingThreadSeesTheFinishedStart();
  AWaitThatTimesOutSaysSoAndTheNextAskSucceeds();
  TheMenuRowIsBoundToTheCarrierBitAndNamedForThePlayer();
  TheBitIsTheInverseOfTheChoice();
  TheCarrierWriteIsLoggedOnce();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "hand_flip_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("hand_flip_test: all checks pass\n");
  return 0;
}
