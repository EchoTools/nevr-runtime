/* The game code the combat level answer (combat_level.h) reads its offsets from, pinned to the store libr15 (build id in pinned_targets.h):
 * the words at each address are the instructions that load the offset the module uses, so a build that moves one fails
 * combat_level_pinned_test (got_pinned_test.cpp, `just test-quest-hooks-pinned`, which needs the store APK) instead of the module reading
 * the wrong word. The words and addresses are the store file's (`llvm-objdump -d`); nothing here is executed.
 *
 *   CncaGame::LoadedGameSpace(CSymbol64) @ 0x17f8ab8 reads the two gamespace tables: `ldr x8,[x0,#0x1f78]` (count 0), `ldr x0,[x20,#0x1f48]`
 *   (rows 0), `ldr x9,[x20,#0x1f48]`, `ldr x1,[x20,#0x1fb8]` (count 1) and `ldr x0,[x20,#0x1f88]` (rows 1).
 *   CncaGameSpace::Initialize @ 0x17f51c0 stores the game at gs+0x8: `stp x1, x2, [x0]` (allocator at +0, game at +8).
 *   CncaGameSpace::LoadLevel @ 0x17e5484 stores the level at gs+0x58: `str x22, [x19, #0x58]` at 0x17e54ec.
 *   CncaGame::CreateGameSpace @ 0x17f4a00 inserts the new CGameSpaceInitFiber into the in-flight table at game+0x1ed8 (`mov w8,#0x1ed8` at
 *   0x17f50a4); CncaGame::UpdateGameSpaceInit @ 0x17ecc7c reads that table (`ldr x9,[x8,x23,lsl #3]` at 0x17ece30) and its count (`ldr x8,[x19,#0x1f08]`
 *   at 0x17ed484); CGameSpaceInitFiber::Run @ 0x17e4d34 reads the fiber's gamespace at fiber+0x30 (`ldr x0,[x0,#0x30]` at 0x17e4d44). A gamespace
 *   enters the two tables above only when its init fiber is done (UpdateGameSpaceInit inserts it at 0x17ed480, on the path taken when the fiber's
 *   done bit, set at 0x17e4d6c after CncaGameSpace::LoadLevel returns, is set), and LoadLevel stores the level at gs+0x58 early (0x17e54ec) and then
 *   builds the level's resources, so while a level's resources are built only the in-flight table holds its gamespace.
 *   CR15Game's vtable (`_ZTVN10NRadEngine8NRadGame8CR15GameE` at 0x362ed20) holds its first virtual at +0x10, which is the value an object's first
 *   word carries (combat_tick_targets.h kGameVptrVaddr = 0x362ed30).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace nevr_quest::combat::level::targets {

struct PinnedWord {
  const char* what;
  std::uint64_t vaddr;
  std::uint32_t word;
};

inline constexpr const char* kLoadedGameSpaceSymbol = "_ZN10NRadEngine8CncaGame15LoadedGameSpaceENS_9CSymbol64E";
inline constexpr std::uint64_t kLoadedGameSpaceVaddr = 0x17f8ab8ULL;
inline constexpr const char* kInitializeSymbol = "_ZN10NRadEngine13CncaGameSpace10InitializeEPNS_10CAllocatorEPNS_8CncaGameE";
inline constexpr std::uint64_t kInitializeVaddr = 0x17f51c0ULL;
inline constexpr const char* kLoadLevelSymbol = "_ZN10NRadEngine13CncaGameSpace9LoadLevelENS_9CSymbol64ERNS_20SncaLevelLoadOptionsE";
inline constexpr std::uint64_t kLoadLevelVaddr = 0x17e5484ULL;
inline constexpr const char* kCR15GameVtableSymbol = "_ZTVN10NRadEngine8NRadGame8CR15GameE";
inline constexpr std::uint64_t kCR15GameVtableVaddr = 0x362ed20ULL;

// The offsets combat_level.cpp reads, in one place, tied to the pinned words below at compile time: an `ldr x, [x, #imm]` (64-bit, unsigned offset)
// carries imm/8 in bits 10-21, a `stp x1, x2, [x0]` stores its second register at +8, so a changed offset or a changed word fails the build.
inline constexpr std::uintptr_t kTable0Rows = 0x1f48, kTable0Count = 0x1f78, kTable1Rows = 0x1f88, kTable1Count = 0x1fb8;
inline constexpr std::uintptr_t kGamespaceOwnerOffset = 0x8;   // gs+0x8: the game
inline constexpr std::uintptr_t kGamespaceLevelOffset = 0x58;  // gs+0x58: the level

// The in-flight init fibers: rows of 8-byte CGameSpaceInitFiber* at game+0x1ed8, count at game+0x1f08, the fiber's gamespace at fiber+0x30.
inline constexpr std::uintptr_t kFiberRows = 0x1ed8, kFiberCount = 0x1f08, kFiberGamespace = 0x30;

constexpr std::uintptr_t LdrStrOffset(std::uint32_t word) { return static_cast<std::uintptr_t>((word >> 10) & 0xfff) * 8; }

inline constexpr PinnedWord kOffsetWords[] = {
    {"LoadedGameSpace: count 0 at +0x1f78", 0x17f8ac4ULL, 0xf94fbc08U},  // ldr x8, [x0, #0x1f78]
    {"LoadedGameSpace: rows 0 at +0x1f48", 0x17f8ae0ULL, 0xf94fa680U},   // ldr x0, [x20, #0x1f48]
    {"LoadedGameSpace: rows 0 at +0x1f48 (again)", 0x17f8b08ULL, 0xf94fa689U},  // ldr x9, [x20, #0x1f48]
    {"LoadedGameSpace: count 1 at +0x1fb8", 0x17f8b4cULL, 0xf94fde81U},  // ldr x1, [x20, #0x1fb8]
    {"LoadedGameSpace: rows 1 at +0x1f88", 0x17f8b60ULL, 0xf94fc680U},   // ldr x0, [x20, #0x1f88]
    {"Initialize: the game at gs+0x8", 0x17f51d8ULL, 0xa9000801U},       // stp x1, x2, [x0]
    {"LoadLevel: the level at gs+0x58", 0x17e54ecULL, 0xf9002e76U},      // str x22, [x19, #0x58]
    {"CreateGameSpace: the fiber rows at game+0x1ed8", 0x17f50a4ULL, 0x5283db08U},  // mov w8, #0x1ed8
    {"UpdateGameSpaceInit: the fiber count at game+0x1f08", 0x17ed484ULL, 0xf94f8668U},  // ldr x8, [x19, #0x1f08]
    {"CGameSpaceInitFiber::Run: the gamespace at fiber+0x30", 0x17e4d44ULL, 0xf9401800U},  // ldr x0, [x0, #0x30]
    {"UpdateGameSpaceInit: a fiber is a pointer in the rows", 0x17ece30ULL, 0xf8777909U},  // ldr x9, [x8, x23, lsl #3]
};

static_assert(LdrStrOffset(kOffsetWords[0].word) == kTable0Count, "the pinned word that loads count 0 does not carry kTable0Count");
static_assert(LdrStrOffset(kOffsetWords[1].word) == kTable0Rows, "the pinned word that loads rows 0 does not carry kTable0Rows");
static_assert(LdrStrOffset(kOffsetWords[2].word) == kTable0Rows, "the second pinned load of rows 0 does not carry kTable0Rows");
static_assert(LdrStrOffset(kOffsetWords[3].word) == kTable1Count, "the pinned word that loads count 1 does not carry kTable1Count");
static_assert(LdrStrOffset(kOffsetWords[4].word) == kTable1Rows, "the pinned word that loads rows 1 does not carry kTable1Rows");
static_assert(kOffsetWords[5].word == 0xa9000801U && kGamespaceOwnerOffset == sizeof(void*),
              "`stp x1, x2, [x0]` stores the game at +8: kGamespaceOwnerOffset must be 8");
static_assert(LdrStrOffset(kOffsetWords[6].word) == kGamespaceLevelOffset, "the pinned store of the level does not carry kGamespaceLevelOffset");
static_assert(((kOffsetWords[7].word >> 5) & 0xffffU) == kFiberRows && (kOffsetWords[7].word & 0xffe00000U) == 0x52800000U,
              "the pinned `mov w8, #imm` does not carry kFiberRows");
static_assert(LdrStrOffset(kOffsetWords[8].word) == kFiberCount, "the pinned load of the fiber count does not carry kFiberCount");
static_assert(LdrStrOffset(kOffsetWords[9].word) == kFiberGamespace, "the pinned load of the fiber's gamespace does not carry kFiberGamespace");
static_assert((kOffsetWords[10].word & 0xffe0fc00U) == 0xf8607800U, "the pinned load is not `ldr x, [x, x, lsl #3]` (8-byte rows)");

}  // namespace nevr_quest::combat::level::targets
