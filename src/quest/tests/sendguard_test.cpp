// Host test of sendguard on Quest (#2179, #1603): the real wrapper (src/quest/combat/sendguard.cpp), built without exceptions as the sentinel
// builds it, installed by the real GotHook into the data word of a libr15 image built in memory, and called the way the game calls it:
// through the word. The game side is a model: CSendEventNode::Enter (what the stock function does with a send that reaches it), the
// GameSpaceMT lookup (loaded, or not), and the node's execution context laid out where the game's code reads it (the offsets are
// spelled in this file, not taken from the module, so a wrong constant there fails here).
//
// The image holds, as the store file does, the one JUMP_SLOT of Enter at ELF 0x36f7928, and the eight bytes of Enter's unwind record at
// ELF 0x356a6b0 (`d0b31bff dc010000`, inside `.eh_frame`), which the q15t plugin took for a second slot. The test shows that the install
// changes the one word and no other byte of the image, and says nothing about the unwind record.
//
// What this test does not do: run Enter, or the lookup, in the game. got_pinned_test.cpp resolves the relocation and the ten code pins in
// the real libr15 (it needs the store APK).
//
// Built and run by `just test-quest-combat`.

#include <elf.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "quest/combat/combat_install.h"
#include "quest/combat/combat_active.h"
#include "quest/combat/sendguard.h"
#include "quest/tests/test_check.h"

// The sentinel's files folder, which the module's production path reads through sentinel::FilesDir(): the test names a folder of its own.
static std::string g_filesDir;
namespace sentinel {
const char* FilesDir() { return g_filesDir.c_str(); }
}  // namespace sentinel

namespace {

namespace sg = nevr_quest::combat::sendguard;
namespace combat = nevr_quest::combat;
namespace pinned = sentinel::pinned;

// ---- the layout, as this test spells it ---------------------------------------------------------------------------------------

constexpr std::size_t kImageSize = 0x3700000;
constexpr std::uint64_t kEnter = 0x2725a80;
constexpr std::uint64_t kSlot = 0x36f7928;     // R_AARCH64_JUMP_SLOT, symbol value 0x2725a80
constexpr std::uint64_t kUnwind = 0x356a6b0;   // inside .eh_frame in the store file: not a pointer
constexpr unsigned char kUnwindBytes[8] = {0xd0, 0xb3, 0x1b, 0xff, 0xdc, 0x01, 0x00, 0x00};
constexpr const char* kSymbol = "_ZN10NRadEngine14CSendEventNode5EnterERKNS_18SSendEventNodeDataE";

struct Word {
  std::uint64_t vaddr;
  std::uint32_t word;
};
// The ten pins, spelled here. The last is the GameSpaceMT stub's first word; its other three follow at +4, +8, +12.
constexpr Word kWords[] = {
    {0x2725a80, 0xd101c3ff}, {0x2725a90, 0xf9400008}, {0x2725a98, 0xf9400021}, {0x2725aa0, 0xf9400109}, {0x2725aa8, 0xf9401929},
    {0x2725aac, 0xf9400129}, {0x2725ab0, 0xf9404133}, {0x2725abc, 0x979f3ee5}, {0x2725b78, 0xf9402e69}, {0xef5650, 0xb0013ef0},
    {0xef5654, 0xf9410a11},  {0xef5658, 0x91084210},  {0xef565c, 0xd61f0220},
};
constexpr std::size_t kWordCount = sizeof(kWords) / sizeof(kWords[0]);

constexpr std::uint64_t kNoLevel = ~0ULL;

// ---- the image ----------------------------------------------------------------------------------------------------------------

constexpr std::uint64_t kNoteAt = 0x100;
constexpr std::uint64_t kDynstrAt = 0x400;
constexpr std::uint64_t kDynsymAt = 0x1000;
constexpr std::uint64_t kJmprelAt = 0x1400;
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
  int flippedWord = -1;           // index into kWords whose word differs
  const void* foreignWord = nullptr;  // put into the slot instead of Enter
};

std::uintptr_t Base() { return g_image.image.base; }
void** SlotPtr() { return reinterpret_cast<void**>(Base() + kSlot); }
void* StockWord() { return reinterpret_cast<void*>(Base() + kEnter); }

template <typename T>
void Put(unsigned char* base, std::size_t offset, T value) {
  std::memcpy(base + offset, &value, sizeof(value));
}

void BuildImage(const ImageOptions& opt) {
  void* mem = mmap(nullptr, kImageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  QCHECK(mem != MAP_FAILED);
  g_image.mem = static_cast<unsigned char*>(mem);
  unsigned char* const m = g_image.mem;

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

  // .dynstr, .dynsym: Enter's real mangled name, defined at its address.
  std::string dynstr(1, '\0');
  Elf64_Sym syms[2] = {};
  syms[1].st_name = static_cast<Elf64_Word>(dynstr.size());
  syms[1].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
  syms[1].st_shndx = 1;
  syms[1].st_value = kEnter;
  dynstr += kSymbol;
  dynstr.push_back('\0');
  std::memcpy(m + kDynstrAt, dynstr.data(), dynstr.size());
  std::memcpy(m + kDynsymAt, syms, sizeof(syms));

  // The one relocation: the JUMP_SLOT.
  Elf64_Rela jmprel[1] = {};
  jmprel[0].r_offset = kSlot;
  jmprel[0].r_info = ELF64_R_INFO(static_cast<Elf64_Xword>(1), sentinel::kNativeRelocs.jumpSlot);
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

  // The words the module proves before it touches the slot, the unwind record, and the slot holding Enter.
  for (std::size_t i = 0; i < kWordCount; ++i) {
    std::uint32_t w = kWords[i].word;
    if (static_cast<int>(i) == opt.flippedWord) w ^= 0x00000020U;
    Put<std::uint32_t>(m, kWords[i].vaddr, w);
  }
  std::memcpy(m + kUnwind, kUnwindBytes, sizeof(kUnwindBytes));
  g_image.image.base = reinterpret_cast<std::uintptr_t>(m);
  *SlotPtr() = opt.foreignWord != nullptr ? const_cast<void*>(opt.foreignWord) : StockWord();

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

// ---- the log ------------------------------------------------------------------------------------------------------------------

struct Line {
  sentinel::LogLevel level;
  std::string text;
};
std::vector<Line> g_lines;
void CaptureLine(sentinel::LogLevel level, const char* line) { g_lines.push_back({level, line}); }

std::string Golden(const std::string& line) {
  const std::size_t at = line.find("\"event\"");
  return at == std::string::npos ? line : line.substr(at);
}
std::vector<Line> LinesOf(const char* event) {
  std::vector<Line> out;
  for (const Line& l : g_lines) {
    if (l.text.find(std::string("\"event\":\"") + event + "\",") != std::string::npos) out.push_back(l);
  }
  return out;
}
std::string OneLine(const char* event) {
  const std::vector<Line> lines = LinesOf(event);
  QCHECK(lines.size() == 1);
  return lines.empty() ? std::string() : Golden(lines[0].text);
}
bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }
// The lines that are not at Info or Debug: what an operator reading the log at its default level would see as a problem.
std::size_t ProblemLines() {
  std::size_t n = 0;
  for (const Line& l : g_lines) n += (l.level == sentinel::LogLevel::kWarn || l.level == sentinel::LogLevel::kError) ? 1 : 0;
  return n;
}

// ---- the game: Enter and the lookup, as models ---------------------------------------------------------------------------------

int g_enterCalls = 0;
void* g_enterContext = nullptr;
const void* g_enterData = nullptr;
void ModelEnter(void* context, const void* data) {
  ++g_enterCalls;
  g_enterContext = context;
  g_enterData = data;
}

int g_lookupCalls = 0;
void* g_lookupSpace = nullptr;
std::uint64_t g_lookupLevel = 0;
void* g_lookupAnswer = nullptr;  // null: the named game space is not loaded
void* ModelLookup(void* space, std::uint64_t level) {
  ++g_lookupCalls;
  g_lookupSpace = space;
  g_lookupLevel = level;
  return g_lookupAnswer;
}

// The node's execution context: ctx -> [0] -> [0] -> [+0x30] -> [0] -> [+0x80] = the current game space.
void* const kSpace = reinterpret_cast<void*>(0x5150);
std::uintptr_t g_a[2], g_b[8], g_c[2], g_d[0x20], g_ctx[2];
void BuildContext() {
  std::memset(g_a, 0, sizeof(g_a));
  std::memset(g_b, 0, sizeof(g_b));
  std::memset(g_c, 0, sizeof(g_c));
  std::memset(g_d, 0, sizeof(g_d));
  std::memset(g_ctx, 0, sizeof(g_ctx));
  g_ctx[0] = reinterpret_cast<std::uintptr_t>(g_a);
  g_a[0] = reinterpret_cast<std::uintptr_t>(g_b);
  g_b[0x30 / 8] = reinterpret_cast<std::uintptr_t>(g_c);
  g_c[0] = reinterpret_cast<std::uintptr_t>(g_d);
  g_d[0x80 / 8] = reinterpret_cast<std::uintptr_t>(kSpace);
}

using EnterFn = sg::EnterSig;
void CallThroughSlot(void* context, const void* data) { reinterpret_cast<EnterFn*>(*SlotPtr())(context, data); }

void Reset() {
  sg::ResetForTest();
  sg::SetLookupForTest(&ModelLookup);
  g_lines.clear();
  g_enterCalls = g_lookupCalls = 0;
  g_enterContext = nullptr;
  g_enterData = nullptr;
  g_lookupSpace = nullptr;
  g_lookupLevel = 0;
  g_lookupAnswer = nullptr;
  BuildContext();
}

// An install on a fresh image; the thunk's original is the model of Enter (the install stored the stock word there; the model stands for
// the game's code, which a host test cannot run).
combat::ModuleResult InstallFresh(const ImageOptions& opt = ImageOptions{}, bool enabled = true, const char* legacyDir = nullptr) {
  Reset();
  BuildImage(opt);
  const combat::ModuleResult r = sg::InstallAt(&Lookup, enabled, legacyDir);
  *sg::EnterThunk::OriginalOut() = reinterpret_cast<void*>(&ModelEnter);
  return r;
}
void Finish() {
  sg::ResetForTest();
  DropImage();
}

// ---- tests ----------------------------------------------------------------------------------------------------------------------

void TheTargetsAreTheStoreLibr15s() {
  QCHECK(sg::kEnterVaddr == kEnter && sg::kEnterSlotVaddr == kSlot);
  QCHECK(std::strcmp(sg::kEnterSymbol, kSymbol) == 0);
  QCHECK(sg::kGameSpaceMtStubVaddr == 0xef5650ULL);
  QCHECK(sg::kDropLines == 8);
  const sentinel::GotTarget target = sg::EnterTarget();
  QCHECK(target.kind == sentinel::RelocKind::kJumpSlot);
  QCHECK(target.slotVaddr.has_value() && *target.slotVaddr == kSlot);
  QCHECK(std::strcmp(target.symbol, kSymbol) == 0);
  QCHECK(std::strcmp(target.module, pinned::kLibR15) == 0);
  QCHECK(std::strcmp(target.buildId, pinned::LibR15PatchedBuildId()) == 0);
  // The pins: the same thirteen words, in rows of the ten the module reads.
  QCHECK(sg::kCodeCount == 10);
  std::size_t words = 0;
  for (std::size_t i = 0; i < sg::kCodeCount; ++i) {
    for (std::size_t w = 0; w < sg::kCode[i].words; ++w) {
      QCHECK(sg::kCode[i].vaddr + 4 * w == kWords[words].vaddr);
      QCHECK(sg::kCode[i].code[w] == kWords[words].word);
      ++words;
    }
  }
  QCHECK(words == kWordCount);
  // The unwind record is not a target: no pin and no target names it.
  QCHECK(*target.slotVaddr != kUnwind);
}

// The install puts the thunk in the one JUMP_SLOT and changes no other byte of the image: the unwind record is as it was, nothing is
// refused, and the log has no Warning or Error.
void AnInstallPutsTheThunkInTheOneWordAndChangesNothingElse() {
  Reset();
  BuildImage(ImageOptions{});
  std::vector<unsigned char> before(g_image.mem, g_image.mem + kImageSize);
  QCHECK(*SlotPtr() == StockWord());
  const combat::ModuleResult r = sg::InstallAt(&Lookup, true, nullptr);
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0);
  QCHECK(*SlotPtr() == sg::EnterThunk::EntryAddress());
  std::vector<unsigned char> after(g_image.mem, g_image.mem + kImageSize);
  std::memcpy(before.data() + kSlot, after.data() + kSlot, 8);  // the one word that changes
  QCHECK(before == after);
  QCHECK(std::memcmp(g_image.mem + kUnwind, kUnwindBytes, sizeof(kUnwindBytes)) == 0);
  QCHECK(OneLine("combat_sendguard_install") ==
         "\"event\":\"combat_sendguard_install\",\"result\":\"installed\",\"hook\":\"ok\","
         "\"consequence\":\"a script send to a game space that is not loaded is dropped, not faulted\"}");
  QCHECK(LinesOf("combat_sendguard_install")[0].level == sentinel::LogLevel::kInfo);
  QCHECK(ProblemLines() == 0);  // the plugin's second slot was refused with a Warning on every launch
  Finish();
}

// A named level that is not loaded: the lookup answers null, the send is dropped, the stock function is not called, one Info line names the
// keys; the lookup was asked the space the context chain leads to and the level key.
void ASendToAnUnloadedGameSpaceIsDropped() {
  QCHECK(InstallFresh().installed);
  g_lines.clear();
  const std::uint64_t data[3] = {0x77, 0x88, 0x99};
  g_lookupAnswer = nullptr;
  CallThroughSlot(g_ctx, data);
  QCHECK(g_enterCalls == 0);
  QCHECK(g_lookupCalls == 1 && g_lookupSpace == kSpace && g_lookupLevel == 0x77);
  QCHECK(sg::CurrentStats().dropped == 1 && sg::CurrentStats().passed == 0);
  QCHECK(OneLine("combat_sendguard_dropped") ==
         "\"event\":\"combat_sendguard_dropped\",\"n\":1,\"level_key\":\"0x0000000000000077\",\"event_key\":\"0x0000000000000088\","
         "\"actor_key\":\"0x0000000000000099\",\"consequence\":\"the send was dropped: the game space the node names is not loaded "
         "(the stock function would fault, or report a missing actor, which is now not reported)\"}");
  QCHECK(LinesOf("combat_sendguard_dropped")[0].level == sentinel::LogLevel::kInfo);
  Finish();
}

// A level that is loaded: the stock function runs with the node's own two arguments, and the lookup was asked the same question.
void ASendToALoadedGameSpaceReachesTheStockFunction() {
  QCHECK(InstallFresh().installed);
  g_lines.clear();
  const std::uint64_t data[3] = {0x77, 0x88, 0x99};
  g_lookupAnswer = reinterpret_cast<void*>(0xbeef);
  CallThroughSlot(g_ctx, data);
  QCHECK(g_enterCalls == 1 && g_enterContext == g_ctx && g_enterData == data);
  QCHECK(g_lookupCalls == 1 && g_lookupSpace == kSpace && g_lookupLevel == 0x77);
  QCHECK(sg::CurrentStats().dropped == 0 && sg::CurrentStats().passed == 1);
  QCHECK(LinesOf("combat_sendguard_dropped").empty());
  Finish();
}

// No level named (-1: send within the current space): no lookup, the stock function runs.
void ASendWithNoLevelIsNotLookedUp() {
  QCHECK(InstallFresh().installed);
  const std::uint64_t data[3] = {kNoLevel, 1, 2};
  CallThroughSlot(g_ctx, data);
  QCHECK(g_lookupCalls == 0 && g_enterCalls == 1 && g_enterData == data);
  const std::uint64_t other[3] = {kNoLevel - 1, 1, 2};  // any other key is a level
  CallThroughSlot(g_ctx, other);
  QCHECK(g_lookupCalls == 1 && g_lookupLevel == kNoLevel - 1);
  Finish();
}

// A null link anywhere on the chain to the current space leaves the stock function to do what it does; so does a null context or a null node.
// The lookup is not asked.
void ANullOnTheChainLeavesTheSendToTheStockFunction() {
  QCHECK(InstallFresh().installed);
  const std::uint64_t data[3] = {0x77, 1, 2};
  g_c[0] = 0;
  CallThroughSlot(g_ctx, data);
  QCHECK(g_enterCalls == 1 && g_lookupCalls == 0);
  BuildContext();
  g_b[0x30 / 8] = 0;
  CallThroughSlot(g_ctx, data);
  QCHECK(g_enterCalls == 2 && g_lookupCalls == 0);
  BuildContext();
  g_a[0] = 0;
  CallThroughSlot(g_ctx, data);
  QCHECK(g_enterCalls == 3 && g_lookupCalls == 0);
  BuildContext();
  CallThroughSlot(nullptr, data);
  QCHECK(g_enterCalls == 4 && g_lookupCalls == 0);
  CallThroughSlot(g_ctx, nullptr);
  QCHECK(g_enterCalls == 5 && g_lookupCalls == 0);
  QCHECK(sg::CurrentStats().dropped == 0 && sg::CurrentStats().passed == 5);
  Finish();
}

// Only the first eight drops are written; every drop is counted, and each send after them is still dropped.
void OnlyTheFirstDropsAreWrittenAndEveryOneIsCounted() {
  QCHECK(InstallFresh().installed);
  g_lines.clear();
  const std::uint64_t data[3] = {0x77, 1, 2};
  for (int i = 0; i < 12; ++i) CallThroughSlot(g_ctx, data);
  QCHECK(g_enterCalls == 0);
  QCHECK(sg::CurrentStats().dropped == 12);
  QCHECK(LinesOf("combat_sendguard_dropped").size() == sg::kDropLines);
  QCHECK(Has(LinesOf("combat_sendguard_dropped").back().text, "\"n\":8,"));
  Finish();
}

// A pinned word that differs refuses the module before the slot is touched, with one Error line that names the pin.
void AWordThatDiffersRefusesTheModuleAndSwapsNothing() {
  for (std::size_t i = 0; i < kWordCount; ++i) {
    const combat::ModuleResult r = InstallFresh(ImageOptions{nullptr, true, static_cast<int>(i), nullptr});
    QCHECK(!r.installed && std::strcmp(r.status, "pin_mismatch") == 0);
    QCHECK(*SlotPtr() == StockWord());
    const std::vector<Line> lines = LinesOf("combat_sendguard_install");
    QCHECK(lines.size() == 1 && lines[0].level == sentinel::LogLevel::kError);
    QCHECK(lines.size() == 1 && Has(lines[0].text, "\"result\":\"refused\",\"why\":\"pin_mismatch\",\"pin\":\"Enter: ") == (i < 9));
    QCHECK(lines.size() == 1 && Has(lines[0].text, "\"consequence\":\"a script send to a game space that is not loaded still faults the game\""));
    Finish();
  }
  // The stub's rows name the stub.
  InstallFresh(ImageOptions{nullptr, true, 12, nullptr});
  QCHECK(Has(OneLine("combat_sendguard_install"), "\"pin\":\"GameSpaceMT stub\""));
  Finish();
}

// The slot holds something else than Enter (another runtime's word): refused, the word is left, the Error line says so, and nothing the
// wrapper does runs.
void AWordThatIsNotEnterIsRefused() {
  const combat::ModuleResult r = InstallFresh(ImageOptions{nullptr, true, -1, reinterpret_cast<const void*>(&ModelEnter)});
  QCHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0);
  QCHECK(*SlotPtr() == reinterpret_cast<void*>(&ModelEnter));
  QCHECK(Has(OneLine("combat_sendguard_install"), "\"result\":\"refused\",\"hook\":\"original_mismatch\""));
  QCHECK(LinesOf("combat_sendguard_install")[0].level == sentinel::LogLevel::kError);
  // The refused slot leaves the handler off: the thunk, called by itself, goes straight to the original and does not guard.
  const std::uint64_t data[3] = {0x77, 1, 2};
  g_lookupAnswer = nullptr;
  reinterpret_cast<EnterFn*>(sg::EnterThunk::EntryAddress())(g_ctx, data);
  QCHECK(g_enterCalls == 1 && g_lookupCalls == 0 && sg::CurrentStats().dropped == 0);
  Finish();
}

// The other ways the install refuses say why, at Error, and touch nothing.
void TheOtherRefusalsSayWhy() {
  {
    Reset();
    BuildImage(ImageOptions{});
    const combat::ModuleResult r = sg::InstallAt(&NoLibr15, true, nullptr);
    QCHECK(!r.installed && std::strcmp(r.status, "module_not_loaded") == 0);
    QCHECK(Has(OneLine("combat_sendguard_install"), "\"result\":\"refused\",\"why\":\"module_not_loaded\",\"consequence\""));
    QCHECK(*SlotPtr() == StockWord());
    Finish();
  }
  {
    const combat::ModuleResult r = InstallFresh(ImageOptions{nullptr, false, -1, nullptr});
    QCHECK(!r.installed && std::strcmp(r.status, "no_build_id") == 0);
    QCHECK(Has(OneLine("combat_sendguard_install"), "\"why\":\"no_build_id\""));
    QCHECK(*SlotPtr() == StockWord());
    Finish();
  }
  {
    const combat::ModuleResult r = InstallFresh(ImageOptions{"0000000000000000000000000000000000000000", true, -1, nullptr});
    QCHECK(!r.installed && std::strcmp(r.status, "build_id_mismatch") == 0);
    QCHECK(Has(OneLine("combat_sendguard_install"), "\"why\":\"build_id_mismatch\""));
    QCHECK(*SlotPtr() == StockWord());
    Finish();
  }
  {
    Reset();
    const combat::ModuleResult r = sg::InstallAt(nullptr, true, nullptr);
    QCHECK(!r.installed && std::strcmp(r.status, "module_not_loaded") == 0);
    Finish();
  }
}

// combat.sendguard false: the module is off, says so at Info with the reason, reports nothing wrong, and touches nothing. The Info line
// and the status are what the sendguard row of the combat_install line shows.
void TheSwitchTurnsTheModuleOff() {
  const combat::ModuleResult r = InstallFresh(ImageOptions{}, /*enabled=*/false);
  QCHECK(!r.installed && r.off && std::strcmp(r.status, "off_config") == 0);  // off is its own answer: not installed, not failed
  QCHECK(*SlotPtr() == StockWord());
  QCHECK(OneLine("combat_sendguard_install") ==
         "\"event\":\"combat_sendguard_install\",\"result\":\"off\",\"reason\":\"off_config\","
         "\"consequence\":\"combat.sendguard is false: a send to a game space that is not loaded reaches the stock function\"}");
  QCHECK(LinesOf("combat_sendguard_install")[0].level == sentinel::LogLevel::kInfo);
  QCHECK(ProblemLines() == 0);
  Finish();
}

// The plugin's switch file is not read: with it present the module installs, and one Warning names the key; with the module off by
// its key, the file changes nothing and the Warning is still written.
void TheOldSwitchFileChangesNothingAndIsToldSo() {
  char tmpl[] = "/tmp/nevr-sendguard-XXXXXX";
  QCHECK(mkdtemp(tmpl) != nullptr);
  const std::string dir = tmpl;
  const std::string file = dir + "/sendguard.off";
  QCHECK(InstallFresh(ImageOptions{}, true, dir.c_str()).installed);
  QCHECK(LinesOf("combat_sendguard_legacy_switch").empty());  // no file, no line
  Finish();
  FILE* f = std::fopen(file.c_str(), "w");
  QCHECK(f != nullptr);
  if (f != nullptr) std::fclose(f);
  const combat::ModuleResult r = InstallFresh(ImageOptions{}, true, dir.c_str());
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0);
  QCHECK(*SlotPtr() == sg::EnterThunk::EntryAddress());  // the file did not turn the module off
  QCHECK(OneLine("combat_sendguard_legacy_switch") ==
         "\"event\":\"combat_sendguard_legacy_switch\",\"key\":\"combat.sendguard\",\"file\":\"sendguard.off\","
         "\"consequence\":\"the file is no longer read; set combat.sendguard: false in config.yaml to turn the module off\"}");
  QCHECK(LinesOf("combat_sendguard_legacy_switch")[0].level == sentinel::LogLevel::kWarn);
  Finish();
  const combat::ModuleResult off = InstallFresh(ImageOptions{}, false, dir.c_str());
  QCHECK(off.off && std::strcmp(off.status, "off_config") == 0 && LinesOf("combat_sendguard_legacy_switch").size() == 1);
  Finish();
  unlink(file.c_str());
  rmdir(dir.c_str());
}

// The table's row: on, it installs (here: finds no libr15 on the host and says so); off, it answers off. The table reading the resolved
// switch is combat_install_test's.
void TheTablesRowFollowsItsSwitch() {
  Reset();
  combat::ModuleResult r = sg::Install(true);
  QCHECK(!r.installed && !r.off && std::strcmp(r.status, "module_not_loaded") == 0);
  g_lines.clear();
  r = sg::Install(false);
  QCHECK(r.off && !r.installed && std::strcmp(r.status, "off_config") == 0);
  Finish();
}

// The counts: Debug every kStatsEvery passes and every kStatsDropEvery drops after the eighth, Info once at the end of the process, and nothing
// at the end when the wrapper is not installed.
void TheCountsAreWrittenSoAHeadsetRunGivesTheRate() {
  QCHECK(InstallFresh().installed);
  g_lines.clear();
  const std::uint64_t pass[3] = {kNoLevel, 1, 2};
  for (unsigned i = 0; i < sg::kStatsEvery - 1; ++i) CallThroughSlot(g_ctx, pass);
  QCHECK(LinesOf("combat_sendguard_stats").empty());
  CallThroughSlot(g_ctx, pass);
  QCHECK(OneLine("combat_sendguard_stats") ==
         "\"event\":\"combat_sendguard_stats\",\"passed\":16384,\"dropped\":0,\"final\":0,\"consequence\":\"passed is the sends that reached "
         "the game's own function, dropped the sends to a game space that is not loaded; their sum over the run's time is the call rate of "
         "CSendEventNode::Enter\"}");
  QCHECK(LinesOf("combat_sendguard_stats")[0].level == sentinel::LogLevel::kDebug);
  g_lines.clear();
  const std::uint64_t drop[3] = {0x77, 1, 2};
  for (unsigned i = 0; i < sg::kStatsDropEvery; ++i) CallThroughSlot(g_ctx, drop);
  QCHECK(LinesOf("combat_sendguard_dropped").size() == sg::kDropLines);  // the ninth and later drops write no dropped line ...
  QCHECK(LinesOf("combat_sendguard_stats").size() == 1);                // ... the 1024th leaves the count
  QCHECK(Has(LinesOf("combat_sendguard_stats")[0].text, "\"passed\":16384,\"dropped\":1024,\"final\":0"));
  g_lines.clear();
  sg::NoteEnd();
  QCHECK(LinesOf("combat_sendguard_stats").size() == 1);
  QCHECK(Has(LinesOf("combat_sendguard_stats")[0].text, "\"passed\":16384,\"dropped\":1024,\"final\":1"));
  QCHECK(LinesOf("combat_sendguard_stats")[0].level == sentinel::LogLevel::kInfo);
  Finish();
  // Not installed (a refused slot, or off): nothing at the end.
  InstallFresh(ImageOptions{nullptr, true, 0, nullptr});
  g_lines.clear();
  sg::NoteEnd();
  QCHECK(g_lines.empty());
  Finish();
  InstallFresh(ImageOptions{}, false);
  g_lines.clear();
  sg::NoteEnd();
  QCHECK(g_lines.empty());
  Finish();
}

// The process end as the sentinel reaches it: combat::NoteProcessEnd over the shipped table writes the final line once when the wrapper is
// installed and the normal step ran, and nothing before the normal step ran or in a plain release.
void TheShippedProcessEndWritesTheFinalLineOnce() {
  QCHECK(InstallFresh().installed);
  const std::uint64_t pass[3] = {kNoLevel, 1, 2};
  CallThroughSlot(g_ctx, pass);
  combat::ResetForTest();
  combat::ResetInstallForTest();
  combat::SetVariant(nevr_quest::build_variant::Variant::kCombat);
  combat::Resolve(/*devBuild=*/false, /*forceAsked=*/false);
  g_lines.clear();
  combat::NoteProcessEnd();
  QCHECK(LinesOf("combat_sendguard_stats").empty());  // the normal step has not run
  combat::InstallNormal();                           // the host has no libr15: the row refuses, the hook installed above stays
  g_lines.clear();
  combat::NoteProcessEnd();
  QCHECK(LinesOf("combat_sendguard_stats").size() == 1);
  QCHECK(Has(LinesOf("combat_sendguard_stats")[0].text, "\"passed\":1,\"dropped\":0,\"final\":1"));
  combat::ResetForTest();
  combat::ResetInstallForTest();
  combat::SetVariant(nevr_quest::build_variant::Variant::kNone);
  combat::Resolve(false, false);
  combat::InstallNormal();
  g_lines.clear();
  combat::NoteProcessEnd();
  QCHECK(LinesOf("combat_sendguard_stats").empty());  // a plain release
  combat::ResetForTest();
  combat::ResetInstallForTest();
  Finish();
}

// The table's row finds the old switch file where the plugin kept it: below the sentinel's files folder, in q15t.
void TheProductionPathLooksWhereThePluginKeptItsSwitchFile() {
  char tmpl[] = "/tmp/nevr-sendguard-files-XXXXXX";
  QCHECK(mkdtemp(tmpl) != nullptr);
  g_filesDir = tmpl;
  const std::string sub = g_filesDir + "/q15t";
  QCHECK(mkdir(sub.c_str(), 0700) == 0);
  Reset();
  combat::ResetInstallForTest();
  combat::ModuleResult r = sg::Install(true);
  QCHECK(!r.installed && LinesOf("combat_sendguard_legacy_switch").empty());  // no file: no Warning
  const std::string file = sub + "/sendguard.off";
  FILE* f = std::fopen(file.c_str(), "w");
  QCHECK(f != nullptr);
  if (f != nullptr) std::fclose(f);
  g_lines.clear();
  r = sg::Install(true);
  QCHECK(LinesOf("combat_sendguard_legacy_switch").size() == 1);
  QCHECK(LinesOf("combat_sendguard_legacy_switch")[0].level == sentinel::LogLevel::kWarn);
  unlink(file.c_str());
  rmdir(sub.c_str());
  rmdir(g_filesDir.c_str());
  g_filesDir.clear();
  Finish();
}

}  // namespace

int main() {
  sentinel::SetLogSink(&CaptureLine);
  TheTargetsAreTheStoreLibr15s();
  AnInstallPutsTheThunkInTheOneWordAndChangesNothingElse();
  ASendToAnUnloadedGameSpaceIsDropped();
  ASendToALoadedGameSpaceReachesTheStockFunction();
  ASendWithNoLevelIsNotLookedUp();
  ANullOnTheChainLeavesTheSendToTheStockFunction();
  OnlyTheFirstDropsAreWrittenAndEveryOneIsCounted();
  AWordThatDiffersRefusesTheModuleAndSwapsNothing();
  AWordThatIsNotEnterIsRefused();
  TheOtherRefusalsSayWhy();
  TheSwitchTurnsTheModuleOff();
  TheOldSwitchFileChangesNothingAndIsToldSo();
  TheTablesRowFollowsItsSwitch();
  TheCountsAreWrittenSoAHeadsetRunGivesTheRate();
  TheProductionPathLooksWhereThePluginKeptItsSwitchFile();
  TheShippedProcessEndWritesTheFinalLineOnce();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "sendguard_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("sendguard_test: all checks passed");
  return 0;
}
