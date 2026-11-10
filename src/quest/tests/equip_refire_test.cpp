// Host test for the equipment re-fire on Quest (#2121): the real wrappers (src/quest/combat/equip_refire.cpp), built without
// exceptions as the sentinel builds it, installed by the real GotHook into the six data words of a libr15 image built in memory,
// and called the way the game calls them: through the word. The game side is a model: three setters (one per slot and word kind, so
// each word's own stored original can be told apart), three change handlers, NetGameMT, CR15NetGame::UserId and the component, the
// net game and the players' records laid out where the game's code reads them (the offsets are spelled in this file, not taken from
// the header, so a wrong constant there fails here).
//
// What this test does not repeat: the debounce, the staleness reasons and the reads' fail-closed rules. They are the shared header's
// (src/runtime/patch/equipment_refire_rules.h) and its GTest (src/runtime/tests/test_equipment_refire.cpp). What it pins is the
// Quest side of that rule: that the six words are the ones hooked and each wrapper calls ITS word's original with the game's three
// arguments, that the handlers called are the slot's own, that the net game probe reads where the game's getter reads, that a word
// that does not hold the setter is refused alone, that a pin that differs refuses the module before any word is touched, and the
// golden event lines. got_pinned_test.cpp resolves the six relocations and the sixteen code pins in the real libr15.
//
// The model setters write the record's slot and nothing else: the setter's own inline event is the game's, so every handler call
// counted here is a re-fire.
//
// Built and run by `just test-quest-combat`.

#include <elf.h>
#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "quest/combat/equip_refire.h"
#include "quest/invite/frame_dispatch.h"
#include "quest/tests/test_check.h"

namespace {

namespace eq = nevr_quest::combat::equip_refire;
namespace frame = nevr_quest::invite::frame;
namespace pinned = sentinel::pinned;

// ---- the layout, as this test spells it ---------------------------------------------------------------------------------------

constexpr std::size_t kOffCtx = 0x80;
constexpr std::size_t kOffIndexTable = 0xc8;
constexpr std::size_t kOffCount = 0xf2;
constexpr std::size_t kOffRecords = 0xf8;
constexpr std::size_t kOffLinks = 0x100;
constexpr std::size_t kCtxNetGameArg = 0x8;
constexpr std::size_t kCtxActorKey = 0x58;
constexpr std::size_t kRecordStride = 0x58;
constexpr std::size_t kRecordValue[3] = {0x18, 0x20, 0x28};  // weapon, tac mod, ordnance
constexpr std::size_t kGameSlots = 0xdc;
constexpr std::size_t kGameSlots2 = 0xde;
constexpr std::size_t kGameLocalField = 0x178;
constexpr std::size_t kGameGeneration = 0x13a;
constexpr std::size_t kNetGameField = 0x8510;

constexpr std::uint32_t kLocalHandle = (5U << 16) | 1;    // the local player's record: sparse 1, dense 0
constexpr std::uint32_t kRemoteHandle = (6U << 16) | 2;   // another player's record: sparse 2, dense 1
constexpr std::uint32_t kLocalHandle2 = (7U << 16) | 3;   // a second record of the local player: sparse 3, dense 2
constexpr std::uint64_t kKey0 = 0xaaaa1111aaaa1111ULL;
constexpr std::uint64_t kLocalKey1 = 0xbbbb2222bbbb2222ULL;
constexpr std::uint64_t kRemoteKey1 = 0xcccc3333cccc3333ULL;
constexpr std::uint32_t kGameLocalSlotHandle = (9U << 16) | 3;   // field 3, generation 9
constexpr std::uint32_t kGameRemoteSlotHandle = (8U << 16) | 4;
constexpr std::uintptr_t kVtable = 0x1234abcd00ULL;

// ---- the game's memory ----------------------------------------------------------------------------------------------------------

alignas(16) unsigned char g_component[0x200];
unsigned char* g_ctx = nullptr;  // a page of its own, read-only once built: the module only reads the game, so a write check fails
alignas(16) unsigned char g_indexTable[64];
alignas(16) unsigned char g_records[8 * kRecordStride];
alignas(16) unsigned char g_links[8 * 0x10];
alignas(16) unsigned char g_game[0x600];
unsigned char* g_netArg = nullptr;  // ends exactly 8 bytes after the field the getter reads: a read at +0x8518 is a guard page
unsigned char* g_netMapping = nullptr;
constexpr std::size_t kNetPages = 13;
constexpr std::size_t kPageBytes = 4096;

template <typename T>
void Put(unsigned char* base, std::size_t offset, T value) {
  std::memcpy(base + offset, &value, sizeof(value));
}
template <typename T>
T Get(const unsigned char* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, base + offset, sizeof(value));
  return value;
}

void BuildNetArg() {
  if (g_netMapping == nullptr) {
    g_netMapping = static_cast<unsigned char*>(
        mmap(nullptr, (kNetPages + 1) * kPageBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    QCHECK(g_netMapping != MAP_FAILED);
    QCHECK(mprotect(g_netMapping + kNetPages * kPageBytes, kPageBytes, PROT_NONE) == 0);
    g_netArg = g_netMapping + kNetPages * kPageBytes - (kNetGameField + 8);
  }
  std::memset(g_netMapping, 0, kNetPages * kPageBytes);
  Put<std::uintptr_t>(g_netArg, kNetGameField, reinterpret_cast<std::uintptr_t>(g_game));
}

void BuildGame() {
  if (g_ctx == nullptr) {
    g_ctx = static_cast<unsigned char*>(mmap(nullptr, kPageBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    QCHECK(g_ctx != MAP_FAILED);
  }
  QCHECK(mprotect(g_ctx, kPageBytes, PROT_READ | PROT_WRITE) == 0);
  std::memset(g_component, 0, sizeof(g_component));
  std::memset(g_ctx, 0, kPageBytes);
  std::memset(g_indexTable, 0, sizeof(g_indexTable));
  std::memset(g_records, 0, sizeof(g_records));
  std::memset(g_links, 0, sizeof(g_links));
  std::memset(g_game, 0, sizeof(g_game));
  BuildNetArg();
  Put<std::uintptr_t>(g_component, 0, kVtable);
  Put<std::uintptr_t>(g_component, kOffCtx, reinterpret_cast<std::uintptr_t>(g_ctx));
  Put<std::uintptr_t>(g_component, kOffIndexTable, reinterpret_cast<std::uintptr_t>(g_indexTable));
  Put<std::uint16_t>(g_component, kOffCount, 3);
  Put<std::uintptr_t>(g_component, kOffRecords, reinterpret_cast<std::uintptr_t>(g_records));
  Put<std::uintptr_t>(g_component, kOffLinks, reinterpret_cast<std::uintptr_t>(g_links));
  Put<std::uintptr_t>(g_ctx, kCtxNetGameArg, reinterpret_cast<std::uintptr_t>(g_netArg));
  Put<std::uint64_t>(g_ctx, kCtxActorKey, kKey0);
  QCHECK(mprotect(g_ctx, kPageBytes, PROT_READ) == 0);
  // sparse 1 -> dense 0 (generation 5), sparse 2 -> dense 1 (6), sparse 3 -> dense 2 (7)
  Put<std::uint16_t>(g_indexTable, 1 * 4, 0);
  Put<std::uint16_t>(g_indexTable, 1 * 4 + 2, 5);
  Put<std::uint16_t>(g_indexTable, 2 * 4, 1);
  Put<std::uint16_t>(g_indexTable, 2 * 4 + 2, 6);
  Put<std::uint16_t>(g_indexTable, 3 * 4, 2);
  Put<std::uint16_t>(g_indexTable, 3 * 4 + 2, 7);
  Put<std::uint64_t>(g_links, 0 * 0x10, kLocalKey1);
  Put<std::uint64_t>(g_links, 1 * 0x10, kRemoteKey1);
  Put<std::uint64_t>(g_links, 2 * 0x10, kLocalKey1);
  // The net game: two slots in use, the local player in field 3 whose generation is 9.
  Put<std::uint16_t>(g_game, kGameSlots, 2);
  Put<std::uint16_t>(g_game, kGameSlots2, 2);
  Put<std::uint16_t>(g_game, kGameLocalField, 3);
  Put<std::uint16_t>(g_game, kGameGeneration + 3 * 4, 9);
}

unsigned char* RecordOf(std::uint32_t handle) {
  const std::size_t sparse = handle & 0xffffU;
  const std::size_t dense = Get<std::uint16_t>(g_indexTable, sparse * 4);
  return g_records + dense * kRecordStride;
}
std::uint64_t ValueOf(std::uint32_t handle, int slot) { return Get<std::uint64_t>(RecordOf(handle), kRecordValue[slot]); }
void SetValue(std::uint32_t handle, int slot, std::uint64_t value) { Put<std::uint64_t>(RecordOf(handle), kRecordValue[slot], value); }

// ---- the game's functions (models) ---------------------------------------------------------------------------------------

struct SetterCall {
  int count = 0;
  void* component = nullptr;
  std::uint32_t handle = 0;
  std::uint64_t value = 0;
};
SetterCall g_setterCalls[3][2];

template <int Slot, int Word>
void ModelSetter(void* component, std::uint32_t handle, std::uint64_t value) {
  SetterCall& call = g_setterCalls[Slot][Word];
  ++call.count;
  call.component = component;
  call.handle = handle;
  call.value = value;
  SetValue(handle, Slot, value);
}

int g_handlerCalls[3];
void* g_handlerComponent[3];
std::uint32_t g_handlerHandle[3];
std::vector<int> g_handlerOrder;

template <int Slot>
void ModelHandler(void* component, std::uint32_t handle) {
  ++g_handlerCalls[Slot];
  g_handlerComponent[Slot] = component;
  g_handlerHandle[Slot] = handle;
  g_handlerOrder.push_back(Slot);
}

// NetGameMT: `mov w8, #0x8510; ldr x0, [x0, x8]; ret`.
std::uintptr_t ModelNetGameMT(std::uintptr_t arg) { return Get<std::uintptr_t>(reinterpret_cast<const unsigned char*>(arg), kNetGameField); }

// CR15NetGame::UserId: the slot handle the net game gives an actor.
std::uint32_t ModelUserId(void* /*game*/, std::uint64_t key0, std::uint64_t key1) {
  if (key0 == kKey0 && key1 == kLocalKey1) return kGameLocalSlotHandle;
  return kGameRemoteSlotHandle;
}

void ModelForeign() {}  // a word some other runtime put in a slot: an executable address

// ---- the libr15 image, in memory -----------------------------------------------------------------------------------------------

constexpr std::size_t kImageSize = 0x3800000;       // vaddrs up to the highest word the module reads or hooks
constexpr std::uint64_t kNoteAt = 0x100;
constexpr std::uint64_t kDynstrAt = 0x400;
constexpr std::uint64_t kDynsymAt = 0x1000;
constexpr std::uint64_t kJmprelAt = 0x1400;
constexpr std::uint64_t kRelaAt = 0x1600;
constexpr std::uint64_t kDynamicAt = 0x1800;
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
bool NoLibr15(const char*, sentinel::ElfImage*) { return false; }

int HexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

struct ImageOptions {
  const char* buildId = nullptr;  // nullptr: the pinned patched build ID
  bool note = true;               // false: no PT_NOTE, so no build ID
  int flippedPin = -1;            // index into kEquipmentRefireCode whose chosen word differs
  bool flipLast = true;           // the last compared word of that pin, else the first
  const void* foreignWord = nullptr;  // put into the SetAbilitySlot GLOB_DAT word instead of the setter
};

std::uintptr_t Base() { return g_image.image.base; }
void** SlotPtr(int slot, int word) {
  const pinned::EquipmentSetterPin& pin = pinned::kEquipmentSetters[slot];
  return reinterpret_cast<void**>(Base() + (word == 0 ? pin.jumpSlot : pin.globDatSlot));
}
void* StockWord(int slot) { return reinterpret_cast<void*>(Base() + pinned::kEquipmentSetters[slot].vaddr); }

void BuildImage(const ImageOptions& opt) {
  void* mem = mmap(nullptr, kImageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  QCHECK(mem != MAP_FAILED);
  g_image.mem = static_cast<unsigned char*>(mem);
  unsigned char* const m = g_image.mem;

  // The build-ID note.
  const char* const id = opt.buildId != nullptr ? opt.buildId : pinned::LibR15PatchedBuildId();
  unsigned char desc[20] = {};
  QCHECK(std::strlen(id) == 40);
  for (std::size_t i = 0; i < 20 && std::strlen(id) == 40; ++i) {
    desc[i] = static_cast<unsigned char>((HexNibble(id[2 * i]) << 4) | HexNibble(id[2 * i + 1]));
  }
  const Elf64_Nhdr nh{4, 20, NT_GNU_BUILD_ID};
  std::memcpy(m + kNoteAt, &nh, sizeof(nh));
  std::memcpy(m + kNoteAt + sizeof(nh), "GNU", 4);
  std::memcpy(m + kNoteAt + sizeof(nh) + 4, desc, 20);

  // .dynstr, .dynsym: the three setters' real mangled names.
  std::string dynstr(1, '\0');
  Elf64_Sym syms[4] = {};
  for (int i = 0; i < 3; ++i) {
    syms[i + 1].st_name = static_cast<Elf64_Word>(dynstr.size());
    syms[i + 1].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
    syms[i + 1].st_shndx = 1;
    syms[i + 1].st_value = pinned::kEquipmentSetters[i].vaddr;
    dynstr += pinned::kEquipmentSetters[i].symbol;
    dynstr.push_back('\0');
  }
  QCHECK(dynstr.size() < kDynsymAt - kDynstrAt);
  std::memcpy(m + kDynstrAt, dynstr.data(), dynstr.size());
  std::memcpy(m + kDynsymAt, syms, sizeof(syms));

  // The six relocations: the three JUMP_SLOTs in the PLT table, the three GLOB_DATs in the other.
  Elf64_Rela jmprel[3] = {};
  Elf64_Rela rela[3] = {};
  for (int i = 0; i < 3; ++i) {
    jmprel[i].r_offset = pinned::kEquipmentSetters[i].jumpSlot;
    jmprel[i].r_info = ELF64_R_INFO(static_cast<Elf64_Xword>(i + 1), sentinel::kNativeRelocs.jumpSlot);
    rela[i].r_offset = pinned::kEquipmentSetters[i].globDatSlot;
    rela[i].r_info = ELF64_R_INFO(static_cast<Elf64_Xword>(i + 1), sentinel::kNativeRelocs.globDat);
  }
  std::memcpy(m + kJmprelAt, jmprel, sizeof(jmprel));
  std::memcpy(m + kRelaAt, rela, sizeof(rela));

  Elf64_Dyn dyn[12] = {};
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
  add(DT_RELA, kRelaAt);
  add(DT_RELASZ, sizeof(rela));
  add(DT_RELAENT, sizeof(Elf64_Rela));
  add(DT_FLAGS, DF_BIND_NOW);
  add(DT_NULL, 0);
  std::memcpy(m + kDynamicAt, dyn, sizeof(dyn));

  // The words the module proves before it touches a slot, then the six slots holding the setters.
  for (std::size_t i = 0; i < pinned::kEquipmentRefireCodeCount; ++i) {
    const pinned::PinnedCode& pin = pinned::kEquipmentRefireCode[i];
    std::memcpy(m + pin.vaddr, pin.code, pin.words * sizeof(std::uint32_t));
    if (static_cast<int>(i) == opt.flippedPin) {
      const std::size_t word = opt.flipLast ? pin.words - 1 : 0;
      std::uint32_t w = Get<std::uint32_t>(m + pin.vaddr, word * 4);
      w ^= 0x00000020U;
      Put<std::uint32_t>(m + pin.vaddr, word * 4, w);
    }
  }
  g_image.image.base = reinterpret_cast<std::uintptr_t>(m);
  for (int slot = 0; slot < 3; ++slot) {
    for (int word = 0; word < 2; ++word) *SlotPtr(slot, word) = StockWord(slot);
  }
  if (opt.foreignWord != nullptr) *SlotPtr(1, 1) = const_cast<void*>(opt.foreignWord);

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
  g_image.image.phnum = opt.note ? 4 : 3;
  std::strcpy(g_image.image.name, pinned::kLibR15);
  g_image.present = true;
}

void DropImage() {
  g_image.present = false;
  if (g_image.mem != nullptr) munmap(g_image.mem, kImageSize);
  g_image.mem = nullptr;
  g_image.image = sentinel::ElfImage{};
}

// ---- the clock, the log, the game's calls through the words ------------------------------------------------------------------------

std::uint64_t g_now = 0;
std::uint64_t Clock() noexcept { return g_now; }

std::vector<std::string> g_lines;
void CaptureLine(sentinel::LogLevel /*level*/, const char* line) { g_lines.emplace_back(line); }

std::string Golden(const std::string& line) {
  const std::size_t at = line.find("\"event\"");
  return at == std::string::npos ? line : line.substr(at);
}
std::vector<std::string> LinesOf(const char* event) {
  std::vector<std::string> out;
  for (const std::string& line : g_lines) {
    if (line.find(std::string("\"event\":\"") + event + "\",") != std::string::npos) out.push_back(line);
  }
  return out;
}
std::string OneLine(const char* event) {
  const std::vector<std::string> lines = LinesOf(event);
  QCHECK(lines.size() == 1);
  return lines.empty() ? std::string() : Golden(lines[0]);
}

using Setter = pinned::EquipmentSetterSig;

// The game calling through a word.
void CallWord(int slot, int word, void* component, std::uint32_t handle, std::uint64_t value) {
  Setter* const fn = reinterpret_cast<Setter*>(*SlotPtr(slot, word));
  fn(component, handle, value);
}

template <int Slot, int Word>
void* EntryOf() {
  return pinned::LibR15EquipmentSetterThunk<Slot, Word>::EntryAddress();
}

// Each thunk's original is the model of its own word's setter (the install stored the stock word there; the model stands for the
// game's code, which a host test cannot run).
template <int Slot, int Word>
void StubOriginal() {
  *pinned::LibR15EquipmentSetterThunk<Slot, Word>::OriginalOut() = reinterpret_cast<void*>(&ModelSetter<Slot, Word>);
}
void StubOriginals() {
  StubOriginal<0, 0>();
  StubOriginal<0, 1>();
  StubOriginal<1, 0>();
  StubOriginal<1, 1>();
  StubOriginal<2, 0>();
  StubOriginal<2, 1>();
}
bool IsEntry(int slot, int word) {
  void* const entry = slot == 0 ? (word == 0 ? EntryOf<0, 0>() : EntryOf<0, 1>())
                      : slot == 1 ? (word == 0 ? EntryOf<1, 0>() : EntryOf<1, 1>())
                                  : (word == 0 ? EntryOf<2, 0>() : EntryOf<2, 1>());
  return *SlotPtr(slot, word) == entry;
}

eq::GameFunctions g_functions;

void Reset() {
  frame::ResetForTest();
  eq::ResetForTest();
  eq::SetClockForTest(&Clock);
  g_functions = {{reinterpret_cast<std::uintptr_t>(&ModelHandler<0>), reinterpret_cast<std::uintptr_t>(&ModelHandler<1>),
                  reinterpret_cast<std::uintptr_t>(&ModelHandler<2>)},
                 reinterpret_cast<std::uintptr_t>(&ModelNetGameMT),
                 reinterpret_cast<std::uintptr_t>(&ModelUserId)};
  eq::SetGameFunctionsForTest(&g_functions);
  g_now = 1000;
  g_lines.clear();
  BuildGame();
  std::memset(g_setterCalls, 0, sizeof(g_setterCalls));
  std::memset(g_handlerCalls, 0, sizeof(g_handlerCalls));
  std::memset(g_handlerComponent, 0, sizeof(g_handlerComponent));
  std::memset(g_handlerHandle, 0, sizeof(g_handlerHandle));
  g_handlerOrder.clear();
}

// A fresh image and an install on it; the originals are the models.
bool InstallFresh(const ImageOptions& opt = ImageOptions{}) {
  Reset();
  BuildImage(opt);
  const bool installed = eq::InstallAt(&Lookup);
  StubOriginals();
  return installed;
}
void Finish() {
  eq::ResetForTest();
  frame::ResetForTest();
  DropImage();
}

// The game thread's next frame, `ms` later.
void Frame(std::uint64_t ms) {
  g_now += ms;
  frame::Tick();
}

void* Comp() { return g_component; }

// ---- tests ----------------------------------------------------------------------------------------------------------------------

void TheLayoutConstantsAreTheStoreLibr15s() {
  QCHECK(eq::kSlotCount == 3);
  QCHECK(eq::kWordCount == 6);
  QCHECK(eq::kChangeLines == 256);
  QCHECK(std::strcmp(eq::InstallResultName(6), "installed") == 0);
  QCHECK(std::strcmp(eq::InstallResultName(1), "partial") == 0);
  QCHECK(std::strcmp(eq::InstallResultName(5), "partial") == 0);
  QCHECK(std::strcmp(eq::InstallResultName(0), "refused") == 0);
  // The six words and the three setters, as the store file names them (`readelf -rW`).
  QCHECK(pinned::kEquipmentSetters[0].jumpSlot == 0x36f3d00ULL && pinned::kEquipmentSetters[0].globDatSlot == 0x37233e8ULL);
  QCHECK(pinned::kEquipmentSetters[1].jumpSlot == 0x36fa900ULL && pinned::kEquipmentSetters[1].globDatSlot == 0x3727b38ULL);
  QCHECK(pinned::kEquipmentSetters[2].jumpSlot == 0x36d8e10ULL && pinned::kEquipmentSetters[2].globDatSlot == 0x3711db8ULL);
  QCHECK(pinned::kEquipmentSetters[0].vaddr == 0x217ca84ULL && pinned::kEquipmentSetters[0].handler == 0x217afb4ULL);
  QCHECK(pinned::kEquipmentSetters[1].vaddr == 0x217cde4ULL && pinned::kEquipmentSetters[1].handler == 0x217b124ULL);
  QCHECK(pinned::kEquipmentSetters[2].vaddr == 0x217cf90ULL && pinned::kEquipmentSetters[2].handler == 0x217b1dcULL);
  QCHECK(pinned::kNetGameMTVaddr == 0x12027d4ULL && pinned::kUserIdVaddr == 0x125a9b8ULL);
  QCHECK(pinned::kEquipmentRefireCodeCount == 16);
  const sentinel::GotTarget jump = pinned::LibR15EquipmentSetter(1, sentinel::RelocKind::kJumpSlot);
  const sentinel::GotTarget glob = pinned::LibR15EquipmentSetter(1, sentinel::RelocKind::kGlobDat);
  QCHECK(jump.kind == sentinel::RelocKind::kJumpSlot && glob.kind == sentinel::RelocKind::kGlobDat);
  QCHECK(jump.slotVaddr.has_value() && *jump.slotVaddr == 0x36fa900ULL);
  QCHECK(glob.slotVaddr.has_value() && *glob.slotVaddr == 0x3727b38ULL);
  QCHECK(std::strcmp(jump.symbol, glob.symbol) == 0);
  QCHECK(std::strstr(jump.symbol, "SetAbilitySlot") != nullptr);
}

// All six words take their thunk, the line says so, and the frame work is registered with the dispatcher.
void AnInstallPutsTheThunksInAllSixWords() {
  Reset();
  BuildImage(ImageOptions{});
  for (int slot = 0; slot < 3; ++slot) {
    for (int word = 0; word < 2; ++word) QCHECK(*SlotPtr(slot, word) == StockWord(slot));
  }
  QCHECK(eq::InstallAt(&Lookup));
  for (int slot = 0; slot < 3; ++slot) {
    for (int word = 0; word < 2; ++word) QCHECK(IsEntry(slot, word));
  }
  QCHECK(OneLine("combat_equip_refire_install") ==
         "\"event\":\"combat_equip_refire_install\",\"result\":\"installed\",\"weapon_jump\":\"ok\",\"weapon_glob\":\"ok\","
         "\"tac_mod_jump\":\"ok\",\"tac_mod_glob\":\"ok\",\"ordnance_jump\":\"ok\",\"ordnance_glob\":\"ok\",\"settle_ms\":2500,"
         "\"consequence\":\"a slot the local player changed is applied again after it has been still for settle_ms\"}");
  QCHECK(LinesOf("combat_equip_refire_install")[0].find("\"level\":\"info\"") != std::string::npos);
  frame::Tick();
  QCHECK(frame::CurrentStats().ran == 1);  // the frame work is registered, once
  Finish();
}

// Three changes within 300 ms through the installed words, both kinds, then frames: nothing before the settle time, exactly one
// handler call at it, for the last handle, and none after.
void ThreeChangesBecomeOneHandlerCallAfterTheSettleTime() {
  QCHECK(InstallFresh());
  g_lines.clear();
  CallWord(0, 0, Comp(), kLocalHandle, 0x11);  // t = 1000, the JUMP_SLOT word
  g_now += 100;
  CallWord(0, 1, Comp(), kLocalHandle, 0x22);  // t = 1100, the GLOB_DAT word
  g_now += 200;
  CallWord(0, 0, Comp(), kLocalHandle, 0x33);  // t = 1300
  QCHECK(eq::CurrentCounts().tracked == 3);
  QCHECK(ValueOf(kLocalHandle, 0) == 0x33);
  Frame(2499);  // 3799: still 1 ms short
  QCHECK(g_handlerCalls[0] == 0 && g_handlerCalls[1] == 0 && g_handlerCalls[2] == 0);
  Frame(1);  // 3800: 2500 ms after the last change
  QCHECK(g_handlerCalls[0] == 1 && g_handlerCalls[1] == 0 && g_handlerCalls[2] == 0);
  QCHECK(g_handlerComponent[0] == Comp());
  QCHECK(g_handlerHandle[0] == kLocalHandle);
  Frame(10000);
  Frame(10000);
  QCHECK(g_handlerCalls[0] == 1);  // once per quiet period
  QCHECK(eq::CurrentCounts().refired == 1 && eq::CurrentCounts().skipped == 0);
  const std::vector<std::string> refired = LinesOf("combat_equip_refire_refired");
  QCHECK(refired.size() == 1);
  if (refired.size() == 1) {
    QCHECK(Golden(refired[0]) ==
           "\"event\":\"combat_equip_refire_refired\",\"msg\":\"equipment re-fired (the game's change handler ran once more so "
           "the slot's scripts see the last choice)\",\"slot\":\"weapon\",\"value\":\"0x0000000000000033\","
           "\"handle\":\"0x00050001\",\"changes\":3,\"quiet_ms\":2500}");
    QCHECK(refired[0].find("\"level\":\"info\"") != std::string::npos);
  }
  Finish();
}

// A later change of another record of the same slot replaces the earlier one: the handler gets the last handle.
void TheHandlerGetsTheLastHandle() {
  QCHECK(InstallFresh());
  CallWord(1, 0, Comp(), kLocalHandle, 0x44);
  g_now += 50;
  CallWord(1, 1, Comp(), kLocalHandle2, 0x55);
  Frame(2500);
  QCHECK(g_handlerCalls[1] == 1);
  QCHECK(g_handlerHandle[1] == kLocalHandle2);
  Finish();
}

// Another player's record is not tracked and never re-fired.
void ARemoteRecordIsNeverRefired() {
  QCHECK(InstallFresh());
  g_lines.clear();
  CallWord(0, 0, Comp(), kRemoteHandle, 0x66);
  CallWord(2, 1, Comp(), kRemoteHandle, 0x77);
  QCHECK(ValueOf(kRemoteHandle, 0) == 0x66);  // the game's setter still ran
  Frame(60000);
  QCHECK(g_handlerCalls[0] == 0 && g_handlerCalls[1] == 0 && g_handlerCalls[2] == 0);
  QCHECK(eq::CurrentCounts().notLocal == 2 && eq::CurrentCounts().tracked == 0);
  const std::vector<std::string> lines = LinesOf("combat_equip_refire_change");
  QCHECK(lines.size() == 2);
  if (!lines.empty()) {
    QCHECK(Golden(lines[0]) ==
           "\"event\":\"combat_equip_refire_change\",\"msg\":\"equipment change\",\"slot\":\"weapon\",\"word\":\"jump_slot\",\"verdict\":\"not_local\","
           "\"handle\":\"0x00060002\",\"value\":\"0x0000000000000066\",\"read\":\"yes\",\"changed\":\"yes\",\"local\":\"no\"}");
  }
  Finish();
}

// A setter given the value the record already holds changes nothing, so nothing is tracked.
void AnUnchangedValueIsNotTracked() {
  QCHECK(InstallFresh());
  SetValue(kLocalHandle, 0, 0x88);
  g_lines.clear();
  CallWord(0, 1, Comp(), kLocalHandle, 0x88);
  QCHECK(g_setterCalls[0][1].count == 1);
  QCHECK(eq::CurrentCounts().unchanged == 1 && eq::CurrentCounts().tracked == 0);
  Frame(60000);
  QCHECK(g_handlerCalls[0] == 0);
  QCHECK(OneLine("combat_equip_refire_change") ==
         "\"event\":\"combat_equip_refire_change\",\"msg\":\"equipment change\",\"slot\":\"weapon\",\"word\":\"glob_dat\",\"verdict\":\"unchanged\","
         "\"handle\":\"0x00050001\",\"value\":\"0x0000000000000088\",\"read\":\"yes\",\"changed\":\"no\",\"local\":\"no\"}");
  QCHECK(LinesOf("combat_equip_refire_change")[0].find("\"level\":\"debug\"") != std::string::npos);
  Finish();
}

// Each word calls the original it stored, with the game's three arguments unchanged, and no other.
void EachWordCallsItsOwnStoredOriginalWithTheGamesArguments() {
  QCHECK(InstallFresh());
  std::uint64_t value = 0x100;
  for (int slot = 0; slot < 3; ++slot) {
    for (int word = 0; word < 2; ++word) {
      std::memset(g_setterCalls, 0, sizeof(g_setterCalls));
      ++value;
      CallWord(slot, word, Comp(), kLocalHandle, value);
      for (int s = 0; s < 3; ++s) {
        for (int w = 0; w < 2; ++w) QCHECK(g_setterCalls[s][w].count == (s == slot && w == word ? 1 : 0));
      }
      const SetterCall& call = g_setterCalls[slot][word];
      QCHECK(call.component == Comp());
      QCHECK(call.handle == kLocalHandle);
      QCHECK(call.value == value);
      QCHECK(ValueOf(kLocalHandle, slot) == value);
    }
  }
  Finish();
}

// The three slots call their own handler, in slot order, when all three are due together.
void TheThreeSlotsCallTheirOwnHandler() {
  QCHECK(InstallFresh());
  CallWord(2, 1, Comp(), kLocalHandle, 0xa3);
  CallWord(0, 0, Comp(), kLocalHandle, 0xa1);
  CallWord(1, 0, Comp(), kLocalHandle, 0xa2);
  Frame(2500);
  QCHECK(g_handlerCalls[0] == 1 && g_handlerCalls[1] == 1 && g_handlerCalls[2] == 1);
  QCHECK(g_handlerOrder.size() == 3);
  if (g_handlerOrder.size() == 3) QCHECK(g_handlerOrder[0] == 0 && g_handlerOrder[1] == 1 && g_handlerOrder[2] == 2);
  for (int slot = 0; slot < 3; ++slot) {
    QCHECK(g_handlerComponent[slot] == Comp());
    QCHECK(g_handlerHandle[slot] == kLocalHandle);
  }
  Finish();
}

// A record changed by something other than the setter during the wait is left as the game made it, and the line says why.
void AChangeUnderTheWaitIsSkippedAndSaid() {
  QCHECK(InstallFresh());
  CallWord(0, 0, Comp(), kLocalHandle, 0x91);
  SetValue(kLocalHandle, 0, 0x92);  // not through a setter
  g_lines.clear();
  Frame(2500);
  QCHECK(g_handlerCalls[0] == 0);
  QCHECK(eq::CurrentCounts().skipped == 1 && eq::CurrentCounts().refired == 0);
  QCHECK(OneLine("combat_equip_refire_skipped") ==
         "\"event\":\"combat_equip_refire_skipped\",\"msg\":\"equipment re-fire skipped (the change is left as the game made "
         "it)\",\"slot\":\"weapon\",\"value\":\"0x0000000000000091\",\"handle\":\"0x00050001\","
         "\"reason\":\"record_changed_elsewhere\"}");
  Finish();
}

// The probe reads where the game's getter reads: the net game argument ends 8 bytes after the field, so a probe one word further is
// a guard page. A wrong field offset makes every change read as not local.
void TheNetGameProbeReadsTheGettersField() {
  QCHECK(InstallFresh());
  CallWord(0, 0, Comp(), kLocalHandle, 0xb1);
  QCHECK(eq::CurrentCounts().tracked == 1 && eq::CurrentCounts().notLocal == 0);
  Finish();
}

// The local player is the net game's slot whose key the game's own lookup returns: a record whose actor the lookup maps to another
// slot is not local, whatever else it holds.
void TheLocalPlayerIsWhatUserIdSays() {
  QCHECK(InstallFresh());
  Put<std::uint64_t>(g_links, 0 * 0x10, kRemoteKey1);  // the local record's actor is now another player's
  CallWord(0, 0, Comp(), kLocalHandle, 0xc1);
  QCHECK(eq::CurrentCounts().notLocal == 1 && eq::CurrentCounts().tracked == 0);
  Put<std::uint64_t>(g_links, 0 * 0x10, kLocalKey1);
  QCHECK(mprotect(g_ctx, kPageBytes, PROT_READ | PROT_WRITE) == 0);
  Put<std::uint64_t>(g_ctx, kCtxActorKey, 0x1111111111111111ULL);  // and now the first half of the key differs
  QCHECK(mprotect(g_ctx, kPageBytes, PROT_READ) == 0);
  CallWord(0, 0, Comp(), kLocalHandle, 0xc2);
  QCHECK(eq::CurrentCounts().notLocal == 2 && eq::CurrentCounts().tracked == 0);
  Finish();
}

// With no stand-ins the module calls base + the pinned addresses: the three handlers, NetGameMT and UserId, as the store file has them.
void TheProductionAddressesAreBasePlusThePinnedVaddrs() {
  Reset();
  eq::SetGameFunctionsForTest(nullptr);
  BuildImage(ImageOptions{});
  QCHECK(eq::InstallAt(&Lookup));
  const eq::GameFunctions fns = eq::PublishedGameFunctions();
  QCHECK(fns.handler[0] == Base() + 0x217afb4ULL);  // HandleWeaponChange
  QCHECK(fns.handler[1] == Base() + 0x217b124ULL);  // HandleAbilityChange
  QCHECK(fns.handler[2] == Base() + 0x217b1dcULL);  // HandleGrenadeChange
  QCHECK(fns.netGame == Base() + 0x12027d4ULL);     // NetGameMT
  QCHECK(fns.userId == Base() + 0x125a9b8ULL);      // CR15NetGame::UserId
  Finish();
  QCHECK(eq::PublishedGameFunctions().netGame == 0);  // Finish forgot them
}

// A second install over a module that is already installed refuses every word (each is already ours), registers no second frame work
// and leaves the working handlers on.
void ASecondInstallLeavesTheFirstWorking() {
  QCHECK(InstallFresh());
  QCHECK(!eq::InstallAt(&Lookup));
  const std::vector<std::string> lines = LinesOf("combat_equip_refire_install");
  QCHECK(lines.size() == 2);
  if (lines.size() == 2) {
    QCHECK(Golden(lines[1]).find("\"result\":\"refused\",\"weapon_jump\":\"already_installed\"") != std::string::npos);
  }
  for (int slot = 0; slot < 3; ++slot) {
    for (int word = 0; word < 2; ++word) QCHECK(IsEntry(slot, word));
  }
  CallWord(2, 1, Comp(), kLocalHandle, 0xf1);
  QCHECK(eq::CurrentCounts().tracked == 1);
  frame::Tick();
  QCHECK(frame::CurrentStats().ran == 1);  // one frame work, not two
  Frame(2500);
  QCHECK(g_handlerCalls[2] == 1);
  Finish();
}

// A word that does not hold the setter is refused on its own: the other five take their thunks, the refused word keeps what the
// other runtime put there, and its handler is off, so a call that reaches the entry anyway is the game's own.
void AWordThatIsNotTheSetterIsRefusedAlone() {
  ImageOptions opt;
  opt.foreignWord = reinterpret_cast<const void*>(&ModelForeign);
  QCHECK(!InstallFresh(opt));
  QCHECK(*SlotPtr(1, 1) == reinterpret_cast<void*>(&ModelForeign));
  QCHECK(IsEntry(0, 0) && IsEntry(0, 1) && IsEntry(1, 0) && IsEntry(2, 0) && IsEntry(2, 1));
  QCHECK(!IsEntry(1, 1));
  QCHECK(OneLine("combat_equip_refire_install") ==
         "\"event\":\"combat_equip_refire_install\",\"result\":\"partial\",\"weapon_jump\":\"ok\",\"weapon_glob\":\"ok\","
         "\"tac_mod_jump\":\"ok\",\"tac_mod_glob\":\"original_mismatch\",\"ordnance_jump\":\"ok\",\"ordnance_glob\":\"ok\","
         "\"settle_ms\":2500,\"consequence\":\"a change that reaches a refused word is not seen; the others are applied again "
         "after settle_ms\"}");
  QCHECK(LinesOf("combat_equip_refire_install")[0].find("\"level\":\"error\"") != std::string::npos);
  // The refused word's thunk is disarmed: its entry passes straight to the original and tracks nothing.
  g_lines.clear();
  *pinned::LibR15EquipmentSetterThunk<1, 1>::OriginalOut() = reinterpret_cast<void*>(&ModelSetter<1, 1>);
  pinned::LibR15EquipmentSetterThunk<1, 1>::EntryFn()(Comp(), kLocalHandle, 0xd1);
  QCHECK(g_setterCalls[1][1].count == 1);
  QCHECK(eq::CurrentCounts().tracked == 0 && LinesOf("combat_equip_refire_change").empty());
  // A word that did take its thunk still tracks.
  CallWord(1, 0, Comp(), kLocalHandle, 0xd2);
  QCHECK(eq::CurrentCounts().tracked == 1);
  Frame(2500);
  QCHECK(g_handlerCalls[1] == 1);
  Finish();
}

// A pin that differs refuses the module before any word is touched: no slot changes, the frame work is not registered, no handler
// is armed, and the line names the pin. Each pin is tried on its first and on its last compared word.
void APinMismatchRefusesTheModuleAndSwapsNothing() {
  for (std::size_t i = 0; i < pinned::kEquipmentRefireCodeCount; ++i) {
    for (const bool last : {true, false}) {
      ImageOptions opt;
      opt.flippedPin = static_cast<int>(i);
      opt.flipLast = last;
      QCHECK(!InstallFresh(opt));
      for (int slot = 0; slot < 3; ++slot) {
        for (int word = 0; word < 2; ++word) QCHECK(*SlotPtr(slot, word) == StockWord(slot));
      }
      const std::vector<std::string> lines = LinesOf("combat_equip_refire_install");
      QCHECK(lines.size() == 1);
      if (lines.size() == 1) {
        QCHECK(Golden(lines[0]) == std::string("\"event\":\"combat_equip_refire_install\",\"result\":\"refused\","
                                               "\"why\":\"pin_mismatch\",\"pin\":\"") +
                                       pinned::kEquipmentRefireCode[i].name +
                                       "\",\"consequence\":\"an equipment change the scripts drop stays dropped\"}");
        QCHECK(lines[0].find("\"level\":\"error\"") != std::string::npos);
      }
      frame::Tick();
      QCHECK(frame::CurrentStats().ran == 0);  // nothing was registered
      // No handler is armed: the entry is the game's setter and nothing else.
      pinned::LibR15EquipmentSetterThunk<0, 0>::EntryFn()(Comp(), kLocalHandle, 0xe1);
      QCHECK(eq::CurrentCounts().tracked == 0 && eq::CurrentCounts().unchanged == 0 && eq::CurrentCounts().notLocal == 0);
      Finish();
    }
  }
}

// Every other way the install is refused, the line says why and nothing changes.
void TheOtherRefusalsSayWhy() {
  const char* consequence = "\"consequence\":\"an equipment change the scripts drop stays dropped\"}";
  {  // libr15 is not loaded
    Reset();
    QCHECK(!eq::InstallAt(&NoLibr15));
    QCHECK(OneLine("combat_equip_refire_install") ==
           std::string("\"event\":\"combat_equip_refire_install\",\"result\":\"refused\",\"why\":\"module_not_loaded\",") + consequence);
    Finish();
  }
  {  // a libr15 of another build
    ImageOptions opt;
    opt.buildId = "0123456789abcdef0123456789abcdef01234567";
    QCHECK(!InstallFresh(opt));
    QCHECK(OneLine("combat_equip_refire_install") ==
           std::string("\"event\":\"combat_equip_refire_install\",\"result\":\"refused\",\"why\":\"build_id_mismatch\",") + consequence);
    for (int slot = 0; slot < 3; ++slot) QCHECK(*SlotPtr(slot, 0) == StockWord(slot) && *SlotPtr(slot, 1) == StockWord(slot));
    Finish();
  }
  {  // an image with no build ID
    ImageOptions opt;
    opt.note = false;
    QCHECK(!InstallFresh(opt));
    QCHECK(OneLine("combat_equip_refire_install") ==
           std::string("\"event\":\"combat_equip_refire_install\",\"result\":\"refused\",\"why\":\"no_build_id\",") + consequence);
    Finish();
  }
  {  // the dispatcher has no room for the frame work: a re-fire could never run, so nothing is installed
    Reset();
    BuildImage(ImageOptions{});
    for (std::size_t i = 0; i < frame::kMaxFrameWork; ++i) QCHECK(frame::RegisterFrameWork([](void*) noexcept { return true; }, nullptr));
    QCHECK(!eq::InstallAt(&Lookup));
    QCHECK(OneLine("combat_equip_refire_install") ==
           std::string("\"event\":\"combat_equip_refire_install\",\"result\":\"refused\",\"why\":\"frame_work_full\",") + consequence);
    for (int slot = 0; slot < 3; ++slot) QCHECK(*SlotPtr(slot, 0) == StockWord(slot) && *SlotPtr(slot, 1) == StockWord(slot));
    Finish();
  }
}

// The per-call record is written for the first kChangeLines calls of the process, then one warning says the rest are counted; the
// counts and the re-fires go on.
void ThePerCallRecordIsCappedAndTheCountsAreNot() {
  QCHECK(InstallFresh());
  g_lines.clear();
  SetValue(kLocalHandle, 0, 0x5);
  for (int i = 0; i < 300; ++i) CallWord(0, i % 2, Comp(), kLocalHandle, 0x5);  // unchanged: cheap, and it still counts
  QCHECK(LinesOf("combat_equip_refire_change").size() == eq::kChangeLines);
  QCHECK(OneLine("combat_equip_refire_change_cap") ==
         "\"event\":\"combat_equip_refire_change_cap\",\"cap\":256,\"consequence\":\"further setter calls are counted, not "
         "written; re-fires are unaffected\"}");
  QCHECK(LinesOf("combat_equip_refire_change_cap")[0].find("\"level\":\"warn\"") != std::string::npos);
  QCHECK(eq::CurrentCounts().unchanged == 300);
  CallWord(0, 0, Comp(), kLocalHandle, 0x6);  // a tracked change after the cap is still tracked and still re-fired
  QCHECK(eq::CurrentCounts().tracked == 1);
  QCHECK(LinesOf("combat_equip_refire_change").size() == eq::kChangeLines);
  Frame(2500);
  QCHECK(g_handlerCalls[0] == 1);
  Finish();
}

}  // namespace

int main() {
  sentinel::SetLogSink(&CaptureLine);
  TheLayoutConstantsAreTheStoreLibr15s();
  AnInstallPutsTheThunksInAllSixWords();
  ThreeChangesBecomeOneHandlerCallAfterTheSettleTime();
  TheHandlerGetsTheLastHandle();
  ARemoteRecordIsNeverRefired();
  AnUnchangedValueIsNotTracked();
  EachWordCallsItsOwnStoredOriginalWithTheGamesArguments();
  TheThreeSlotsCallTheirOwnHandler();
  AChangeUnderTheWaitIsSkippedAndSaid();
  TheNetGameProbeReadsTheGettersField();
  TheLocalPlayerIsWhatUserIdSays();
  TheProductionAddressesAreBasePlusThePinnedVaddrs();
  ASecondInstallLeavesTheFirstWorking();
  AWordThatIsNotTheSetterIsRefusedAlone();
  APinMismatchRefusesTheModuleAndSwapsNothing();
  TheOtherRefusalsSayWhy();
  ThePerCallRecordIsCappedAndTheCountsAreNot();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "equip_refire_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("equip_refire_test: ok\n");
  return 0;
}
