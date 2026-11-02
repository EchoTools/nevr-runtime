// Host test for the controller-input seam (#1202, docs/adr/0025-quest-autopilot-route.md): the pure rules
// (src/quest/plugin/input_seam.h) and the hook (src/quest/plugin/input_seam_hook.cpp), built without exceptions as the
// sentinel builds it. The handler is driven through the thunk's entry with stand-ins for libr15's vrapi_GetCurrentInputState
// and vrapi_GetInputDeviceCapabilities. No GOT slot is touched: the registration's install is injected, and
// got_pinned_test.cpp resolves both pinned slots in the real library.
//
// Built and run by `just test-quest-hooks`.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hook_log.h"
#include "pinned_targets.h"
#include "quest/plugin/input_seam.h"
#include "quest/plugin/input_seam_hook.h"
#include "quest/plugin/script_redirect.h"
#include "quest/tests/test_check.h"

namespace {

namespace is = nevr_quest::input_seam;
using Thunk = sentinel::pinned::LibR15GetCurrentInputStateThunk;
using sentinel::pinned::OvrMobileOpaque;

OvrMobileOpaque* const kOvr = reinterpret_cast<OvrMobileOpaque*>(0x0bad);
constexpr std::uint32_t kLeftId = 0x101;
constexpr std::uint32_t kRightId = 0x202;
constexpr std::uint32_t kBothId = 0x303;

// ---- the stand-in runtime ---------------------------------------------------------------------------------------------

// What the stand-in writes: a full state with every field distinct, so a stray write shows.
is::VrapiTrackedRemoteState RuntimeState(std::uint32_t type) {
  is::VrapiTrackedRemoteState s;
  std::memset(&s, 0xA5, sizeof(s));
  s.header.controllerType = type;
  s.header.timeInSeconds = 12.5;
  s.buttons = 0x00000001u;
  s.touches = 0x00000010u;
  s.indexTrigger = 0.25f;
  s.gripTrigger = 0.75f;
  s.joystickX = 0.125f;
  s.joystickY = -0.125f;
  s.joystickNoDeadZoneX = 0.1875f;
  s.joystickNoDeadZoneY = -0.1875f;
  return s;
}

int g_runtimeRc = 0;
std::uint32_t g_runtimeType = is::kControllerTypeTrackedRemote;
int g_runtimeCalls = 0;
OvrMobileOpaque* g_lastOvr = nullptr;
std::uint32_t g_lastId = 0;
int FakeRuntime(OvrMobileOpaque* ovr, std::uint32_t id, void* out) {
  ++g_runtimeCalls;
  g_lastOvr = ovr;
  g_lastId = id;
  if (out != nullptr) {
    const is::VrapiTrackedRemoteState s = RuntimeState(g_runtimeType);
    std::memcpy(out, &s, sizeof(s));
  }
  return g_runtimeRc;
}

int g_capsCalls = 0;
int g_capsRc = 0;
int FakeCaps(void* ovr, is::VrapiTrackedRemoteCaps* caps) {
  ++g_capsCalls;
  if (ovr != kOvr || caps->type != is::kControllerTypeTrackedRemote) return -1;
  if (g_capsRc != 0) return g_capsRc;
  switch (caps->deviceId) {
    case kLeftId: caps->controllerCapabilities = is::kCapsLeftHand | 0x10; return 0;
    case kRightId: caps->controllerCapabilities = is::kCapsRightHand | 0x10; return 0;
    case kBothId: caps->controllerCapabilities = is::kCapsLeftHand | is::kCapsRightHand; return 0;
    default: return -2;
  }
}

Thunk::Fn Entry() { return reinterpret_cast<Thunk::Fn>(Thunk::EntryAddress()); }

// ---- the injected environment -----------------------------------------------------------------------------------------

int g_installCalls = 0;
sentinel::GotStatus g_installStatus = sentinel::GotStatus::kOk;
// Set by the install: what a game call that lands the moment the slot changes would see.
int g_filterCallsDuringInstall = -1;
int g_filterCalls = 0;

sentinel::GotStatus FakeInstall() noexcept {
  ++g_installCalls;
  if (g_installStatus != sentinel::GotStatus::kOk) return g_installStatus;
  *Thunk::OriginalOut() = reinterpret_cast<void*>(&FakeRuntime);
  is::VrapiTrackedRemoteState s{};
  const int before = g_filterCalls;
  Entry()(kOvr, kLeftId, &s);
  g_filterCallsDuringInstall = g_filterCalls - before;
  return sentinel::GotStatus::kOk;
}
int g_resolveCalls = 0;
is::CapsFn ResolveCaps() noexcept {
  ++g_resolveCalls;
  return &FakeCaps;
}
is::CapsFn ResolveNothing() noexcept {
  ++g_resolveCalls;
  return nullptr;
}

is::Environment DevEnv() {
  is::Environment env;
  env.devBuild = true;
  env.install = &FakeInstall;
  env.resolveCaps = &ResolveCaps;
  return env;
}

// ---- filters ----------------------------------------------------------------------------------------------------------

std::vector<NevrQuestControllerState> g_seen;
void* g_lastUser = nullptr;
void Recording(void* user, NevrQuestControllerState* state) {
  ++g_filterCalls;
  g_lastUser = user;
  g_seen.push_back(*state);
}
void Driving(void* user, NevrQuestControllerState* state) {
  Recording(user, state);
  state->buttons = 0x00000002u;
  state->index_trigger = 1.0f;
  state->stick_x = 0.5f;
}

// ---- log capture ------------------------------------------------------------------------------------------------------

struct Line {
  sentinel::LogLevel level;
  std::string text;
};
std::vector<Line> g_lines;
void CaptureLine(sentinel::LogLevel level, const char* line) { g_lines.push_back({level, line}); }
struct Capture {
  sentinel::LogSink previous;
  Capture() : previous(sentinel::SetLogSink(&CaptureLine)) { g_lines.clear(); }
  ~Capture() { sentinel::SetLogSink(previous); }
};

std::string Field(const std::string& line, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const std::size_t at = line.find(needle);
  if (at == std::string::npos) return "<absent>";
  const std::size_t from = at + needle.size();
  if (from < line.size() && line[from] == '"') {
    const std::size_t end = line.find('"', from + 1);
    return line.substr(from + 1, end == std::string::npos ? std::string::npos : end - from - 1);
  }
  const std::size_t end = line.find_first_of(",}", from);
  return line.substr(from, end == std::string::npos ? std::string::npos : end - from);
}

// The one "input_seam" line, or an empty Line when there is not exactly one.
Line TheLine() {
  Line found{sentinel::LogLevel::kInfo, ""};
  int n = 0;
  for (const Line& l : g_lines) {
    if (l.text.find("\"event\":\"input_seam\"") != std::string::npos) {
      found = l;
      ++n;
    }
  }
  return n == 1 ? found : Line{sentinel::LogLevel::kInfo, ""};
}

void Reset() {
  is::ResetForTest();
  Thunk::Reset();
  *Thunk::OriginalOut() = reinterpret_cast<void*>(&FakeRuntime);
  g_runtimeRc = 0;
  g_runtimeType = is::kControllerTypeTrackedRemote;
  g_runtimeCalls = 0;
  g_lastOvr = nullptr;
  g_lastId = 0;
  g_capsCalls = 0;
  g_capsRc = 0;
  g_installCalls = 0;
  g_installStatus = sentinel::GotStatus::kOk;
  g_filterCallsDuringInstall = -1;
  g_filterCalls = 0;
  g_resolveCalls = 0;
  g_seen.clear();
  g_lastUser = nullptr;
}

bool SameBytes(const is::VrapiTrackedRemoteState& a, const is::VrapiTrackedRemoteState& b) {
  return std::memcmp(&a, &b, sizeof(a)) == 0;
}

// ---- the pure rules ---------------------------------------------------------------------------------------------------

void HandIsTheOneHandBit() {
  QCHECK(is::HandFromCaps(is::kCapsLeftHand) == NEVR_QUEST_HAND_LEFT);
  QCHECK(is::HandFromCaps(is::kCapsRightHand) == NEVR_QUEST_HAND_RIGHT);
  QCHECK(is::HandFromCaps(is::kCapsLeftHand | 0x30) == NEVR_QUEST_HAND_LEFT);
  QCHECK(is::HandFromCaps(is::kCapsLeftHand | is::kCapsRightHand) == NEVR_QUEST_HAND_UNKNOWN);
  QCHECK(is::HandFromCaps(0) == NEVR_QUEST_HAND_UNKNOWN);
}

void ReadStateCopiesTheSixFields() {
  const is::VrapiTrackedRemoteState vr = RuntimeState(4);
  const NevrQuestControllerState s = is::ReadState(vr, NEVR_QUEST_HAND_RIGHT);
  QCHECK(s.hand == NEVR_QUEST_HAND_RIGHT);
  QCHECK(s.buttons == 0x1u);
  QCHECK(s.touches == 0x10u);
  QCHECK(s.index_trigger == 0.25f);
  QCHECK(s.grip_trigger == 0.75f);
  QCHECK(s.stick_x == 0.125f);
  QCHECK(s.stick_y == -0.125f);
}

void AnUnchangedStateWritesNothing() {
  is::VrapiTrackedRemoteState vr = RuntimeState(4);
  vr.joystickX = std::nanf("");  // a NaN the runtime wrote and the filter kept is unchanged: the raw pair stays
  const is::VrapiTrackedRemoteState original = vr;
  const NevrQuestControllerState s = is::ReadState(vr, NEVR_QUEST_HAND_LEFT);
  is::WriteChanged(s, s, &vr);
  QCHECK(SameBytes(vr, original));
}

void EachChangedFieldIsWrittenAlone() {
  struct Case {
    const char* name;
    void (*change)(NevrQuestControllerState*);
    void (*expect)(is::VrapiTrackedRemoteState*);
  };
  const Case cases[] = {
      {"buttons", [](NevrQuestControllerState* s) { s->buttons = 0x3u; },
       [](is::VrapiTrackedRemoteState* v) { v->buttons = 0x3u; }},
      {"touches", [](NevrQuestControllerState* s) { s->touches = 0x7u; },
       [](is::VrapiTrackedRemoteState* v) { v->touches = 0x7u; }},
      {"index", [](NevrQuestControllerState* s) { s->index_trigger = 1.0f; },
       [](is::VrapiTrackedRemoteState* v) { v->indexTrigger = 1.0f; }},
      {"grip", [](NevrQuestControllerState* s) { s->grip_trigger = 0.0f; },
       [](is::VrapiTrackedRemoteState* v) { v->gripTrigger = 0.0f; }},
      // A stick change writes both pairs, the unchanged axis included.
      {"stick_x", [](NevrQuestControllerState* s) { s->stick_x = 0.5f; },
       [](is::VrapiTrackedRemoteState* v) {
         v->joystickX = 0.5f;
         v->joystickY = -0.125f;
         v->joystickNoDeadZoneX = 0.5f;
         v->joystickNoDeadZoneY = -0.125f;
       }},
      {"stick_y", [](NevrQuestControllerState* s) { s->stick_y = -1.0f; },
       [](is::VrapiTrackedRemoteState* v) {
         v->joystickX = 0.125f;
         v->joystickY = -1.0f;
         v->joystickNoDeadZoneX = 0.125f;
         v->joystickNoDeadZoneY = -1.0f;
       }},
  };
  for (const Case& c : cases) {
    is::VrapiTrackedRemoteState vr = RuntimeState(4);
    is::VrapiTrackedRemoteState want = vr;
    const NevrQuestControllerState before = is::ReadState(vr, NEVR_QUEST_HAND_LEFT);
    NevrQuestControllerState after = before;
    c.change(&after);
    c.expect(&want);
    is::WriteChanged(before, after, &vr);
    if (!SameBytes(vr, want)) std::fprintf(stderr, "WriteChanged case %s\n", c.name);
    QCHECK(SameBytes(vr, want));
  }
}

void TheHandCacheRemembersEachDevice() {
  is::HandCache cache;
  std::uint32_t hand = 99;
  QCHECK(!cache.Find(7, &hand));
  QCHECK(!cache.Find(0, &hand));  // an empty entry is not device 0
  cache.Store(7, NEVR_QUEST_HAND_LEFT);
  cache.Store(8, NEVR_QUEST_HAND_RIGHT);
  cache.Store(7, NEVR_QUEST_HAND_RIGHT);  // already stored: the first answer stays
  QCHECK(cache.Find(7, &hand) && hand == NEVR_QUEST_HAND_LEFT);
  QCHECK(cache.Find(8, &hand) && hand == NEVR_QUEST_HAND_RIGHT);
  QCHECK(!cache.Find(9, &hand));
  // Device 0 is an ID like any other: the valid bit, not the word, marks an entry.
  cache.Store(0, NEVR_QUEST_HAND_UNKNOWN);
  QCHECK(cache.Find(0, &hand) && hand == NEVR_QUEST_HAND_UNKNOWN);
  for (std::uint32_t id = 100; id < 100 + is::HandCache::kEntries; ++id) cache.Store(id, NEVR_QUEST_HAND_RIGHT);
  // Eight entries: 7, 8, 0 and the first five of the loop; the rest are not kept.
  QCHECK(cache.Find(104, &hand));
  QCHECK(!cache.Find(105, &hand));
  cache.Clear();
  QCHECK(!cache.Find(7, &hand));
}

// ---- registration -----------------------------------------------------------------------------------------------------

void BadArgumentsAreRefusedBeforeAnything() {
  int dummy = 0;
  struct Case {
    const char* id;
    NevrQuestInputFilterFn filter;
  };
  const Case cases[] = {{nullptr, &Recording}, {"", &Recording}, {"autopilot", nullptr}};
  for (const Case& c : cases) {
    Reset();
    Capture cap;
    QCHECK(is::RegisterWith(DevEnv(), c.id, c.filter, &dummy) == NEVR_QUEST_FILTER_BAD_ARGUMENT);
    QCHECK(g_installCalls == 0);
    QCHECK(g_resolveCalls == 0);
    const Line l = TheLine();
    QCHECK(Field(l.text, "result") == "refused");
    QCHECK(Field(l.text, "reason") == "bad_argument");
    QCHECK(l.level == sentinel::LogLevel::kError);
  }
  // Nothing was claimed: a good registration still succeeds.
  Reset();
  QCHECK(is::RegisterWith(DevEnv(), "autopilot", &Recording, nullptr) == NEVR_QUEST_FILTER_OK);
}

void AReleaseBuildRefusesAndInstallsNothing() {
  Reset();
  Capture cap;
  is::Environment env = DevEnv();
  env.devBuild = false;
  QCHECK(is::RegisterWith(env, "autopilot", &Recording, nullptr) == NEVR_QUEST_FILTER_NOT_DEV_BUILD);
  QCHECK(g_installCalls == 0);
  QCHECK(g_resolveCalls == 0);
  const Line l = TheLine();
  QCHECK(Field(l.text, "reason") == "not_dev_build");
  QCHECK(Field(l.text, "plugin") == "autopilot");
  // The release API is the one that refuses; the dev one is another table.
  const NevrQuestHostApi* release = is::HostApi(false);
  const NevrQuestHostApi* dev = is::HostApi(true);
  QCHECK(release != nullptr && dev != nullptr && release != dev);
  QCHECK(release->struct_size == sizeof(NevrQuestHostApi));
  QCHECK(dev->struct_size == sizeof(NevrQuestHostApi));
  QCHECK(release->register_input_filter("autopilot", &Recording, nullptr) == NEVR_QUEST_FILTER_NOT_DEV_BUILD);
  // The dev API runs the production registration: on the host no libr15 is loaded, so the capabilities slot does not
  // resolve and nothing is installed.
  {
    Capture inner;
    QCHECK(dev->register_input_filter("autopilot", &Recording, nullptr) == NEVR_QUEST_FILTER_HOOK_REFUSED);
    QCHECK(Field(TheLine().text, "reason") == "caps_unresolved");
  }
  // Still free: the release refusal claimed nothing.
  QCHECK(is::RegisterWith(DevEnv(), "autopilot", &Recording, nullptr) == NEVR_QUEST_FILTER_OK);
}

// The same struct carries the script redirect (#1737) in both builds: a combat unit is not an input filter, so the release API
// offers it too. Each member reaches script_redirect::Register, which looks in this binary's directory: no combat library is
// there, so the answer is MISSING_LIBRARY, and a null id is refused as a bad argument.
void ScriptRedirectIsOfferedByBothApis() {
  namespace sr = nevr_quest::script_redirect;
  const std::uint64_t hashes[] = {0x37f88adb0d8066f7ULL};
  for (const bool devBuild : {false, true}) {
    const NevrQuestHostApi* api = is::HostApi(devBuild);
    QCHECK(api != nullptr && api->struct_size >= offsetof(NevrQuestHostApi, register_script_redirect) +
                                                     sizeof(api->register_script_redirect));
    if (api == nullptr || api->register_script_redirect == nullptr) {
      QCHECK(false);  // the member is missing
      continue;
    }
    Capture inner;
    sr::ResetForTest();
    QCHECK(api->register_script_redirect("test", hashes, 1) == NEVR_SCRIPT_REDIRECT_MISSING_LIBRARY);
    QCHECK(api->register_script_redirect(nullptr, hashes, 1) == NEVR_SCRIPT_REDIRECT_BAD_ARGUMENT);
    QCHECK(sr::ArmedCount() == 0);
  }
  sr::ResetForTest();
}

void UnresolvedCapabilitiesRefuseWithoutInstalling() {
  Reset();
  Capture cap;
  is::Environment env = DevEnv();
  env.resolveCaps = &ResolveNothing;
  QCHECK(is::RegisterWith(env, "autopilot", &Recording, nullptr) == NEVR_QUEST_FILTER_HOOK_REFUSED);
  QCHECK(g_resolveCalls == 1);
  QCHECK(g_installCalls == 0);
  const Line l = TheLine();
  QCHECK(Field(l.text, "reason") == "caps_unresolved");
  QCHECK(Field(l.text, "status") == "<absent>");
  // The claim was given back.
  QCHECK(is::RegisterWith(DevEnv(), "autopilot", &Recording, nullptr) == NEVR_QUEST_FILTER_OK);
}

void AFailedInstallRefusesAndLeavesNoFilter() {
  Reset();
  Capture cap;
  g_installStatus = sentinel::GotStatus::kBuildIdMismatch;
  QCHECK(is::RegisterWith(DevEnv(), "autopilot", &Recording, nullptr) == NEVR_QUEST_FILTER_HOOK_REFUSED);
  QCHECK(g_installCalls == 1);
  const Line l = TheLine();
  QCHECK(Field(l.text, "result") == "refused");
  QCHECK(Field(l.text, "reason") == "hook_refused");
  QCHECK(Field(l.text, "status") == sentinel::GotStatusName(sentinel::GotStatus::kBuildIdMismatch));
  // A call through the entry (a poisoned slot) passes straight through, with no filter.
  is::VrapiTrackedRemoteState s{};
  QCHECK(Entry()(kOvr, kLeftId, &s) == 0);
  QCHECK(g_runtimeCalls == 1);
  QCHECK(g_filterCalls == 0);
  QCHECK(SameBytes(s, RuntimeState(4)));
  // The claim was given back.
  g_installStatus = sentinel::GotStatus::kOk;
  QCHECK(is::RegisterWith(DevEnv(), "autopilot", &Recording, nullptr) == NEVR_QUEST_FILTER_OK);
}

void TheFirstRegistrationOwnsTheSeam() {
  Reset();
  Capture cap;
  QCHECK(is::RegisterWith(DevEnv(), "autopilot", &Recording, nullptr) == NEVR_QUEST_FILTER_OK);
  {
    const Line l = TheLine();
    QCHECK(l.level == sentinel::LogLevel::kInfo);
    QCHECK(Field(l.text, "result") == "installed");
    QCHECK(Field(l.text, "reason") == "ok");
    QCHECK(Field(l.text, "plugin") == "autopilot");
    QCHECK(Field(l.text, "slot") == "vrapi_GetCurrentInputState");
    QCHECK(Field(l.text, "status") == sentinel::GotStatusName(sentinel::GotStatus::kOk));
  }
  // The filter was live before the slot changed: a call landing during the install was filtered.
  QCHECK(g_filterCallsDuringInstall == 1);
  g_lines.clear();
  QCHECK(is::RegisterWith(DevEnv(), "other", &Driving, nullptr) == NEVR_QUEST_FILTER_OWNED);
  QCHECK(g_installCalls == 1);
  const Line l = TheLine();
  QCHECK(Field(l.text, "reason") == "owned");
  QCHECK(Field(l.text, "plugin") == "other");
  QCHECK(Field(l.text, "owned_by") == "plugin:autopilot");
}

// ---- the handler ------------------------------------------------------------------------------------------------------

void WithNoRegistrationTheEntryPassesThrough() {
  Reset();
  is::VrapiTrackedRemoteState s{};
  g_runtimeRc = 0;
  QCHECK(Entry()(kOvr, kRightId, &s) == 0);
  QCHECK(g_runtimeCalls == 1);
  QCHECK(g_lastOvr == kOvr && g_lastId == kRightId);
  QCHECK(SameBytes(s, RuntimeState(4)));
  QCHECK(g_capsCalls == 0);
}

void TheFilterSeesTheHandAndItsChangesReachTheGame() {
  Reset();
  int user = 0;
  QCHECK(is::RegisterWith(DevEnv(), "autopilot", &Driving, &user) == NEVR_QUEST_FILTER_OK);
  g_seen.clear();
  g_runtimeCalls = 0;
  const int capsAfterInstall = g_capsCalls;  // the install's own call asked for the left device
  is::VrapiTrackedRemoteState left{};
  is::VrapiTrackedRemoteState right{};
  is::VrapiTrackedRemoteState both{};
  QCHECK(Entry()(kOvr, kLeftId, &left) == 0);
  QCHECK(Entry()(kOvr, kRightId, &right) == 0);
  QCHECK(Entry()(kOvr, kBothId, &both) == 0);
  QCHECK(g_runtimeCalls == 3);
  QCHECK(g_lastUser == &user);
  QCHECK(g_seen.size() == 3);
  if (g_seen.size() == 3) {
    QCHECK(g_seen[0].hand == NEVR_QUEST_HAND_LEFT);
    QCHECK(g_seen[1].hand == NEVR_QUEST_HAND_RIGHT);
    QCHECK(g_seen[2].hand == NEVR_QUEST_HAND_UNKNOWN);
    // The filter is handed what the runtime wrote.
    QCHECK(g_seen[1].buttons == 0x1u && g_seen[1].index_trigger == 0.25f && g_seen[1].stick_x == 0.125f);
  }
  // Left was cached by the install's call; right and both are asked once each.
  QCHECK(g_capsCalls - capsAfterInstall == 2);
  is::VrapiTrackedRemoteState want = RuntimeState(4);
  want.buttons = 0x2u;
  want.indexTrigger = 1.0f;
  want.joystickX = 0.5f;
  want.joystickY = -0.125f;
  want.joystickNoDeadZoneX = 0.5f;
  want.joystickNoDeadZoneY = -0.125f;
  QCHECK(SameBytes(right, want));
  // Asked once per device: the next frame finds every hand in the cache.
  QCHECK(Entry()(kOvr, kRightId, &right) == 0);
  QCHECK(Entry()(kOvr, kBothId, &both) == 0);
  QCHECK(g_capsCalls - capsAfterInstall == 2);
}

void AFailedCapabilitiesCallIsUnknownAndAskedAgain() {
  Reset();
  QCHECK(is::RegisterWith(DevEnv(), "autopilot", &Recording, nullptr) == NEVR_QUEST_FILTER_OK);
  g_seen.clear();
  g_capsCalls = 0;
  g_capsRc = -7;
  is::VrapiTrackedRemoteState s{};
  QCHECK(Entry()(kOvr, kRightId, &s) == 0);
  QCHECK(Entry()(kOvr, kRightId, &s) == 0);
  QCHECK(g_capsCalls == 2);
  QCHECK(g_seen.size() == 2 && g_seen[0].hand == NEVR_QUEST_HAND_UNKNOWN);
  g_capsRc = 0;
  QCHECK(Entry()(kOvr, kRightId, &s) == 0);
  QCHECK(g_seen.size() == 3 && g_seen[2].hand == NEVR_QUEST_HAND_RIGHT);
  QCHECK(g_capsCalls == 3);
}

void OnlyASuccessfulTrackedRemoteStateIsFiltered() {
  Reset();
  QCHECK(is::RegisterWith(DevEnv(), "autopilot", &Driving, nullptr) == NEVR_QUEST_FILTER_OK);
  g_filterCalls = 0;
  // A failed call: the runtime's result comes back and the buffer is the runtime's.
  g_runtimeRc = -1001;
  is::VrapiTrackedRemoteState s{};
  QCHECK(Entry()(kOvr, kRightId, &s) == -1001);
  QCHECK(SameBytes(s, RuntimeState(4)));
  // Another controller type (a hand, a gamepad): untouched.
  g_runtimeRc = 0;
  g_runtimeType = 8;
  QCHECK(Entry()(kOvr, kRightId, &s) == 0);
  QCHECK(SameBytes(s, RuntimeState(8)));
  // No buffer: the runtime is still called with the game's arguments, nothing is read.
  g_runtimeType = is::kControllerTypeTrackedRemote;
  QCHECK(Entry()(kOvr, kRightId, nullptr) == 0);
  QCHECK(g_lastId == kRightId);
  QCHECK(g_filterCalls == 0);
}

void AFilterThatChangesNothingLeavesEveryByte() {
  Reset();
  QCHECK(is::RegisterWith(DevEnv(), "autopilot", &Recording, nullptr) == NEVR_QUEST_FILTER_OK);
  is::VrapiTrackedRemoteState s{};
  QCHECK(Entry()(kOvr, kLeftId, &s) == 0);
  QCHECK(SameBytes(s, RuntimeState(4)));
}

void TheResultNamesAreTheLogTokens() {
  QCHECK(std::strcmp(is::ResultName(NEVR_QUEST_FILTER_OK), "ok") == 0);
  QCHECK(std::strcmp(is::ResultName(NEVR_QUEST_FILTER_BAD_ARGUMENT), "bad_argument") == 0);
  QCHECK(std::strcmp(is::ResultName(NEVR_QUEST_FILTER_NOT_DEV_BUILD), "not_dev_build") == 0);
  QCHECK(std::strcmp(is::ResultName(NEVR_QUEST_FILTER_OWNED), "owned") == 0);
  QCHECK(std::strcmp(is::ResultName(NEVR_QUEST_FILTER_HOOK_REFUSED), "hook_refused") == 0);
  QCHECK(std::strcmp(is::ResultName(42), "unknown") == 0);
}

}  // namespace

int main() {
  HandIsTheOneHandBit();
  ReadStateCopiesTheSixFields();
  AnUnchangedStateWritesNothing();
  EachChangedFieldIsWrittenAlone();
  TheHandCacheRemembersEachDevice();
  BadArgumentsAreRefusedBeforeAnything();
  AReleaseBuildRefusesAndInstallsNothing();
  ScriptRedirectIsOfferedByBothApis();
  UnresolvedCapabilitiesRefuseWithoutInstalling();
  AFailedInstallRefusesAndLeavesNoFilter();
  TheFirstRegistrationOwnsTheSeam();
  WithNoRegistrationTheEntryPassesThrough();
  TheFilterSeesTheHandAndItsChangesReachTheGame();
  AFailedCapabilitiesCallIsUnknownAndAskedAgain();
  OnlyASuccessfulTrackedRemoteStateIsFiltered();
  AFilterThatChangesNothingLeavesEveryByte();
  TheResultNamesAreTheLogTokens();
  is::ResetForTest();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "input_seam_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("input_seam_test: all checks pass\n");
  return 0;
}
