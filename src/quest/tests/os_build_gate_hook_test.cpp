// Host test for "the game never refuses a headset for its OS build number" (#945): the real handler
// (src/quest/sentinel/os_build_gate_hook.cpp), built without exceptions as the sentinel builds it, driven through the
// thunk's entry. The handler measures the property through a fake reader and never calls the game's function, so the
// stand-in below is a tripwire: it must never be called. The floor is read from an in-memory libr15 look-alike, and the
// install (InstallAt) goes through the real GotHook into a fixture slot in one, never a real GOT.
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

// The property the fake reader returns, as __system_property_get would.
const char* g_property = "";
int g_readerCalls = 0;

int FakeReader(const char* name, char* value) {
  ++g_readerCalls;
  QCHECK(std::strcmp(name, "ro.build.version.incremental") == 0);
  std::snprintf(value, 92, "%s", g_property);
  return static_cast<int>(std::strlen(g_property));
}

// The stand-in for the game's function. The hook must never call it: if it runs, the tripwire counts it. When it runs it
// answers g_standInAnswer and, for an answer of 1, writes g_standInNumber to the game's variable.
int g_standInAnswer = 1;
std::uint64_t g_standInNumber = 999;
int g_originalCalls = 0;

int StandIn(std::uint64_t* version) {
  ++g_originalCalls;
  if (g_standInAnswer != 0) *version = g_standInNumber;
  return g_standInAnswer;
}

// A fresh handler with `property` as what the headset reports, the stand-in as the game's function.
void Reset(const char* property) {
  g_property = property;
  g_readerCalls = 0;
  g_originalCalls = 0;
  gate::SetPropertyReaderForTest(&FakeReader);
  gate::ResetForTest();
  gate::Thunk::Reset();
  *gate::Thunk::OriginalOut() = reinterpret_cast<void*>(&StandIn);
}

// Without the hook armed the entry is the game's call: a number below the floor is returned as read, which is what
// sends Initialize to "os needs update".
void WithoutTheHookTheGameSeesItsNumber() {
  g_standInAnswer = 1;
  g_standInNumber = 1234;
  Reset("1234");
  std::uint64_t version = 0;
  QCHECK(Entry()(&version) == 1);
  QCHECK(version == 1234);
  QCHECK(gate::CurrentSeen().calls == 0);
  g_standInNumber = 999;
}

// The headset the floor refuses: the game is told "no number" and its variable is not written, whether the game's
// function would have answered 1 or 0; the line says the game would have refused.
void ANumberBelowTheFloorIsAnsweredNoNumber() {
  for (const int answer : {1, 0}) {
    g_standInAnswer = answer;
    Reset("6023800248999999");
    gate::ArmForTest(kFloor);
    std::uint64_t version = 77;
    QCHECK(Entry()(&version) == 0);
    QCHECK(version == 77);
    QCHECK(g_originalCalls == 0);
    const gate::Seen seen = gate::CurrentSeen();
    QCHECK(seen.calls == 1);
    QCHECK(seen.read);
    QCHECK(seen.os_build == kFloor - 1);
    QCHECK(seen.floor == kFloor);
    QCHECK(seen.would_refuse);
    QCHECK(std::strcmp(seen.reason, "number") == 0);
  }
  g_standInAnswer = 1;
}

// The Quest 3 and Quest 2 numbers on record (docs/adr/0021) are above the floor: the answer is the same, and the line
// says the game would not have refused.
void ANumberAboveTheFloorIsAnsweredNoNumberToo() {
  const char* properties[] = {"52641960030600520", "52242990035800150", "6023800249000000"};
  const std::uint64_t numbers[] = {52641960030600520ULL, 52242990035800150ULL, kFloor};
  for (std::size_t i = 0; i < 3; ++i) {
    Reset(properties[i]);
    gate::ArmForTest(kFloor);
    std::uint64_t version = 0;
    QCHECK(Entry()(&version) == 0);
    QCHECK(version == 0);
    QCHECK(g_originalCalls == 0);
    const gate::Seen seen = gate::CurrentSeen();
    QCHECK(seen.read && seen.os_build == numbers[i]);
    QCHECK(!seen.would_refuse);
  }
}

// A property the game cannot read as a number: the hook answers 0 and says why it did not read one.
void NoNumberStaysNoNumber() {
  const char* properties[] = {"", "abc", "123456789012345678901"};
  const char* reasons[] = {"no_property", "not_a_number", "too_long"};
  for (std::size_t i = 0; i < 3; ++i) {
    g_standInAnswer = 0;
    Reset(properties[i]);
    gate::ArmForTest(kFloor);
    std::uint64_t version = 5;
    QCHECK(Entry()(&version) == 0);
    QCHECK(version == 5);
    QCHECK(g_originalCalls == 0);
    const gate::Seen seen = gate::CurrentSeen();
    QCHECK(seen.calls == 1 && !seen.read && seen.os_build == 0 && !seen.would_refuse);
    QCHECK(std::strcmp(seen.reason, reasons[i]) == 0);
  }
  g_standInAnswer = 1;
}

// An unknown floor (the install could not read it) still answers "no number"; the line cannot say whether the game would
// have refused, so it says it would not.
void AnUnknownFloorStillAnswersNoNumber() {
  Reset("1");
  gate::ArmForTest(0);
  std::uint64_t version = 0;
  QCHECK(Entry()(&version) == 0);
  QCHECK(!gate::CurrentSeen().would_refuse);
}

// Every call is answered, and only the first is measured: the property is read once.
void EveryCallIsAnsweredAndTheFirstIsMeasured() {
  Reset("10");
  gate::ArmForTest(kFloor);
  std::uint64_t version = 0;
  QCHECK(Entry()(&version) == 0);
  g_property = "20";
  QCHECK(Entry()(&version) == 0);
  QCHECK(g_originalCalls == 0);
  QCHECK(g_readerCalls == 1);
  const gate::Seen seen = gate::CurrentSeen();
  QCHECK(seen.calls == 2);
  QCHECK(seen.os_build == 10);
}

// The line is written once per process, and it is the same whichever answer the game's function would have given.
void TheHandlerLineIsWrittenOnceAndIsTheSameOnBothInputs() {
  const char* const keys[] = {"answer", "os_build_read", "os_build", "floor", "game_would_refuse", "reason"};
  std::vector<std::string> runs[2];
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  for (int i = 0; i < 2; ++i) {
    g_lines.clear();
    g_standInAnswer = i == 0 ? 1 : 0;
    Reset("6023800248999999");
    gate::ArmForTest(kFloor);
    std::uint64_t version = 0;
    QCHECK(Entry()(&version) == 0);
    runs[i] = LinesFor("os_build_gate");
    QCHECK(runs[i].size() == 1);
    QCHECK(Entry()(&version) == 0);
    QCHECK(LinesFor("os_build_gate").size() == 1);
    if (runs[i].size() == 1) {
      const std::string& line = runs[i][0];
      QCHECK(Field(line, "answer") == "no_number");
      QCHECK(Field(line, "os_build_read") == "1");
      QCHECK(Field(line, "os_build") == "6023800248999999");
      QCHECK(Field(line, "floor") == "6023800249000000");
      QCHECK(Field(line, "game_would_refuse") == "1");
      QCHECK(Field(line, "reason") == "number");
    }
  }
  sentinel::SetLogSink(previous);
  g_standInAnswer = 1;
  QCHECK(runs[0].size() == 1 && runs[1].size() == 1);
  if (runs[0].size() == 1 && runs[1].size() == 1) {
    for (const char* key : keys) QCHECK(Field(runs[0][0], key) == Field(runs[1][0], key));
  }
}

// The game's parse of ro.build.version.incremental, row by row (CSysInfo::OSBuildVersion: IsUInt64 and ToUInt64).
void ParseOsBuildTable() {
  struct Row {
    const char* text;
    gate::Reading want;
    std::uint64_t value;
  };
  const Row rows[] = {
      {"52641960030600520", gate::Reading::kNumber, 52641960030600520ULL},
      {"18446744073709551615", gate::Reading::kNumber, 18446744073709551615ULL},
      {"99999999999999999999", gate::Reading::kNumber, 7766279631452241919ULL},
      {"123456789012345678901", gate::Reading::kTooLong, 0},
      {"  42", gate::Reading::kNumber, 42},
      {"\t+42", gate::Reading::kNumber, 42},
      {"0x2A", gate::Reading::kNumber, 42},
      {"0", gate::Reading::kNumber, 0},
      {"", gate::Reading::kNoProperty, 0},
      {"12a", gate::Reading::kNotANumber, 0},
      {"0x", gate::Reading::kNotANumber, 0},
      {"-1", gate::Reading::kNotANumber, 0},
      {"4 2", gate::Reading::kNotANumber, 0},
  };
  for (const Row& row : rows) {
    std::uint64_t out = 0xdeadbeefULL;
    const gate::Reading got = gate::ParseOsBuild(row.text, static_cast<int>(std::strlen(row.text)), &out);
    QCHECK(got == row.want);
    if (got == gate::Reading::kNumber && got == row.want) QCHECK(out == row.value);
  }
}

// ---- the floor read (ReadPinnedFloorFrom, the part of Install that reads libr15) ----------------------------------------

// An in-memory libr15 look-alike: a build note carrying `buildIdHex`, and `value` at `valueVaddr` (the pinned floor's
// address by default). The note sits 0x100 below the value, so both are inside its one PT_LOAD.
struct FloorImage {
  alignas(8) unsigned char memory[0x200] = {};
  Elf64_Phdr phdr[2] = {};
  sentinel::ElfImage image;

  FloorImage(const char* buildIdHex, std::uint64_t value,
             std::uint64_t valueVaddr = sentinel::pinned::kMinOSBuildVersionVaddr) {
    const std::uint64_t noteVaddr = valueVaddr - 0x100;
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
    std::memcpy(memory + 0x100, &value, sizeof(value));
    phdr[0].p_type = PT_LOAD;
    phdr[0].p_vaddr = noteVaddr;
    phdr[0].p_memsz = sizeof(memory);
    phdr[1].p_type = PT_NOTE;
    phdr[1].p_vaddr = noteVaddr;
    phdr[1].p_memsz = sizeof(nhdr) + 4 + 20;
    image.base = reinterpret_cast<std::uintptr_t>(memory) - noteVaddr;
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

// The original's first instruction word, read from the loaded libr15 and classified; never written.
void TheOriginalBodyIsClassified() {
  const char* kind = "unset";
  const char* const id = sentinel::pinned::LibR15PatchedBuildId();
  FloorImage stock(id, 0xa90853f5d10283ffULL, sentinel::pinned::kOSBuildVersionVaddr);
  QCHECK(std::strcmp(gate::ReadOriginalBodyFrom(stock.image, &kind), "queries_property") == 0);
  QCHECK(std::strcmp(kind, "queries_property") == 0);

  FloorImage ref(id, 0xd65f03c0d2800000ULL, sentinel::pinned::kOSBuildVersionVaddr);
  QCHECK(std::strcmp(gate::ReadOriginalBodyFrom(ref.image, &kind), "returns_zero") == 0);

  FloorImage other(id, 0, sentinel::pinned::kOSBuildVersionVaddr);
  QCHECK(std::strcmp(gate::ReadOriginalBodyFrom(other.image, &kind), "other_code") == 0);

  FloorImage wrongId("00112233445566778899aabbccddeeff00112233", 0xa90853f5d10283ffULL,
                     sentinel::pinned::kOSBuildVersionVaddr);
  QCHECK(std::strcmp(gate::ReadOriginalBodyFrom(wrongId.image, &kind), "build_id_mismatch") == 0);

  const sentinel::ElfImage none{};
  QCHECK(std::strcmp(gate::ReadOriginalBodyFrom(none, &kind), "no_build_id") == 0);
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
  QCHECK(!gate::InstallAt(target, kFloor, "pinned", "queries_property", &FixtureLookup));
  g_fixtureLoaded = true;
  QCHECK(*Fixture().Slot() == original);

  // The library is there but has no such symbol: refused, the game's call stays in the slot.
  QCHECK(!gate::InstallAt(missing, kFloor, "pinned", "queries_property", &FixtureLookup));
  QCHECK(*Fixture().Slot() == original);

  // The real symbol: installed, the slot holds the thunk's entry.
  QCHECK(gate::InstallAt(target, kFloor, "pinned", "queries_property", &FixtureLookup));
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
    QCHECK(Field(lines[2], "original_body") == "queries_property");
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
  TheHandlerLineIsWrittenOnceAndIsTheSameOnBothInputs();
  ParseOsBuildTable();
  TheFloorIsReadOnlyFromAnImageWhoseBuildIdIsThePinnedOne();
  TheOriginalBodyIsClassified();
  TheGlueInstallsThroughTheHookAndRefusesWhatItCannotInstall();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "os_build_gate_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("os_build_gate_hook_test: all checks pass\n");
  return 0;
}
