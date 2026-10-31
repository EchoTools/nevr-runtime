// Host test for "a free that would overflow libr15's array of free GPU ranges is not run" (#1759): the real handler
// (src/quest/sentinel/vk_free_guard.cpp), built without exceptions as the sentinel builds it, driven through the thunk's entry
// with a model of CGRawInterfaceVK::Free as the game's function. The model does what the pinned image's Free does
// (vk_hole_table.h): it scans the array, grows the entry that touches the freed range, else appends at index `count` and stores
// `count + 1`, with no compare against 512. The array and the count sit in one buffer in the game's layout, so the model's
// 513th append lands on the count, as the game's does. No GOT slot is touched: got_pinned_test.cpp checks the pinned words,
// the slot and the layout against the real libr15.
//
// Built and run by `just test-quest-hooks`.

#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hook_log.h"
#include "quest/tests/test_check.h"
#include "vk_free_guard.h"

namespace {

namespace guard = nevr_quest::vk_free_guard;
namespace hole = nevr_quest::vk_hole;
namespace layout = sentinel::pinned::vk_hole_layout;

guard::Thunk::Fn Entry() { return reinterpret_cast<guard::Thunk::Fn>(guard::Thunk::EntryAddress()); }

// The game's array and the words after it: 512 entries, the count, and room for the entry that is written past the end.
struct Arena {
  alignas(8) unsigned char bytes[hole::kCapacity * sizeof(hole::Entry) + 8 + 5 * sizeof(std::uint64_t)];
  hole::Entry* table() { return reinterpret_cast<hole::Entry*>(bytes); }
  volatile std::uint32_t* count() { return reinterpret_cast<volatile std::uint32_t*>(bytes + hole::kCapacity * sizeof(hole::Entry)); }
};
Arena g_arena;
int g_gameCalls = 0;
void* g_lastMemory = nullptr;
unsigned long g_lastBegin = 0;
unsigned long g_lastUnused = 0;
unsigned long g_lastEnd = 0;

void ClearArena() {
  std::memset(g_arena.bytes, 0, sizeof(g_arena.bytes));
  g_gameCalls = 0;
}

// The game's Free, by the rule in vk_hole_table.h and the words at 0x188544c..0x1885740 of the pinned image.
void ModelFree(void* memory, unsigned long begin, unsigned long unused, unsigned long end) {
  ++g_gameCalls;
  g_lastMemory = memory;
  g_lastBegin = begin;
  g_lastUnused = unused;
  g_lastEnd = end;
  const std::uint32_t count = *g_arena.count();
  hole::Entry* const table = g_arena.table();
  const std::uint64_t handle = reinterpret_cast<std::uint64_t>(memory);
  for (std::uint32_t i = 0; i < count; ++i) {
    if (table[i].memory != handle) continue;
    if (table[i].begin == end) {
      table[i].begin = begin;
      return;
    }
    if (table[i].end == begin) {
      table[i].end = end;
      return;
    }
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    if (table[i].memory != handle) continue;
    // The append: the count first, then the entry at the old count (str w12 / stp / stp / str of the pinned image).
    *g_arena.count() = count + 1;
    unsigned char* const at = g_arena.bytes + static_cast<std::size_t>(count) * sizeof(hole::Entry);
    const hole::Entry entry = {handle, begin, end, table[i].aux0, table[i].aux1};
    std::memcpy(at, &entry, sizeof(entry));
    return;
  }
  // No entry for the block: the whole block is freed (vkFreeMemory); nothing is appended.
}

void Fill(std::uint32_t entries) {
  ClearArena();
  for (std::uint32_t i = 0; i < entries; ++i) {
    // Each entry is a free range of a block of its own, 0x1000 wide, with a gap on both sides.
    g_arena.table()[i] = {0x1000u + i, 0x10000u * (i + 1), 0x10000u * (i + 1) + 0x1000, 1, 2};
  }
  *g_arena.count() = entries;
}

void Reset() {
  guard::ResetForTest();
  ClearArena();
  guard::Thunk::Reset();
  *guard::Thunk::OriginalOut() = reinterpret_cast<void*>(&ModelFree);
}

void Arm() { guard::ArmForTest(g_arena.count(), g_arena.table()); }

// A free of a range with a gap on both sides of the free range of block 1 (0x1000 + 0): it would append.
void FreeApart(std::uint32_t block, unsigned long begin) { Entry()(reinterpret_cast<void*>(0x1000u + block), begin, 0, begin + 0x100); }

// The failure the guard exists for, with the guard absent: the model of the game's Free appends past 512, the count is
// overwritten by the VkDeviceMemory handle, and the next scan would run that many entries.
void WithoutTheGuardTheFiveHundredAndThirteenthEntryOverwritesTheCount() {
  Reset();
  Fill(hole::kCapacity);
  const std::uint64_t handle = 0x1000 + 7;
  ModelFree(reinterpret_cast<void*>(handle), 0x7ffff000, 0, 0x7ffff100);
  QCHECK(g_gameCalls == 1);
  QCHECK(*g_arena.count() == static_cast<std::uint32_t>(handle));  // the count is the handle's low word: 4103
  QCHECK(*g_arena.count() != hole::kCapacity + 1);
}

// Below kGuardFrom the handler calls the game and reads no entry.
void BelowTheGuardTheGameRuns() {
  Reset();
  Fill(hole::kGuardFrom - 1);
  Arm();
  Entry()(reinterpret_cast<void*>(0x1003), 0x7ffff000, 0x55, 0x7ffff100);
  QCHECK(g_gameCalls == 1);
  QCHECK(g_lastMemory == reinterpret_cast<void*>(0x1003) && g_lastBegin == 0x7ffff000 && g_lastUnused == 0x55 &&
         g_lastEnd == 0x7ffff100);  // the game gets all four arguments as they came
  QCHECK(*g_arena.count() == hole::kGuardFrom);  // the model appended; the guard did not stand in the way
  QCHECK(guard::CurrentCounts().leaked == 0);
}

// The case the guard exists for: an array with room for no more than the headroom, a free that would append.
void AFreeThatWouldAppendToAFullArrayLeaksTheRange() {
  Reset();
  Fill(hole::kGuardFrom);
  Arm();
  FreeApart(3, 0x7ffff000);
  QCHECK(g_gameCalls == 0);
  QCHECK(*g_arena.count() == hole::kGuardFrom);
  QCHECK(guard::CurrentCounts().leaked == 1);
  QCHECK(guard::CurrentCounts().corrupt == 0);
}

// A thousand such frees leave the count where it was; the same thousand without the guard overflow it.
void AThousandAppendingFreesNeverPassTheCapacity() {
  Reset();
  Fill(hole::kGuardFrom - 4);
  Arm();
  for (unsigned long i = 0; i < 1000; ++i) FreeApart(static_cast<std::uint32_t>(i % 50), 0x7f000000 + i * 0x1000);
  QCHECK(*g_arena.count() <= hole::kCapacity);
  QCHECK(*g_arena.count() >= hole::kGuardFrom);
  QCHECK(guard::CurrentCounts().leaked > 900);

  Reset();
  Fill(hole::kGuardFrom - 4);
  for (unsigned long i = 0; i < 1000 && *g_arena.count() <= hole::kCapacity; ++i) {
    ModelFree(reinterpret_cast<void*>(0x1000u + (i % 50)), 0x7f000000 + i * 0x1000, 0, 0x7f000000 + i * 0x1000 + 0x100);
  }
  QCHECK(*g_arena.count() > hole::kCapacity);  // the model without the guard runs the count off the array
}

// A free next to an entry grows it in place and appends nothing: the game runs, whatever the count.
void AFreeNextToAnEntryRunsAtAFullArray() {
  Reset();
  Fill(hole::kCapacity);
  Arm();
  // Block 5's free range is [0x60000, 0x61000): the range [0x61000, 0x61100) touches its end, [0x5ff00, 0x60000) its begin.
  Entry()(reinterpret_cast<void*>(0x1000u + 5), 0x61000, 0, 0x61100);
  Entry()(reinterpret_cast<void*>(0x1000u + 5), 0x5ff00, 0, 0x60000);
  QCHECK(g_gameCalls == 2);
  QCHECK(guard::CurrentCounts().leaked == 0);
  QCHECK(*g_arena.count() == hole::kCapacity);
}

// A block with no entry is a whole-block free: nothing is appended, so the game runs.
void AFreeOfABlockWithNoEntryRunsAtAFullArray() {
  Reset();
  Fill(hole::kCapacity);
  Arm();
  Entry()(reinterpret_cast<void*>(0x9999999), 0x1000, 0, 0x2000);
  QCHECK(g_gameCalls == 1);
  QCHECK(guard::CurrentCounts().leaked == 0);
}

// A count past the array is already the damage; the game's Free would scan that far, so it is not called.
void ACountPastTheArrayIsNotScanned() {
  Reset();
  Fill(hole::kCapacity);
  *g_arena.count() = 0xfffff000u;  // a handle's low word, as the 513th append leaves it
  Arm();
  FreeApart(3, 0x7ffff000);
  QCHECK(g_gameCalls == 0);
  QCHECK(guard::CurrentCounts().corrupt == 1);
  QCHECK(guard::CurrentCounts().leaked == 1);
}

// Not armed, the entry is the game's call.
void WithoutTheHookArmedTheEntryIsTheGamesFree() {
  Reset();
  Fill(hole::kGuardFrom);
  FreeApart(3, 0x7ffff000);
  QCHECK(g_gameCalls == 1);
  QCHECK(guard::CurrentCounts().leaked == 0);
}

std::vector<std::string> g_lines;
void CaptureLine(sentinel::LogLevel /*level*/, const char* line) { g_lines.emplace_back(line); }

int CountLines(const char* needle) {
  int n = 0;
  for (const std::string& line : g_lines) n += line.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

int CountLinesWithBoth(const char* first, const char* second) {
  int n = 0;
  for (const std::string& line : g_lines) n += line.find(first) != std::string::npos && line.find(second) != std::string::npos ? 1 : 0;
  return n;
}

// The array's fill is in the log: one line for each of 128, 256, 384 and 504 entries, once each, so the last lines of a crash
// report say how full it was. The first leak and every 1024th are logged, with the block and the range.
void TheLogSaysHowFullTheArrayGotAndWhatWasLeaked() {
  Reset();
  g_lines.clear();
  sentinel::SetLogSink(&CaptureLine);
  Fill(100);
  Arm();
  FreeApart(1, 0x7ff00000);
  QCHECK(CountLines("\"event\":\"vk_hole_table\"") == 0);
  for (const std::uint32_t count : {130u, 300u, 300u, 400u, 504u, 504u}) {
    Fill(count);
    FreeApart(2, 0x7ff10000);
  }
  QCHECK(CountLines("\"event\":\"vk_hole_table\"") == 4);
  QCHECK(CountLines("\"count\":128") == 0 && CountLines("\"count\":130") == 1);
  QCHECK(CountLines("\"event\":\"vk_free_leak\"") == 1);  // two leaks (the 504 twice); the first one is logged
  QCHECK(CountLines("\"verdict\":\"leak\"") == 1);
  QCHECK(CountLines("\"memory\":\"0x0000000000001002\"") == 1);
  QCHECK(CountLines("\"leaked\":1") == 1);
  QCHECK(guard::CurrentCounts().high_water == 504);

  g_lines.clear();
  Fill(hole::kGuardFrom);
  for (int i = 0; i < 2100; ++i) FreeApart(2, 0x7ff10000 + static_cast<unsigned long>(i) * 0x1000);
  QCHECK(CountLines("\"event\":\"vk_free_leak\"") == 2);  // the 1024th and the 2048th; the first two were counted above
  sentinel::SetLogSink(&CaptureLine);
}

// ---- the install ------------------------------------------------------------------------------------------------------------

// An image with the pinned words where the guard reads them.
constexpr std::size_t kImageBytes = 0x37a0000;
unsigned char* MakeImage() {
  void* mem = mmap(nullptr, kImageBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return nullptr;
  auto* image = static_cast<unsigned char*>(mem);
  const sentinel::pinned::PinnedFunction& f = sentinel::pinned::kVkFree;
  std::memcpy(image + f.vaddr, f.code, f.words * sizeof(std::uint32_t));
  for (const sentinel::pinned::PinnedCode* block :
       {&sentinel::pinned::kVkFreeCountBlock, &sentinel::pinned::kVkFreeAppendBlock, &sentinel::pinned::kVkFreeAppendStore}) {
    std::memcpy(image + block->vaddr, block->code, block->words * sizeof(std::uint32_t));
  }
  return image;
}

// libr15 is not loaded on a host, so the slot does not resolve: the install refuses and leaves nothing armed.
void AnInstallWhoseSlotDoesNotResolveLeavesTheGamesFree() {
  Reset();
  unsigned char* image = MakeImage();
  QCHECK(image != nullptr);
  if (image == nullptr) return;
  g_lines.clear();
  sentinel::SetLogSink(&CaptureLine);
  QCHECK(!guard::InstallAt(image, sentinel::pinned::LibR15VkFree()));
  sentinel::SetLogSink(&CaptureLine);
  QCHECK(CountLines("\"event\":\"vk_free_guard_install\"") == 1);
  QCHECK(CountLines("\"result\":\"refused\"") == 1);
  QCHECK(CountLinesWithBoth("\"event\":\"vk_free_guard_install\"", "\"status\":\"module_not_loaded\"") == 1);
  Fill(hole::kGuardFrom);
  FreeApart(3, 0x7ffff000);  // nothing is armed: the entry is the game's call
  QCHECK(g_gameCalls == 1);
  munmap(image, kImageBytes);
}

// Each pinned block is checked: a word that is not the pinned image's refuses the install, naming the block, before any slot.
void APinnedWordThatDiffersRefusesTheInstall() {
  struct Case {
    std::uint64_t vaddr;
    const char* name;
  };
  const Case cases[] = {{sentinel::pinned::kVkFree.vaddr + 12, sentinel::pinned::kVkFree.name},
                        {sentinel::pinned::kVkFreeCountBlock.vaddr + 4, sentinel::pinned::kVkFreeCountBlock.name},
                        {sentinel::pinned::kVkFreeAppendBlock.vaddr + 20, sentinel::pinned::kVkFreeAppendBlock.name},
                        {sentinel::pinned::kVkFreeAppendStore.vaddr, sentinel::pinned::kVkFreeAppendStore.name}};
  for (const Case& c : cases) {
    Reset();
    unsigned char* image = MakeImage();
    QCHECK(image != nullptr);
    if (image == nullptr) return;
    image[c.vaddr] ^= 1;
    g_lines.clear();
    sentinel::SetLogSink(&CaptureLine);
    QCHECK(!guard::InstallAt(image, sentinel::pinned::LibR15VkFree()));
    sentinel::SetLogSink(&CaptureLine);
    QCHECK(CountLines("\"why\":\"pin_mismatch\"") == 1);
    QCHECK(CountLines(c.name) == 1);
    QCHECK(CountLines("\"status\"") == 0);  // refused before the GOT was asked
    munmap(image, kImageBytes);
  }
  Reset();
  g_lines.clear();
  sentinel::SetLogSink(&CaptureLine);
  QCHECK(!guard::InstallAt(nullptr, sentinel::pinned::LibR15VkFree()));
  sentinel::SetLogSink(&CaptureLine);
  QCHECK(CountLines("\"why\":\"no_image\"") == 1);
}

// The hook's layout and the pure rule's agree: the pins say 512 entries of 0x28 bytes with the count after the last.
void ThePinnedLayoutIsTheRulesLayout() {
  QCHECK(layout::kCapacity == hole::kCapacity);
  QCHECK(layout::kEntryBytes == sizeof(hole::Entry));
  QCHECK(layout::kCountVaddr - layout::kTableVaddr == static_cast<std::uint64_t>(hole::kCapacity) * sizeof(hole::Entry));
  QCHECK(hole::kGuardFrom + hole::kHeadroom == hole::kCapacity);
  // The 0x5000 in the pinned append block's `mov w14, #0x5000` is the same distance.
  QCHECK(((sentinel::pinned::kVkFreeAppendBlock.code[5] >> 5) & 0xffff) == layout::kCountVaddr - layout::kTableVaddr);
}

}  // namespace

int main() {
  sentinel::SetLogSink(&CaptureLine);  // the default sink writes every line to stderr
  WithoutTheGuardTheFiveHundredAndThirteenthEntryOverwritesTheCount();
  BelowTheGuardTheGameRuns();
  AFreeThatWouldAppendToAFullArrayLeaksTheRange();
  AThousandAppendingFreesNeverPassTheCapacity();
  AFreeNextToAnEntryRunsAtAFullArray();
  AFreeOfABlockWithNoEntryRunsAtAFullArray();
  ACountPastTheArrayIsNotScanned();
  WithoutTheHookArmedTheEntryIsTheGamesFree();
  TheLogSaysHowFullTheArrayGotAndWhatWasLeaked();
  AnInstallWhoseSlotDoesNotResolveLeavesTheGamesFree();
  APinnedWordThatDiffersRefusesTheInstall();
  ThePinnedLayoutIsTheRulesLayout();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "vk_free_guard_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("vk_free_guard_test: all checks pass\n");
  return 0;
}
