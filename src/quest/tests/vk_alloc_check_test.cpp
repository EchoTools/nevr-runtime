// Host test for "an allocation of GPU memory reported as made with no memory is refused" (#2262): the real handler
// (src/quest/sentinel/vk_alloc_check.cpp), built without exceptions as the sentinel builds it, driven through the thunk's entry
// with a model of CGRawInterfaceVK::Allocate as the game's function and a model of CreateDeviceResource as its caller. The
// Allocate model does what the pinned image's does (vk_alloc_check.h): it clears the result, "calls vkAllocateMemory", and
// returns 0 whatever that returned, with the handle the driver left (null when the device is out of memory). The caller model
// does what CreateDeviceResource does at 0x18e6c54..0x18e6c74: `cbnz w0` returns the error, else it binds [base+0] at
// [base+0x10]. No GOT slot is touched: got_pinned_test.cpp checks the pinned words and the slot against the real libr15.
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
#include "vk_alloc_check.h"

namespace {

namespace check = nevr_quest::vk_alloc_check;
namespace pinned = sentinel::pinned;

check::Thunk::Fn Entry() { return reinterpret_cast<check::Thunk::Fn>(check::Thunk::EntryAddress()); }

// What the model's vkAllocateMemory does on the next call.
enum class Device { kHasMemory, kOutOfMemory };
Device g_device = Device::kHasMemory;
std::uint64_t g_gameStatus = 0;  // what the model returns for a reason of its own ("Could not find memory type"), else 0
int g_gameCalls = 0;
const check::MemoryRequirements* g_lastReqs = nullptr;
unsigned g_lastArgs[4] = {};

constexpr std::uint64_t kHandle = 0x7f00c0de0000ULL;
constexpr std::uint64_t kOffset = 0x40000;
constexpr int kGameError = 0x5151;  // the error id the game's error maker returns

// The game's Allocate, by the words at 0x188813c..0x18886a4 of the pinned image.
std::uint64_t ModelAllocate(const check::MemoryRequirements* reqs, unsigned desired, unsigned minimal, unsigned pooled, unsigned pool,
                            check::AllocatedBase* out) {
  ++g_gameCalls;
  g_lastReqs = reqs;
  g_lastArgs[0] = desired;
  g_lastArgs[1] = minimal;
  g_lastArgs[2] = pooled;
  g_lastArgs[3] = pool;
  std::memset(out, 0, sizeof(*out));
  if (g_gameStatus != 0) return g_gameStatus;
  // vkAllocateMemory's result is not read: an out-of-memory device leaves the handle null, and the range is cut anyway.
  out->memory = g_device == Device::kHasMemory ? kHandle : 0;
  out->offset = kOffset;
  out->end = kOffset + reqs->size;
  return 0;
}

int g_errorCalls = 0;
std::uint64_t g_lastErrorCode = 0;
std::string g_lastErrorMessage;
int g_errorReturns = kGameError;
int ModelCreateAndAddError(const char* /*file*/, unsigned /*line*/, std::uint64_t code, const char* message, ...) {
  ++g_errorCalls;
  g_lastErrorCode = code;
  g_lastErrorMessage = message != nullptr ? message : "";
  return g_errorReturns;
}

// The binds the caller made: a bind of memory 0 is the crash (the driver reads +0x10c of a null object).
struct Bind {
  std::uint64_t memory;
  std::uint64_t offset;
};
std::vector<Bind> g_binds;

// CreateDeviceResource at 0x18e6c54..0x18e6c74: Allocate through its PLT stub (here, the thunk's entry), `cbnz w0` to the error
// return, else vkBindImageMemory(device, image, [base+0], [base+0x10]).
std::uint64_t ModelCreateDeviceResource(std::uint64_t size) {
  const check::MemoryRequirements reqs = {size, 0x1000, 0x7};
  check::AllocatedBase base = {};
  const std::uint64_t status = Entry()(&reqs, 1, 1, 1, 0, &base);
  if (static_cast<std::uint32_t>(status) != 0) return status;
  g_binds.push_back({base.memory, base.offset});
  return 0;
}

int NullBinds() {
  int n = 0;
  for (const Bind& b : g_binds) n += b.memory == 0 ? 1 : 0;
  return n;
}

volatile std::uint32_t g_blockCount = 0;
volatile std::uint32_t g_tableCount = 0;

void Reset() {
  check::ResetForTest();
  check::Thunk::Reset();
  *check::Thunk::OriginalOut() = reinterpret_cast<void*>(&ModelAllocate);
  g_device = Device::kHasMemory;
  g_gameStatus = 0;
  g_gameCalls = 0;
  g_errorCalls = 0;
  g_errorReturns = kGameError;
  g_binds.clear();
  g_blockCount = 0;
  g_tableCount = 0;
}

void Arm() { check::ArmForTest(&ModelCreateAndAddError, &g_blockCount, &g_tableCount); }

// The crash, with the check absent: an out-of-memory device makes the game's Allocate report success, and its caller binds null.
void WithoutTheCheckAnOutOfMemoryDeviceIsBoundAsNull() {
  Reset();
  g_device = Device::kOutOfMemory;
  QCHECK(ModelCreateDeviceResource(0x100000) == 0);  // the entry is the game's Allocate: not armed
  QCHECK(g_gameCalls == 1);
  QCHECK(NullBinds() == 1);
}

// The case the check exists for: the same device, armed. The caller takes its error return and binds nothing.
void AnAllocationWithNoMemoryIsRefusedAndNotBound() {
  Reset();
  Arm();
  g_device = Device::kOutOfMemory;
  QCHECK(ModelCreateDeviceResource(0x100000) == kGameError);
  QCHECK(g_gameCalls == 1);
  QCHECK(g_binds.empty());
  QCHECK(g_errorCalls == 1);
  QCHECK(g_lastErrorCode == pinned::kVkAllocErrorCode);  // the error symbol the game's own Allocate failure carries
  QCHECK(g_lastErrorMessage.find("vkAllocateMemory") != std::string::npos);
  QCHECK(check::CurrentCounts().refused == 1);
}

// The refused result is cleared, as the game's Allocate leaves it before it fills it.
void ARefusedResultIsCleared() {
  Reset();
  Arm();
  g_device = Device::kOutOfMemory;
  const check::MemoryRequirements reqs = {0x2000, 0x100, 1};
  check::AllocatedBase base = {1, 2, 3, 4, 5};
  QCHECK(Entry()(&reqs, 1, 1, 1, 0, &base) == kGameError);
  QCHECK(base.memory == 0 && base.block == 0 && base.offset == 0 && base.end == 0 && base.mapped == 0);
}

// A device with memory: the game's result passes through untouched, with all six arguments as they came.
void AnAllocationWithMemoryPassesThrough() {
  Reset();
  Arm();
  const check::MemoryRequirements reqs = {0x3000, 0x200, 0x3};
  check::AllocatedBase base = {};
  QCHECK(Entry()(&reqs, 7, 1, 1, 4, &base) == 0);
  QCHECK(g_lastReqs == &reqs);
  QCHECK(g_lastArgs[0] == 7 && g_lastArgs[1] == 1 && g_lastArgs[2] == 1 && g_lastArgs[3] == 4);
  QCHECK(base.memory == kHandle && base.offset == kOffset && base.end == kOffset + 0x3000);
  QCHECK(g_errorCalls == 0);
  QCHECK(check::CurrentCounts().refused == 0);
  QCHECK(ModelCreateDeviceResource(0x1000) == 0);
  QCHECK(g_binds.size() == 1 && g_binds[0].memory == kHandle);
}

// The game's own error passes through untouched: no second error is made.
void TheGamesOwnErrorPassesThrough() {
  Reset();
  Arm();
  g_gameStatus = 0x4242;
  QCHECK(ModelCreateDeviceResource(0x1000) == 0x4242);
  QCHECK(g_errorCalls == 0);
  QCHECK(check::CurrentCounts().refused == 0);
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

// The first refusal and every 256th are logged with the request, the table's fill and the free guard's leaks, so the next
// crash report says which way the GPU ran out.
void TheLogSaysWhatWasRefusedAndHowFullTheTableWas() {
  Reset();
  g_lines.clear();
  sentinel::SetLogSink(&CaptureLine);
  Arm();
  g_blockCount = 300;
  g_tableCount = 450;
  g_device = Device::kOutOfMemory;
  for (int i = 0; i < 600; ++i) ModelCreateDeviceResource(0x155000);
  QCHECK(CountLines("\"event\":\"vk_alloc_refused\"") == 3);  // the 1st, the 256th and the 512th
  QCHECK(CountLinesWithBoth("\"event\":\"vk_alloc_refused\"", "\"size\":1396736") == 3);
  QCHECK(CountLines("\"table_count\":450") == 3);
  QCHECK(CountLines("\"block_count\":300") == 3);
  QCHECK(CountLines("\"guard_leaked\":") == 3);
  QCHECK(CountLines("\"guard_high_water\":") == 3);
  QCHECK(CountLines("\"refused\":1,") == 1 && CountLines("\"refused\":256") == 1 && CountLines("\"refused\":512") == 1);
  QCHECK(CountLines("\"pooled\":1") == 3);
  QCHECK(check::CurrentCounts().refused == 600);
  QCHECK(NullBinds() == 0);
  sentinel::SetLogSink(&CaptureLine);
}

// An error maker that returns 0 would let the caller bind: that is logged every time, and counted.
void AZeroErrorCodeIsLoudEveryTime() {
  Reset();
  g_lines.clear();
  sentinel::SetLogSink(&CaptureLine);
  Arm();
  g_errorReturns = 0;
  g_device = Device::kOutOfMemory;
  ModelCreateDeviceResource(0x1000);
  ModelCreateDeviceResource(0x1000);
  QCHECK(check::CurrentCounts().zero_code == 2);
  QCHECK(CountLinesWithBoth("\"event\":\"vk_alloc_refused\"", "\"error_code_zero\":1") == 2);
  sentinel::SetLogSink(&CaptureLine);
}

// ---- the bound ----------------------------------------------------------------------------------------------------------------

// A refusal leaves its null block in libr15's arrays, which have no bound check: one entry below the block bound it is refused,
// at the bound the game gets its null result, crashes where it did before the hook, and the decision is logged once.
void TheBlockArrayBoundDecidesBetweenARefusalAndTheGamesCrash() {
  Reset();
  g_lines.clear();
  sentinel::SetLogSink(&CaptureLine);
  Arm();
  g_device = Device::kOutOfMemory;
  g_tableCount = 100;
  g_blockCount = check::kBlockRefuseBelow - 1;
  QCHECK(ModelCreateDeviceResource(0x1000) == kGameError);
  QCHECK(NullBinds() == 0);
  QCHECK(check::CurrentCounts().refused == 1);
  g_blockCount = check::kBlockRefuseBelow;
  QCHECK(ModelCreateDeviceResource(0x1000) == 0);
  QCHECK(NullBinds() == 1);  // the game's own crash, where it always was
  g_blockCount = check::kBlockCapacity;
  ModelCreateDeviceResource(0x1000);
  QCHECK(NullBinds() == 2);
  QCHECK(check::CurrentCounts().refused == 1);
  QCHECK(check::CurrentCounts().let_through == 2);
  QCHECK(CountLines("\"event\":\"vk_alloc_let_through\"") == 1);
  QCHECK(CountLinesWithBoth("\"event\":\"vk_alloc_let_through\"", "\"block_count\":496") == 1);
  QCHECK(CountLinesWithBoth("\"event\":\"vk_alloc_let_through\"", "\"block_refuse_below\":496") == 1);
  sentinel::SetLogSink(&CaptureLine);
}

// Past the bound the game's result is exactly what the game's Allocate left (the null handle, its offset and its end), and no
// error is made: the game crashes in the bind as it did before the hook.
void ALetThroughLeavesTheGamesResultAndMakesNoError() {
  for (const bool blockFull : {true, false}) {
    Reset();
    Arm();
    g_device = Device::kOutOfMemory;
    g_blockCount = blockFull ? check::kBlockRefuseBelow : 10;
    g_tableCount = blockFull ? 10 : nevr_quest::vk_hole::kGuardFrom;
    const check::MemoryRequirements reqs = {0x2000, 0x100, 1};
    check::AllocatedBase base = {};
    QCHECK(Entry()(&reqs, 1, 1, 1, 0, &base) == 0);
    QCHECK(g_errorCalls == 0);
    QCHECK(base.memory == 0 && base.offset == kOffset && base.end == kOffset + 0x2000);
    QCHECK(check::CurrentCounts().let_through == 1 && check::CurrentCounts().refused == 0);
  }
}

// The free-range array's bound is the free guard's: below it a refusal, at it the game's result.
void TheFreeRangeArrayBoundDecidesToo() {
  Reset();
  Arm();
  g_device = Device::kOutOfMemory;
  g_blockCount = 10;
  g_tableCount = nevr_quest::vk_hole::kGuardFrom - 1;
  QCHECK(ModelCreateDeviceResource(0x1000) == kGameError);
  QCHECK(NullBinds() == 0);
  g_tableCount = nevr_quest::vk_hole::kGuardFrom;
  QCHECK(ModelCreateDeviceResource(0x1000) == 0);
  QCHECK(NullBinds() == 1);
  QCHECK(check::CurrentCounts().refused == 1 && check::CurrentCounts().let_through == 1);
}

// A count that cannot be read is not room: the game's result goes through.
void AnUnreadableCountLetsTheGamesResultThrough() {
  Reset();
  check::ArmForTest(&ModelCreateAndAddError, nullptr, &g_tableCount);
  g_device = Device::kOutOfMemory;
  QCHECK(ModelCreateDeviceResource(0x1000) == 0);
  QCHECK(NullBinds() == 1);
  QCHECK(check::CurrentCounts().refused == 0 && check::CurrentCounts().let_through == 1);
}

// The margin is the one the header gives: 16 entries, twice the free guard's headroom.
void TheMarginIsTwiceTheFreeGuardsHeadroom() {
  QCHECK(check::kBlockHeadroom == 2 * nevr_quest::vk_hole::kHeadroom);
  QCHECK(check::kBlockRefuseBelow == 496);
  QCHECK(check::kBlockCapacity == pinned::vk_block_layout::kCapacity);
  QCHECK(pinned::vk_block_layout::kCountVaddr - pinned::vk_block_layout::kTableVaddr ==
         static_cast<std::uint64_t>(pinned::vk_block_layout::kCapacity) * pinned::vk_block_layout::kEntryBytes);
  // `ldr w12, [x23, #0x3000]` in the pinned block append: the count is 0x3000 past the base.
  QCHECK(((pinned::kVkAllocBlockAppend.code[0] >> 10) & 0xfff) * 4 == 0x3000);
}

// ---- 32-bit results -----------------------------------------------------------------------------------------------------------

// Allocate's callers test w0, so a status whose low word is 0 is a success to them, whatever x0's high word holds.
std::uint64_t g_wideStatus = 0;
std::uint64_t ModelAllocateWide(const check::MemoryRequirements* reqs, unsigned, unsigned, unsigned, unsigned, check::AllocatedBase* out) {
  ++g_gameCalls;
  std::memset(out, 0, sizeof(*out));
  out->end = reqs->size;
  return g_wideStatus;
}

void ALowWordOfZeroIsASuccessToTheCallers() {
  Reset();
  *check::Thunk::OriginalOut() = reinterpret_cast<void*>(&ModelAllocateWide);
  Arm();
  g_wideStatus = 0xdead00000000ULL;  // w0 is 0: the caller binds
  QCHECK(ModelCreateDeviceResource(0x1000) == kGameError);
  QCHECK(NullBinds() == 0);
  QCHECK(check::CurrentCounts().refused == 1);
}

// The error maker returns an int; the handler returns it zero-extended, as the game's own error return leaves w0.
void TheErrorIdIsReturnedAsAThirtyTwoBitValue() {
  Reset();
  Arm();
  g_device = Device::kOutOfMemory;
  g_errorReturns = -2;
  const check::MemoryRequirements reqs = {0x1000, 0x100, 1};
  check::AllocatedBase base = {};
  QCHECK(Entry()(&reqs, 1, 1, 1, 0, &base) == 0xfffffffeULL);
  QCHECK(check::CurrentCounts().zero_code == 0);
}

// ---- the install ------------------------------------------------------------------------------------------------------------

// An image with the pinned words where the check reads them.
constexpr std::size_t kImageBytes = 0x37a0000;
unsigned char* MakeImage() {
  void* mem = mmap(nullptr, kImageBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) return nullptr;
  auto* image = static_cast<unsigned char*>(mem);
  for (const pinned::PinnedFunction* f : {&pinned::kVkAllocate, &pinned::kErrMsgCreateAndAddError}) {
    std::memcpy(image + f->vaddr, f->code, f->words * sizeof(std::uint32_t));
  }
  for (const pinned::PinnedCode* b :
       {&pinned::kVkAllocTableAdrp, &pinned::kVkAllocTableAdd, &pinned::kVkAllocFreeAppend, &pinned::kVkAllocBlockAppend}) {
    std::memcpy(image + b->vaddr, b->code, b->words * sizeof(std::uint32_t));
  }
  const std::uint64_t code = pinned::kVkAllocErrorCode;
  std::memcpy(image + pinned::kVkAllocErrorCodeVaddr, &code, sizeof(code));
  return image;
}

// libr15 is not loaded on a host, so the slot does not resolve: the install refuses and leaves nothing armed.
void AnInstallWhoseSlotDoesNotResolveLeavesTheGamesAllocate() {
  Reset();
  unsigned char* image = MakeImage();
  QCHECK(image != nullptr);
  if (image == nullptr) return;
  g_lines.clear();
  sentinel::SetLogSink(&CaptureLine);
  QCHECK(!check::InstallAt(image, pinned::LibR15VkAllocate()));
  sentinel::SetLogSink(&CaptureLine);
  QCHECK(CountLines("\"event\":\"vk_alloc_check_install\"") == 1);
  QCHECK(CountLinesWithBoth("\"event\":\"vk_alloc_check_install\"", "\"status\":\"module_not_loaded\"") == 1);
  g_device = Device::kOutOfMemory;
  ModelCreateDeviceResource(0x1000);
  QCHECK(g_errorCalls == 0);  // nothing armed: the entry is the game's call
  munmap(image, kImageBytes);
}

// Each pin is checked: a word that is not the pinned image's refuses the install, naming the pin, before any slot.
void APinnedWordThatDiffersRefusesTheInstall() {
  struct Case {
    std::uint64_t vaddr;
    const char* name;
  };
  const Case cases[] = {{pinned::kVkAllocate.vaddr + 4, pinned::kVkAllocate.name},
                        {pinned::kErrMsgCreateAndAddError.vaddr + 12, pinned::kErrMsgCreateAndAddError.name},
                        {pinned::kVkAllocTableAdrp.vaddr + 4, pinned::kVkAllocTableAdrp.name},
                        {pinned::kVkAllocTableAdd.vaddr, pinned::kVkAllocTableAdd.name},
                        {pinned::kVkAllocFreeAppend.vaddr + 8, pinned::kVkAllocFreeAppend.name},
                        {pinned::kVkAllocBlockAppend.vaddr + 20, pinned::kVkAllocBlockAppend.name},
                        {pinned::kVkAllocErrorCodeVaddr + 7, "CGRawInterfaceVK::Allocate error symbol"}};
  for (const Case& c : cases) {
    Reset();
    unsigned char* image = MakeImage();
    QCHECK(image != nullptr);
    if (image == nullptr) return;
    image[c.vaddr] ^= 1;
    g_lines.clear();
    sentinel::SetLogSink(&CaptureLine);
    QCHECK(!check::InstallAt(image, pinned::LibR15VkAllocate()));
    sentinel::SetLogSink(&CaptureLine);
    QCHECK(CountLines("\"why\":\"pin_mismatch\"") == 1);
    QCHECK(CountLines(c.name) == 1);
    QCHECK(CountLines("\"status\"") == 0);  // refused before the GOT was asked
    munmap(image, kImageBytes);
  }
  Reset();
  g_lines.clear();
  sentinel::SetLogSink(&CaptureLine);
  QCHECK(!check::InstallAt(nullptr, pinned::LibR15VkAllocate()));
  sentinel::SetLogSink(&CaptureLine);
  QCHECK(CountLines("\"why\":\"no_image\"") == 1);
}

}  // namespace

int main() {
  sentinel::SetLogSink(&CaptureLine);  // the default sink writes every line to stderr
  WithoutTheCheckAnOutOfMemoryDeviceIsBoundAsNull();
  AnAllocationWithNoMemoryIsRefusedAndNotBound();
  ARefusedResultIsCleared();
  AnAllocationWithMemoryPassesThrough();
  TheGamesOwnErrorPassesThrough();
  TheLogSaysWhatWasRefusedAndHowFullTheTableWas();
  AZeroErrorCodeIsLoudEveryTime();
  TheBlockArrayBoundDecidesBetweenARefusalAndTheGamesCrash();
  ALetThroughLeavesTheGamesResultAndMakesNoError();
  TheFreeRangeArrayBoundDecidesToo();
  AnUnreadableCountLetsTheGamesResultThrough();
  TheMarginIsTwiceTheFreeGuardsHeadroom();
  ALowWordOfZeroIsASuccessToTheCallers();
  TheErrorIdIsReturnedAsAThirtyTwoBitValue();
  AnInstallWhoseSlotDoesNotResolveLeavesTheGamesAllocate();
  APinnedWordThatDiffersRefusesTheInstall();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "vk_alloc_check_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("vk_alloc_check_test: all checks pass\n");
  return 0;
}
