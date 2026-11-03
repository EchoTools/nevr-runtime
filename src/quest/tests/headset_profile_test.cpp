// Host test for the headset identity (ADR 0031): the rules of quest/headset_profile.h on device names, the settings each profile
// holds against the values the code gave before that table (the eye pair per texture level, the q15t System heap, the level
// rate), and the identity step (integration/headset_identity_step.cpp). A host has no system property service, so the step
// reads every property as empty: it must name the generic headset, set the profile the texture hook serves and write one
// headset_profile line.
//
// Built and run by `just test-quest-hooks`, next to texture_quality_hook_test.

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "quest/headset_profile.h"
#include "quest/integration/headset_identity_step.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/texture_quality.h"
#include "quest/sentinel/texture_quality_hook.h"
#include "quest/tests/test_check.h"

namespace {

namespace headset_profile = nevr_quest::headset_profile;
namespace tq = nevr_quest::texture_quality;

// ---- the rules ----------------------------------------------------------------------------------------------------------

void ExpectRule(const char* device, const char* rule, headset_profile::Id id) {
  const headset_profile::Match match = headset_profile::IdentifyDevice(device);
  QCHECK(std::strcmp(match.rule, rule) == 0);
  QCHECK(match.profile->id == id);
}

void TheDeviceNamesTheRule() {
  ExpectRule("hollywood", "quest2_device", headset_profile::Id::kQuest2);
  ExpectRule("eureka", "quest3_device", headset_profile::Id::kQuest3);
  ExpectRule("panther", "generic", headset_profile::Id::kGeneric);
  ExpectRule("", "generic", headset_profile::Id::kGeneric);  // no device property: the generic rule matches
}

// The rules compare the whole device string: a case change, a trailing space or a longer name is not that headset.
void TheDeviceMatchesExactly() {
  ExpectRule("Hollywood", "generic", headset_profile::Id::kGeneric);
  ExpectRule("hollywood ", "generic", headset_profile::Id::kGeneric);
  ExpectRule("hollywood2", "generic", headset_profile::Id::kGeneric);
  ExpectRule("eurekas", "generic", headset_profile::Id::kGeneric);
}

// Only the device decides a rule (ADR 0031, R2): the model and the manufacturer do not.
void OnlyTheDeviceDecides() {
  headset_profile::Inputs in;
  in.manufacturer = "Oculus";
  in.model = "Quest 3";
  in.device = "hollywood";
  QCHECK(headset_profile::Identify(in).profile->id == headset_profile::Id::kQuest2);
  in.model = "Quest 2";
  in.device = "eureka";
  QCHECK(headset_profile::Identify(in).profile->id == headset_profile::Id::kQuest3);
}

// ---- the settings -------------------------------------------------------------------------------------------------------

// The eye pairs the code gave before the table (texture_quality_hook_test.cpp's rows), indexed by the level's ordinal (0 stock).
struct Pair {
  int width;
  int height;
};
constexpr Pair kQuest2Eyes[headset_profile::kEyeLevelCount] = {
    {0, 0}, {1152, 1264}, {1440, 1584}, {1728, 1904}, {2016, 2224}, {2304, 2528}, {2592, 2848}};
constexpr Pair kQuest3Eyes[headset_profile::kEyeLevelCount] = {
    {0, 0}, {1344, 1408}, {1680, 1760}, {2376, 2568}, {3072, 3380}, {3584, 3944}, {4096, 4506}};

void TheEyeTablesAreTodaysTables() {
  for (std::size_t i = 0; i < headset_profile::kEyeLevelCount; ++i) {
    QCHECK(headset_profile::kQuest2.eye[i].width == kQuest2Eyes[i].width);
    QCHECK(headset_profile::kQuest2.eye[i].height == kQuest2Eyes[i].height);
    QCHECK(headset_profile::kQuest3.eye[i].width == kQuest3Eyes[i].width);
    QCHECK(headset_profile::kQuest3.eye[i].height == kQuest3Eyes[i].height);
    QCHECK(headset_profile::kGeneric.eye[i].width == kQuest3Eyes[i].width);
    QCHECK(headset_profile::kGeneric.eye[i].height == kQuest3Eyes[i].height);
  }
}

// Each texture level answers its pair through the profile (texture_quality.h SizeFor); stock answers nothing of its own.
void EachLevelIsTodaysPair() {
  const tq::Level levels[] = {tq::Level::kVeryLow, tq::Level::kLow, tq::Level::kMedium,
                              tq::Level::kHigh,    tq::Level::kVeryHigh, tq::Level::kUltra};
  for (std::size_t i = 0; i < 6; ++i) {
    const tq::EyeSize q2 = tq::SizeFor(levels[i], headset_profile::kQuest2);
    QCHECK(q2.width == kQuest2Eyes[i + 1].width && q2.height == kQuest2Eyes[i + 1].height);
    const tq::EyeSize q3 = tq::SizeFor(levels[i], headset_profile::kQuest3);
    QCHECK(q3.width == kQuest3Eyes[i + 1].width && q3.height == kQuest3Eyes[i + 1].height);
    const tq::EyeSize generic = tq::SizeFor(levels[i], headset_profile::kGeneric);
    QCHECK(generic.width == q3.width && generic.height == q3.height);
  }
  QCHECK(tq::SizeFor(tq::Level::kStock, headset_profile::kQuest2).width == 0);
}

// The q15t System heap is 512 MiB on a Quest 2 and 0 on the others (the row keeps its own 640 MiB target). The level rate is 72
// on a Quest 2 and 90 on the others.
void TheHeapAndTheRateAreTodaysValues() {
  QCHECK(headset_profile::kQuest2.systemHeapMiB == 512);
  QCHECK(headset_profile::kQuest3.systemHeapMiB == 0);
  QCHECK(headset_profile::kGeneric.systemHeapMiB == 0);
  QCHECK(headset_profile::kQuest2.levelHz == 72.f);
  QCHECK(headset_profile::kQuest3.levelHz == 90.f);
  QCHECK(headset_profile::kGeneric.levelHz == 90.f);
}

// ---- the identity step on a host ----------------------------------------------------------------------------------------

std::vector<std::string> g_lines;
void CaptureLine(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }

void TheStepNamesTheGenericHeadsetOnAHostAndLogsOneLine() {
  tq::SetHeadsetProfile(nullptr);
  g_lines.clear();
  const sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  QCHECK(nevr_quest::integration::IdentifyHeadset());
  sentinel::SetLogSink(previous);

  // The step sets the profile the texture hook serves: the generic headset's table, which is the Quest 3 pairs.
  QCHECK(tq::ActiveHeadsetProfile() == &headset_profile::kGeneric);
  const tq::EyeSize high = tq::SizeFor(tq::Level::kHigh, *tq::ActiveHeadsetProfile());
  QCHECK(high.width == 3072 && high.height == 3380);

  std::string line;
  int lines = 0;
  for (const std::string& l : g_lines) {
    if (l.find("\"event\":\"headset_profile\"") != std::string::npos) {
      line = l;
      ++lines;
    }
  }
  QCHECK(lines == 1);
  QCHECK(line.find("\"level\":\"info\"") != std::string::npos);
  QCHECK(line.find("\"device\":\"\"") != std::string::npos);
  QCHECK(line.find("\"system_name\":\"unread\"") != std::string::npos);
  QCHECK(line.find("\"rule\":\"generic\"") != std::string::npos);
  QCHECK(line.find("\"profile\":\"generic\"") != std::string::npos);
}

}  // namespace

int main() {
  TheDeviceNamesTheRule();
  TheDeviceMatchesExactly();
  OnlyTheDeviceDecides();
  TheEyeTablesAreTodaysTables();
  EachLevelIsTodaysPair();
  TheHeapAndTheRateAreTodaysValues();
  TheStepNamesTheGenericHeadsetOnAHostAndLogsOneLine();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "headset_profile_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("headset_profile_test: all checks pass\n");
  return 0;
}
