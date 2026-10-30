// Host test for "the game never refuses a headset for its OS build number" (#945): the real handler
// (src/quest/sentinel/os_build_gate_hook.cpp), built without exceptions as the sentinel builds it, driven through the
// thunk's entry with a stand-in for libr15's CSysInfo::OSBuildVersion. The floor is read from an in-memory libr15
// look-alike, and the install (InstallAt) goes through the real GotHook into a fixture slot in one, never a real GOT.
// got_pinned_test.cpp resolves the pinned slot, the floor and the one call site in the real library.
//
// Built and run by `just test-quest-hooks`.

#include <elf.h>
#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "got_hook.h"
#include "hook_log.h"
#include "os_build_gate_hook.h"
#include "quest/tests/test_check.h"

namespace {

namespace gate = nevr_quest::os_build_gate;

constexpr std::uint64_t kFloor = sentinel::pinned::kMinOSBuildVersionValue;

// ---- log capture (as vk_ext_window_test.cpp) --------------------------------------------------------------------------

std::vector<std::string> g_lines;
void CaptureLine(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }

std::vector<std::string> LinesFor(const char* event) {
  std::vector<std::string> out;
  const std::string needle = std::string("\"event\":\"") + event + "\"";
  for (const std::string& l : g_lines) {
    if (l.find(needle) != std::string::npos) out.push_back(l);
  }
  return out;
}

// The text after `"key":` up to the closing quote (a string value) or the next ',' or '}' (a number or boolean).
std::string Field(const std::string& line, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const std::size_t at = line.find(needle);
  if (at == std::string::npos) return "<absent>";
  std::size_t from = at + needle.size();
  if (from < line.size() && line[from] == '"') {
    const std::size_t end = line.find('"', from + 1);
    return line.substr(from + 1, end == std::string::npos ? std::string::npos : end - from - 1);
  }
  const std::size_t end = line.find_first_of(",}", from);
  return line.substr(from, end == std::string::npos ? std::string::npos : end - from);
}

gate::Thunk::Fn Entry() { return reinterpret_cast<gate::Thunk::Fn>(gate::Thunk::EntryAddress()); }

// The stand-in for the game's function: what ro.build.version.incremental would make it answer.
int g_answer = 0;
std::uint64_t g_number = 0;
int g_originalCalls = 0;

int StandIn(std::uint64_t* version) {
  ++g_originalCalls;
  if (g_answer != 0) *version = g_number;
  return g_answer;
}

void Reset(int answer, std::uint64_t number) {
  g_answer = answer;
  g_number = number;
  g_originalCalls = 0;
  gate::ResetForTest();
  gate::Thunk::Reset();
  *gate::Thunk::OriginalOut() = reinterpret_cast<void*>(&StandIn);
}

// Without the hook armed the entry is the game's call: a number below the floor is returned as read, which is what
// sends Initialize to "os needs update".
void WithoutTheHookTheGameSeesItsNumber() {
  Reset(1, 1234);
  std::uint64_t version = 0;
  QCHECK(Entry()(&version) == 1);
  QCHECK(version == 1234);
  QCHECK(gate::CurrentSeen().calls == 0);
}

// The headset the floor refuses: the game is told "no number" and its variable is not written; the line says the game
// would have refused.
void ANumberBelowTheFloorIsAnsweredNoNumber() {
  Reset(1, kFloor - 1);
  gate::ArmForTest(kFloor);
  std::uint64_t version = 77;
  QCHECK(Entry()(&version) == 0);
  QCHECK(version == 77);
  QCHECK(g_originalCalls == 1);
  const gate::Seen seen = gate::CurrentSeen();
  QCHECK(seen.calls == 1);
  QCHECK(seen.read);
  QCHECK(seen.os_build == kFloor - 1);
  QCHECK(seen.floor == kFloor);
  QCHECK(seen.would_refuse);
}

// The Quest 3 and Quest 2 numbers on record (docs/adr/0021) are above the floor: the answer is the same, and the line
// says the game would not have refused.
void ANumberAboveTheFloorIsAnsweredNoNumberToo() {
  const std::uint64_t numbers[] = {52641960030600520ULL, 52242990035800150ULL, kFloor};
  for (const std::uint64_t number : numbers) {
    Reset(1, number);
    gate::ArmForTest(kFloor);
    std::uint64_t version = 0;
    QCHECK(Entry()(&version) == 0);
    QCHECK(version == 0);
    const gate::Seen seen = gate::CurrentSeen();
    QCHECK(seen.read && seen.os_build == number);
    QCHECK(!seen.would_refuse);
  }
}

// A property the game cannot read as a number: the original answers 0 and so does the hook.
void NoNumberStaysNoNumber() {
  Reset(0, 0);
  gate::ArmForTest(kFloor);
  std::uint64_t version = 5;
  QCHECK(Entry()(&version) == 0);
  QCHECK(version == 5);
  const gate::Seen seen = gate::CurrentSeen();
  QCHECK(seen.calls == 1 && !seen.read && seen.os_build == 0 && !seen.would_refuse);
}

// An unknown floor (the install could not read it) still answers "no number"; the line cannot say whether the game would
// have refused, so it says it would not.
void AnUnknownFloorStillAnswersNoNumber() {
  Reset(1, 1);
  gate::ArmForTest(0);
  std::uint64_t version = 0;
  QCHECK(Entry()(&version) == 0);
  QCHECK(!gate::CurrentSeen().would_refuse);
}

// Every call is answered, and only the first is measured.
void EveryCallIsAnsweredAndTheFirstIsMeasured() {
  Reset(1, 10);
  gate::ArmForTest(kFloor);
  std::uint64_t version = 0;
  QCHECK(Entry()(&version) == 0);
  g_number = 20;
  QCHECK(Entry()(&version) == 0);
  QCHECK(g_originalCalls == 2);
  const gate::Seen seen = gate::CurrentSeen();
  QCHECK(seen.calls == 2);
  QCHECK(seen.os_build == 10);
}

// ---- the floor read (ReadPinnedFloorFrom, the part of Install that reads libr15) ----------------------------------------

// The pinned floor sits at kMinOSBuildVersionVaddr. This image's bytes start 0x100 below it, so the build note and the
// floor are both inside its one PT_LOAD, at the link-time addresses the pinned table names.
constexpr std::uint64_t kNoteVaddr = sentinel::pinned::kMinOSBuildVersionVaddr - 0x100;

// An in-memory libr15 look-alike: a build note carrying `buildIdHex`, and `floor` at the pinned address.
struct FloorImage {
  alignas(8) unsigned char memory[0x200] = {};
  Elf64_Phdr phdr[2] = {};
  sentinel::ElfImage image;

  FloorImage(const char* buildIdHex, std::uint64_t floor) {
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
    std::memcpy(memory + 0x100, &floor, sizeof(floor));
    phdr[0].p_type = PT_LOAD;
    phdr[0].p_vaddr = kNoteVaddr;
    phdr[0].p_memsz = sizeof(memory);
    phdr[1].p_type = PT_NOTE;
    phdr[1].p_vaddr = kNoteVaddr;
    phdr[1].p_memsz = sizeof(nhdr) + 4 + 20;
    image.base = reinterpret_cast<std::uintptr_t>(memory) - kNoteVaddr;
    image.phdr = phdr;
    image.phnum = 2;
  }
};

void TheFloorIsReadOnlyFromAnImageWhoseBuildIdIsThePinnedOne() {
  const char* source = "unset";
  FloorImage good(sentinel::pinned::LibR15PatchedBuildId(), kFloor);
  QCHECK(gate::ReadPinnedFloorFrom(good.image, &source) == kFloor);
  QCHECK(std::strcmp(source, "pinned") == 0);

  // Another build: its addresses mean something else, so nothing is read.
  FloorImage other("00112233445566778899aabbccddeeff00112233", kFloor);
  QCHECK(gate::ReadPinnedFloorFrom(other.image, &source) == 0);
  QCHECK(std::strcmp(source, "build_id_mismatch") == 0);

  // No build note to check against: nothing is read.
  const sentinel::ElfImage none{};
  QCHECK(gate::ReadPinnedFloorFrom(none, &source) == 0);
  QCHECK(std::strcmp(source, "no_build_id") == 0);
}

// ---- the install (InstallAt through the real GotHook entry, on a fixture library) ------------------------------------

// The game's own function, the value the fixture's slot holds before the install.
int GameOriginal(std::uint64_t* /*version*/) { return 0; }

constexpr std::size_t kSlotImageBytes = 4 * 4096;
constexpr std::uint64_t kSlotStrtab = 0x100, kSlotSymtab = 0x200, kSlotJmprel = 0x400, kSlotDynamic = 0x1800;
constexpr std::uint64_t kSlotAt = 0x3008;  // a writable slot, not RELRO

// A libr15 look-alike with one writable PT_LOAD: the tables a JUMP_SLOT install reads (DT_STRTAB, DT_SYMTAB, DT_JMPREL,
// DT_FLAGS BIND_NOW, a PT_DYNAMIC) and the slot for CSysInfo::OSBuildVersion, holding the game's function. Link-time
// dynamic pointers (the Bionic shape), so the image is mapped at its own address. Kept for the process's life: the hook
// stays installed in it.
struct SlotImage {
  unsigned char* mem = nullptr;
  Elf64_Phdr phdr[2] = {};
  sentinel::ElfImage image;

  SlotImage() {
    void* const region = mmap(nullptr, kSlotImageBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    QCHECK(region != MAP_FAILED);
    mem = static_cast<unsigned char*>(region);
    const char* const name = sentinel::pinned::kOSBuildVersionSymbol;
    const std::size_t len = std::strlen(name);
    std::memcpy(mem + kSlotStrtab + 1, name, len + 1);  // "\0" name "\0"
    Elf64_Sym syms[2] = {};
    syms[1].st_name = 1;
    std::memcpy(mem + kSlotSymtab, syms, sizeof(syms));
    Elf64_Rela rel{};
    rel.r_offset = kSlotAt;
    rel.r_info = ELF64_R_INFO(1, sentinel::kNativeRelocs.jumpSlot);
    std::memcpy(mem + kSlotJmprel, &rel, sizeof(rel));
    Elf64_Dyn dyn[8] = {};
    std::size_t n = 0;
    auto add = [&dyn, &n](Elf64_Sxword tag, std::uint64_t value) {
      dyn[n].d_tag = tag;
      dyn[n].d_un.d_val = value;
      ++n;
    };
    add(DT_STRTAB, kSlotStrtab);
    add(DT_SYMTAB, kSlotSymtab);
    add(DT_STRSZ, len + 2);
    add(DT_JMPREL, kSlotJmprel);
    add(DT_PLTRELSZ, sizeof(rel));
    add(DT_PLTREL, DT_RELA);
    add(DT_FLAGS, DF_BIND_NOW);
    add(DT_NULL, 0);
    std::memcpy(mem + kSlotDynamic, dyn, sizeof(dyn));
    void* const original = reinterpret_cast<void*>(&GameOriginal);
    std::memcpy(mem + kSlotAt, &original, sizeof(original));

    phdr[0].p_type = PT_LOAD;
    phdr[0].p_flags = PF_R | PF_W;
    phdr[0].p_filesz = kSlotImageBytes;
    phdr[0].p_memsz = kSlotImageBytes;
    phdr[1].p_type = PT_DYNAMIC;
    phdr[1].p_flags = PF_R | PF_W;
    phdr[1].p_vaddr = kSlotDynamic;
    phdr[1].p_filesz = sizeof(dyn);
    phdr[1].p_memsz = sizeof(dyn);
    image.base = reinterpret_cast<std::uintptr_t>(mem);
    image.phdr = phdr;
    image.phnum = 2;
  }
  void** Slot() const { return reinterpret_cast<void**>(mem + kSlotAt); }
};

SlotImage& Fixture() {
  static SlotImage fixture;
  return fixture;
}

// The fixture is what the install sees as the loaded library; nullptr stands for a library that is not loaded.
bool g_fixtureLoaded = true;
bool FixtureLookup(const char*, sentinel::ElfImage* out) {
  if (!g_fixtureLoaded) return false;
  *out = Fixture().image;
  return true;
}

void TheGlueInstallsThroughTheHookAndRefusesWhatItCannotInstall() {
  gate::ResetForTest();
  g_lines.clear();
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  const sentinel::GotTarget target{"libr15.so", sentinel::pinned::kOSBuildVersionSymbol, sentinel::RelocKind::kJumpSlot};
  const sentinel::GotTarget missing{"libr15.so", "_ZN10NRadEngine8CSysInfo14NoSuchSymbolERy",
                                    sentinel::RelocKind::kJumpSlot};
  void* const original = reinterpret_cast<void*>(&GameOriginal);
  void* const entry = gate::Thunk::EntryAddress();

  // A library that is not loaded: refused, and the slot is not reached.
  g_fixtureLoaded = false;
  QCHECK(!gate::InstallAt(target, kFloor, "pinned", &FixtureLookup));
  g_fixtureLoaded = true;
  QCHECK(*Fixture().Slot() == original);

  // The library is there but has no such symbol: refused, the game's call stays in the slot.
  QCHECK(!gate::InstallAt(missing, kFloor, "pinned", &FixtureLookup));
  QCHECK(*Fixture().Slot() == original);

  // The real symbol: installed, the slot holds the thunk's entry.
  QCHECK(gate::InstallAt(target, kFloor, "pinned", &FixtureLookup));
  QCHECK(*Fixture().Slot() == entry);

  sentinel::SetLogSink(previous);
  const std::vector<std::string> lines = LinesFor("os_build_gate_install");
  QCHECK(lines.size() == 3);
  if (lines.size() == 3) {
    QCHECK(Field(lines[0], "result") == "refused" && Field(lines[0], "status") == "module_not_loaded");
    QCHECK(lines[0].find("\"level\":\"error\"") != std::string::npos);
    QCHECK(Field(lines[1], "result") == "refused" && Field(lines[1], "status") == "symbol_not_found");
    QCHECK(Field(lines[2], "result") == "installed" && Field(lines[2], "status") == "ok");
    QCHECK(Field(lines[2], "slot") == sentinel::pinned::kOSBuildVersionSymbol);
    QCHECK(Field(lines[2], "floor") == "6023800249000000" && Field(lines[2], "floor_source") == "pinned");
    QCHECK(lines[2].find("\"level\":\"info\"") != std::string::npos);
  }
}

}  // namespace

int main() {
  WithoutTheHookTheGameSeesItsNumber();
  ANumberBelowTheFloorIsAnsweredNoNumber();
  ANumberAboveTheFloorIsAnsweredNoNumberToo();
  NoNumberStaysNoNumber();
  AnUnknownFloorStillAnswersNoNumber();
  EveryCallIsAnsweredAndTheFirstIsMeasured();
  TheFloorIsReadOnlyFromAnImageWhoseBuildIdIsThePinnedOne();
  TheGlueInstallsThroughTheHookAndRefusesWhatItCannotInstall();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "os_build_gate_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("os_build_gate_hook_test: all checks pass\n");
  return 0;
}
