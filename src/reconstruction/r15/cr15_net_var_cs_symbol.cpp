// NRadEngine::NRadGame::CR15NetVarCS::Symbol
// Quest libr15.so VA 0x22bdd9c; ReVault binary 576.
// ReVault raw parent 100304956, reconstruction node 100600807; grade M: all 92 instructions (0x22bdd9c-0x22bdf08) mapped and checked, 0 placeholders; the ldar/ldaxr/stlxr lock sequence is modelled as a compare-exchange, so it is not byte-exact; ReVault names the thunk at 0x1007410 thunk_FUN_02401168 and the function 0x2401168 RefreshCache with no class, so the CComponentHandle<CR15NetVarCS> binding rests on the thunk's GOT slot 0x37db0f0 (not byte-checked) and the vslot 0x1d0 use.
// @0x22bdd9c — NRadEngine::NRadGame::CR15NetVarCS::Symbol
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

struct CSymbol64 {
  std::uint64_t key;
};
static_assert(sizeof(CSymbol64) == 0x08);

// A compact pool handle, passed in w1. Its low 16 bits index the table at component+0xd0 (IsComponentValid, 0x23564e4).
struct SCompactPoolHandle {
  std::uint32_t index;
};
static_assert(sizeof(SCompactPoolHandle) == 0x04);

// CComponentHandle<T>: 0x50 bytes. +0x00 owner (a pointer that RefreshCache tests for zero), +0x08 to +0x3f not recovered
// here, +0x40 the cached T*, +0x48 the compact pool handle, +0x4c the lock word (zero in a stack copy).
template <typename T>
class CComponentHandle {
 public:
  // NRadEngine::CComponentHandle<NRadEngine::NRadGame::CR15NetVarCS>::RefreshCache() const. Reached through the thunk at
  // 0x1007410 (ReVault: thunk_FUN_02401168). The body at 0x2401168 is this template's refresh (owner+0x2130 stamp, lock word
  // at +0x4c). The thunk's GOT slot 0x37db0f0 holds 0x00ecfb40 on disk, so that binding is ReVault's and not byte-checked.
  void RefreshCache() const;

  std::uint64_t owner;                  // +0x00
  std::byte reserved[0x38];             // +0x08 to +0x3f
  T* component;                         // +0x40
  SCompactPoolHandle compact_handle;    // +0x48
  std::uint32_t lock_word;              // +0x4c
};

namespace NRadGame {

class CR15NetGameMT;

class CR15NetVarCS {
 public:
  // NRadEngine::NRadGame::CR15NetVarCS::Symbol(NRadEngine::NRadGame::CR15NetGameMT const&, NRadEngine::CSymbol64) @ 0x22bdd9c.
  // Static: x0 is the game and x1 the symbol. Returns the value stored for the symbol, or -1 (0xffffffffffffffff).
  static std::uint64_t Symbol(const CR15NetGameMT& game, CSymbol64 symbol);
};
static_assert(sizeof(CComponentHandle<CR15NetVarCS>) == 0x50);
static_assert(offsetof(CComponentHandle<CR15NetVarCS>, component) == 0x40);
static_assert(offsetof(CComponentHandle<CR15NetVarCS>, compact_handle) == 0x48);
static_assert(offsetof(CComponentHandle<CR15NetVarCS>, lock_word) == 0x4c);

}  // namespace NRadGame
}  // namespace NRadEngine

namespace {

// The -1 the function returns.
constexpr std::uint64_t kNotFound = ~std::uint64_t{0};

// Byte-offset read of an object whose layout is not recovered; the offsets are the listing's.
template <typename T>
T ReadAt(const void* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof value);
  return value;
}

// The function pointer in the vtable slot at byte offset `offset` of `object` (the vptr is the object's first word).
template <typename Fn>
Fn VirtualSlot(const void* object, std::size_t offset) {
  const void* const vtable = ReadAt<const void*>(object, 0);
  Fn fn{};
  std::memcpy(&fn, static_cast<const std::byte*>(vtable) + offset, sizeof fn);
  return fn;
}

}  // namespace

namespace NRadEngine {
namespace NRadGame {

std::uint64_t CR15NetVarCS::Symbol(const CR15NetGameMT& game, CSymbol64 symbol) {
  // 0x22bddb8-0x22bdde4: the handle at game+0x2e00 is copied to the stack: 0x48 bytes (+0x00..+0x47), then the u32 at
  // game+0x2e48 is the compact handle and +0x4c is stored as zero. The word at game+0x2e4c is RefreshCache's lock word
  // and is not copied. x19 = symbol.
  CComponentHandle<CR15NetVarCS> handle{};
  const auto* const game_bytes = static_cast<const std::byte*>(static_cast<const void*>(&game));
  std::memcpy(&handle, game_bytes + 0x2e00, 0x48);
  handle.compact_handle.index = ReadAt<std::uint32_t>(game_bytes, 0x2e48);
  handle.lock_word = 0;

  // 0x22bdde8-0x22bddec: RefreshCache on the copy, then x20 = the copy's +0x40 read after the call. The decompile shows the
  // value from before the call; the listing's load is after it, and that is the value used here.
  handle.RefreshCache();
  CR15NetVarCS* component = handle.component;

  // 0x22bddf0-0x22bddfc: a null component, or an owner of zero, returns -1 with no lock taken.
  if (component == nullptr || handle.owner == 0) {
    return kNotFound;
  }

  // 0x22bde00-0x22bde04: RefreshCache again.
  handle.RefreshCache();

  // 0x22bde08-0x22bde1c: vtable slot 0x1d0 of the component (IsComponentValid, vtable 0x379bc68, index 58), with the
  // compact handle as read after the refresh (ldr w1, [sp, #0x48]). A false result returns -1.
  using IsComponentValidFn = bool (*)(const void*, SCompactPoolHandle);
  if (!VirtualSlot<IsComponentValidFn>(component, 0x1d0)(component, handle.compact_handle)) {
    return kNotFound;
  }

  // 0x22bde20-0x22bde28: RefreshCache, then x20 = the cached component as read after the refresh.
  handle.RefreshCache();
  component = handle.component;

  // 0x22bde2c-0x22bde34: RefreshCache; the index is the low 16 bits of the handle as read after it.
  handle.RefreshCache();
  const auto index = static_cast<std::uint16_t>(handle.compact_handle.index);

  // 0x22bde38-0x22bde50: the u16 at index_table + 4*index is an entry number; the entries are 0x30 bytes each, from
  // component+0x100 (index_table is component+0xd0).
  const std::byte* const index_table = ReadAt<const std::byte*>(component, 0xd0);
  const std::byte* const entries = ReadAt<const std::byte*>(component, 0x100);
  const auto entry_number = ReadAt<std::uint16_t>(index_table, static_cast<std::size_t>(index) * 4);
  const std::byte* const entry = entries + static_cast<std::size_t>(entry_number) * 0x30;

  // 0x22bde54-0x22bde74: the entry's lock is the int* at entry+0x08. Spin while the word is nonzero, then take it from 0 to 1
  // (ldar, ldaxr, stlxr; modelled as a compare-exchange, so a spurious failure re-enters the spin read where the binary
  // retries the exclusive pair; the clrex of a lost exclusive is implicit).
  auto* const lock = ReadAt<std::int32_t*>(entry, 0x08);
  for (;;) {
    while (__atomic_load_n(lock, __ATOMIC_ACQUIRE) != 0) {
    }
    std::int32_t expected = 0;
    if (__atomic_compare_exchange_n(lock, &expected, 1, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      break;
    }
  }

  // 0x22bde78-0x22bde88: entry+0x10 is a sorted table: count at +0x38; the records pointer at +0x08 is loaded only when
  // the count is non-zero.
  const std::byte* const sorted = ReadAt<const std::byte*>(entry, 0x10);
  const auto count = ReadAt<std::uint64_t>(sorted, 0x38);
  const std::byte* const records = count != 0 ? ReadAt<const std::byte*>(sorted, 0x08) : nullptr;
  std::uint64_t result = kNotFound;

  // 0x22bde8c-0x22bdeec: binary search over the records, 0x120 bytes each, for the symbol: key at +0x00 (signed), value at
  // +0x118. A zero count, or a symbol that is not present, leaves -1.
  std::uint64_t lo = 0;
  std::uint64_t hi = count;
  while (lo < hi) {
    const std::uint64_t mid = lo + ((hi - lo) >> 1);
    const std::size_t record = static_cast<std::size_t>(mid) * 0x120;
    const auto key = ReadAt<std::int64_t>(records, record);
    if (key > static_cast<std::int64_t>(symbol.key)) {
      hi = mid;
    } else if (key == static_cast<std::int64_t>(symbol.key)) {
      result = ReadAt<std::uint64_t>(records, record + 0x118);
      break;
    } else {
      lo = mid + 1;
    }
  }

  // 0x22bdef8-0x22bdf08: the lock is released (stlr wzr) on every path that took it; the result is returned.
  __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
  return result;
}

}  // namespace NRadGame
}  // namespace NRadEngine
