// Host test for the headset identity (ADR 0031): the rules of quest/headset_profile.h on device names, the settings each profile
// holds against the values the code gave before that table (the eye pair per texture level, the q15t System heap, the level
// rate), and the identity step (integration/headset_identity_step.cpp) run over property sets. The step's one headset_profile
// line is checked field by field against the properties the reader gave it, so a blanked, swapped or misread input fails here.
//
// The property sets are fixtures. The tree records no manufacturer or model value for these devices, so they carry the device
// names the rules match and nothing more; the headset run checks the real values (the PR's headset prediction).
//
// Built and run by `just test-quest-hooks`, next to texture_quality_hook_test.

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
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

// Identify and IdentifyDevice answer the same rule for the same device, whatever the other properties are. The q15t plugin calls
// IdentifyDevice with the one property it reads; the identity step calls Identify with all three (ADR 0031, R2).
void IdentifyDeviceAgreesWithIdentifyForEveryRule() {
  const std::string_view devices[] = {"hollywood", "eureka", "panther", "", "Hollywood", "hollywood2"};
  for (const std::string_view device : devices) {
    headset_profile::Inputs in;
    in.manufacturer = "Acme";
    in.model = "Unlisted 1";
    in.device = device;
    const headset_profile::Match full = headset_profile::Identify(in);
    const headset_profile::Match one = headset_profile::IdentifyDevice(device);
    QCHECK(std::strcmp(full.rule, one.rule) == 0);
    QCHECK(full.profile == one.profile);
  }
  for (const headset_profile::Rule& rule : headset_profile::kRules) {
    if (!rule.device.empty()) QCHECK(std::strcmp(headset_profile::IdentifyDevice(rule.device).rule, rule.name) == 0);
  }
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
    QCHECK(headset_profile::kGeneric.eye[i].width == kQuest2Eyes[i].width);
    QCHECK(headset_profile::kGeneric.eye[i].height == kQuest2Eyes[i].height);
  }
}

// The generic eye table is the Quest 2 one, which is no larger than the Quest 3 table at any level: a headset no rule names is
// never given a bigger eye buffer than the Quest 3 values would give it.
void GenericIsNoLargerThanQuest3AtEveryLevel() {
  for (std::size_t i = 1; i < headset_profile::kEyeLevelCount; ++i) {
    QCHECK(headset_profile::kGeneric.eye[i].width <= kQuest3Eyes[i].width);
    QCHECK(headset_profile::kGeneric.eye[i].height <= kQuest3Eyes[i].height);
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
    QCHECK(generic.width == q2.width && generic.height == q2.height);
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

// ---- the identity step on recorded property sets ------------------------------------------------------------------------

// The three properties the step reads.
struct PropertySet {
  const char* manufacturer;
  const char* model;
  const char* device;
};

// Fixtures: a Quest 2 and a Quest 3 by their device names, and a device no rule names.
constexpr PropertySet kHollywood = {"Oculus", "Quest 2", "hollywood"};
constexpr PropertySet kEureka = {"Oculus", "Quest 3", "eureka"};
constexpr PropertySet kUnlisted = {"Acme", "Unlisted 1", "panther"};

PropertySet g_set = {"", "", ""};

// The reader the step is given: the set's three properties, and an empty value for anything else.
int ReadFromSet(const char* name, char* value) {
  const char* text = "";
  if (std::strcmp(name, "ro.product.manufacturer") == 0) {
    text = g_set.manufacturer;
  } else if (std::strcmp(name, "ro.product.model") == 0) {
    text = g_set.model;
  } else if (std::strcmp(name, "ro.product.device") == 0) {
    text = g_set.device;
  }
  std::snprintf(value, 92, "%s", text);
  return static_cast<int>(std::strlen(value));
}

std::vector<std::string> g_lines;
void CaptureLine(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }

// The one headset_profile line the last run wrote.
std::string TheOneLine() {
  std::string line;
  int lines = 0;
  for (const std::string& l : g_lines) {
    if (l.find("\"event\":\"headset_profile\"") != std::string::npos) {
      line = l;
      ++lines;
    }
  }
  QCHECK(lines == 1);
  return line;
}

// Runs the step over the reader's set and returns the line it wrote. The profile is reset first, so what is checked is what
// this run set.
std::string IdentifyWith(const PropertySet& set) {
  g_set = set;
  tq::SetHeadsetProfile(nullptr);
  g_lines.clear();
  const sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  const bool ok = nevr_quest::integration::IdentifyHeadsetWith(&ReadFromSet);
  sentinel::SetLogSink(previous);
  QCHECK(ok);
  return TheOneLine();
}

// True when the line carries `field`, a JSON key and its value as the sink writes them.
bool Has(const std::string& line, const std::string& field) { return line.find(field) != std::string::npos; }

// Every input the reader gave is on the line under its own key, and the rule, profile and consequence are the ones named.
void ExpectTheLine(const PropertySet& set, const char* rule, const char* profile, const char* consequence) {
  const std::string line = IdentifyWith(set);
  QCHECK(Has(line, "\"level\":\"info\""));
  QCHECK(Has(line, "\"system_name\":\"unread\""));
  QCHECK(Has(line, std::string("\"manufacturer\":\"") + set.manufacturer + "\""));
  QCHECK(Has(line, std::string("\"model\":\"") + set.model + "\""));
  QCHECK(Has(line, std::string("\"device\":\"") + set.device + "\""));
  QCHECK(Has(line, std::string("\"rule\":\"") + rule + "\""));
  QCHECK(Has(line, std::string("\"profile\":\"") + profile + "\""));
  QCHECK(Has(line, std::string("\"consequence\":\"") + consequence + "\""));
}

// A Quest 2 by its device: the line names its inputs, the Quest 2 rule and profile, the high pair the Quest 2 table gives, the
// 512 MiB heap and the 72 Hz rate; the step sets the Quest 2 profile for the texture hook.
void TheQuest2LineNamesItsInputsAndWhatFollows() {
  ExpectTheLine(kHollywood, "quest2_device", "quest2", "eye_high=2016x2224 system_heap_mib=512 level_hz=72");
  QCHECK(tq::ActiveHeadsetProfile() == &headset_profile::kQuest2);
}

// A Quest 3 by its device: the Quest 3 rule, the Quest 3 high pair, the row's heap and 90 Hz.
void TheQuest3LineNamesItsInputsAndWhatFollows() {
  ExpectTheLine(kEureka, "quest3_device", "quest3", "eye_high=3072x3380 system_heap_mib=row_target level_hz=90");
  QCHECK(tq::ActiveHeadsetProfile() == &headset_profile::kQuest3);
}

// A device no rule names: the generic rule and profile, with the generic eye table (the Quest 2 high pair), the row's heap and 90 Hz.
void AnUnlistedDeviceTakesTheGenericProfile() {
  ExpectTheLine(kUnlisted, "generic", "generic", "eye_high=2016x2224 system_heap_mib=row_target level_hz=90");
  QCHECK(tq::ActiveHeadsetProfile() == &headset_profile::kGeneric);
}

// On a host the property service is absent, so the default reader gives every property empty and the step names the generic
// headset.
void TheHostDefaultReadsNothingAndNamesTheGenericHeadset() {
  tq::SetHeadsetProfile(nullptr);
  g_lines.clear();
  const sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  QCHECK(nevr_quest::integration::IdentifyHeadset());
  sentinel::SetLogSink(previous);
  const std::string line = TheOneLine();
  QCHECK(Has(line, "\"manufacturer\":\"\""));
  QCHECK(Has(line, "\"model\":\"\""));
  QCHECK(Has(line, "\"device\":\"\""));
  QCHECK(Has(line, "\"rule\":\"generic\""));
  QCHECK(Has(line, "\"profile\":\"generic\""));
  QCHECK(tq::ActiveHeadsetProfile() == &headset_profile::kGeneric);
}

}  // namespace

int main() {
  TheDeviceNamesTheRule();
  TheDeviceMatchesExactly();
  OnlyTheDeviceDecides();
  IdentifyDeviceAgreesWithIdentifyForEveryRule();
  TheEyeTablesAreTodaysTables();
  GenericIsNoLargerThanQuest3AtEveryLevel();
  EachLevelIsTodaysPair();
  TheHeapAndTheRateAreTodaysValues();
  TheQuest2LineNamesItsInputsAndWhatFollows();
  TheQuest3LineNamesItsInputsAndWhatFollows();
  AnUnlistedDeviceTakesTheGenericProfile();
  TheHostDefaultReadsNothingAndNamesTheGenericHeadset();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "headset_profile_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("headset_profile_test: all checks pass\n");
  return 0;
}
