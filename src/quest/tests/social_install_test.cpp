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
  // The reporter takes kMaxReportCounters counters in all; the social package uses 26 (the thunk's calls and faults, three
  // pass-through counters, the selection, the facade's thirteen, the rich presence trace's four and its three local
  // answers) and leaves the rest.
  sentinel::StopReporter();
  QCHECK(RegisterSocialReportCounters());
  for (unsigned i = 0; i < sentinel::kMaxReportCounters - 26; ++i) {
    QCHECK(sentinel::RegisterReportCounter("filler", &g_dummy));  // 26 + the rest = the whole table
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

// ---- rich presence trace (#393) ------------------------------------------------------------------------

bool Is(const char* actual, const char* expected) { return actual != nullptr && std::strcmp(actual, expected) == 0; }

constexpr std::size_t kVtableWords = 2 + kOvrRichPresenceSlotCount;
std::uintptr_t g_fakeVtable[kVtableWords];
std::uintptr_t g_presenceBias = 0;
PnsovrView PresenceLoaded() { return {true, true, g_presenceBias}; }

int g_destinationAnswer = -1;
unsigned g_countAnswer = 0;
const char* g_nameAnswer = "Social Lobby";
int g_setCalls = 0;
int FakeDestination(const void*) { return g_destinationAnswer; }
unsigned FakeCount(const void*) { return g_countAnswer; }
const char* FakeName(const void*, unsigned) { return g_nameAnswer; }
void FakeSet(void*, const void*) { ++g_setCalls; }
// The functions that talk to Meta (#396): what the game's own versions leave in the state word is modelled
// (ShareData: dirty and in-flight bits go, in-flight comes back; Clear: the clearing bit goes up, and its
// callback, which a test fires by hand, takes it down again).
int g_shareCalls = 0;
int g_refreshCalls = 0;
int g_clearCalls = 0;
std::uint32_t FlagsOf(const FakeObject& o) {
  std::uint32_t f = 0;
  std::memcpy(&f, reinterpret_cast<const char*>(&o) + kRichPresenceFlagsOffset, sizeof(f));
  return f;
}
void SetFlags(FakeObject* o, std::uint32_t f) {
  std::memcpy(reinterpret_cast<char*>(o) + kRichPresenceFlagsOffset, &f, sizeof(f));
}
void FakeShareData(void* self) {
  ++g_shareCalls;
  FakeObject* o = static_cast<FakeObject*>(self);
  SetFlags(o, (FlagsOf(*o) & ~3u) | kRichPresenceFlagInFlight);
}
void FakeRefresh(void*) { ++g_refreshCalls; }
void FakeClear(void* self) {
  ++g_clearCalls;
  FakeObject* o = static_cast<FakeObject*>(self);
  SetFlags(o, FlagsOf(*o) | kRichPresenceFlagClearing);
}
const char* g_encodeText = "{\"game_type\":\"Social_2.0\",\"joinable\":true}";
bool g_encodeFails = false;
unsigned FakeEncodeJson(const void*, char* out, unsigned long long* size, unsigned, const char*) {
  if (g_encodeFails) return 9;
  const std::size_t n = std::strlen(g_encodeText);
  std::memcpy(out, g_encodeText, n);
  *size = n;
  return 0;
}

template <typename Fn>
std::uintptr_t Word(Fn fn) {
  std::uintptr_t w = 0;
  std::memcpy(&w, &fn, sizeof(w));
  return w;
}

// A vtable of callable fake functions at the pinned slots, an object pointing at it, and the bias that makes the
// object look like pnsovr's CNSOVRRichPresence.
void BuildFakePresence(FakeObject* object) {
  for (std::size_t i = 0; i < kVtableWords; ++i) g_fakeVtable[i] = 0x1000 + i;
  g_fakeVtable[0] = 0;       // offset to top
  g_fakeVtable[1] = 0x7777;  // typeinfo
  g_fakeVtable[2 + kRichPresenceSlotDestinationCount] = Word(&FakeCount);
  g_fakeVtable[2 + kRichPresenceSlotDestinationName] = Word(&FakeName);
  g_fakeVtable[2 + kRichPresenceSlotDestination] = Word(&FakeDestination);
  g_fakeVtable[2 + kRichPresenceSlotSet] = Word(&FakeSet);
  g_fakeVtable[2 + kRichPresenceSlotShareData] = Word(&FakeShareData);
  g_fakeVtable[2 + kRichPresenceSlotRefreshDestinations] = Word(&FakeRefresh);
  g_fakeVtable[2 + kRichPresenceSlotClear] = Word(&FakeClear);
  const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(&g_fakeVtable[2]);
  g_presenceBias = address - static_cast<std::uintptr_t>(kOvrRichPresenceVptrVaddr);
  object->vptr = address;
}

std::uintptr_t SlotOf(const FakeObject& object, std::size_t slot) {
  const std::uintptr_t* vtable = nullptr;
  std::memcpy(&vtable, &object.vptr, sizeof(vtable));
  return vtable[slot];
}

void TestPresenceSelection() {
  ResetPresenceForTest();
  const PresenceCounters counters = PresenceCountersView();
  g_lines.clear();
  FakeObject object;
  BuildFakePresence(&object);
  const std::uintptr_t original = object.vptr;

  // The pinned slots must hold the pinned functions: fakes living elsewhere are left alone.
  QCHECK(SelectRichPresenceObject(&object, &PresenceLoaded) == &object);
  QCHECK(object.vptr == original && C(counters.passThrough) == 1 && C(counters.selected) == 0);

  // Everything that is not that object passes through, counted.
  QCHECK(SelectRichPresenceObject(nullptr, &PresenceLoaded) == nullptr);
  QCHECK(SelectRichPresenceObject(&object, nullptr) == &object);
  QCHECK(SelectRichPresenceObject(&object, &Absent) == &object);
  QCHECK(SelectRichPresenceObject(&object, &WrongBuild) == &object);
  FakeObject foreign = object;
  foreign.vptr += 8;
  SetPresenceSeamsForTest(false, nullptr);
  QCHECK(SelectRichPresenceObject(&foreign, &PresenceLoaded) == &foreign && foreign.vptr == original + 8);
  QCHECK(C(counters.passThrough) == 6 && C(counters.selected) == 0);
  QCHECK(g_lines.empty());  // the decision never logs

  // The pinned object (the slot check off for the fakes): the vtable is replaced by a copy that differs in
  // exactly the six wrapped slots and carries the same offset and typeinfo words.
  QCHECK(SelectRichPresenceObject(&object, &PresenceLoaded) == &object);
  QCHECK(C(counters.selected) == 1 && object.vptr != original);
  for (std::size_t slot = 0; slot < kOvrRichPresenceSlotCount; ++slot) {
    const bool wrapped = slot == kRichPresenceSlotDestinationName || slot == kRichPresenceSlotDestination ||
                         slot == kRichPresenceSlotSet || slot == kRichPresenceSlotShareData ||
                         slot == kRichPresenceSlotRefreshDestinations || slot == kRichPresenceSlotClear;
    QCHECK((SlotOf(object, slot) != g_fakeVtable[2 + slot]) == wrapped);
  }
  const std::uintptr_t* copy = nullptr;
  std::memcpy(&copy, &object.vptr, sizeof(copy));
  QCHECK(copy[-2] == 0 && copy[-1] == 0x7777);
  QCHECK(SelectRichPresenceObject(&object, &PresenceLoaded) == &object && C(counters.selected) == 1);  // once

  // A second object of the class shares the copy.
  FakeObject second;
  second.vptr = original;
  QCHECK(SelectRichPresenceObject(&second, &PresenceLoaded) == &second && second.vptr == object.vptr);
  QCHECK(C(counters.selected) == 2);
  QCHECK(g_lines.empty());
}

void TestPresenceSlotCheckAcceptsTheRealAddresses() {
  ResetPresenceForTest();
  g_lines.clear();
  FakeObject object;
  BuildFakePresence(&object);
  const std::uintptr_t original = object.vptr;
  g_fakeVtable[2 + kRichPresenceSlotDestinationCount] = g_presenceBias + kOvrRichPresenceDestinationCountVaddr;
  g_fakeVtable[2 + kRichPresenceSlotDestinationName] = g_presenceBias + kOvrRichPresenceDestinationNameVaddr;
  g_fakeVtable[2 + kRichPresenceSlotDestination] = g_presenceBias + kOvrRichPresenceDestinationVaddr;
  g_fakeVtable[2 + kRichPresenceSlotSet] = g_presenceBias + kOvrRichPresenceSetVaddr;
  g_fakeVtable[2 + kRichPresenceSlotShareData] = g_presenceBias + kOvrRichPresenceShareDataVaddr;
  g_fakeVtable[2 + kRichPresenceSlotRefreshDestinations] = g_presenceBias + kOvrRichPresenceRefreshDestinationsVaddr;
  g_fakeVtable[2 + kRichPresenceSlotClear] = g_presenceBias + kOvrRichPresenceClearVaddr;
  QCHECK(SelectRichPresenceObject(&object, &PresenceLoaded) == &object);
  QCHECK(object.vptr != original && C(PresenceCountersView().selected) == 1);
  // One slot off by a word: not the pinned build's function, nothing is replaced.
  ResetPresenceForTest();
  FakeObject other;
  other.vptr = original;
  g_fakeVtable[2 + kRichPresenceSlotDestination] += 4;
  QCHECK(SelectRichPresenceObject(&other, &PresenceLoaded) == &other && other.vptr == original);
  QCHECK(C(PresenceCountersView().passThrough) == 1);
  // Each of the three slots that talk to Meta is pinned too (#396): one off by a word and nothing is replaced.
  g_fakeVtable[2 + kRichPresenceSlotDestination] -= 4;
  for (const std::size_t slot : {kRichPresenceSlotShareData, kRichPresenceSlotRefreshDestinations, kRichPresenceSlotClear}) {
    ResetPresenceForTest();
    g_fakeVtable[2 + slot] += 4;
    FakeObject off;
    off.vptr = original;
    QCHECK(SelectRichPresenceObject(&off, &PresenceLoaded) == &off && off.vptr == original);
    QCHECK(C(PresenceCountersView().passThrough) == 1);
    g_fakeVtable[2 + slot] -= 4;
  }
}

void TestPresenceWrappersPassThroughAndLogOnChange() {
  ResetPresenceForTest();
  FakeObject object;
  BuildFakePresence(&object);
  SetPresenceSeamsForTest(false, &FakeEncodeJson);
  QCHECK(SelectRichPresenceObject(&object, &PresenceLoaded) == &object);
  g_lines.clear();
  using DestinationFn = int (*)(const void*);
  using NameFn = const char* (*)(const void*, unsigned);
  using SetFn = void (*)(void*, const void*);
  DestinationFn destination;
  NameFn name;
  SetFn set;
  std::uintptr_t w = SlotOf(object, kRichPresenceSlotDestination);
  std::memcpy(&destination, &w, sizeof(w));
  w = SlotOf(object, kRichPresenceSlotDestinationName);
  std::memcpy(&name, &w, sizeof(w));
  w = SlotOf(object, kRichPresenceSlotSet);
  std::memcpy(&set, &w, sizeof(w));

  // The failed GetDestinations: none found, an empty list. The answer is the original's, logged once.
  g_destinationAnswer = -1;
  g_countAnswer = 0;
  QCHECK(destination(&object) == -1);
  QCHECK(destination(&object) == -1);
  QCHECK(CountLines("\"event\":\"rich_presence_destination\",\"index\":-1,\"count\":0") == 1);
  // The list fills: a new line, the new answer.
  g_destinationAnswer = 0;
  g_countAnswer = 1;
  QCHECK(destination(&object) == 0);
  QCHECK(CountLines("\"event\":\"rich_presence_destination\"") == 2);
  QCHECK(CountLines("\"index\":0,\"count\":1") == 1);

  QCHECK(Is(name(&object, 0), "Social Lobby"));
  QCHECK(Is(name(&object, 0), "Social Lobby"));
  QCHECK(CountLines("\"event\":\"rich_presence_name\",\"index\":0,\"name\":\"Social Lobby\"") == 1);
  g_nameAnswer = "Arena";
  QCHECK(Is(name(&object, 0), "Arena"));
  QCHECK(CountLines("\"event\":\"rich_presence_name\"") == 2);
  g_nameAnswer = nullptr;
  QCHECK(name(&object, 1) == nullptr);  // the original's answer, even null
  QCHECK(CountLines("\"name\":\"(null)\"") == 1);

  // Set: the original always runs; the document is logged when it changes.
  g_setCalls = 0;
  int document = 0;
  set(&object, &document);
  set(&object, &document);
  QCHECK(g_setCalls == 2);
  QCHECK(CountLines("\"event\":\"rich_presence_set\"") == 1);
  QCHECK(CountLines("game_type") == 1);
  SetPresenceSeamsForTest(false, nullptr);  // the game's encoder unknown: said once, the original still runs
  ResetPresenceForTest();
  g_lines.clear();
  set(&object, &document);
  set(&object, &document);
  QCHECK(g_setCalls == 4);
  QCHECK(CountLines("\"result\":\"game_json_unavailable\"") == 1);
  g_destinationAnswer = -1;
  g_countAnswer = 0;
  g_nameAnswer = "Social Lobby";
}

// An encoder that fails on every Set is one line, and a Set that reads out again is logged as usual.
void TestPresenceEncodeFailureIsLoggedOnce() {
  ResetPresenceForTest();
  FakeObject object;
  BuildFakePresence(&object);
  SetPresenceSeamsForTest(false, &FakeEncodeJson);
  QCHECK(SelectRichPresenceObject(&object, &PresenceLoaded) == &object);
  using SetFn = void (*)(void*, const void*);
  SetFn set;
  const std::uintptr_t w = SlotOf(object, kRichPresenceSlotSet);
  std::memcpy(&set, &w, sizeof(w));
  int document = 0;
  g_lines.clear();
  g_encodeFails = true;
  for (int i = 0; i < 5; ++i) set(&object, &document);
  QCHECK(CountLines("\"result\":\"encode_failed\"") == 1);
  g_encodeFails = false;
  set(&object, &document);
  QCHECK(CountLines("\"event\":\"rich_presence_set\",\"json\"") == 1);
  g_encodeFails = true;  // failing again after a good read is a new episode
  set(&object, &document);
  set(&object, &document);
  QCHECK(CountLines("\"result\":\"encode_failed\"") == 2);
  g_encodeFails = false;
}

// #393 (a2): with the feature on and no destination found by the game, the table answers by the game type of the
// presence just set; the game's own answer wins when it found one; off, nothing changes.
void TestPresenceNameTable() {
  QCHECK(Is(PresenceDisplayName("social_2.0"), "Social Lobby"));
  QCHECK(Is(PresenceDisplayName("Social_2.0"), "Social Lobby"));  // the game's spelling
  QCHECK(Is(PresenceDisplayName("ECHO_ARENA"), "Arena"));
  QCHECK(Is(PresenceDisplayName("echo_combat"), "Combat"));
  for (const char* priv : {"echo_arena_private", "echo_combat_private", "social_2.0_private"}) {
    QCHECK(Is(PresenceDisplayName(priv), "Private Match"));
  }
  for (const char* unknown : {"echo_arena_tournament", "echo_arena_", "social_2.", "social_2.0x", "", "x"}) {
    QCHECK(PresenceDisplayName(unknown) == nullptr);  // prefixes and extensions are different names
  }
  QCHECK(PresenceDisplayName(nullptr) == nullptr);
}

void TestPresenceNamesAnswerOnlyWhenEnabledAndOnlyWhenTheGameFoundNone() {
  ResetPresenceForTest();
  FakeObject object;
  BuildFakePresence(&object);
  SetPresenceSeamsForTest(false, &FakeEncodeJson);
  QCHECK(SelectRichPresenceObject(&object, &PresenceLoaded) == &object);
  using DestinationFn = int (*)(const void*);
  using NameFn = const char* (*)(const void*, unsigned);
  using SetFn = void (*)(void*, const void*);
  DestinationFn destination;
  NameFn name;
  SetFn set;
  std::uintptr_t w = SlotOf(object, kRichPresenceSlotDestination);
  std::memcpy(&destination, &w, sizeof(w));
  w = SlotOf(object, kRichPresenceSlotDestinationName);
  std::memcpy(&name, &w, sizeof(w));
  w = SlotOf(object, kRichPresenceSlotSet);
  std::memcpy(&set, &w, sizeof(w));
  int document = 0;
  g_destinationAnswer = -1;
  g_countAnswer = 0;
  g_nameAnswer = "from Meta";

  // Off (the default): the game's -1 stands, whatever the game type.
  g_encodeText = "{\"game_type\":\"Social_2.0\"}";
  set(&object, &document);
  QCHECK(destination(&object) == -1);

  // On: the table answers for a known game type, and the name is ours; the original is not asked for it.
  SetPresenceNames(true);
  set(&object, &document);
  const int answered = destination(&object);
  QCHECK(answered == kPresenceNameBase);
  QCHECK(Is(name(&object, static_cast<unsigned>(answered)), "Social Lobby"));
  QCHECK(CountLines((std::string("\"index\":") + std::to_string(kPresenceNameBase) + ",\"count\":0,\"source\":\"table\"").c_str()) == 1);
  g_encodeText = "{\"game_type\":\"echo_arena\",\"joinable\":false}";
  set(&object, &document);
  QCHECK(destination(&object) == kPresenceNameBase + 1);
  QCHECK(Is(name(&object, kPresenceNameBase + 1), "Arena"));

  // A game type the table does not know, or no game type: the game's answer, and the name is the game's.
  g_encodeText = "{\"game_type\":\"echo_arena_tournament\"}";
  set(&object, &document);
  QCHECK(destination(&object) == -1);
  g_encodeText = "{\"lobby_id\":\"x\"}";
  set(&object, &document);
  QCHECK(destination(&object) == -1);
  QCHECK(Is(name(&object, 0), "from Meta"));

  // The game found its own destination: never replaced, and its index reaches the name slot unchanged.
  g_encodeText = "{\"game_type\":\"social_2.0\"}";
  set(&object, &document);
  g_destinationAnswer = 2;
  g_countAnswer = 3;
  QCHECK(destination(&object) == 2);
  QCHECK(Is(name(&object, 2), "from Meta"));

  // An index just outside the table's range is not ours.
  QCHECK(Is(name(&object, static_cast<unsigned>(kPresenceNameBase) + 6U), "from Meta"));
  SetPresenceNames(false);
  g_destinationAnswer = -1;
  g_countAnswer = 0;
  g_nameAnswer = "Social Lobby";
  g_encodeText = "{\"game_type\":\"Social_2.0\",\"joinable\":true}";
}

// #396: with presence_local the game's ShareData / RefreshDestinations / Clear are not run (they are the ones
// that make group_presence requests to Meta); the state word ends up as the game's own versions leave it once
// the answer has come back. Without it the game's functions run, unchanged.
void TestPresenceLocalAnswersWithoutTheGamesFunctions() {
  ResetPresenceForTest();
  g_lines.clear();
  FakeObject object;
  BuildFakePresence(&object);
  SetPresenceSeamsForTest(false, &FakeEncodeJson);
  QCHECK(SelectRichPresenceObject(&object, &PresenceLoaded) == &object);
  using VoidFn = void (*)(void*);
  const auto call = [&](std::size_t slot) {
    VoidFn fn;
    const std::uintptr_t w = SlotOf(object, slot);
    std::memcpy(&fn, &w, sizeof(w));
    fn(&object);
  };
  g_shareCalls = g_refreshCalls = g_clearCalls = 0;
  const PresenceCounters counters = PresenceCountersView();

  // Off (the default): the game's own functions run.
  SetFlags(&object, kRichPresenceFlagDirty);
  call(kRichPresenceSlotShareData);
  call(kRichPresenceSlotRefreshDestinations);
  QCHECK(g_shareCalls == 1 && g_refreshCalls == 1);
  QCHECK(FlagsOf(object) == kRichPresenceFlagInFlight);  // what the game's ShareData leaves
  QCHECK(C(counters.localShare) == 0 && C(counters.localRefresh) == 0);

  // On: nothing of the game's runs; a dirty object ends with the dirty bit consumed and nothing in flight.
  SetPresenceLocal(true);
  SetFlags(&object, kRichPresenceFlagDirty);
  call(kRichPresenceSlotShareData);
  QCHECK(g_shareCalls == 1);  // not called
  QCHECK(FlagsOf(object) == 0);
  QCHECK(C(counters.localShare) == 1);
  call(kRichPresenceSlotRefreshDestinations);
  QCHECK(g_refreshCalls == 1 && C(counters.localRefresh) == 1);

  // The same gate as the game's ShareData: an object with a clear or a share in flight is left alone.
  SetFlags(&object, kRichPresenceFlagDirty | kRichPresenceFlagClearing);
  call(kRichPresenceSlotShareData);
  QCHECK(FlagsOf(object) == (kRichPresenceFlagDirty | kRichPresenceFlagClearing));
  SetFlags(&object, kRichPresenceFlagDirty | kRichPresenceFlagInFlight);
  call(kRichPresenceSlotShareData);
  QCHECK(FlagsOf(object) == (kRichPresenceFlagDirty | kRichPresenceFlagInFlight));
  QCHECK(g_shareCalls == 1 && C(counters.localShare) == 1);

  // Clear: no request, and no bit left behind (the game's Clear plus its callback net to the bit clear); the
  // other bits are kept.
  SetFlags(&object, kRichPresenceFlagDirty);
  call(kRichPresenceSlotClear);
  QCHECK(g_clearCalls == 0 && FlagsOf(object) == kRichPresenceFlagDirty);
  // And a share goes through afterwards: the object is not gated for good.
  call(kRichPresenceSlotShareData);
  QCHECK(FlagsOf(object) == 0);
  QCHECK(C(counters.localClear) == 1);

  // One line per operation, the first time; none from the repeats.
  QCHECK(CountLines("\"event\":\"rich_presence_local\"") == 3);
  call(kRichPresenceSlotClear);
  QCHECK(CountLines("\"event\":\"rich_presence_local\"") == 3 && C(counters.localClear) == 2);

  // Off again: the game's Clear runs.
  SetPresenceLocal(false);
  call(kRichPresenceSlotClear);
  QCHECK(g_clearCalls == 1);
  SetPresenceLocal(false);
}

NEVR_HOOK_RECORD(kTestPresenceHook, PresenceThunk, &OnPresenceHandler);

void TestPresenceThroughThunk() {
  ResetPresenceForTest();
  SetPresenceSeamsForTest(false, nullptr);
  FakeObject object;
  BuildFakePresence(&object);
  const std::uintptr_t original = object.vptr;
  PnsovrLookup previous = SetPnsovrLookup(&PresenceLoaded);
  PresenceThunk::Reset();
  *PresenceThunk::OriginalOut() = reinterpret_cast<void*>(&FakeOriginal);
  PresenceThunk::Arm(kTestPresenceHook);
  using Entry = void* (*)(std::uint64_t);
  Entry entry = nullptr;
  void* addr = PresenceThunk::EntryAddress();
  std::memcpy(&entry, &addr, sizeof(entry));
  g_lines.clear();
  g_fakeResult = &object;
  g_fakeCalls = 0;
  QCHECK(entry(0x55) == static_cast<void*>(&object));  // the same object comes back
  QCHECK(g_fakeCalls == 1);                            // the original ran first
  QCHECK(object.vptr != original);                     // with the tracing vtable
  QCHECK(PresenceThunk::Calls() == 1 && C(PresenceCountersView().selected) == 1);
  QCHECK(g_lines.empty());                             // the handler path logged nothing
  g_fakeResult = nullptr;
  QCHECK(entry(0x55) == nullptr);
  QCHECK(C(PresenceCountersView().passThrough) == 1);
  PresenceThunk::Disarm();
  g_fakeResult = &object;
  QCHECK(entry(0x55) == static_cast<void*>(&object));
  PresenceThunk::Reset();
  SetPnsovrLookup(previous);

  // libr15 is not loaded here: the install says so and leaves the thunk disarmed.
  g_lines.clear();
  QCHECK(InstallPresenceTrace() == sentinel::GotStatus::kModuleNotLoaded);
  QCHECK(CountLines("\"event\":\"rich_presence_install\"") == 1 && CountLines("\"status\":\"hook_failed\"") == 1);
  *PresenceThunk::OriginalOut() = reinterpret_cast<void*>(&FakeOriginal);
  g_fakeResult = nullptr;
  g_fakeCalls = 0;
  QCHECK(entry(1) == nullptr && g_fakeCalls == 1);
  PresenceThunk::Reset();
}

void TestPresenceTarget() {
  const sentinel::GotTarget t = LibR15RichPresence();
  QCHECK(std::strcmp(t.module, "libr15.so") == 0);
  QCHECK(std::strcmp(t.symbol, "_ZN10NRadEngine11CNSProvider12RichPresenceEm") == 0);
  QCHECK(t.kind == sentinel::RelocKind::kJumpSlot);
  QCHECK(t.buildId != nullptr && std::strcmp(t.buildId, "b243509c08ce677aeb95fa348016949b3fc45230") == 0);
  QCHECK(t.slotVaddr.has_value() && *t.slotVaddr == 0x36d2710ULL);
  QCHECK(kOvrRichPresenceVptrVaddr == 0x6a13e0ULL && kOvrRichPresenceSlotCount == 17);
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

// The script event post function is found only in libr15 of the pinned build, at its pinned address.
void TestResolveGameEvents() {
  for (const sentinel::ImageLookup lookup : {&NoImage, &EmptyImage, static_cast<sentinel::ImageLookup>(nullptr)}) {
    QCHECK(ResolveGameEvents(lookup).send == nullptr);
  }
  // Probe off (the default): no post function, whatever the lookup finds.
  QCHECK(SelectGameEvents(false, &NoImage).send == nullptr);
  QCHECK(SelectGameEvents(true, &NoImage).send == nullptr);
  QCHECK(kLibR15SendComponentEventVaddr == 0x1244758ULL);
  QCHECK(kSymEvtArmComputerFriends == 0x976edb4d0c250317ULL);
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
  TestResolveGameEvents();
  TestTarget();
  TestPresenceSelection();
  TestPresenceSlotCheckAcceptsTheRealAddresses();
  TestPresenceWrappersPassThroughAndLogOnChange();
  TestPresenceThroughThunk();
  TestPresenceTarget();
  TestPresenceEncodeFailureIsLoggedOnce();
  TestPresenceNameTable();
  TestPresenceNamesAnswerOnlyWhenEnabledAndOnlyWhenTheGameFoundNone();
  TestPresenceLocalAnswersWithoutTheGamesFunctions();
  sentinel::SetLogSink(previous);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "social_install_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("social_install_test: all checks pass\n");
  return 0;
}
