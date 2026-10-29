// Host test for the render-configuration log (#1139, docs/adr/0022-render-config-log.md): the real hooks and poller
// (src/quest/sentinel/render_config_hook.cpp), built without exceptions as the sentinel builds it, driven through the thunk
// entries with stand-ins for the VrApi functions. No GOT slot is touched: got_pinned_test.cpp resolves the pinned slots in the
// real library, and tests/quest TestHookFramesCarryNoPersonality covers the frames.
//
// The expected values are written out here as literals, so a change to a field name, a unit or a source fails this test
// rather than following it.
//
// Built and run by `just test-quest-hooks`.

#include <elf.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hook_log.h"
#include "pinned_targets.h"
#include "quest/tests/test_check.h"
#include "render_config.h"
#include "render_config_share.h"
#include "texture_quality_hook.h"

namespace {

namespace rc = nevr_quest::render_config;
namespace tq = nevr_quest::texture_quality;
namespace pinned = sentinel::pinned;
using pinned::OvrJavaOpaque;
using pinned::OvrMobileOpaque;
using pinned::OvrSubmitFrameDescriptionOpaque;

// ---- stand-ins for the VrApi functions -------------------------------------------------------------------------------------

int g_refreshResult = 0;
int g_clockResult = 0;
float g_reportedHertz = 90.0f;
int g_calls = 0;
int g_lastPropertyCall = -1;
int g_lastValueCall = -1;

int FakeSetRefreshRate(OvrMobileOpaque*, float) {
  ++g_calls;
  return g_refreshResult;
}
int FakeSetClockLevels(OvrMobileOpaque*, int, int) {
  ++g_calls;
  return g_clockResult;
}
int FakeSetPropertyInt(const OvrJavaOpaque*, int property, int value) {
  ++g_calls;
  g_lastPropertyCall = property;
  g_lastValueCall = value;
  return 0;
}
float FakeGetSystemPropertyFloat(const OvrJavaOpaque*, int property) {
  ++g_calls;
  return property == 4 ? g_reportedHertz : 12345.0f;
}
int FakeSubmitFrame(OvrMobileOpaque*, const OvrSubmitFrameDescriptionOpaque*) {
  ++g_calls;
  return 0;
}
int FakeSysPropInt(const OvrJavaOpaque*, int id) {
  if (id == 5) return 1440;
  if (id == 6) return 1584;
  return 0;
}

using RefreshT = pinned::LibR15SetDisplayRefreshRateThunk;
using ClockT = pinned::LibR15SetClockLevelsThunk;
using PropT = pinned::LibR15SetPropertyIntThunk;
using FloatT = pinned::LibR15GetSystemPropertyFloatThunk;
using SubmitT = pinned::LibR15SubmitFrame2Thunk;

template <typename Thunk>
typename Thunk::Fn Entry() {
  return reinterpret_cast<typename Thunk::Fn>(Thunk::EntryAddress());
}

OvrMobileOpaque* const kOvr = reinterpret_cast<OvrMobileOpaque*>(0x0b17);
const OvrJavaOpaque* const kJava = reinterpret_cast<const OvrJavaOpaque*>(0x5eed);

// Puts the stand-ins behind the five thunks and arms the handlers, as an install does.
void WireThunks() {
  RefreshT::Reset();
  ClockT::Reset();
  PropT::Reset();
  FloatT::Reset();
  SubmitT::Reset();
  *RefreshT::OriginalOut() = reinterpret_cast<void*>(&FakeSetRefreshRate);
  *ClockT::OriginalOut() = reinterpret_cast<void*>(&FakeSetClockLevels);
  *PropT::OriginalOut() = reinterpret_cast<void*>(&FakeSetPropertyInt);
  *FloatT::OriginalOut() = reinterpret_cast<void*>(&FakeGetSystemPropertyFloat);
  *SubmitT::OriginalOut() = reinterpret_cast<void*>(&FakeSubmitFrame);
  rc::ArmForTest();
}

// ---- log capture ------------------------------------------------------------------------------------------------------------

std::mutex g_linesMutex;
std::vector<std::string> g_lines;
void CaptureLine(sentinel::LogLevel, const char* line) {
  std::lock_guard<std::mutex> lock(g_linesMutex);
  g_lines.emplace_back(line);
}

struct Capture {
  sentinel::LogSink previous;
  Capture() : previous(sentinel::SetLogSink(&CaptureLine)) { Clear(); }
  ~Capture() { sentinel::SetLogSink(previous); }
  static void Clear() {
    std::lock_guard<std::mutex> lock(g_linesMutex);
    g_lines.clear();
  }
};

std::vector<std::string> Lines(const char* event) {
  std::lock_guard<std::mutex> lock(g_linesMutex);
  std::vector<std::string> out;
  const std::string needle = std::string("\"event\":\"") + event + "\"";
  for (const std::string& l : g_lines) {
    if (l.find(needle) != std::string::npos) out.push_back(l);
  }
  return out;
}

// The text after `"key":` up to the closing quote (a string) or the next ',' or '}' (a number); "<absent>" when missing.
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

// ---- the sources ------------------------------------------------------------------------------------------------------------

rc::Globals g_globals;
bool g_globalsReadable = true;
bool ReadFakeGlobals(rc::Globals* out) noexcept {
  if (!g_globalsReadable) return false;
  *out = g_globals;
  return true;
}
void ReadFakeDevice(rc::Device* out) noexcept {
  std::snprintf(out->model, sizeof(out->model), "%s", "Quest 3");
  std::snprintf(out->osBuild, sizeof(out->osBuild), "%s", "SQ3A.220605.009.A1");
}
const rc::Source kFake{&ReadFakeGlobals, &ReadFakeDevice};

void ResetAll() {
  rc::ResetForTest();
  tq::ResetForTest();
  tq::SystemPropertyIntThunk::Reset();
  g_globals = rc::Globals{};
  g_globals.eyeW = 1440;
  g_globals.eyeH = 1584;
  g_globals.overrideW = 0;
  g_globals.overrideH = 0;
  g_globals.msaaMode = 1;
  g_globalsReadable = true;
  g_refreshResult = 0;
  g_clockResult = 0;
  g_reportedHertz = 90.0f;
  g_calls = 0;
  WireThunks();
}

// The game's calls before the first frame: what UpdateTracking and the foveation setters do.
void GameStartup() {
  QCHECK(Entry<RefreshT>()(kOvr, 72.0f) == 0);
  QCHECK(Entry<ClockT>()(kOvr, 2, 2) == 0);
  QCHECK(Entry<PropT>()(kJava, 0xf, 4) == 0);
  QCHECK(Entry<PropT>()(kJava, 0x1e, 1) == 0);
  QCHECK(Entry<FloatT>()(kJava, 4) == 90.0f);
}

// ---- the hooks ----------------------------------------------------------------------------------------------------------------

void EveryHookReturnsTheOriginalsResultAndPassesTheArgumentsOn() {
  Capture capture;
  ResetAll();
  g_refreshResult = -1000;
  g_clockResult = -7;
  QCHECK(Entry<RefreshT>()(kOvr, 90.0f) == -1000);
  QCHECK(Entry<ClockT>()(kOvr, 5, 5) == -7);
  g_reportedHertz = 120.0f;
  QCHECK(Entry<FloatT>()(kJava, 4) == 120.0f);
  QCHECK(Entry<FloatT>()(kJava, 9) == 12345.0f);  // every other property: the runtime's answer, untouched
  QCHECK(Entry<PropT>()(kJava, 0xf, 3) == 0 && g_lastPropertyCall == 0xf && g_lastValueCall == 3);
  QCHECK(Entry<PropT>()(kJava, 0x33, 9) == 0 && g_lastPropertyCall == 0x33 && g_lastValueCall == 9);
  QCHECK(Entry<SubmitT>()(kOvr, nullptr) == 0);
  QCHECK(g_calls == 7);
  QCHECK(g_lines.empty());  // a hook never logs
}

// ---- the line at the first frame and on a change ---------------------------------------------------------------------------

void NothingIsLoggedBeforeTheFirstFrame() {
  Capture capture;
  ResetAll();
  GameStartup();
  QCHECK(!rc::PollOnce(kFake));
  QCHECK(Lines("render_config").empty());
}

void TheFirstFrameWritesOneLineWithTheConfiguration() {
  Capture capture;
  ResetAll();
  GameStartup();
  QCHECK(Entry<SubmitT>()(kOvr, nullptr) == 0);
  QCHECK(rc::PollOnce(kFake));
  const std::vector<std::string> lines = Lines("render_config");
  QCHECK(lines.size() == 1);
  if (lines.size() != 1) return;
  const std::string& l = lines[0];
  QCHECK(Field(l, "platform") == "quest");
  QCHECK(Field(l, "reason") == "first_frame");
  QCHECK(Field(l, "changed") == "<absent>");
  QCHECK(Field(l, "device_model") == "Quest 3");
  QCHECK(Field(l, "os_build") == "SQ3A.220605.009.A1");
  QCHECK(Field(l, "refresh_requested_hz") == "72.0");
  QCHECK(Field(l, "refresh_request_result") == "0");
  QCHECK(Field(l, "refresh_reported_hz") == "90.0");
  QCHECK(Field(l, "eye_w") == "1440" && Field(l, "eye_h") == "1584");
  QCHECK(Field(l, "eye_source") == "runtime_suggested");
  QCHECK(Field(l, "texture_quality") == "stock" && Field(l, "texture_quality_source") == "default");
  QCHECK(Field(l, "native_eye_w") == "<absent>" && Field(l, "native_eye_h") == "<absent>");
  QCHECK(Field(l, "foveation_level") == "4" && Field(l, "foveation_dynamic") == "1");
  QCHECK(Field(l, "cpu_level") == "2" && Field(l, "gpu_level") == "2" && Field(l, "clock_result") == "0");
  QCHECK(Field(l, "msaa_mode") == "1");
  QCHECK(Field(l, "spacewarp") == "unknown");
  QCHECK(Field(l, "thunk_faults") == "<absent>");
  QCHECK(Field(l, "frames") == "1");
  QCHECK(l.find("\"level\":\"info\"") != std::string::npos);
  QCHECK(l.find("\"truncated\"") == std::string::npos && l.size() < 768);  // the whole line fits one log line
}

void ANewFrameWithNothingChangedWritesNothing() {
  Capture capture;
  ResetAll();
  GameStartup();
  Entry<SubmitT>()(kOvr, nullptr);
  QCHECK(rc::PollOnce(kFake));
  for (int i = 0; i < 100; ++i) Entry<SubmitT>()(kOvr, nullptr);  // frames grow every poll; they are not a change
  QCHECK(!rc::PollOnce(kFake));
  QCHECK(!rc::PollOnce(kFake));
  QCHECK(Lines("render_config").size() == 1);
}

void AForcedFoveationChangeWritesALineNamingIt() {
  Capture capture;
  ResetAll();
  GameStartup();
  Entry<SubmitT>()(kOvr, nullptr);
  QCHECK(rc::PollOnce(kFake));
  // Pause: CVR::SetFoveationLevel(0); unpause: (4).
  Entry<PropT>()(kJava, 0xf, 0);
  QCHECK(rc::PollOnce(kFake));
  Entry<PropT>()(kJava, 0xf, 4);
  QCHECK(rc::PollOnce(kFake));
  const std::vector<std::string> lines = Lines("render_config");
  QCHECK(lines.size() == 3);
  if (lines.size() != 3) return;
  QCHECK(Field(lines[1], "reason") == "change" && Field(lines[1], "changed") == "foveation_level");
  QCHECK(Field(lines[1], "foveation_level") == "0");
  QCHECK(Field(lines[2], "reason") == "change" && Field(lines[2], "changed") == "foveation_level");
  QCHECK(Field(lines[2], "foveation_level") == "4");
}

void EachFieldThatChangesIsNamedInTheLine() {
  Capture capture;
  ResetAll();
  GameStartup();
  Entry<SubmitT>()(kOvr, nullptr);
  QCHECK(rc::PollOnce(kFake));

  Entry<RefreshT>()(kOvr, 90.0f);  // the game asks for another rate
  QCHECK(rc::PollOnce(kFake));
  g_reportedHertz = 120.0f;  // the runtime reports another
  Entry<FloatT>()(kJava, 4);
  QCHECK(rc::PollOnce(kFake));
  Entry<ClockT>()(kOvr, 5, 5);
  QCHECK(rc::PollOnce(kFake));
  Entry<PropT>()(kJava, 0x1e, 0);
  QCHECK(rc::PollOnce(kFake));
  g_globals.msaaMode = 2;
  QCHECK(rc::PollOnce(kFake));
  g_globals.eyeW = 1680;
  g_globals.eyeH = 1760;
  QCHECK(rc::PollOnce(kFake));
  g_globals.overrideW = 1920;  // the game's RT-size override takes over
  g_globals.overrideH = 1080;
  QCHECK(rc::PollOnce(kFake));

  const std::vector<std::string> lines = Lines("render_config");
  QCHECK(lines.size() == 8);
  if (lines.size() != 8) return;
  QCHECK(Field(lines[1], "changed") == "refresh_requested_hz" && Field(lines[1], "refresh_requested_hz") == "90.0");
  QCHECK(Field(lines[2], "changed") == "refresh_reported_hz" && Field(lines[2], "refresh_reported_hz") == "120.0");
  QCHECK(Field(lines[3], "changed") == "cpu_level,gpu_level" && Field(lines[3], "cpu_level") == "5");
  QCHECK(Field(lines[4], "changed") == "foveation_dynamic" && Field(lines[4], "foveation_dynamic") == "0");
  QCHECK(Field(lines[5], "changed") == "msaa_mode" && Field(lines[5], "msaa_mode") == "2");
  QCHECK(Field(lines[6], "changed") == "eye_w,eye_h" && Field(lines[6], "eye_w") == "1680");
  QCHECK(Field(lines[7], "changed") == "eye_source" && Field(lines[7], "eye_source") == "game_rt_size_override");
}

void UnreadableGlobalsLeaveTheirFieldsOutOfTheLine() {
  Capture capture;
  ResetAll();
  g_globalsReadable = false;
  GameStartup();
  Entry<SubmitT>()(kOvr, nullptr);
  QCHECK(rc::PollOnce(kFake));
  const std::vector<std::string> lines = Lines("render_config");
  QCHECK(lines.size() == 1);
  if (lines.size() != 1) return;
  QCHECK(Field(lines[0], "eye_w") == "<absent>" && Field(lines[0], "eye_h") == "<absent>");
  QCHECK(Field(lines[0], "eye_source") == "<absent>" && Field(lines[0], "msaa_mode") == "<absent>");
  QCHECK(Field(lines[0], "refresh_requested_hz") == "72.0");  // the hooks' fields do not depend on the globals
}

void AGameThatNeverSetsAFieldLeavesItOut() {
  Capture capture;
  ResetAll();
  Entry<SubmitT>()(kOvr, nullptr);
  QCHECK(rc::PollOnce(kFake));
  const std::vector<std::string> lines = Lines("render_config");
  QCHECK(lines.size() == 1);
  if (lines.size() != 1) return;
  for (const char* key : {"refresh_requested_hz", "refresh_request_result", "refresh_reported_hz", "cpu_level", "gpu_level",
                          "clock_result", "foveation_level", "foveation_dynamic"}) {
    QCHECK(Field(lines[0], key) == "<absent>");
  }
}

void AThunkWithoutAnOriginalIsReportedInTheLine() {
  Capture capture;
  ResetAll();
  GameStartup();
  Entry<SubmitT>()(kOvr, nullptr);
  QCHECK(rc::PollOnce(kFake));
  ClockT::Reset();  // no original: the thunk returns without calling the handler and counts a fault
  QCHECK(Entry<ClockT>()(kOvr, 5, 5) == 0);
  QCHECK(rc::PollOnce(kFake));
  const std::vector<std::string> lines = Lines("render_config");
  QCHECK(lines.size() == 2);
  if (lines.size() != 2) return;
  QCHECK(Field(lines[1], "changed") == "thunk_faults" && Field(lines[1], "thunk_faults") == "1");
  QCHECK(lines[1].find("\"level\":\"warn\"") != std::string::npos);
}

// ---- the texture-quality choice ---------------------------------------------------------------------------------------------

sentinel::GotStatus InstallAllOk(int) noexcept { return sentinel::GotStatus::kOk; }
sentinel::GotStatus InstallNone(int) noexcept { return sentinel::GotStatus::kModuleNotLoaded; }

void ALevelThatReplacesTheSizeIsNamedWithTheRuntimesOwnAnswer() {
  Capture capture;
  ResetAll();
  // The texture-quality hook serves a level; its native counters hold the runtime's own answers.
  *tq::SystemPropertyIntThunk::OriginalOut() = reinterpret_cast<void*>(&FakeSysPropInt);
  QCHECK(tq::ArmForTest(tq::Level::kHigh));
  QCHECK(Entry<tq::SystemPropertyIntThunk>()(kJava, 5) == 2016);  // the Quest 2 `high` size, replaced
  QCHECK(Entry<tq::SystemPropertyIntThunk>()(kJava, 6) == 2224);
  QCHECK(rc::InstallVia(tq::Level::kHigh, true, &InstallAllOk));
  g_globals.eyeW = 2016;
  g_globals.eyeH = 2224;
  GameStartup();
  Entry<SubmitT>()(kOvr, nullptr);
  QCHECK(rc::PollOnce(kFake));
  const std::vector<std::string> lines = Lines("render_config");
  QCHECK(lines.size() == 1);
  if (lines.size() != 1) return;
  QCHECK(Field(lines[0], "eye_source") == "nevr_texture_quality");
  QCHECK(Field(lines[0], "texture_quality") == "high" && Field(lines[0], "texture_quality_source") == "config");
  QCHECK(Field(lines[0], "native_eye_w") == "1440" && Field(lines[0], "native_eye_h") == "1584");
  QCHECK(Field(lines[0], "eye_w") == "2016" && Field(lines[0], "eye_h") == "2224");
  QCHECK(lines[0].find("\"truncated\"") == std::string::npos && lines[0].size() < 768);  // the widest line fits
  tq::ResetForTest();
}

void TheDefaultLevelIsReportedAsTheDefault() {
  Capture capture;
  ResetAll();
  QCHECK(rc::InstallVia(tq::Level::kStock, false, &InstallAllOk));
  GameStartup();
  Entry<SubmitT>()(kOvr, nullptr);
  QCHECK(rc::PollOnce(kFake));
  const std::vector<std::string> lines = Lines("render_config");
  QCHECK(lines.size() == 1);
  if (lines.size() != 1) return;
  QCHECK(Field(lines[0], "texture_quality") == "stock" && Field(lines[0], "texture_quality_source") == "default");
  QCHECK(Field(lines[0], "eye_source") == "runtime_suggested");
}

// ---- the install ------------------------------------------------------------------------------------------------------------

int g_failSlot = -1;
sentinel::GotStatus InstallAllButOne(int slot) noexcept {
  return slot == g_failSlot ? sentinel::GotStatus::kSlotChanged : sentinel::GotStatus::kOk;
}

void AnInstallOwnsTheSharedSlotsAndSaysSo() {
  Capture capture;
  ResetAll();
  QCHECK(rc::InstallVia(tq::Level::kStock, false, &InstallAllOk));
  for (const sentinel::RenderConfigSlot slot :
       {sentinel::RenderConfigSlot::kSetDisplayRefreshRate, sentinel::RenderConfigSlot::kSetClockLevels,
        sentinel::RenderConfigSlot::kSetPropertyInt, sentinel::RenderConfigSlot::kGetSystemPropertyFloat}) {
    QCHECK(sentinel::RenderConfigSlotOwned(slot));
  }
  const std::vector<std::string> lines = Lines("render_config_install");
  QCHECK(lines.size() == 1);
  if (lines.size() != 1) return;
  QCHECK(Field(lines[0], "result") == "installed" && Field(lines[0], "installed") == "5" && Field(lines[0], "wanted") == "5");
  QCHECK(Field(lines[0], "vrapi_SubmitFrame2") == "ok" && Field(lines[0], "vrapi_SetClockLevels") == "ok");
  QCHECK(lines[0].find("\"level\":\"info\"") != std::string::npos);
}

void ARefusedSlotIsNamedAndTheOthersStayInstalled() {
  Capture capture;
  ResetAll();
  g_failSlot = 2;  // vrapi_SetPropertyInt
  QCHECK(!rc::InstallVia(tq::Level::kStock, false, &InstallAllButOne));
  g_failSlot = -1;
  QCHECK(!sentinel::RenderConfigSlotOwned(sentinel::RenderConfigSlot::kSetPropertyInt));
  QCHECK(sentinel::RenderConfigSlotOwned(sentinel::RenderConfigSlot::kSetClockLevels));
  const std::vector<std::string> lines = Lines("render_config_install");
  QCHECK(lines.size() == 1);
  if (lines.size() != 1) return;
  QCHECK(Field(lines[0], "result") == "partial" && Field(lines[0], "installed") == "4");
  QCHECK(Field(lines[0], "vrapi_SetPropertyInt") == "slot_changed" && Field(lines[0], "vrapi_SetClockLevels") == "ok");
  QCHECK(lines[0].find("\"level\":\"error\"") != std::string::npos);
}

// ---- the globals ------------------------------------------------------------------------------------------------------------

constexpr std::uint64_t kSegBase = 0x379c000;
constexpr std::size_t kSegSize = 0x8000;
constexpr std::uint64_t kMsaaAt = 0x379c080;
constexpr std::uint64_t kEyeAt = 0x37a33e0;
constexpr std::uint64_t kOverrideAt = 0x37a33e8;

// An in-memory libr15 look-alike: one PT_LOAD over a buffer, one PT_NOTE carrying `buildIdHex` at its start.
struct FakeImage {
  alignas(8) unsigned char memory[kSegSize] = {};
  Elf64_Phdr phdr[2] = {};
  sentinel::ElfImage image;

  explicit FakeImage(const char* buildIdHex, std::uint64_t loadSize = kSegSize) {
    Elf64_Nhdr nhdr{};
    nhdr.n_namesz = 4;
    nhdr.n_descsz = 20;
    nhdr.n_type = NT_GNU_BUILD_ID;
    std::memcpy(memory, &nhdr, sizeof(nhdr));
    std::memcpy(memory + sizeof(nhdr), "GNU", 4);
    for (int i = 0; i < 20; ++i) {
      unsigned byte = 0;
      std::sscanf(buildIdHex + 2 * i, "%2x", &byte);
      memory[sizeof(nhdr) + 4 + i] = static_cast<unsigned char>(byte);
    }
    phdr[0].p_type = PT_LOAD;
    phdr[0].p_vaddr = kSegBase;
    phdr[0].p_memsz = loadSize;
    phdr[1].p_type = PT_NOTE;
    phdr[1].p_vaddr = kSegBase;
    phdr[1].p_memsz = sizeof(nhdr) + 4 + 20;
    image.base = reinterpret_cast<std::uintptr_t>(memory) - kSegBase;
    image.phdr = phdr;
    image.phnum = 2;
  }
  void Put32(std::uint64_t vaddr, std::int32_t value) { std::memcpy(memory + (vaddr - kSegBase), &value, sizeof(value)); }
};

void TheGlobalsAreReadOnlyFromTheProvedImage() {
  FakeImage ok(pinned::LibR15PatchedBuildId());
  ok.Put32(kEyeAt, 1440);
  ok.Put32(kEyeAt + 4, 1584);
  ok.Put32(kOverrideAt, 0);
  ok.Put32(kOverrideAt + 4, 0);
  ok.Put32(kMsaaAt, 1);
  rc::Globals g;
  QCHECK(rc::ReadGlobalsFrom(ok.image, pinned::LibR15PatchedBuildId(), &g));
  QCHECK(g.eyeW == 1440 && g.eyeH == 1584 && g.overrideW == 0 && g.overrideH == 0 && g.msaaMode == 1);

  // Another build of the library: its addresses mean something else, so nothing is read.
  FakeImage other("00112233445566778899aabbccddeeff00112233");
  rc::Globals untouched;
  QCHECK(!rc::ReadGlobalsFrom(other.image, pinned::LibR15PatchedBuildId(), &untouched));
  QCHECK(untouched.eyeW == rc::kUnset && untouched.msaaMode == rc::kUnset);

  // An address outside every PT_LOAD is never dereferenced.
  FakeImage small(pinned::LibR15PatchedBuildId(), 0x100);
  QCHECK(!rc::ReadGlobalsFrom(small.image, pinned::LibR15PatchedBuildId(), &untouched));
}

void ASizeOutsideTheRangeOfAnEyeBufferIsNotReported() {
  for (const std::int32_t bad : {0, -1, 16385, 0x3f800000}) {  // the last is the bit pattern of the float 1.0
    FakeImage image(pinned::LibR15PatchedBuildId());
    image.Put32(kEyeAt, bad);
    image.Put32(kEyeAt + 4, 1584);
    image.Put32(kMsaaAt, 1);
    rc::Globals g;
    QCHECK(rc::ReadGlobalsFrom(image.image, pinned::LibR15PatchedBuildId(), &g));
    QCHECK(g.eyeW == rc::kUnset && g.eyeH == rc::kUnset);
    QCHECK(g.msaaMode == 1);
  }
}

// ---- the pure parts ---------------------------------------------------------------------------------------------------------

void TheChangedListNamesEveryDifferingFieldInOrder() {
  rc::Snapshot a, b;
  char out[256];
  QCHECK(rc::ChangedFields(a, b, out, sizeof(out)) == 0 && out[0] == '\0');
  b.frames = 99;  // frames never count
  b.overrideW = 1;  // nor does the override pair (it feeds eye_source)
  QCHECK(rc::ChangedFields(a, b, out, sizeof(out)) == 0);
  b.msaaMode = 2;
  b.refreshReportedMilli = 120000;
  b.thunkFaults = 1;
  QCHECK(rc::ChangedFields(a, b, out, sizeof(out)) == 3);
  QCHECK(std::strcmp(out, "refresh_reported_hz,msaa_mode,thunk_faults") == 0);
  char tiny[8];
  QCHECK(rc::ChangedFields(a, b, tiny, sizeof(tiny)) == 3 && std::strlen(tiny) < sizeof(tiny));  // counted, never overrun
}

void HertzAreWrittenInTenths() {
  char out[16];
  rc::FormatHertz(72000, out, sizeof(out));
  QCHECK(std::strcmp(out, "72.0") == 0);
  rc::FormatHertz(89990, out, sizeof(out));
  QCHECK(std::strcmp(out, "90.0") == 0);
  rc::FormatHertz(119950, out, sizeof(out));
  QCHECK(std::strcmp(out, "120.0") == 0);
  rc::FormatHertz(rc::kUnset, out, sizeof(out));
  QCHECK(out[0] == '\0');
}

void AHostWithoutLibr15ReportsNoGlobalsAndNoCrash() {
  Capture capture;
  ResetAll();
  GameStartup();
  Entry<SubmitT>()(kOvr, nullptr);
  QCHECK(rc::PollOnce(rc::ProductionSource()));  // libr15 is not loaded here: the line is written without the globals
  const std::vector<std::string> lines = Lines("render_config");
  QCHECK(lines.size() == 1);
  if (lines.size() != 1) return;
  QCHECK(Field(lines[0], "eye_w") == "<absent>" && Field(lines[0], "refresh_requested_hz") == "72.0");
  QCHECK(Field(lines[0], "device_model") == "unknown");
}

// ---- the poller thread ------------------------------------------------------------------------------------------------------

void TheThreadWritesTheLineAfterTheFirstFrameAndAgainAfterAChange() {
  Capture capture;
  ResetAll();
  GameStartup();
  QCHECK(rc::StartPollerForTest(kFake, 20));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  QCHECK(Lines("render_config").empty());  // no frame yet
  Entry<SubmitT>()(kOvr, nullptr);
  auto waitFor = [](std::size_t count) {
    for (int i = 0; i < 500 && Lines("render_config").size() < count; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return Lines("render_config").size() >= count;
  };
  QCHECK(waitFor(1));
  Entry<PropT>()(kJava, 0xf, 0);
  QCHECK(waitFor(2));
  rc::StopPollerForTest();
  const std::vector<std::string> lines = Lines("render_config");
  QCHECK(lines.size() == 2);
  if (lines.size() == 2) QCHECK(Field(lines[1], "changed") == "foveation_level");
}

// Install() starts the poller once a slot holds its thunk, and not otherwise.
void AnInstallStartsThePollerOnlyWhenASlotHoldsItsThunk() {
  Capture capture;
  ResetAll();
  GameStartup();
  Entry<SubmitT>()(kOvr, nullptr);
  QCHECK(rc::InstallVia(tq::Level::kStock, false, &InstallAllOk, &kFake, 20));
  for (int i = 0; i < 500 && Lines("render_config").empty(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  QCHECK(Lines("render_config").size() == 1);
  rc::StopPollerForTest();

  ResetAll();
  Capture::Clear();
  GameStartup();
  Entry<SubmitT>()(kOvr, nullptr);
  QCHECK(!rc::InstallVia(tq::Level::kStock, false, &InstallNone, &kFake, 20));  // nothing installed: nothing to wait for
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  QCHECK(Lines("render_config").empty());
  QCHECK(Field(Lines("render_config_install")[0], "result") == "not_installed");
}

}  // namespace

int main() {
  EveryHookReturnsTheOriginalsResultAndPassesTheArgumentsOn();
  NothingIsLoggedBeforeTheFirstFrame();
  TheFirstFrameWritesOneLineWithTheConfiguration();
  ANewFrameWithNothingChangedWritesNothing();
  AForcedFoveationChangeWritesALineNamingIt();
  EachFieldThatChangesIsNamedInTheLine();
  UnreadableGlobalsLeaveTheirFieldsOutOfTheLine();
  AGameThatNeverSetsAFieldLeavesItOut();
  AThunkWithoutAnOriginalIsReportedInTheLine();
  ALevelThatReplacesTheSizeIsNamedWithTheRuntimesOwnAnswer();
  TheDefaultLevelIsReportedAsTheDefault();
  AnInstallOwnsTheSharedSlotsAndSaysSo();
  ARefusedSlotIsNamedAndTheOthersStayInstalled();
  TheGlobalsAreReadOnlyFromTheProvedImage();
  ASizeOutsideTheRangeOfAnEyeBufferIsNotReported();
  TheChangedListNamesEveryDifferingFieldInOrder();
  HertzAreWrittenInTenths();
  AHostWithoutLibr15ReportsNoGlobalsAndNoCrash();
  TheThreadWritesTheLineAfterTheFirstFrameAndAgainAfterAChange();
  AnInstallStartsThePollerOnlyWhenASlotHoldsItsThunk();
  rc::ResetForTest();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "render_config_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("render_config_hook_test: all checks passed\n");
  return 0;
}
