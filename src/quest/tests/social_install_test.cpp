// The social hook's decision and its installation, on the host.
//
//   - SelectSocialObject hands the game the facade only for pnsovr's CNSOVRSocial of the pinned
//     build, and passes everything else through, counting why. It never logs.
//   - The handler, armed through a record this test defines and run through the real CallbackThunk
//     entry the GOT slot points at, applies that decision to the original's result.
//   - InstallSocialHook(false) touches nothing; with libr15.so not loaded it fails without arming.
//
// Run: social_install_test

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"
#include "quest/social/social_abi.h"
#include "quest/social/social_facade.h"
#include "quest/social/social_install.h"
#include "quest/social/social_invite_gate.h"
#include "quest/tests/test_check.h"
#include "runtime/compat/social_names.h"

namespace {

using namespace quest_social;

std::vector<std::string> g_lines;
void Capture(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }

std::size_t CountLines(const char* needle) {
  std::size_t n = 0;
  for (const std::string& l : g_lines) n += l.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

constexpr std::uintptr_t kBias = 0x7000000000ULL;

PnsovrView Loaded() { return {true, true, kBias}; }
PnsovrView Absent() { return {false, false, 0}; }
PnsovrView WrongBuild() { return {true, false, kBias}; }

// An object whose first word is a vptr.
struct FakeObject {
  std::uintptr_t vptr = 0;
  std::uint8_t rest[56] = {};
};

FakeObject OvrSocial() {
  FakeObject o;
  o.vptr = kBias + static_cast<std::uintptr_t>(kOvrSocialVptrVaddr);
  return o;
}

// This test's own record for the production handler (tests define their own records).
NEVR_HOOK_RECORD(kTestSocialHook, SocialThunk, &OnSocialHandler);

std::atomic<std::uint64_t> g_dummy{0};
int g_fakeCalls = 0;
void* g_fakeResult = nullptr;
void* FakeOriginal(std::uint64_t) {
  ++g_fakeCalls;
  return g_fakeResult;
}

std::uint64_t C(const std::atomic<std::uint64_t>& counter) { return counter.load(); }

void TestSelect() {
  FakeObject facade;  // any distinct address stands in for the facade object
  FakeObject ovr = OvrSocial();
  ResetCountersForTest();
  const SocialCounters counters = Counters();
  g_lines.clear();
  QCHECK(SelectSocialObject(&ovr, &facade, &Loaded) == &facade);
  QCHECK(C(counters.selected) == 1);

  QCHECK(SelectSocialObject(nullptr, &facade, &Loaded) == nullptr);  // a provider with no social object stays that way
  QCHECK(C(counters.nullResult) == 1);
  QCHECK(SelectSocialObject(&ovr, &facade, &Absent) == &ovr);
  QCHECK(SelectSocialObject(&ovr, &facade, &WrongBuild) == &ovr);
  QCHECK(C(counters.pnsovrUnavailable) == 2);

  FakeObject foreign = OvrSocial();
  foreign.vptr += 8;  // another class's vtable (or a shifted one)
  QCHECK(SelectSocialObject(&foreign, &facade, &Loaded) == &foreign);
  FakeObject other;
  other.vptr = kBias;  // the library base itself is not the vtable
  QCHECK(SelectSocialObject(&other, &facade, &Loaded) == &other);
  QCHECK(C(counters.foreignObject) == 2);

  QCHECK(SelectSocialObject(&ovr, nullptr, &Loaded) == &ovr);  // no facade: never substitute nothing
  QCHECK(SelectSocialObject(&ovr, &facade, nullptr) == &ovr);
  QCHECK(C(counters.selected) == 1);  // none of the pass-throughs counted as a selection
  QCHECK(g_lines.empty());            // the decision never logs
}

void TestHandlerThroughThunk() {
  FakeObject ovr = OvrSocial();
  PnsovrLookup previous = SetPnsovrLookup(&Loaded);
  PublishFacadeObject();
  ResetCountersForTest();
  SocialThunk::Reset();
  *SocialThunk::OriginalOut() = reinterpret_cast<void*>(&FakeOriginal);
  SocialThunk::Arm(kTestSocialHook);
  using Entry = void* (*)(std::uint64_t);
  Entry entry = nullptr;
  void* addr = SocialThunk::EntryAddress();
  std::memcpy(&entry, &addr, sizeof(entry));
  g_lines.clear();

  // Pinned pnsovr loaded and the provider returned its CNSOVRSocial: the game receives the facade.
  g_fakeResult = &ovr;
  g_fakeCalls = 0;
  void* got = entry(0x1234);
  QCHECK(g_fakeCalls == 1);  // the original always runs first
  QCHECK(got == Facade::Instance().Object());
  QCHECK(got != static_cast<void*>(&ovr));
  QCHECK(C(Counters().selected) == 1);
  QCHECK(SocialThunk::Calls() == 1);

  // The provider returned nothing: nothing is substituted.
  g_fakeResult = nullptr;
  QCHECK(entry(0x1234) == nullptr);
  QCHECK(C(Counters().nullResult) == 1);

  // pnsovr is not the pinned build: the original object passes through.
  SetPnsovrLookup(&WrongBuild);
  g_fakeResult = &ovr;
  QCHECK(entry(0x1234) == static_cast<void*>(&ovr));
  QCHECK(C(Counters().pnsovrUnavailable) == 1);

  // Disarmed: straight through to the original.
  SetPnsovrLookup(&Loaded);
  SocialThunk::Disarm();
  QCHECK(entry(0x1234) == static_cast<void*>(&ovr));
  QCHECK(g_lines.empty());  // the whole hook path logged nothing

  SocialThunk::Reset();
  SetPnsovrLookup(previous);
}

void TestCounterRegistration() {
  // The reporter takes kMaxReportCounters counters in all; the social package uses 19 (the thunk's calls and faults, three
  // pass-through counters, the selection, and the facade's thirteen) and leaves the rest.
  sentinel::StopReporter();
  QCHECK(RegisterSocialReportCounters());
  for (unsigned i = 0; i < sentinel::kMaxReportCounters - 19; ++i) {
    QCHECK(sentinel::RegisterReportCounter("filler", &g_dummy));  // 19 + the rest = the whole table
  }
  QCHECK(!sentinel::RegisterReportCounter("one-too-many", &g_dummy));
  sentinel::StopReporter();
}

void TestInstall() {
  g_lines.clear();
  // Quest registers the friend-name decoder explicitly (no static initializer): a disabled install leaves it
  // alone, an enabled one registers it.
  QCHECK(nevr_social_names::DecoderSlot().load() == nullptr);
  const InstallResult off = InstallSocialHook(false);
  QCHECK(off.status == InstallStatus::kDisabled);
  QCHECK(CountLines("\"status\":\"disabled\"") == 1);
  QCHECK(nevr_social_names::DecoderSlot().load() == nullptr);

  // libr15.so is not loaded in this process: GotHook refuses and the callback is left disarmed.
  g_lines.clear();
  const InstallResult on = InstallSocialHook(true);
  QCHECK(on.status == InstallStatus::kHookFailed);
  QCHECK(on.got == sentinel::GotStatus::kModuleNotLoaded);
  QCHECK(nevr_social_names::DecoderSlot().load() != nullptr);
  QCHECK(CountLines("\"status\":\"hook_failed\"") == 1);
  QCHECK(CountLines("\"got\":\"") >= 1);
  *SocialThunk::OriginalOut() = reinterpret_cast<void*>(&FakeOriginal);
  g_fakeResult = nullptr;
  g_fakeCalls = 0;
  using Entry = void* (*)(std::uint64_t);
  Entry entry = nullptr;
  void* addr = SocialThunk::EntryAddress();
  std::memcpy(&entry, &addr, sizeof(entry));
  QCHECK(entry(1) == nullptr && g_fakeCalls == 1);  // pass-through, not the handler
  SocialThunk::Reset();

  QCHECK(std::string(InstallStatusName(InstallStatus::kOk)) == "ok");
}

// This test's own record for the invite gate's production handler.
NEVR_HOOK_RECORD(kTestGateHook, GateThunk, &OnBooleanHandler);

int g_boolCalls = 0;
std::uint32_t g_boolValue = 0;
std::uint32_t FakeBoolean(const void*, const char*, std::uint32_t, std::uint32_t) {
  ++g_boolCalls;
  return g_boolValue;
}

void TestInviteGateResult() {
  QCHECK(GateResult("npe|firstmatch|completed", 0) == 1);  // the gate reads true whatever the profile says
  QCHECK(GateResult("npe|firstmatch|completed", 1) == 1);
  for (const char* other : {"npe|firstmatch|completedX", "npe|firstmatch|complete", "npe|firstmatch", "Npe|firstmatch|completed",
                            "", "npe|firstmatch|completed|x", "social|group"}) {
    QCHECK(GateResult(other, 0) == 0);  // every other path is the game's own answer
    QCHECK(GateResult(other, 1) == 1);
  }
  QCHECK(GateResult(nullptr, 0) == 0 && GateResult(nullptr, 1) == 1);
}

void TestInviteGateThroughThunk() {
  ResetInviteGateCountersForTest();
  GateThunk::Reset();
  *GateThunk::OriginalOut() = reinterpret_cast<void*>(&FakeBoolean);
  GateThunk::Arm(kTestGateHook);
  using Entry = std::uint32_t (*)(const void*, const char*, std::uint32_t, std::uint32_t);
  Entry entry = nullptr;
  void* addr = GateThunk::EntryAddress();
  std::memcpy(&entry, &addr, sizeof(entry));
  g_lines.clear();
  g_boolCalls = 0;
  g_boolValue = 0;
  QCHECK(entry(nullptr, "npe|firstmatch|completed", 0, 0) == 1);  // profile said false: forced true
  QCHECK(C(InviteGateCounters().forced) == 1);
  g_boolValue = 1;
  QCHECK(entry(nullptr, "npe|firstmatch|completed", 0, 0) == 1);  // profile said true: nothing to force
  QCHECK(C(InviteGateCounters().forced) == 1);
  g_boolValue = 0;
  QCHECK(entry(nullptr, "social|group", 0, 0) == 0);  // other reads are the game's
  g_boolValue = 1;
  QCHECK(entry(nullptr, "social|group", 0, 0) == 1);
  QCHECK(g_boolCalls == 4);  // the original always runs
  QCHECK(GateThunk::Calls() == 4);
  QCHECK(g_lines.empty());  // the handler path logged nothing
  GateThunk::Disarm();
  g_boolValue = 0;
  QCHECK(entry(nullptr, "npe|firstmatch|completed", 0, 0) == 0);  // disarmed: straight through
  GateThunk::Reset();
}

bool NoImage(const char*, sentinel::ElfImage*) { return false; }
bool EmptyImage(const char*, sentinel::ElfImage* out) {
  *out = sentinel::ElfImage{};  // loaded, but with no program headers: no build id can be read
  return true;
}

// The game's CJson functions are found only in libr15 of the pinned build.
void TestResolveGameJson() {
  for (const sentinel::ImageLookup lookup : {&NoImage, &EmptyImage, static_cast<sentinel::ImageLookup>(nullptr)}) {
    const GameJson json = ResolveGameJson(lookup);
    QCHECK(json.reset == nullptr && json.decode == nullptr && json.encode == nullptr);
  }
  QCHECK(kLibR15CJsonResetVaddr == 0xfa227cULL);
  QCHECK(kLibR15CJsonDecodeFromVaddr == 0xfa7e8cULL);
  QCHECK(kLibR15CJsonEncodeToCompactVaddr == 0xfa7e64ULL);
}

void TestTarget() {
  const sentinel::GotTarget t = LibR15Social();
  QCHECK(std::strcmp(t.module, "libr15.so") == 0);
  QCHECK(std::strcmp(t.symbol, "_ZN10NRadEngine11CNSProvider6SocialEm") == 0);
  QCHECK(t.kind == sentinel::RelocKind::kJumpSlot);
  QCHECK(t.buildId != nullptr && std::strcmp(t.buildId, "b243509c08ce677aeb95fa348016949b3fc45230") == 0);
  QCHECK(t.slotVaddr.has_value() && *t.slotVaddr == 0x36ef528ULL);
}

}  // namespace

int main() {
  const sentinel::LogSink previous = sentinel::SetLogSink(&Capture);
  TestSelect();
  TestHandlerThroughThunk();
  TestInviteGateResult();
  TestInviteGateThroughThunk();
  TestCounterRegistration();
  TestInstall();
  TestResolveGameJson();
  TestTarget();
  sentinel::SetLogSink(previous);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "social_install_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("social_install_test: all checks pass\n");
  return 0;
}
