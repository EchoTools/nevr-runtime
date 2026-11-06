// Host test of the booster-pool module (src/quest/combat/boosterpool.cpp, #2181): the real handler through the thunk, installed by the real GotHook
// into the JUMP_SLOT of AllocatePooledActor in a libr15 image built in memory, and called the way the game calls it: through the word. The game side
// is a model: a fake AllocatePooledActor, a fake CSymbol64::Append and a fake plain-name word. The addresses are spelled in this file, not taken from
// the header, so a wrong constant there fails here.
//
// What it pins: the word holds the thunk and the original is the function; the plain name passes through unchanged; the one suffixed name the stock
// builds under the lowspec bit is replaced by the plain name, counted and logged; any other name, from any caller, is untouched; the names are
// computed once and a call after that costs no Append; an unreadable plain name passes the call through and is counted; a word that holds anything
// else refuses the module; the module installs only with levelname; the switch; the golden lines. got_pinned_test.cpp resolves the slot and the code
// words in the real libr15.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/boosterpool.h"

#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "quest/combat/combat_level.h"
#include "quest/combat/safe_read.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/test_check.h"

namespace {

namespace boosterpool = nevr_quest::combat::boosterpool;
namespace pinned = sentinel::pinned;
using combat_test::Image;
using combat_test::Reloc;

// the pinned build's ELF vaddrs (disassembly VA = ELF + 0x100000)
constexpr std::uintptr_t kSlot = 0x36cd1f8;       // the JUMP_SLOT of the AllocatePooledActor import
constexpr std::uintptr_t kFn = 0x18037c4;         // the real function
constexpr std::uintptr_t kAppend = 0xf31ea0;      // CSymbol64::Append (a PLT stub)
constexpr std::uintptr_t kPlainName = 0x37af868;  // the booster pool actor's plain CSymbol64
constexpr const char* kSymbol = "_ZN10NRadEngine13CncaGameSpace19AllocatePooledActorENS_9CSymbol64E";

struct ActorPair {
  std::uint64_t id, gen;
};
constexpr std::uint64_t kPlain = 0x1122334455667788ULL;
std::uint64_t Suffixed(std::uint64_t plain) { return plain ^ 0xabcdef5a5a5aULL; }

int g_realCalls = 0;
std::uint64_t g_realName = 0;
void* g_realSt = nullptr;
ActorPair FakeReal(void* st, std::uint64_t name) {
  ++g_realCalls;
  g_realSt = st;
  g_realName = name;
  return ActorPair{name ^ 0x5a5a, 7};
}
// Append(name, "_lowspec", 1, 1) is the suffixed name; any other call returns a marker the checks would see.
int g_appendCalls = 0;
std::uint64_t FakeAppend(std::uint64_t name, const char* suffix, std::uint32_t a, std::uint32_t b) {
  ++g_appendCalls;
  if (std::strcmp(suffix, "_lowspec") != 0 || a != 1 || b != 1) return 0xdead;
  return Suffixed(name);
}

using Fn = ActorPair (*)(void*, std::uint64_t);

Image g_image;
bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}
bool NoLibr15(const char*, sentinel::ElfImage*) { return false; }

struct Line {
  sentinel::LogLevel level;
  std::string text;
};
std::vector<Line> g_lines;
void Capture(sentinel::LogLevel level, const char* line) { g_lines.push_back({level, line}); }
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
std::vector<Line> Lines(const char* event) {
  std::vector<Line> out;
  const std::string key = std::string("\"event\":\"") + event + "\"";
  for (const Line& l : g_lines) {
    if (Has(l.text, key)) out.push_back(l);
  }
  return out;
}
std::string Golden(const std::string& line) {
  const std::size_t at = line.find("\"event\"");
  return at == std::string::npos ? line : line.substr(at);
}

std::uintptr_t Word() { return g_image.Get<std::uintptr_t>(kSlot); }
ActorPair CallSlot(void* st, std::uint64_t name) { return reinterpret_cast<Fn>(Word())(st, name); }

void* const kSt = reinterpret_cast<void*>(0x5000);  // the game space's ST: passed through, never read

void BuildImage() {
  g_image.Build({{kSymbol, kSlot, false, false, kFn}});
  g_image.Put<std::uintptr_t>(kSlot, g_image.Base() + kFn);
  g_image.Put<std::uintptr_t>(kSlot - 8, 0x1111);  // neighbours, never touched
  g_image.Put<std::uintptr_t>(kSlot + 8, 0x2222);
  g_image.Put<std::uint64_t>(kPlainName, kPlain);
}
void Begin() {
  g_lines.clear();
  boosterpool::ResetForTest();
  boosterpool::SetImageLookupForTest(&Lookup);
  g_realCalls = g_appendCalls = 0;
  g_realName = 0;
  g_realSt = nullptr;
  sentinel::SetLogSink(&Capture);
  BuildImage();
}
void End() {
  boosterpool::ResetForTest();
  boosterpool::SetImageLookupForTest(nullptr);
  g_image.Drop();
  sentinel::SetLogSink(nullptr);
}

// An install, and the fakes standing in for the three things the module calls and reads: the original the manager published (zeros in the image),
// Append (a PLT stub) and the plain name's address.
nevr_quest::combat::ModuleResult InstallWithFakes() {
  const nevr_quest::combat::ModuleResult r = boosterpool::InstallAt(&Lookup, true, true);
  if (!r.installed) return r;
  *boosterpool::targets::Thunk::OriginalOut() = reinterpret_cast<void*>(&FakeReal);
  boosterpool::SetTargetsForTest(&FakeAppend, g_image.Base() + kPlainName);
  return r;
}

void TheConstantsAreTheStoreLibr15s() {
  namespace t = boosterpool::targets;
  QCHECK(t::kSlotVaddr == kSlot && t::kFnVaddr == kFn && t::kAppendStubVaddr == kAppend && t::kPlainNameVaddr == kPlainName);
  QCHECK(std::strcmp(t::kSymbol, kSymbol) == 0 && std::strcmp(t::kSuffix, "_lowspec") == 0);
  QCHECK(sizeof(t::ActorPair) == sizeof(ActorPair));
}

// The word holds the thunk, the original is base + the pinned function, and the module reads Append and the plain name where the pins say.
void AnInstallPutsTheThunkInTheWordAndKeepsTheOriginal() {
  Begin();
  const nevr_quest::combat::ModuleResult r = boosterpool::InstallAt(&Lookup, true, true);
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0);
  QCHECK(Word() == reinterpret_cast<std::uintptr_t>(boosterpool::targets::Thunk::EntryAddress()));
  QCHECK(g_image.Get<std::uintptr_t>(kSlot - 8) == 0x1111 && g_image.Get<std::uintptr_t>(kSlot + 8) == 0x2222);
  QCHECK(reinterpret_cast<std::uintptr_t>(boosterpool::targets::Thunk::Original()) == g_image.Base() + kFn);
  boosterpool::AppendFn append = nullptr;
  std::uintptr_t plain = 0;
  boosterpool::TargetsForTest(&append, &plain);
  QCHECK(reinterpret_cast<std::uintptr_t>(append) == g_image.Base() + kAppend && plain == g_image.Base() + kPlainName);
  const std::vector<Line> install = Lines("combat_boosterpool_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo);
  QCHECK(Golden(install[0].text) ==
         "\"event\":\"combat_boosterpool_install\",\"result\":\"installed\",\"reason\":\"ok\",\"key\":\"combat.levelname\",\"slot\":\"ok\","
         "\"consequence\":\"an allocation for the suffixed booster actor asks for the plain one: the booster works on the full-size global_mp\"}");
  End();
}

// The plain name (the lowspec bit clear, or a caller that asks for the plain name): passed through unchanged.
void ThePlainNameIsPassedThrough() {
  Begin();
  QCHECK(InstallWithFakes().installed);
  const std::uint64_t kept0 = boosterpool::KeptCount();
  const ActorPair a = CallSlot(kSt, kPlain);
  QCHECK(g_realCalls == 1 && g_realName == kPlain);
  QCHECK(g_realSt == kSt);  // the ST argument was passed through
  QCHECK(a.id == (kPlain ^ 0x5a5a) && a.gen == 7);  // the actor pair is returned as the function gave it
  QCHECK(boosterpool::KeptCount() == kept0);
  End();
}

// The suffixed name the stock builds under lowspec: the function gets the plain name instead, counted and logged.
void TheSuffixedNameIsAskedAsThePlainOne() {
  Begin();
  QCHECK(InstallWithFakes().installed);
  const std::uint64_t kept0 = boosterpool::KeptCount();
  const ActorPair a = CallSlot(kSt, Suffixed(kPlain));
  QCHECK(g_realCalls == 1 && g_realName == kPlain);
  QCHECK(g_realSt == kSt && a.id == (kPlain ^ 0x5a5a) && a.gen == 7);
  QCHECK(boosterpool::KeptCount() == kept0 + 1);
  const std::vector<Line> kept = Lines("combat_boosterpool_kept");
  QCHECK(kept.size() == 1U && kept[0].level == sentinel::LogLevel::kInfo);
  QCHECK(Has(kept[0].text, "\"event\":\"combat_boosterpool_kept\"") && Has(kept[0].text, "\"n\":1,\"answer\":\"plain name\"}"));
  char name[19];
  std::snprintf(name, sizeof(name), "0x%016llx", static_cast<unsigned long long>(Suffixed(kPlain)));
  QCHECK(Has(kept[0].text, std::string("\"name\":\"") + name + "\""));
  for (int i = 0; i < 40; ++i) CallSlot(kSt, Suffixed(kPlain));
  QCHECK(Lines("combat_boosterpool_kept").size() == 16U && boosterpool::KeptCount() == 41);  // the lines are the first 16; the count goes on
  End();
}

// Some other name, even one near the suffixed one: untouched.
void AnyOtherNameIsUntouched() {
  Begin();
  QCHECK(InstallWithFakes().installed);
  const std::uint64_t kept0 = boosterpool::KeptCount();
  CallSlot(kSt, kPlain ^ 1);
  QCHECK(g_realCalls == 1 && g_realName == (kPlain ^ 1));
  CallSlot(kSt, Suffixed(kPlain) ^ 1);
  QCHECK(g_realCalls == 2 && g_realName == (Suffixed(kPlain) ^ 1));
  CallSlot(kSt, 0);
  QCHECK(g_realCalls == 3 && g_realName == 0);
  QCHECK(boosterpool::KeptCount() == kept0);
  End();
}

// An arena match pays nothing, by count (docs/adr/0046; FOLD-MOVE "Prove arena pays nothing, by count"). The module is not scoped by level (the
// substitution is right in arena, docs/quest/combat/levelname.md), so its first call reads the plain name once and builds the suffixed one; every later
// call, of any name, makes no read and no scan, and no scan is ever made.
void AnArenaCallMakesNoReadAfterTheFirstAndNoScan() {
  Begin();
  QCHECK(InstallWithFakes().installed);
  nevr_quest::combat::ResetSafeReadCallCount();
  nevr_quest::combat::level::ResetScanCallCountForTest();
  CallSlot(kSt, kPlain ^ 1);
  QCHECK(nevr_quest::combat::SafeReadCallCount() == 1);  // the plain name, once
  nevr_quest::combat::ResetSafeReadCallCount();
  for (int i = 0; i < 1000; ++i) CallSlot(kSt, 0x1000 + static_cast<std::uint64_t>(i));
  QCHECK(nevr_quest::combat::SafeReadCallCount() == 0);
  QCHECK(nevr_quest::combat::level::ScanCallCountForTest() == 0);
  End();
}

// The names are computed once: the first call asks Append, a later one does not, and the answer is the same.
void TheNamesAreComputedOnce() {
  Begin();
  QCHECK(InstallWithFakes().installed);
  CallSlot(kSt, kPlain);
  QCHECK(g_appendCalls == 1);
  for (int i = 0; i < 100; ++i) CallSlot(kSt, 0x1000 + static_cast<std::uint64_t>(i));
  CallSlot(kSt, Suffixed(kPlain));
  QCHECK(g_appendCalls == 1);
  QCHECK(g_realName == kPlain && boosterpool::KeptCount() == 1);
  // a plain name of 0 is the game's static not yet set: used for the call, not remembered
  g_image.Put<std::uint64_t>(kPlainName, 0);
  boosterpool::SetTargetsForTest(&FakeAppend, g_image.Base() + kPlainName);
  g_appendCalls = 0;
  CallSlot(kSt, 5);
  CallSlot(kSt, 5);
  QCHECK(g_appendCalls == 2);
  g_image.Put<std::uint64_t>(kPlainName, kPlain);
  CallSlot(kSt, 5);
  CallSlot(kSt, Suffixed(kPlain));
  QCHECK(g_appendCalls == 3 && g_realName == kPlain);
  End();
}

// The plain name unreadable: the call passes through, counted as unreadable, never as a substitution.
void AnUnreadablePlainNamePassesTheCallThrough() {
  Begin();
  QCHECK(InstallWithFakes().installed);
  boosterpool::SetTargetsForTest(&FakeAppend, 0x1000);
  const std::uint64_t unread0 = boosterpool::UnreadableCount();
  const std::uint64_t kept0 = boosterpool::KeptCount();
  CallSlot(kSt, Suffixed(kPlain));
  QCHECK(g_realCalls == 1 && g_realName == Suffixed(kPlain));  // the call was not changed
  QCHECK(boosterpool::UnreadableCount() == unread0 + 1 && boosterpool::KeptCount() == kept0);
  End();
}

// With no Append function published the call passes through (nothing to build the suffixed name with) and nothing is counted.
void ANullAppendPassesTheCallThrough() {
  Begin();
  QCHECK(InstallWithFakes().installed);
  boosterpool::SetTargetsForTest(nullptr, g_image.Base() + kPlainName);
  const std::uint64_t unread0 = boosterpool::UnreadableCount();
  CallSlot(kSt, Suffixed(kPlain));
  QCHECK(g_realCalls == 1 && g_realName == Suffixed(kPlain) && boosterpool::KeptCount() == 0 && boosterpool::UnreadableCount() == unread0);
  End();
}

// The word holds something else: refused with the status, nothing written, the thunk disarmed.
void AWordThatIsNotTheFunctionIsRefused() {
  Begin();
  g_image.Put<std::uintptr_t>(kSlot, g_image.Base() + kFn + 0x10);
  const nevr_quest::combat::ModuleResult r = boosterpool::InstallAt(&Lookup, true, true);
  QCHECK(!r.installed && std::strcmp(r.status, "refused") == 0 && !boosterpool::targets::Thunk::Original());
  QCHECK(Word() == g_image.Base() + kFn + 0x10);  // a refused install wrote nothing
  {  // a thunk entry called by hand with the names in place: the handler is disarmed, so the call passes through
    *boosterpool::targets::Thunk::OriginalOut() = reinterpret_cast<void*>(&FakeReal);
    boosterpool::SetTargetsForTest(&FakeAppend, g_image.Base() + kPlainName);
    reinterpret_cast<Fn>(boosterpool::targets::Thunk::EntryAddress())(kSt, Suffixed(kPlain));
    QCHECK(g_realCalls == 1 && g_realName == Suffixed(kPlain) && boosterpool::KeptCount() == 0);
  }
  const std::vector<Line> install = Lines("combat_boosterpool_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError);
  QCHECK(Has(install[0].text, "\"result\":\"refused\"") && Has(install[0].text, "\"reason\":\"original_mismatch\"") &&
         Has(install[0].text, "\"slot\":\"original_mismatch\""));
  // the retry works once the word is right
  g_image.Put<std::uintptr_t>(kSlot, g_image.Base() + kFn);
  QCHECK(boosterpool::InstallAt(&Lookup, true, true).installed);
  End();
}

// A second install, a missing libr15 and the switch.
void TheOtherRefusalsSayWhy() {
  Begin();
  nevr_quest::combat::ModuleResult r = boosterpool::InstallAt(&NoLibr15, true, true);
  QCHECK(!r.installed && std::strcmp(r.status, "module_not_loaded") == 0);
  QCHECK(Lines("combat_boosterpool_install").size() == 1U && Lines("combat_boosterpool_install")[0].level == sentinel::LogLevel::kError);
  r = boosterpool::InstallAt(&Lookup, true, true);
  QCHECK(r.installed);
  const std::uintptr_t thunk = Word();
  g_lines.clear();
  r = boosterpool::InstallAt(&Lookup, true, true);
  QCHECK(!r.installed && std::strcmp(r.status, "already_ran") == 0 && Word() == thunk);
  QCHECK(Lines("combat_boosterpool_refused").size() == 1U && Lines("combat_boosterpool_refused")[0].level == sentinel::LogLevel::kWarn);
  End();

  Begin();  // combat.levelname false
  r = boosterpool::InstallAt(&Lookup, false, true);
  QCHECK(!r.installed && std::strcmp(r.status, "off") == 0 && Word() == g_image.Base() + kFn);
  QCHECK(Has(Lines("combat_boosterpool_install")[0].text, "\"reason\":\"off_config\"") &&
         Lines("combat_boosterpool_install")[0].level == sentinel::LogLevel::kInfo);
  End();
}

// The substitution is right only while levelname's level is the loaded one: without levelname installed the module installs nothing.
void ItInstallsOnlyWithLevelname() {
  Begin();
  const nevr_quest::combat::ModuleResult r = boosterpool::InstallAt(&Lookup, true, /*levelnameInstalled=*/false);
  QCHECK(!r.installed && std::strcmp(r.status, "needs_levelname") == 0);
  QCHECK(Word() == g_image.Base() + kFn);
  const std::vector<Line> install = Lines("combat_boosterpool_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"reason\":\"needs_levelname\""));
  // with the key off the reason is the key, not the missing companion
  g_lines.clear();
  boosterpool::InstallAt(&Lookup, false, false);
  QCHECK(Has(Lines("combat_boosterpool_install")[0].text, "\"reason\":\"off_config\""));
  End();
}

}  // namespace

int main() {
  TheConstantsAreTheStoreLibr15s();
  AnInstallPutsTheThunkInTheWordAndKeepsTheOriginal();
  ThePlainNameIsPassedThrough();
  TheSuffixedNameIsAskedAsThePlainOne();
  AnyOtherNameIsUntouched();
  AnArenaCallMakesNoReadAfterTheFirstAndNoScan();
  TheNamesAreComputedOnce();
  AnUnreadablePlainNamePassesTheCallThrough();
  ANullAppendPassesTheCallThrough();
  AWordThatIsNotTheFunctionIsRefused();
  TheOtherRefusalsSayWhy();
  ItInstallsOnlyWithLevelname();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "boosterpool_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("boosterpool_test: ok\n");
  return 0;
}
