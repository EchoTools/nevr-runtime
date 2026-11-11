// Host test for the combat area's game-thread tick (#2172, docs/adr/0046; docs/adr/0039): src/quest/combat/combat_tick.cpp, built without
// exceptions as the sentinel builds it, installed by the real GotHook into the JUMP_SLOT of `CncaGame::ExclusiveUpdateGame` in a libr15
// image built in memory, and called the way the game calls it: through the slot, with the CR15Game as the one argument.
//
// What it pins: the callback table (16 fit, the 17th and a null are refused, a callback added after the install runs from the next
// frame), the handler (the game's function first, then the callbacks in registration order with the object, only for an object that
// carries the CR15Game's vptr, one Error the first time it does not), the pinned slot, and who owns the slot on each variant of the APK:
//
//   plain release   nothing installs (combat_install_test.cpp), so the slot keeps the game's function and no callback runs;
//   combat, q15t    this module alone, through the runner's normal step: the slot holds its thunk, which calls the game's function.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/combat_tick.h"

#include <elf.h>
#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hook_log.h"
#include "quest/combat/combat_active.h"
#include "quest/combat/combat_install.h"
#include "quest/combat/combat_tick_targets.h"
#include "quest/tests/test_check.h"

// sendguard's table row reads the sentinel's files folder (the plugin's old switch file); the test names a folder that does not exist.
namespace sentinel {
const char* FilesDir() { return "/nonexistent-nevr-files"; }
}  // namespace sentinel

namespace {

namespace bv = nevr_quest::build_variant;
namespace combat = nevr_quest::combat;
namespace tick = nevr_quest::combat::tick;
namespace pinned = sentinel::pinned;

// ---- the log ------------------------------------------------------------------------------------------------------------------

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
bool AnyLineHas(const char* needle) {
  for (const Line& l : g_lines) {
    if (Has(l.text, needle)) return true;
  }
  return false;
}

// ---- the game's function and the callbacks, in the order they ran ----------------------------------------------------------------

std::vector<std::string> g_order;
std::vector<std::uintptr_t> g_seenGame;

void GameExclusiveUpdate(void* game) {  // the game's own function: what a slot nobody hooked holds
  g_order.emplace_back("game");
  g_seenGame.push_back(reinterpret_cast<std::uintptr_t>(game));
}
template <int N>
void CombatCallback(std::uintptr_t game) noexcept {
  g_order.push_back("combat" + std::to_string(N));
  g_seenGame.push_back(game);
}

// ---- the libr15 image, in memory ----------------------------------------------------------------------------------------------

constexpr std::size_t kImageSize = 0x3800000;
constexpr std::uint64_t kNoteAt = 0x100, kDynstrAt = 0x400, kDynsymAt = 0x1000, kJmprelAt = 0x1400, kDynamicAt = 0x1800;
constexpr std::uint64_t kRwStart = 0x3600000;

struct Image {
  unsigned char* mem = nullptr;
  Elf64_Phdr phdr[4] = {};
  sentinel::ElfImage image;
  bool present = false;
};
Image g_image;

bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.present || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_image.image;
  return true;
}

int HexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

std::uintptr_t Base() { return g_image.image.base; }
void** Slot() { return reinterpret_cast<void**>(Base() + tick::targets::kSlotVaddr); }
std::uintptr_t VptrValue() { return Base() + tick::targets::kGameVptrVaddr; }

void BuildImage() {
  void* mem = mmap(nullptr, kImageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  QCHECK(mem != MAP_FAILED);
  g_image.mem = static_cast<unsigned char*>(mem);
  unsigned char* const m = g_image.mem;

  const char* const id = pinned::LibR15PatchedBuildId();
  unsigned char desc[20] = {};
  QCHECK(std::strlen(id) == 40);
  for (std::size_t i = 0; i < 20 && std::strlen(id) == 40; ++i) desc[i] = static_cast<unsigned char>((HexNibble(id[2 * i]) << 4) | HexNibble(id[2 * i + 1]));
  const Elf64_Nhdr nh{4, 20, NT_GNU_BUILD_ID};
  std::memcpy(m + kNoteAt, &nh, sizeof(nh));
  std::memcpy(m + kNoteAt + sizeof(nh), "GNU", 4);
  std::memcpy(m + kNoteAt + sizeof(nh) + 4, desc, 20);

  std::string dynstr(1, '\0');
  Elf64_Sym syms[2] = {};
  syms[1].st_name = static_cast<Elf64_Word>(dynstr.size());
  syms[1].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
  syms[1].st_shndx = 1;
  syms[1].st_value = tick::targets::kStockVaddr;
  dynstr += tick::targets::kSymbol;
  dynstr.push_back('\0');
  std::memcpy(m + kDynstrAt, dynstr.data(), dynstr.size());
  std::memcpy(m + kDynsymAt, syms, sizeof(syms));

  Elf64_Rela jmprel[1] = {};
  jmprel[0].r_offset = tick::targets::kSlotVaddr;
  jmprel[0].r_info = ELF64_R_INFO(1, sentinel::kNativeRelocs.jumpSlot);
  std::memcpy(m + kJmprelAt, jmprel, sizeof(jmprel));

  Elf64_Dyn dyn[8] = {};
  std::size_t n = 0;
  auto add = [&dyn, &n](Elf64_Sxword tag, std::uint64_t value) {
    dyn[n].d_tag = tag;
    dyn[n].d_un.d_val = value;
    ++n;
  };
  add(DT_STRTAB, kDynstrAt);
  add(DT_SYMTAB, kDynsymAt);
  add(DT_STRSZ, dynstr.size());
  add(DT_JMPREL, kJmprelAt);
  add(DT_PLTRELSZ, sizeof(jmprel));
  add(DT_PLTREL, DT_RELA);
  add(DT_FLAGS, DF_BIND_NOW);
  add(DT_NULL, 0);
  std::memcpy(m + kDynamicAt, dyn, sizeof(dyn));

  g_image.image.base = reinterpret_cast<std::uintptr_t>(m);
  *Slot() = reinterpret_cast<void*>(&GameExclusiveUpdate);  // the slot holds the game's function

  g_image.phdr[0].p_type = PT_LOAD;
  g_image.phdr[0].p_flags = PF_R | PF_X;
  g_image.phdr[0].p_vaddr = 0;
  g_image.phdr[0].p_filesz = g_image.phdr[0].p_memsz = kRwStart;
  g_image.phdr[1].p_type = PT_LOAD;
  g_image.phdr[1].p_flags = PF_R | PF_W;
  g_image.phdr[1].p_vaddr = kRwStart;
  g_image.phdr[1].p_filesz = g_image.phdr[1].p_memsz = kImageSize - kRwStart;
  g_image.phdr[2].p_type = PT_DYNAMIC;
  g_image.phdr[2].p_flags = PF_R | PF_W;
  g_image.phdr[2].p_vaddr = kDynamicAt;
  g_image.phdr[2].p_filesz = g_image.phdr[2].p_memsz = sizeof(dyn);
  g_image.phdr[3].p_type = PT_NOTE;
  g_image.phdr[3].p_vaddr = kNoteAt;
  g_image.phdr[3].p_filesz = g_image.phdr[3].p_memsz = sizeof(nh) + 4 + 20;
  g_image.image.phdr = g_image.phdr;
  g_image.image.phnum = 4;
  std::strcpy(g_image.image.name, pinned::kLibR15);
  g_image.present = true;
}

void DropImage() {
  g_image.present = false;
  if (g_image.mem != nullptr) munmap(g_image.mem, kImageSize);
  g_image.mem = nullptr;
  g_image.image = sentinel::ElfImage{};
}

// A game object: its first word is the vptr value the pinned build gives CR15Game (or anything else).
std::uintptr_t g_gameWords[8] = {};
void* Game(std::uintptr_t vptr) {
  g_gameWords[0] = vptr;
  return g_gameWords;
}

// The game calling through the slot.
void CallSlot(void* game) { reinterpret_cast<void (*)(void*)>(*Slot())(game); }

// A fresh process of this variant: the image, the logs and the module's state.
void World(bv::Variant variant) {
  tick::ResetForTest();
  combat::ResetForTest();
  combat::ResetInstallForTest();
  g_lines.clear();
  g_order.clear();
  g_seenGame.clear();
  sentinel::SetLogSink(&Capture);
  combat::SetVariant(variant);
  combat::Resolve(/*devBuild=*/false, /*forceAsked=*/false);
  g_lines.clear();
  BuildImage();
}
void Done() {
  tick::ResetForTest();
  DropImage();
  sentinel::SetLogSink(nullptr);
}

// ---- tests --------------------------------------------------------------------------------------------------------------------

void TheTableTakesSixteenCallbacksAndRefusesTheRest() {
  tick::ResetForTest();
  QCHECK(tick::kMaxCallbacks == 16);
  QCHECK(tick::Count() == 0);
  QCHECK(!tick::Add(nullptr));
  const tick::TickFn all[16] = {&CombatCallback<1>,  &CombatCallback<2>,  &CombatCallback<3>,  &CombatCallback<4>, &CombatCallback<5>,  &CombatCallback<6>,
                                &CombatCallback<7>,  &CombatCallback<8>,  &CombatCallback<9>,  &CombatCallback<10>, &CombatCallback<11>, &CombatCallback<12>,
                                &CombatCallback<13>, &CombatCallback<14>, &CombatCallback<15>, &CombatCallback<16>};
  for (std::size_t i = 0; i < 16; ++i) {
    QCHECK(tick::Add(all[i]));
    QCHECK(tick::Count() == i + 1);
  }
  QCHECK(!tick::Add(&CombatCallback<17>));
  QCHECK(tick::Count() == 16);
  tick::ResetForTest();
}

void ThePinnedSlotIsExclusiveUpdateGames() {
  const sentinel::GotTarget t = tick::targets::Target();
  QCHECK(std::strcmp(t.module, "libr15.so") == 0);
  QCHECK(std::strcmp(t.symbol, "_ZN10NRadEngine8CncaGame19ExclusiveUpdateGameEv") == 0);
  QCHECK(t.kind == sentinel::RelocKind::kJumpSlot);
  QCHECK(t.slotVaddr.has_value() && *t.slotVaddr == 0x36e7f28ULL);
  QCHECK(tick::targets::kStockVaddr == 0x17ef62cULL && tick::targets::kGameVptrVaddr == 0x362ed30ULL);
  QCHECK(t.expectedOriginal == nullptr);  // a wrapper another owner put in the slot first is taken as the function the thunk calls
  QCHECK(t.owner != nullptr && std::strcmp(t.owner, "combat_tick") == 0);
  QCHECK(t.buildId != nullptr && std::strcmp(t.buildId, pinned::LibR15PatchedBuildId()) == 0);
}

// The combat variant: this module alone. The game's function runs first, then the callbacks in registration order with the object.
void TheHandlerRunsTheGamesFunctionThenTheCallbacksInOrder() {
  World(bv::Variant::kCombat);
  QCHECK(tick::Add(&CombatCallback<1>));
  QCHECK(tick::Add(&CombatCallback<2>));
  QCHECK(tick::InstallAt(&Lookup));
  QCHECK(*Slot() != reinterpret_cast<void*>(&GameExclusiveUpdate));  // the slot holds the thunk
  void* const game = Game(VptrValue());
  CallSlot(game);
  QCHECK((g_order == std::vector<std::string>{"game", "combat1", "combat2"}));
  QCHECK(g_seenGame.size() == 3U && g_seenGame[0] == reinterpret_cast<std::uintptr_t>(game) && g_seenGame[1] == g_seenGame[0] &&
         g_seenGame[2] == g_seenGame[0]);
  QCHECK(tick::Calls() == 1);
  // A callback added after the install runs from the next frame.
  QCHECK(tick::Add(&CombatCallback<3>));
  g_order.clear();
  CallSlot(game);
  QCHECK((g_order == std::vector<std::string>{"game", "combat1", "combat2", "combat3"}));
  QCHECK(tick::Calls() == 2);
  // One install line, Info, with the result and the callback count.
  const std::vector<Line> install = Lines("combat_tick_install");
  QCHECK(install.size() == 1U);
  if (install.size() == 1U) {
    QCHECK(install[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Has(install[0].text, "\"result\":\"installed\"") && Has(install[0].text, "\"status\":\"ok\"") && Has(install[0].text, "\"callbacks\":2"));
    QCHECK(Has(install[0].text, "\"msg\":"));
  }
  Done();
}

// The vptr is read once for the object that passed, not once per frame: the game calls with the same CR15Game every frame. A new pointer is read
// again, and an object that failed is read (and refused) every time, never remembered.
void TheVptrIsReadOncePerObjectNotOncePerFrame() {
  World(bv::Variant::kCombat);
  QCHECK(tick::Add(&CombatCallback<1>));
  QCHECK(tick::InstallAt(&Lookup));
  void* const game = Game(VptrValue());
  for (int frame = 0; frame < 1000; ++frame) CallSlot(game);
  QCHECK(tick::Calls() == 1000 && tick::VptrReads() == 4);  // once, then at frames 256, 512 and 768
  int ran = 0;
  for (const std::string& s : g_order) ran += s == "combat1" ? 1 : 0;
  QCHECK(ran == 1000);  // every frame still ran the callback
  static std::uintptr_t second[8] = {};
  second[0] = VptrValue();
  CallSlot(second);
  QCHECK(tick::VptrReads() == 5);  // a new pointer: read again
  static std::uintptr_t wrong[8] = {};
  wrong[0] = 0x777;
  for (int frame = 0; frame < 3; ++frame) CallSlot(wrong);
  QCHECK(tick::VptrReads() == 8);  // a refused object is read every frame
  g_order.clear();
  CallSlot(second);  // the verified object is still the last one that passed
  QCHECK(tick::VptrReads() == 9 && g_order.size() == 2U);  // `second` was forgotten by the refused frames: read again, then it runs
  // A reused address: the object at the verified pointer stops being a CR15Game (a freed object's address taken by another class). Its callbacks
  // stop within the re-check interval, and one Error line says so.
  g_order.clear();
  Game(VptrValue());
  for (int frame = 0; frame < 10; ++frame) CallSlot(game);
  g_gameWords[0] = 0x888;  // the same address, another class
  int ranAfter = 0;
  for (int frame = 0; frame < 300; ++frame) {
    g_order.clear();
    CallSlot(game);
    ranAfter += g_order.size() > 1U ? 1 : 0;
  }
  QCHECK(ranAfter < 256);  // bounded by the interval, not for ever
  QCHECK(Lines("combat_tick_object").size() == 1U);
  Done();
}

// An object that is not the CR15Game runs the game's function and no callback, and says so once, loudly.
void AnObjectThatIsNotTheGameRunsNoCallbackAndSaysSoOnce() {
  World(bv::Variant::kCombat);
  QCHECK(tick::Add(&CombatCallback<1>));
  QCHECK(tick::InstallAt(&Lookup));
  CallSlot(Game(0x777));
  QCHECK((g_order == std::vector<std::string>{"game"}));
  std::vector<Line> object = Lines("combat_tick_object");
  QCHECK(object.size() == 1U);
  if (object.size() == 1U) {
    QCHECK(object[0].level == sentinel::LogLevel::kError);
    QCHECK(Has(object[0].text, "\"readable\":1"));
  }
  g_order.clear();
  CallSlot(Game(0x778));
  QCHECK((g_order == std::vector<std::string>{"game"}));
  QCHECK(Lines("combat_tick_object").size() == 1U);  // once
  CallSlot(Game(VptrValue()));
  QCHECK((g_order == std::vector<std::string>{"game", "game", "combat1"}));  // the real game still ticks
  Done();

  World(bv::Variant::kCombat);
  QCHECK(tick::Add(&CombatCallback<1>));
  QCHECK(tick::InstallAt(&Lookup));
  CallSlot(nullptr);  // a null object is unreadable, never a fault
  QCHECK((g_order == std::vector<std::string>{"game"}));
  object = Lines("combat_tick_object");
  QCHECK(object.size() == 1U && Has(object[0].text, "\"readable\":0"));
  Done();
}

// The image has no libr15 (the module is not loaded), or the wrong build: refused, one Error install line, and the game's slot untouched.
void ARefusedInstallLeavesTheSlotAloneAndSaysWhy() {
  World(bv::Variant::kCombat);
  QCHECK(!tick::InstallAt([](const char*, sentinel::ElfImage*) { return false; }));
  void* const slotBefore = *Slot();
  QCHECK(slotBefore == reinterpret_cast<void*>(&GameExclusiveUpdate));
  const std::vector<Line> install = Lines("combat_tick_install");
  QCHECK(install.size() == 1U);
  if (install.size() == 1U) {
    QCHECK(install[0].level == sentinel::LogLevel::kError);
    QCHECK(Has(install[0].text, "\"result\":\"refused\"") && Has(install[0].text, "\"status\":\"module_not_loaded\""));
  }
  Done();
}

// The q15t variant: no plugin wraps the slot, so the runner's normal step installs this module alone, as on
// the combat variant: the game's function runs once, then combat's callbacks, and nothing logs `unexpected_original`.
void OnAQ15tApkTheRunnersStepInstallsTheTickAlone() {
  World(bv::Variant::kQ15t);
  static const combat::Module kTable[] = {{"tick", nullptr, [](void) noexcept -> combat::ModuleResult {
                                             const bool ok = tick::InstallAt(&Lookup);
                                             return {ok, ok ? "installed" : "refused"};
                                           }}};
  QCHECK(tick::Add(&CombatCallback<1>));
  const combat::Outcome outcome = combat::RunPhase(combat::Phase::kNormal, kTable, 1);
  QCHECK(outcome.ran && outcome.installed == 1 && outcome.failed == 0);
  QCHECK(*Slot() != reinterpret_cast<void*>(&GameExclusiveUpdate));  // this module's thunk
  CallSlot(Game(VptrValue()));
  QCHECK((g_order == std::vector<std::string>{"game", "combat1"}));
  QCHECK(!AnyLineHas("unexpected_original"));
  QCHECK(Lines("combat_install").size() == 1U && Lines("combat_install_module").size() == 1U &&
         Has(Lines("combat_install_module")[0].text, "\"module\":\"tick\",\"outcome\":\"installed\",\"status\":\"installed\""));
  const std::vector<Line> install = Lines("combat_tick_install");
  QCHECK(install.size() == 1U && Has(install[0].text, "\"result\":\"installed\""));
  Done();
}

// A wrapper another owner put in the slot before the normal step (combat_tick.h, "The slot"): the target pins no expected original, so
// the install takes the wrapper as the function its thunk calls. The wrapper runs, then the game's function through it, then combat's
// callbacks, and nothing is refused.
void OtherOwnersWrapper(void* game) {
  g_order.emplace_back("wrapper");
  GameExclusiveUpdate(game);
}
void AWrapperAlreadyInTheSlotIsTheFunctionTheThunkCalls() {
  World(bv::Variant::kCombat);
  *Slot() = reinterpret_cast<void*>(&OtherOwnersWrapper);
  QCHECK(tick::Add(&CombatCallback<1>));
  QCHECK(tick::InstallAt(&Lookup));
  QCHECK(*Slot() != reinterpret_cast<void*>(&OtherOwnersWrapper));  // this module's thunk, above the wrapper
  CallSlot(Game(VptrValue()));
  QCHECK((g_order == std::vector<std::string>{"wrapper", "game", "combat1"}));
  QCHECK(!AnyLineHas("unexpected_original"));
  Done();
}

// The plain release: the runner installs nothing (combat is not active), so the slot keeps the game's function and no callback runs.
void OnAPlainReleaseNothingInstallsAndTheGamesFunctionRunsAlone() {
  World(bv::Variant::kNone);
  static const combat::Module kTable[] = {{"tick", nullptr, [](void) noexcept -> combat::ModuleResult {
                                             const bool ok = tick::InstallAt(&Lookup);
                                             return {ok, ok ? "installed" : "refused"};
                                           }}};
  QCHECK(tick::Add(&CombatCallback<1>));
  const combat::Outcome outcome = combat::RunPhase(combat::Phase::kNormal, kTable, 1);
  QCHECK(!outcome.ran);
  QCHECK(*Slot() == reinterpret_cast<void*>(&GameExclusiveUpdate));
  CallSlot(Game(VptrValue()));
  QCHECK((g_order == std::vector<std::string>{"game"}));
  QCHECK(Lines("combat_tick_install").empty());
  Done();
}

}  // namespace

int main() {
  TheTableTakesSixteenCallbacksAndRefusesTheRest();
  ThePinnedSlotIsExclusiveUpdateGames();
  TheHandlerRunsTheGamesFunctionThenTheCallbacksInOrder();
  AnObjectThatIsNotTheGameRunsNoCallbackAndSaysSoOnce();
  TheVptrIsReadOncePerObjectNotOncePerFrame();
  ARefusedInstallLeavesTheSlotAloneAndSaysWhy();
  OnAPlainReleaseNothingInstallsAndTheGamesFunctionRunsAlone();
  OnAQ15tApkTheRunnersStepInstallsTheTickAlone();
  AWrapperAlreadyInTheSlotIsTheFunctionTheThunkCalls();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "combat_tick_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("combat_tick_test: all checks passed");
  return 0;
}
