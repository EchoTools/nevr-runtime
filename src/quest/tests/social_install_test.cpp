// The social hook's decision and its installation, on the host.
//
//   - SelectSocialObject hands the game the facade only for pnsovr's CNSOVRSocial of the pinned
//     build, and passes everything else through with one log line.
//   - The armed callback, run through the real CallbackThunk entry the GOT slot points at, applies
//     that decision to the original's result.
//   - InstallSocialHook(false) touches nothing; with libr15.so not loaded it fails without arming.
//
// Run: social_install_test

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hook_log.h"
#include "quest/social/social_abi.h"
#include "quest/social/social_facade.h"
#include "quest/social/social_install.h"
#include "quest/tests/test_check.h"

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

int g_fakeCalls = 0;
void* g_fakeResult = nullptr;
void* FakeOriginal(std::uint64_t) {
  ++g_fakeCalls;
  return g_fakeResult;
}

void TestSelect() {
  FakeObject facade;  // any distinct address stands in for the facade object
  FakeObject ovr = OvrSocial();
  QCHECK(SelectSocialObject(&ovr, &facade, &Loaded) == &facade);

  QCHECK(SelectSocialObject(nullptr, &facade, &Loaded) == nullptr);  // a provider with no social object stays that way
  QCHECK(SelectSocialObject(&ovr, &facade, &Absent) == &ovr);
  QCHECK(SelectSocialObject(&ovr, &facade, &WrongBuild) == &ovr);

  FakeObject foreign = OvrSocial();
  foreign.vptr += 8;  // another class's vtable (or a shifted one)
  QCHECK(SelectSocialObject(&foreign, &facade, &Loaded) == &foreign);
  FakeObject other;
  other.vptr = kBias;  // the library base itself is not the vtable
  QCHECK(SelectSocialObject(&other, &facade, &Loaded) == &other);

  QCHECK(SelectSocialObject(&ovr, nullptr, &Loaded) == &ovr);  // no facade: never substitute nothing
  QCHECK(SelectSocialObject(&ovr, &facade, nullptr) == &ovr);
}

void TestHandlerThroughThunk() {
  FakeObject ovr = OvrSocial();
  PnsovrLookup previous = SetPnsovrLookup(&Loaded);
  SocialThunk::Reset();
  *SocialThunk::OriginalOut() = reinterpret_cast<void*>(&FakeOriginal);
  SocialThunk::Arm(SocialHandler());
  using Entry = void* (*)(std::uint64_t);
  Entry entry = nullptr;
  void* addr = SocialThunk::EntryAddress();
  std::memcpy(&entry, &addr, sizeof(entry));

  // Pinned pnsovr loaded and the provider returned its CNSOVRSocial: the game receives the facade.
  g_fakeResult = &ovr;
  g_fakeCalls = 0;
  void* got = entry(0x1234);
  QCHECK(g_fakeCalls == 1);  // the original always runs first
  QCHECK(got == Facade::Instance().Object());
  QCHECK(got != static_cast<void*>(&ovr));

  // The provider returned nothing: nothing is substituted.
  g_fakeResult = nullptr;
  QCHECK(entry(0x1234) == nullptr);

  // pnsovr is not the pinned build: the original object passes through.
  SetPnsovrLookup(&WrongBuild);
  g_fakeResult = &ovr;
  QCHECK(entry(0x1234) == static_cast<void*>(&ovr));

  // Disarmed: straight through to the original.
  SetPnsovrLookup(&Loaded);
  SocialThunk::Arm(nullptr);
  QCHECK(entry(0x1234) == static_cast<void*>(&ovr));

  SocialThunk::Reset();
  SetPnsovrLookup(previous);
}

void TestInstall() {
  g_lines.clear();
  const InstallResult off = InstallSocialHook(false);
  QCHECK(off.status == InstallStatus::kDisabled);
  QCHECK(CountLines("\"status\":\"disabled\"") == 1);

  // libr15.so is not loaded in this process: GotHook refuses and the callback is left disarmed.
  g_lines.clear();
  const InstallResult on = InstallSocialHook(true);
  QCHECK(on.status == InstallStatus::kHookFailed);
  QCHECK(on.got == sentinel::GotStatus::kModuleNotLoaded);
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
  TestInstall();
  TestTarget();
  sentinel::SetLogSink(previous);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "social_install_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("social_install_test: all checks pass\n");
  return 0;
}
