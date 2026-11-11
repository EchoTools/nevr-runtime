// Host test of voipdrop (src/quest/combat/voipdrop.cpp, #2178): the real handler through the thunk, installed by the real GotHook into the JUMP_SLOT of a
// libr15 image built in memory, and called the way the game calls it: through the slot. The game side is a model: a fake CR15NetVoipReceiverCS (free head,
// page size, allocator block) and a fake allocator with a vtable (+0x10 alloc, +0x38 free) behind fake CMemoryContext functions.
//
// What it pins (the q15t plugin's voipdrop_test assertions, on the new fixture): a table with a free page reaches the game's function with no probe at all;
// head -1 and an allocator that can give the page probes with the table's size and block, gives the block back, restores the context and calls the game's
// function with the same arguments; an allocator out of memory (or none) drops the packet; every doubt runs the game's function; the drop lines are capped
// at 16 with a running total at every 50th drop, at most 16; a slot that does not hold ReceiveVoip is refused and left alone with the pin named; a missing
// engine export refuses the install; every code word flipped in turn refuses it. New with the move: the guard acts in every level (no scope is asked), the
// switch, the legacy switch file (a Warning, never a switch), the missing image and build ID, the stats lines and the second install.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/legacy_switch.h"
#include "quest/combat/voipdrop.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/combat_log_capture.h"
#include "quest/tests/combat_pin_check.h"
#include "quest/tests/combat_pins.h"
#include "quest/tests/test_check.h"

namespace {

namespace vd = nevr_quest::combat::voipdrop;
namespace pinned = sentinel::pinned;
using combat_test::Has;
using combat_test::Image;
using combat_test::Line;
using combat_test::Lines;
using nevr_quest::combat::ModuleResult;
using nevr_quest::combat::pinned::Word;
// The tables themselves, not the module's own `kWordPins` (which a bug could shorten): the test puts every pinned word in the image and flips every one.
const nevr_quest::combat::pinned::Pins kAllWords = {vd::kWords, vd::kWordCount, nullptr, 0};

using RecvFn = void (*)(void*, std::uint64_t, const void*, const void*, std::uint32_t);

// ---- the fake receiver and allocator ----------------------------------------------------------------------------------------------

alignas(16) std::uint8_t g_cs[0x200];
void* const kSentinel = reinterpret_cast<void*>(0x7777);  // the memory context's current allocator before the probe
void* g_cur = kSentinel;
void* g_allocObj[2];  // [0] = vtable
std::uintptr_t g_vt[10];
char g_page[16];
bool g_allocOk = true, g_noAllocator = false;
int g_probeAllocs = 0, g_probeFrees = 0, g_setCalls = 0;
std::uint32_t g_probeBytes = 0;
const void* g_blockSeen = nullptr;
void* g_freed = nullptr;

void* FBlock(const void* block) {
  g_blockSeen = block;
  return g_noAllocator ? nullptr : static_cast<void*>(g_allocObj);
}
void* FCurPtr() { return g_cur; }
void FSetCur(void* a) {
  g_cur = a;
  ++g_setCalls;
}
void* FCur() { return g_cur; }
void* FAlloc(void*, std::uint32_t bytes) {
  ++g_probeAllocs;
  g_probeBytes = bytes;
  return g_allocOk ? static_cast<void*>(g_page) : nullptr;
}
void FFree(void*, void* p) {
  ++g_probeFrees;
  g_freed = p;
}

int g_realCalls = 0;
void* g_rSelf = nullptr;
std::uint64_t g_rHandle = 0;
const void* g_rEvent = nullptr;
const void* g_rData = nullptr;
std::uint32_t g_rBytes = 0;
extern "C" __attribute__((noinline)) void RealRecv(void* self, std::uint64_t handle, const void* event, const void* data, std::uint32_t bytes) {
  ++g_realCalls;
  g_rSelf = self;
  g_rHandle = handle;
  g_rEvent = event;
  g_rData = data;
  g_rBytes = bytes;
}
extern "C" __attribute__((noinline)) void OtherRecv(void*, std::uint64_t, const void*, const void*, std::uint32_t) {}

void World(std::int32_t freeHead, std::uint32_t pageBytes) {
  std::memset(g_cs, 0, sizeof(g_cs));
  std::memcpy(g_cs + 0x1b8, &freeHead, sizeof(freeHead));
  std::memcpy(g_cs + 0x1cc, &pageBytes, sizeof(pageBytes));
  g_allocObj[0] = g_vt;
  std::memset(g_vt, 0, sizeof(g_vt));
  g_vt[0x10 / 8] = reinterpret_cast<std::uintptr_t>(&FAlloc);
  g_vt[0x38 / 8] = reinterpret_cast<std::uintptr_t>(&FFree);
  g_cur = kSentinel;
  g_allocOk = true;
  g_noAllocator = false;
  g_probeAllocs = g_probeFrees = g_setCalls = g_realCalls = 0;
  g_probeBytes = 0;
  g_blockSeen = nullptr;
  g_freed = nullptr;
}

vd::Fns Fns() {
  vd::Fns f{};
  f.blockAllocator = &FBlock;
  f.currentPtr = &FCurPtr;
  f.setCurrent = &FSetCur;
  f.current = &FCur;
  return f;
}

// ---- the libr15 image ----------------------------------------------------------------------------------------------------------

Image g_image;
vd::Pins g_pins = vd::kPins;
bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}

// A libr15 holding the pinned words and the slot, which holds `slotWord` (the stand-in when 0).
void Build(std::uintptr_t slotWord = 0) {
  g_pins = vd::kPins;
  g_image.Build({{vd::kReceiveVoipSymbol, g_pins.slot, false, false, g_pins.fn}});
  // base + fn is the stand-in's address: the real function is not in the image, so the pin holds the stand-in's offset.
  g_pins.fn = reinterpret_cast<std::uintptr_t>(&RealRecv) - g_image.Base();
  combat_test::PutPins(g_image, &kAllWords, 1);
  *g_image.Slot(g_pins.slot) = slotWord != 0 ? reinterpret_cast<void*>(slotWord) : reinterpret_cast<void*>(&RealRecv);
}
RecvFn Slot() { return reinterpret_cast<RecvFn>(*g_image.Slot(g_pins.slot)); }
bool Hooked() { return *g_image.Slot(g_pins.slot) == vd::ReceiveVoipThunk::EntryAddress(); }
bool SlotIsTheGames() { return *g_image.Slot(g_pins.slot) == reinterpret_cast<void*>(&RealRecv); }

void Begin() {
  combat_test::StartCapture();
  vd::ResetForTest();
  Build();
}
void End() {
  vd::ResetForTest();
  g_image.Drop();
  combat_test::StopCapture();
}

ModuleResult Install(bool on = true, const char* legacyDir = nullptr, const vd::Fns& fns = Fns()) {
  nevr_quest::combat::legacy::SetDirectoryForTest(legacyDir);  // where the plugin's voipdrop.off would sit (nullptr: production)
  const ModuleResult r = vd::InstallAt(&Lookup, g_pins, fns, on);
  nevr_quest::combat::legacy::SetDirectoryForTest(nullptr);
  return r;
}

void* const kEvent = reinterpret_cast<void*>(0xe0e0);
const void* const kData = reinterpret_cast<void*>(0xdada);

void Installed() {
  Begin();
  QCHECK(Install().installed);
}

// ---- the tests ------------------------------------------------------------------------------------------------------------------

void ThePinsAreTheReadelfAndDisassemblyPins() {
  QCHECK(vd::kReceiveVoipSlotVaddr == 0x36c5d80 && vd::kReceiveVoipVaddr == 0x21c3238);
  QCHECK(std::strcmp(vd::kReceiveVoipSymbol,
                     "_ZN10NRadEngine8NRadGame21CR15NetVoipReceiverCS11ReceiveVoipENS_18SCompactPoolHandleERKNS0_20SR15NetUserVoipEventEPKvj") == 0);
  QCHECK(vd::kFreeHead == 0x1b8 && vd::kPageBytes == 0x1cc && vd::kPageBlock == 0x180 && vd::kVtAlloc == 0x10 && vd::kVtFree == 0x38);
  QCHECK(std::strcmp(vd::kBlockAllocatorSymbol, "_ZNK10NRadEngine9CMemBlock9AllocatorEv") == 0);
  QCHECK(std::strcmp(vd::kCurrentPtrSymbol, "_ZN10NRadEngine14CMemoryContext10CurrentPtrEv") == 0);
  QCHECK(std::strcmp(vd::kSetCurrentSymbol, "_ZN10NRadEngine14CMemoryContext10SetCurrentEPNS_10CAllocatorE") == 0);
  QCHECK(std::strcmp(vd::kCurrentSymbol, "_ZN10NRadEngine14CMemoryContext7CurrentEv") == 0);
  static const Word kWant[10] = {{"", 0x21c3238, 0xd10203ff}, {"", 0x21c3698, 0x9105a2a0}, {"", 0x21c36b4, 0x97b5dab7}, {"", 0xedcd30, 0xb0013f50},
                                       {"", 0xedcd34, 0xf946c211},  {"", 0xedcd38, 0x91360210}, {"", 0x23164d8, 0x91006016}, {"", 0x2316504, 0xb9406661},
                                       {"", 0x2316508, 0xf9400908}, {"", 0x23165a8, 0xb9005269}};
  QCHECK(vd::kWordCount == 10);
  for (std::size_t i = 0; i < 10; ++i) QCHECK(vd::kWords[i].vaddr == kWant[i].vaddr && vd::kWords[i].word == kWant[i].word && vd::kWords[i].what[0] != '\0');
  QCHECK(combat_test::Fingerprint(vd::kWords, vd::kWordCount) == 0xa2a0b8aa8d6d97a8ULL);
}

void TheThunkTakesTheSlotAndTheLineSaysSo() {
  Begin();
  const ModuleResult r = Install();
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0 && !r.off);
  QCHECK(Hooked());
  const std::vector<Line> install = Lines("combat_voipdrop_install");
  QCHECK(install.size() == 1U);
  if (install.size() == 1U) {
    QCHECK(install[0].level == sentinel::LogLevel::kInfo);
    QCHECK(combat_test::Golden(install[0].text) ==
           "\"event\":\"combat_voipdrop_install\",\"result\":\"installed\",\"reason\":\"ok\",\"hook\":\"ok\",\"consequence\":\"a voice packet whose "
           "receiver table needs a page that cannot be allocated is dropped, not written through a null page\"}");
  }
  End();
}

// 1. The table has a free page (head != -1): the game's function, no probe at all; head 0 is a valid page.
void ATableWithAFreePageReachesTheGamesFunctionWithNoProbe() {
  Installed();
  World(5, 0x80000);
  Slot()(g_cs, 0x1122334455ULL, kEvent, kData, 321);
  QCHECK(g_realCalls == 1 && g_probeAllocs == 0 && g_setCalls == 0);
  QCHECK(g_rSelf == g_cs && g_rHandle == 0x1122334455ULL && g_rEvent == kEvent && g_rData == kData && g_rBytes == 321);
  World(0, 0x80000);
  Slot()(g_cs, 1, kEvent, kData, 1);
  QCHECK(g_realCalls == 1 && g_probeAllocs == 0);  // head 0 is a valid page, not "needs a page"
  QCHECK(vd::CurrentStats().packets == 2 && vd::CurrentStats().probed == 0 && vd::CurrentStats().dropped == 0);
  End();
}

// 2. Head -1 and the allocator can give the page: probed with the table's size and block, given back, context restored, the game's function called.
void AProbeThatSucceedsGivesTheBlockBackAndRestoresTheContext() {
  Installed();
  World(-1, 0x80000);
  Slot()(g_cs, 7, kEvent, kData, 99);
  QCHECK(g_probeAllocs == 1 && g_probeBytes == 0x80000);
  QCHECK(g_blockSeen == g_cs + 0x180);  // the probe is on the table's allocator block
  QCHECK(g_probeFrees == 1 && g_freed == g_page);
  QCHECK(g_cur == kSentinel);
  QCHECK(g_realCalls == 1 && g_rHandle == 7 && g_rEvent == kEvent && g_rData == kData && g_rBytes == 99);
  QCHECK(vd::CurrentStats().dropped == 0 && vd::CurrentStats().probed == 1);
  End();
}

// 3. Head -1 and the allocator is out of memory: the packet is dropped, the game's function is not called, the line says so.
void AProbeThatFailsDropsThePacket() {
  Installed();
  World(-1, 0x80000);
  g_allocOk = false;
  Slot()(g_cs, 7, kEvent, kData, 99);
  QCHECK(g_realCalls == 0);
  QCHECK(g_probeFrees == 0);  // a block that was never given is not freed
  QCHECK(g_cur == kSentinel);
  QCHECK(vd::CurrentStats().dropped == 1 && vd::CurrentStats().probed == 1);
  const std::vector<Line> dropped = Lines("combat_voipdrop_dropped");
  QCHECK(dropped.size() == 1U);
  if (dropped.size() == 1U) {
    QCHECK(dropped[0].level == sentinel::LogLevel::kInfo);
    QCHECK(combat_test::Golden(dropped[0].text) ==
           "\"event\":\"combat_voipdrop_dropped\",\"n\":1,\"page_bytes\":524288,\"consequence\":\"no memory for a voice table page: the packet "
           "was dropped, so that speaker is silent until memory returns\"}");
  }
  End();
}

// 4. No allocator at all behind the block (Current() is null): dropped too, context restored.
void NoAllocatorDropsThePacket() {
  Installed();
  World(-1, 0x80000);
  g_noAllocator = true;
  Slot()(g_cs, 7, kEvent, kData, 99);
  QCHECK(g_realCalls == 0 && vd::CurrentStats().dropped == 1 && g_cur == kSentinel);
  End();
}

// 5. Doubt: self null, vtable unreadable or without the two entries: the game's function runs. (An unreadable non-null self is not a doubt the guard
// handles: the game dereferences the receiver on every path that reaches the table, so it faults with or without the guard.)
void EveryDoubtRunsTheGamesFunction() {
  Installed();
  World(-1, 0x80000);
  Slot()(nullptr, 1, kEvent, kData, 1);
  QCHECK(g_realCalls == 1 && g_probeAllocs == 0);
  World(-1, 0x80000);
  g_allocObj[0] = reinterpret_cast<void*>(0x30);  // vtable pointer unreadable
  g_allocOk = false;
  Slot()(g_cs, 1, kEvent, kData, 1);
  QCHECK(g_realCalls == 1 && g_cur == kSentinel);
  World(-1, 0x80000);
  g_vt[0x10 / 8] = 0;
  Slot()(g_cs, 1, kEvent, kData, 1);
  QCHECK(g_realCalls == 1 && g_cur == kSentinel);
  World(-1, 0x80000);
  g_vt[0x38 / 8] = 0;
  Slot()(g_cs, 1, kEvent, kData, 1);
  QCHECK(g_realCalls == 1 && g_probeAllocs == 0);  // no free entry: not probed (the block could not be given back)
  // The free head reads -1 but the page size is unreadable (the receiver ends on a page boundary).
  {
    auto* two = static_cast<std::uint8_t*>(mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    mprotect(two + 4096, 4096, PROT_NONE);
    std::uint8_t* edge = two + 4096 - 0x1bc;  // edge+0x1b8 is the last readable word; edge+0x1cc is in the PROT_NONE page
    World(-1, 0x80000);
    const std::int32_t minus1 = -1;
    std::memcpy(edge + 0x1b8, &minus1, sizeof(minus1));
    g_allocOk = false;
    Slot()(edge, 1, kEvent, kData, 1);
    QCHECK(g_realCalls == 1 && g_probeAllocs == 0 && vd::CurrentStats().dropped == 0);
    munmap(two, 8192);
  }
  End();
}

// 6. Log caps: 16 lines, then a total at every 50th drop, at most 16 totals.
void TheDropLinesAreCappedAndCounted() {
  Installed();
  World(-1, 0x80000);
  g_allocOk = false;
  for (int i = 0; i < 16; ++i) Slot()(g_cs, 1, kEvent, kData, 1);
  QCHECK(Lines("combat_voipdrop_dropped").size() == 16U && Lines("combat_voipdrop_log_cap").empty());  // inside the cap nothing is said
  for (int i = 0; i < 33; ++i) Slot()(g_cs, 1, kEvent, kData, 1);
  QCHECK(Lines("combat_voipdrop_dropped").size() == 16U && Lines("combat_voipdrop_dropped_total").empty());
  QCHECK(Lines("combat_voipdrop_log_cap").size() == 1U);  // the 17th drop said the first-drop lines stopped, once
  Slot()(g_cs, 1, kEvent, kData, 1);
  std::vector<Line> totals = Lines("combat_voipdrop_dropped_total");
  QCHECK(totals.size() == 1U && Has(totals[0].text, "\"n\":50,") && totals[0].level == sentinel::LogLevel::kInfo);
  for (int i = 0; i < 850; ++i) Slot()(g_cs, 1, kEvent, kData, 1);
  QCHECK(Lines("combat_voipdrop_dropped_total").size() == 16U);
  const std::vector<Line> caps = Lines("combat_voipdrop_log_cap");
  QCHECK(caps.size() == 2U);  // and the 17th total said the totals stopped, once
  if (caps.size() == 2U) {
    QCHECK(caps[0].level == sentinel::LogLevel::kWarn && Has(caps[0].text, "\"lines\":\"dropped\",\"cap\":16") && Has(caps[0].text, "combat_voipdrop_stats"));
    QCHECK(Has(caps[1].text, "\"lines\":\"dropped_total\",\"cap\":16") && Has(caps[1].text, "no further"));
  }
  QCHECK(vd::CurrentStats().dropped == 900 && g_realCalls == 0);
  QCHECK(Lines("combat_voipdrop_dropped").size() == 16U);
  vd::NoteEnd();  // the end line carries what was dropped
  const std::vector<Line> stats = Lines("combat_voipdrop_stats");
  QCHECK(stats.size() == 1U && Has(stats[0].text, "\"packets\":900,\"probed\":900,\"dropped\":900,\"final\":1"));
  End();
}

// The four exports are found by their symbol names, each into its own member.
void TheExportsAreFoundByTheirSymbolNames() {
  const auto find = [](void*, const char* symbol) -> void* {
    if (std::strcmp(symbol, vd::kBlockAllocatorSymbol) == 0) return reinterpret_cast<void*>(&FBlock);
    if (std::strcmp(symbol, vd::kCurrentPtrSymbol) == 0) return reinterpret_cast<void*>(&FCurPtr);
    if (std::strcmp(symbol, vd::kSetCurrentSymbol) == 0) return reinterpret_cast<void*>(&FSetCur);
    if (std::strcmp(symbol, vd::kCurrentSymbol) == 0) return reinterpret_cast<void*>(&FCur);
    return nullptr;
  };
  const vd::Fns f = vd::ResolveFnsWith(nullptr, find);
  QCHECK(f.blockAllocator == &FBlock && f.currentPtr == &FCurPtr && f.setCurrent == &FSetCur && f.current == &FCur);
  const vd::Fns none = vd::ResolveFnsWith(nullptr, [](void*, const char*) -> void* { return nullptr; });
  QCHECK(none.blockAllocator == nullptr && none.currentPtr == nullptr && none.setCurrent == nullptr && none.current == nullptr);
}

// 7. A slot that does not hold ReceiveVoip is refused and left alone, with the reason named; so is a word that points at other code.
void ASlotThatDoesNotHoldReceiveVoipIsRefusedAndLeftAlone() {
  Begin();
  Build(0x4242420);
  ModuleResult r = Install();
  QCHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0);
  QCHECK(*g_image.Slot(g_pins.slot) == reinterpret_cast<void*>(0x4242420));
  const std::vector<Line> install = Lines("combat_voipdrop_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"result\":\"refused\"") &&
         Has(install[0].text, "\"reason\":\"slot_refused\"") && Has(install[0].text, "\"hook\":\"original_mismatch\""));
  End();
  Begin();
  Build(reinterpret_cast<std::uintptr_t>(&OtherRecv));  // real executable code, but not ReceiveVoip: no acceptance by plausibility
  r = Install();
  QCHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0);
  QCHECK(*g_image.Slot(g_pins.slot) == reinterpret_cast<void*>(&OtherRecv));
  End();
}

// 8. A missing engine export: refused, nothing swapped, Error line. The resolver on a host without libr15 refuses the same way.
void AMissingEngineExportRefusesTheInstall() {
  Begin();
  vd::Fns f = Fns();
  f.setCurrent = nullptr;
  const ModuleResult r = Install(true, nullptr, f);
  QCHECK(!r.installed && std::strcmp(r.status, "export_missing") == 0 && SlotIsTheGames());
  std::vector<Line> install = Lines("combat_voipdrop_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"reason\":\"export_missing\""));
  End();
  for (int missing = 0; missing < 4; ++missing) {
    Begin();
    vd::Fns g = Fns();
    if (missing == 0) g.blockAllocator = nullptr;
    if (missing == 1) g.currentPtr = nullptr;
    if (missing == 2) g.setCurrent = nullptr;
    if (missing == 3) g.current = nullptr;
    QCHECK(std::strcmp(Install(true, nullptr, g).status, "export_missing") == 0 && SlotIsTheGames());
    End();
  }
  Begin();
  vd::SetImageLookupForTest(&Lookup);
  const vd::Fns none = vd::ResolveFns();  // no game library on the host
  QCHECK(none.blockAllocator == nullptr && none.currentPtr == nullptr && none.setCurrent == nullptr && none.current == nullptr);
  const ModuleResult viaInstall = vd::Install(true);
  QCHECK(!viaInstall.installed && std::strcmp(viaInstall.status, "export_missing") == 0 && SlotIsTheGames());
  End();
}

// 9. The plugin's kill file: a Warning that names the key; the module is installed all the same.
void TheLegacySwitchFileIsOnlyToldAbout() {
  char tmpl[] = "/tmp/voipdrop-legacy-XXXXXX";
  QCHECK(mkdtemp(tmpl) != nullptr);
  const std::string dir = tmpl;
  const std::string file = dir + "/voipdrop.off";
  Begin();
  ModuleResult r = Install(true, dir.c_str());  // no file: nothing said
  QCHECK(r.installed && Lines("combat_voipdrop_legacy_switch").empty());
  End();
  std::FILE* f = std::fopen(file.c_str(), "w");
  QCHECK(f != nullptr);
  if (f != nullptr) std::fclose(f);
  Begin();
  r = Install(true, dir.c_str());
  QCHECK(r.installed && Hooked());  // the file turns nothing off
  const std::vector<Line> legacy = Lines("combat_voipdrop_legacy_switch");
  QCHECK(legacy.size() == 1U && legacy[0].level == sentinel::LogLevel::kWarn);
  if (legacy.size() == 1U) QCHECK(Has(legacy[0].text, "\"key\":\"combat.voipdrop\"") && Has(legacy[0].text, "\"file\":\"voipdrop.off\""));
  End();
  Begin();
  r = Install(false, dir.c_str());  // off by the key, with the file there: still told, still off
  QCHECK(!r.installed && r.off && Lines("combat_voipdrop_legacy_switch").size() == 1U);
  End();
  unlink(file.c_str());
  rmdir(dir.c_str());
}

// combat.voipdrop: false installs nothing and says so.
void TheSwitchOffInstallsNothingAndSaysSo() {
  Begin();
  const ModuleResult r = Install(false);
  QCHECK(!r.installed && r.off && std::strcmp(r.status, "off_config") == 0 && SlotIsTheGames());
  const std::vector<Line> install = Lines("combat_voipdrop_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo);
  if (install.size() == 1U) {
    QCHECK(combat_test::Golden(install[0].text) ==
           "\"event\":\"combat_voipdrop_install\",\"result\":\"off\",\"reason\":\"off_config\",\"consequence\":\"combat.voipdrop is false: a voice "
           "packet whose table page cannot be allocated still reaches the game's function\"}");
  }
  End();
}

// The image: not loaded, no build ID, another build.
void AMissingOrOtherImageInstallsNothing() {
  Begin();
  QCHECK(std::strcmp(vd::InstallAt(nullptr, g_pins, Fns(), true).status, "module_not_loaded") == 0);
  g_image.Drop();
  QCHECK(std::strcmp(Install().status, "module_not_loaded") == 0);
  Build();
  g_image.Build({}, pinned::kLibR15StockBuildId);
  QCHECK(std::strcmp(Install().status, "build_id_mismatch") == 0);
  g_image.Build({}, nullptr, /*note=*/false);
  QCHECK(std::strcmp(Install().status, "no_build_id") == 0);
  const std::vector<Line> lines = Lines("combat_voipdrop_install");
  QCHECK(lines.size() == 4U);
  for (const Line& l : lines) QCHECK(l.level == sentinel::LogLevel::kError && Has(l.text, "\"result\":\"refused\""));
  End();
}

// The pinned words: the table is pinned, every word flipped in turn (and an unreadable one) refuses the install before the slot is touched.
void EveryPinnedWordGatesTheInstall() {
  Begin();
  combat_test::EveryPinIsRefused(
      "combat_voipdrop_install", kAllWords, g_image, [] {
        combat_test::StartCapture();
        vd::ResetForTest();
        Build();
      },
      [] { return Install(); }, [] { return SlotIsTheGames(); });
  End();
}

// A second install: the slot already holds the hook, nothing is installed again.
void ASecondInstallDoesNothing() {
  Begin();
  QCHECK(Install().installed);
  const ModuleResult again = Install();
  QCHECK(!again.installed && Hooked());
  End();
}

// The guard acts in every level: with no level model at all (no scope, no tick) it drops and passes as in the other tests.
void ItNeedsNoLevelToAct() {
  Installed();
  World(-1, 0x80000);
  g_allocOk = false;
  Slot()(g_cs, 1, kEvent, kData, 1);
  QCHECK(g_realCalls == 0 && vd::CurrentStats().dropped == 1);
  End();
}

// The stats: a Debug line every kStatsEvery packets that reach the game's function, the Info line at the end of the process only when installed.
void TheStatsAreWrittenEveryKStatsEveryPacketsAndAtTheEnd() {
  Installed();
  World(5, 0x80000);
  for (unsigned i = 0; i < vd::kStatsEvery - 1; ++i) Slot()(g_cs, 1, kEvent, kData, 1);
  QCHECK(Lines("combat_voipdrop_stats").empty());
  Slot()(g_cs, 1, kEvent, kData, 1);
  std::vector<Line> stats = Lines("combat_voipdrop_stats");
  QCHECK(stats.size() == 1U && stats[0].level == sentinel::LogLevel::kDebug && Has(stats[0].text, "\"final\":0") &&
         Has(stats[0].text, "\"packets\":16384,\"probed\":0,\"dropped\":0"));
  vd::NoteEnd();
  stats = Lines("combat_voipdrop_stats");
  QCHECK(stats.size() == 2U && stats[1].level == sentinel::LogLevel::kInfo && Has(stats[1].text, "\"final\":1"));
  if (stats.size() == 2U) {
    QCHECK(combat_test::Golden(stats[1].text) ==
           "\"event\":\"combat_voipdrop_stats\",\"packets\":16384,\"probed\":0,\"dropped\":0,\"final\":1,\"consequence\":\"packets is every voice "
           "packet that reached the handler, probed those whose table needed a page, dropped those whose page could not be allocated; packets "
           "over the run's time is the call rate of ReceiveVoip\"}");
  }
  End();
  Begin();  // not installed: no final line
  vd::NoteEnd();
  QCHECK(Lines("combat_voipdrop_stats").empty());
  End();
}

}  // namespace

int main() {
  ThePinsAreTheReadelfAndDisassemblyPins();
  TheThunkTakesTheSlotAndTheLineSaysSo();
  ATableWithAFreePageReachesTheGamesFunctionWithNoProbe();
  AProbeThatSucceedsGivesTheBlockBackAndRestoresTheContext();
  AProbeThatFailsDropsThePacket();
  NoAllocatorDropsThePacket();
  EveryDoubtRunsTheGamesFunction();
  TheDropLinesAreCappedAndCounted();
  TheExportsAreFoundByTheirSymbolNames();
  ASlotThatDoesNotHoldReceiveVoipIsRefusedAndLeftAlone();
  AMissingEngineExportRefusesTheInstall();
  TheLegacySwitchFileIsOnlyToldAbout();
  TheSwitchOffInstallsNothingAndSaysSo();
  AMissingOrOtherImageInstallsNothing();
  EveryPinnedWordGatesTheInstall();
  ASecondInstallDoesNothing();
  ItNeedsNoLevelToAct();
  TheStatsAreWrittenEveryKStatsEveryPacketsAndAtTheEnd();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "voipdrop_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("voipdrop_test: all checks passed");
  return 0;
}
