// Host test for the Controller Hand Flip toggle's pure half (#1986, src/quest/sentinel/hand_flip.h): the roll, the start from the
// configuration's graphics.hand_flip (#2082), the save through NevrConfig::Set and the golden log lines. The handlers that use it are driven by hand_pose_log_hook_test.cpp (the
// tracking roll) and texture_menu_hook_test.cpp (the menu delivery); the menu document by texture_menu_test.cpp.
//
// KNOWN-WRONG #2043 Everything under test is the sentinel interim, dropped when the roll moves into the VrApi layer.
//
// Built and run by `just test-quest-hooks`.

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pthread.h>
#include <time.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

#include "core/nevr_config.h"
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

// The file an earlier build (#1986) kept the choice in, beside the game's settings: nothing reads, moves or writes it now.
constexpr const char* kLeftoverName = "nevr_hand_flip.json";

// A game directory with RAD/LoneEcho in it, and `leftover` as the earlier build's file when given. config.yaml is the directory's own
// (the Quest's files directory), absent until `config` is given or the first save creates it.
std::string MakeDirectory(const char* config, const char* leftover = nullptr) {
  char dir[] = "/tmp/handflipXXXXXX";
  if (mkdtemp(dir) == nullptr) return "";
  const std::string root = dir;
  const std::string rad = root + "/RAD";
  const std::string lone = rad + "/LoneEcho";
  if (mkdir(rad.c_str(), 0700) != 0 || mkdir(lone.c_str(), 0700) != 0) return "";
  if (leftover != nullptr) {
    std::FILE* f = std::fopen((lone + "/" + kLeftoverName).c_str(), "w");
    if (f == nullptr) return "";
    std::fputs(leftover, f);
    std::fclose(f);
  }
  if (config != nullptr) {
    std::FILE* f = std::fopen((root + "/config.yaml").c_str(), "w");
    if (f == nullptr) return "";
    std::fputs(config, f);
    std::fclose(f);
  }
  return root;
}

std::string ConfigPath(const std::string& root) { return root + "/config.yaml"; }

std::string Slurp(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "r");
  if (f == nullptr) return "";
  char buffer[512] = {};
  const std::size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, f);
  std::fclose(f);
  return std::string(buffer, n);
}

std::string ReadConfig(const std::string& root) { return Slurp(ConfigPath(root)); }
std::string ReadLeftover(const std::string& root) { return Slurp(root + "/RAD/LoneEcho/" + kLeftoverName); }
bool ConfigExists(const std::string& root) { return access(ConfigPath(root).c_str(), F_OK) == 0; }

// Every name in a directory, sorted, without . and .. .
std::vector<std::string> Names(const std::string& directory) {
  std::vector<std::string> names;
  DIR* d = opendir(directory.c_str());
  if (d == nullptr) return names;
  while (const dirent* e = readdir(d)) {
    if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) names.emplace_back(e->d_name);
  }
  closedir(d);
  std::sort(names.begin(), names.end());
  return names;
}

void RemoveDirectory(const std::string& root) {
  for (const std::string& name : Names(root + "/RAD/LoneEcho")) std::remove((root + "/RAD/LoneEcho/" + name).c_str());
  for (const std::string& name : Names(root)) std::remove((root + "/" + name).c_str());
  rmdir((root + "/RAD/LoneEcho").c_str());
  rmdir((root + "/RAD").c_str());
  rmdir(root.c_str());
}

// What the Quest's start does in production, before any hook can ask for the start: the file's path and what the reader found.
void Configure(const std::string& root, bool fromConfig, bool flip) { hf::Configure(ConfigPath(root).c_str(), fromConfig, flip); }

const char* const kStartConsequence =
    "\"consequence\":\"this start is not saved; only a change in the menu is written to config.yaml for the next launch\"}";

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

// ---- the start -----------------------------------------------------------------------------------------------------------

void NoKeyMeansOffAndNothingIsWritten() {
  hf::ResetForTest();
  const std::string dir = MakeDirectory(nullptr);
  QCHECK(!dir.empty());
  Capture log;
  Configure(dir, false, false);
  hf::Start();
  QCHECK(hf::Started() && !hf::Enabled() && hf::Current() == 0 && hf::CurrentBy() == hf::kByDefault);
  QCHECK(hf::Poll() == 1);  // the start line only
  QCHECK(g_lines.size() == 1);
  if (g_lines.size() == 1) {
    QCHECK(Golden(g_lines[0]) == std::string("\"event\":\"hand_flip_start\",\"source\":\"default\",\"flip\":0,") + kStartConsequence);
  }
  QCHECK(hf::Poll() == 0);        // once
  QCHECK(!ConfigExists(dir));     // a start, derived or default, is never saved
  QCHECK(Names(dir) == std::vector<std::string>{"RAD"});
  RemoveDirectory(dir);
}

// The key the reader found is the player's choice, On or Off: the start says `config` for both, and does not write what it read.
void TheKeyIsThePlayersChoice() {
  for (const int stored : {0, 1}) {
    hf::ResetForTest();
    const std::string text = std::string("graphics:\n  hand_flip: ") + (stored == 1 ? "true" : "false") + "\n";
    const std::string dir = MakeDirectory(text.c_str());
    QCHECK(!dir.empty());
    Capture log;
    Configure(dir, true, stored == 1);
    hf::Start();
    QCHECK(hf::Started() && hf::Current() == stored && hf::Enabled() == (stored == 1));
    QCHECK(hf::CurrentBy() == hf::kByConfig);
    QCHECK(hf::Poll() == 1);
    QCHECK(g_lines.size() == 1);
    if (g_lines.size() == 1) {
      const std::string want = std::string("\"event\":\"hand_flip_start\",\"source\":\"config\",\"flip\":") + (stored == 1 ? "1" : "0") +
                               "," + kStartConsequence;
      QCHECK(Golden(g_lines[0]) == want);
    }
    QCHECK(ReadConfig(dir) == text);  // read, not rewritten
    RemoveDirectory(dir);
  }
}

// A start nobody configured (no file path, nothing from the reader) is Off, from the default.
void ANeverConfiguredStartIsOff() {
  hf::ResetForTest();
  hf::Start();
  QCHECK(hf::Started() && !hf::Enabled() && hf::CurrentBy() == hf::kByDefault);
}

// The first resolver wins; a second one changes nothing.
void TheStartIsResolvedOnce() {
  hf::ResetForTest();
  const std::string dir = MakeDirectory(nullptr);
  Configure(dir, true, true);
  hf::Start();
  Configure(dir, true, false);
  hf::Start();
  QCHECK(hf::Current() == 1);
  RemoveDirectory(dir);
}

// The tracking handler's way in resolves the start from the configuration, once.
void EnsureStartedResolvesTheStartOnce() {
  hf::ResetForTest();
  const std::string dir = MakeDirectory(nullptr);
  Configure(dir, true, true);
  hf::EnsureStarted();
  QCHECK(hf::Started() && hf::Enabled() && hf::CurrentBy() == hf::kByConfig);
  Configure(dir, true, false);
  hf::EnsureStarted();
  QCHECK(hf::Enabled());
  hf::ResetForTest();
  hf::EnsureStarted();
  QCHECK(hf::Started() && !hf::Enabled());  // nothing configured: the default
  RemoveDirectory(dir);
}

// A file an earlier build kept the choice in is not the configuration: nothing reads it, so the start is Off and from the default
// (the player's earlier On is not carried over), nothing is said about it, and it is left exactly as it was.
void ALeftoverMarkerFileIsNeitherReadNorTouched() {
  for (const char* leftover : {R"({"version":1,"hand_flip":1})", R"({"version":1,"hand_flip":0})", R"({"hand_flip":1})", "not json"}) {
    hf::ResetForTest();
    const std::string dir = MakeDirectory(nullptr, leftover);
    QCHECK(!dir.empty());
    Capture log;
    Configure(dir, false, false);
    hf::Start();
    QCHECK(hf::Started() && !hf::Enabled() && hf::CurrentBy() == hf::kByDefault);
    QCHECK(hf::Poll() == 1 && g_lines.size() == 1);  // the start line, and no line about the file
    if (g_lines.size() == 1) {
      QCHECK(Golden(g_lines[0]) == std::string("\"event\":\"hand_flip_start\",\"source\":\"default\",\"flip\":0,") + kStartConsequence);
    }
    // A change in the menu writes config.yaml and still leaves the old file alone.
    hf::SetFromMenu(1);
    QCHECK(hf::Poll() == 1);
    QCHECK(ReadConfig(dir) == "graphics:\n  hand_flip: true\n");
    QCHECK(ReadLeftover(dir) == leftover);
    QCHECK(Names(dir + "/RAD/LoneEcho") == std::vector<std::string>{kLeftoverName});
    RemoveDirectory(dir);
  }
}

// ---- the choice ----------------------------------------------------------------------------------------------------------

// Only a change in the menu writes config.yaml, and nothing else is written: the key, then the other boolean, then the same again.
void AMenuChangeWritesTheKey() {
  hf::ResetForTest();
  const std::string dir = MakeDirectory(nullptr);
  Capture log;
  Configure(dir, false, false);
  hf::Start();
  QCHECK(hf::Poll() == 1);  // the start
  g_lines.clear();

  hf::SetFromMenu(1);
  QCHECK(hf::Enabled() && hf::CurrentBy() == hf::kByMenu);  // live: the flag is set at once, before any poll
  QCHECK(!ConfigExists(dir));                               // and the file waits for the poller
  QCHECK(hf::Poll() == 1);
  QCHECK(ReadConfig(dir) == "graphics:\n  hand_flip: true\n");
  QCHECK(g_lines.size() == 1);
  if (g_lines.size() == 1) {
    QCHECK(Golden(g_lines[0]) == "\"event\":\"hand_flip_saved\",\"result\":\"written\",\"key\":\"graphics.hand_flip\",\"flip\":1,"
                                 "\"errno\":0,\"config_file\":\"" + ConfigPath(dir) +
                                     "\",\"consequence\":\"the next launch starts with this choice\"}");
    QCHECK(g_lines[0].find("\"level\":\"info\"") != std::string::npos);
  }
  QCHECK(hf::Poll() == 0);  // once per change

  g_lines.clear();
  hf::SetFromMenu(0);
  QCHECK(!hf::Enabled());
  QCHECK(hf::Poll() == 1);
  QCHECK(ReadConfig(dir) == "graphics:\n  hand_flip: false\n");

  // The same choice again changes no byte, and says so.
  g_lines.clear();
  hf::SetFromMenu(0);
  QCHECK(hf::Poll() == 1 && g_lines.size() == 1 && g_lines[0].find("\"result\":\"unchanged\"") != std::string::npos);
  QCHECK(ReadConfig(dir) == "graphics:\n  hand_flip: false\n");

  // Nothing else was written anywhere: no marker file, no temporary file.
  QCHECK(Names(dir) == (std::vector<std::string>{"RAD", "config.yaml"}));
  QCHECK(Names(dir + "/RAD/LoneEcho").empty());

  // The next launch reads it back through the configuration, as the Quest's reader does.
  const nevr::NevrConfig reloaded = nevr::NevrConfig::LoadFromFile(ConfigPath(dir));
  QCHECK(reloaded.GetBool("graphics.hand_flip").has_value() && !*reloaded.GetBool("graphics.hand_flip"));
  hf::ResetForTest();
  Configure(dir, true, *reloaded.GetBool("graphics.hand_flip"));
  hf::Start();
  QCHECK(hf::Current() == 0 && hf::CurrentBy() == hf::kByConfig);
  RemoveDirectory(dir);
}

// The key joins the file as it is: the other keys, the comments and the order stay.
void TheKeyIsSplicedIntoAnExistingFile() {
  hf::ResetForTest();
  const std::string dir = MakeDirectory("# mine\ngraphics:\n  texture_quality: high   # chosen\n\nsocial:\n  facade: true\n");
  Capture log;
  Configure(dir, false, false);
  hf::Start();
  hf::Poll();
  hf::SetFromMenu(1);
  QCHECK(hf::Poll() == 1);
  QCHECK(ReadConfig(dir) == "# mine\ngraphics:\n  texture_quality: high   # chosen\n  hand_flip: true\n\nsocial:\n  facade: true\n");
  RemoveDirectory(dir);
}

// A file that cannot be edited is loud, with the reason and the errno and what it costs; the choice still holds for this run.
void AChoiceThatCannotBeWrittenIsLoud() {
  hf::ResetForTest();
  const std::string dir = MakeDirectory(nullptr);
  Capture log;
  Configure(dir, false, false);
  hf::Start();
  hf::Poll();
  g_lines.clear();
  RemoveDirectory(dir);  // the directory is gone: the temporary file cannot be made
  hf::SetFromMenu(1);
  QCHECK(hf::Poll() == 1);
  QCHECK(hf::Enabled());
  QCHECK(g_lines.size() == 1);
  if (g_lines.size() == 1) {
    QCHECK(g_lines[0].find("\"event\":\"hand_flip_saved\",\"result\":\"refused\",\"key\":\"graphics.hand_flip\",\"reason\":\"") !=
           std::string::npos);
    QCHECK(g_lines[0].find("\"flip\":1,\"errno\":2,\"config_file\":\"" + ConfigPath(dir) + "\",\"consequence\":\"this choice is not "
                           "remembered yet; the previous choice comes back at the next launch unless a later attempt (one every "
                           "poll) writes it\"}") != std::string::npos);
    QCHECK(g_lines[0].find("\"level\":\"warn\"") != std::string::npos);  // a feature that keeps running: a warning, not an error
  }
}

void AChoiceWithNoConfigFileIsLoud() {
  hf::ResetForTest();
  Capture log;
  hf::Start();
  hf::Poll();
  g_lines.clear();
  hf::SetFromMenu(1);
  QCHECK(hf::Poll() == 1);
  QCHECK(g_lines.size() == 1);
  if (g_lines.size() == 1) {
    QCHECK(Golden(g_lines[0]) ==
           "\"event\":\"hand_flip_saved\",\"result\":\"no_config_file\",\"key\":\"graphics.hand_flip\",\"flip\":1,\"errno\":0,"
           "\"consequence\":\"this choice is not remembered; the previous choice comes back at the next launch\"}");
    QCHECK(g_lines[0].find("\"level\":\"warn\"") != std::string::npos);
  }
  QCHECK(hf::Poll() == 0);  // no file cannot be retried: said once
  // A path that does not fit the buffer is no path.
  hf::ResetForTest();
  const std::string tooLong(sizeof(hf::g_configFile), 'a');
  hf::Configure(tooLong.c_str(), true, true);
  hf::Start();
  hf::Poll();
  g_lines.clear();
  hf::SetFromMenu(1);
  QCHECK(hf::Poll() == 1 && g_lines.size() == 1 && g_lines[0].find("\"result\":\"no_config_file\"") != std::string::npos);
}

// A save that failed is kept and tried again at the next poll, without repeating its line, and the line of the write that finally
// works says so. A newer choice replaces the one waiting.
void AFailedSaveIsRetriedAtTheNextPollAndSaidOnce() {
  hf::ResetForTest();
  const std::string dir = MakeDirectory(nullptr);
  Capture log;
  Configure(dir, false, false);
  hf::Start();
  hf::Poll();
  g_lines.clear();
  const std::string moved = dir + "-elsewhere";
  QCHECK(std::rename(dir.c_str(), moved.c_str()) == 0);  // the directory is gone: the file cannot be made
  hf::SetFromMenu(1);
  QCHECK(hf::Poll() == 1 && g_lines.size() == 1 && g_lines[0].find("\"result\":\"refused\"") != std::string::npos);
  QCHECK(hf::Poll() == 0 && g_lines.size() == 1);  // retried, still failing, not said again
  QCHECK(hf::Poll() == 0 && g_lines.size() == 1);
  QCHECK(std::rename(moved.c_str(), dir.c_str()) == 0);  // the disk is back
  QCHECK(hf::Poll() == 1 && g_lines.size() == 2);
  QCHECK(g_lines.size() == 2 && g_lines[1].find("\"result\":\"written\"") != std::string::npos && g_lines[1].find("\"flip\":1") != std::string::npos);
  QCHECK(ReadConfig(dir) == "graphics:\n  hand_flip: true\n");
  QCHECK(hf::Poll() == 0);  // and no further attempt
  // A later choice is the one that is written, not the one that failed.
  QCHECK(std::rename(dir.c_str(), moved.c_str()) == 0);
  hf::SetFromMenu(0);
  QCHECK(hf::Poll() == 1);  // fails
  hf::SetFromMenu(1);       // the player chose again while it was failing
  QCHECK(std::rename(moved.c_str(), dir.c_str()) == 0);
  g_lines.clear();
  QCHECK(hf::Poll() == 1);
  QCHECK(g_lines.size() == 1 && g_lines[0].find("\"result\":\"unchanged\"") != std::string::npos &&
         g_lines[0].find("\"flip\":1") != std::string::npos);  // the newer choice, not the one that failed (the file already holds it)
  QCHECK(ReadConfig(dir) == "graphics:\n  hand_flip: true\n");
  RemoveDirectory(dir);
}

// The save runs on the poller thread, never on the game's call: SetFromMenu (the menu delivery's call) only queues it.
void TheGameCallNeverWritesAFile() {
  hf::ResetForTest();
  const std::string dir = MakeDirectory(nullptr);
  Capture log;
  Configure(dir, false, false);
  hf::Start();
  hf::SetFromMenu(1);
  hf::SetFromMenu(0);
  hf::SetFromMenu(1);
  QCHECK(!ConfigExists(dir) && g_lines.empty());  // no file, no line, until the poller runs
  QCHECK(hf::g_pendingSave.load() == 1);
  RemoveDirectory(dir);
}

// The row is offered only when the hooks that roll, save and log it say so: the product starts not offered, and a start that is not
// offered ignores the configured choice (the roll stays Off) and says so once, where the hand pose poller is not running.
void ANotOfferedStartIgnoresTheConfiguredChoiceAndSaysSoOnce() {
  hf::ResetForTest();
  hf::SetOffered(false);
  const std::string text = "graphics:\n  hand_flip: true\n";
  const std::string dir = MakeDirectory(text.c_str());
  Capture log;
  Configure(dir, true, true);
  hf::Start();
  QCHECK(hf::Started() && !hf::Enabled() && hf::CurrentBy() == hf::kByDefault);
  QCHECK(hf::PollRow() == 1 && g_lines.size() == 1);
  if (g_lines.size() == 1) {
    QCHECK(Golden(g_lines[0]) ==
           "\"event\":\"hand_flip_row\",\"offered\":0,\"reason\":\"the hand pose hooks or their poller are not installed\","
           "\"consequence\":\"the Controller Hand Flip row is not in the menu, a saved choice is ignored and the roll stays Off\"}");
    QCHECK(g_lines[0].find("\"level\":\"warn\"") != std::string::npos);
  }
  QCHECK(hf::PollRow() == 0);  // once
  QCHECK(ReadConfig(dir) == text);  // and the file is left alone
  // Offered, the same key is the player's choice and nothing is said.
  hf::ResetForTest();
  g_lines.clear();
  Configure(dir, true, true);
  hf::Start();
  QCHECK(hf::Enabled() && hf::CurrentBy() == hf::kByConfig && hf::PollRow() == 0 && g_lines.empty());
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
void* StartThread(void*) {
  hf::Start();
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
  const std::string dir = MakeDirectory(nullptr);
  Configure(dir, true, true);
  g_seamEntered.store(0);
  g_seamReleased.store(0);
  g_awaitResult.store(-1);
  g_flagSeenByAwait.store(-1);
  hf::g_startSeam.store(&BlockingSeam);
  hf::g_startWaitMs.store(5000);  // long: the waiter must be woken by the start finishing, not by this timeout
  pthread_t starter, waiter;
  QCHECK(pthread_create(&starter, nullptr, &StartThread, nullptr) == 0);
  QCHECK(WaitFor(g_seamEntered, 1));
  QCHECK(!hf::Started() && hf::Current() == 0);  // mid-resolve: the choice is read, the flag is not stored
  QCHECK(pthread_create(&waiter, nullptr, &AwaitThread, nullptr) == 0);
  QCHECK(WaitFor(hf::g_startWaiters, 1));         // it is waiting inside AwaitStart
  QCHECK(g_awaitResult.load() == -1);
  timespec before, after;
  clock_gettime(CLOCK_MONOTONIC, &before);
  g_seamReleased.store(1);
  pthread_join(starter, nullptr);
  pthread_join(waiter, nullptr);
  clock_gettime(CLOCK_MONOTONIC, &after);
  const long wokeAfterMs = (after.tv_sec - before.tv_sec) * 1000L + (after.tv_nsec - before.tv_nsec) / 1000000L;
  QCHECK(g_awaitResult.load() == 1 && g_flagSeenByAwait.load() == 1);
  QCHECK(wokeAfterMs < 2000);  // woken by the start, not by the 5 s timeout
  QCHECK(hf::g_startWaiters.load() == 0);
  hf::g_startSeam.store(nullptr);
  RemoveDirectory(dir);
}

// A wait that gives up says so once, leaves the flag as it was and counts; the next ask, after the start, succeeds.
void AWaitThatTimesOutSaysSoAndTheNextAskSucceeds() {
  hf::ResetForTest();
  const std::string dir = MakeDirectory(nullptr);
  Configure(dir, true, true);
  Capture log;
  g_seamEntered.store(0);
  g_seamReleased.store(0);
  hf::g_startSeam.store(&BlockingSeam);
  hf::g_startWaitMs.store(50);
  pthread_t starter;
  QCHECK(pthread_create(&starter, nullptr, &StartThread, nullptr) == 0);
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
  QCHECK(std::strcmp(hf::kMenuTrueLabel, "Off") == 0 && std::strcmp(hf::kMenuFalseLabel, "On") == 0);  // true is the bit set
  QCHECK(std::strcmp(hf::kConfigKey, "graphics.hand_flip") == 0);  // the key the reader (quest_config.cpp) reads and the poller writes
  QCHECK(std::strcmp(hf::ByName(hf::kByDefault), "default") == 0 && std::strcmp(hf::ByName(hf::kByConfig), "config") == 0 &&
         std::strcmp(hf::ByName(hf::kByMenu), "menu") == 0);
}

// The game's own value of the bit is 1 and that is Off: the bit and the choice are each other's inverse, and the row lists On first.
void TheBitIsTheInverseOfTheChoice() {
  QCHECK(hf::BitFor(0) == 1 && hf::BitFor(1) == 0);  // Off is the bit set, the game's own value
  QCHECK(hf::FlipFor(1) == 0 && hf::FlipFor(0) == 1);
  QCHECK(hf::MenuIndexFor(1) == 0 && hf::MenuIndexFor(0) == 1);  // "On" is index 0
  QCHECK(!hf::MenuValueFor(0) && hf::MenuValueFor(1));            // the row's value: On is false, Off is true
  for (const int flip : {0, 1}) QCHECK(hf::MenuValueFor(hf::MenuIndexFor(flip)) == (hf::BitFor(flip) != 0));  // the value is the bit
  for (const int flip : {0, 1}) QCHECK(hf::FlipFor(hf::BitFor(flip)) == flip);
}

// The first delivery's write into the game's bit is logged once, by the poller, with the bit as the game held it and the bit it was
// set to.
void TheCarrierWriteIsLoggedOnce() {
  hf::ResetForTest();
  Capture log;
  hf::Start();
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
  NoKeyMeansOffAndNothingIsWritten();
  TheKeyIsThePlayersChoice();
  ANeverConfiguredStartIsOff();
  TheStartIsResolvedOnce();
  EnsureStartedResolvesTheStartOnce();
  ALeftoverMarkerFileIsNeitherReadNorTouched();
  AMenuChangeWritesTheKey();
  TheKeyIsSplicedIntoAnExistingFile();
  AChoiceThatCannotBeWrittenIsLoud();
  AChoiceWithNoConfigFileIsLoud();
  AFailedSaveIsRetriedAtTheNextPollAndSaidOnce();
  TheGameCallNeverWritesAFile();
  ANotOfferedStartIgnoresTheConfiguredChoiceAndSaysSoOnce();
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
