/* The game code the combat level answer (combat_level.h) reads its offsets from, pinned to the store libr15 (build id in pinned_targets.h):
 * the words at each address are the instructions that load the offset the module uses, so a build that moves one fails
 * combat_level_pinned_test (got_pinned_test.cpp, `just test-quest-hooks-pinned`, which needs the store APK) instead of the module reading
 * the wrong word. The words and addresses are the store file's (`llvm-objdump -d`); nothing here is executed.
 *
 *   CncaGame::LoadedGameSpace(CSymbol64) @ 0x17f8ab8 reads the two gamespace tables: `ldr x8,[x0,#0x1f78]` (count 0), `ldr x0,[x20,#0x1f48]`
 *   (rows 0), `ldr x9,[x20,#0x1f48]`, `ldr x1,[x20,#0x1fb8]` (count 1) and `ldr x0,[x20,#0x1f88]` (rows 1).
 *   CncaGameSpace::Initialize @ 0x17f51c0 stores the game at gs+0x8: `stp x1, x2, [x0]` (allocator at +0, game at +8).
 *   CncaGameSpace::LoadLevel @ 0x17e5484 stores the level at gs+0x58: `str x22, [x19, #0x58]` at 0x17e54ec.
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

inline constexpr PinnedWord kOffsetWords[] = {
    {"LoadedGameSpace: count 0 at +0x1f78", 0x17f8ac4ULL, 0xf94fbc08U},  // ldr x8, [x0, #0x1f78]
    {"LoadedGameSpace: rows 0 at +0x1f48", 0x17f8ae0ULL, 0xf94fa680U},   // ldr x0, [x20, #0x1f48]
    {"LoadedGameSpace: rows 0 at +0x1f48 (again)", 0x17f8b08ULL, 0xf94fa689U},  // ldr x9, [x20, #0x1f48]
    {"LoadedGameSpace: count 1 at +0x1fb8", 0x17f8b4cULL, 0xf94fde81U},  // ldr x1, [x20, #0x1fb8]
    {"LoadedGameSpace: rows 1 at +0x1f88", 0x17f8b60ULL, 0xf94fc680U},   // ldr x0, [x20, #0x1f88]
    {"Initialize: the game at gs+0x8", 0x17f51d8ULL, 0xa9000801U},       // stp x1, x2, [x0]
    {"LoadLevel: the level at gs+0x58", 0x17e54ecULL, 0xf9002e76U},      // str x22, [x19, #0x58]
};

}  // namespace nevr_quest::combat::level::targets
