// NRadEngine::CHashTable<NRadEngine::SBroadcastPeer, unsigned long long, &NRadEngine::CastHashKey<unsigned long long>>::Destroy
// Quest libr15.so VA 0x260c3b0; ReVault binary 576.
// ReVault raw parent 100318504, reconstruction node 100600782; grade H: all 95 instructions (0x260c3b0-0x260c528, ret at 0x260c518) read and mapped to the code below, every callee and type name is ReVault's (placeholders: none), table and element fields are raw offsets, the bool result is w0 = 0 or 1.
// @0x260c3b0 — NRadEngine::CHashTable<NRadEngine::SBroadcastPeer, unsigned long long, &NRadEngine::CastHashKey<unsigned long long>>::Destroy
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

struct CAllocator;

struct CMemoryContext {
  // NRadEngine::CMemoryContext::CurrentPtr(), Quest libr15.so VA 0x12cae90 (thunk 0xffbc50); the merged
  // cmemory_context_current_ptr.cpp declares it the same way.
  static CAllocator* CurrentPtr();
  // NRadEngine::CMemoryContext::SetCurrent(CAllocator*), Quest libr15.so VA 0x12cafac (thunk 0xfd3590); the merged
  // cmemory_context_set_current.cpp declares it the same way.
  static void SetCurrent(CAllocator* allocator);
};

// A CMemBlock is 0x38 bytes (static_assert in the merged session_api_execute.cpp and chttp_listener_request_body.cpp).
// Allocator() is NRadEngine::CMemBlock::Allocator(), VA 0x12dc610 (thunk 0x1050e50): `ldr x0, [x0, #0x10]; ret`, so it
// returns the CAllocator* stored at +0x10 of the block that x0 points at.
struct CMemBlock {
  alignas(8) std::byte raw_[0x38];

  CAllocator* Allocator();
};
static_assert(sizeof(CMemBlock) == 0x38);
static_assert(alignof(CMemBlock) == 8);

// NRadEngine::SBroadcastPeer; ReVault has no type record for it. Layout not recovered: this function only runs its
// destructor in place, at element + 0x18 of the table below. ~SBroadcastPeer() is VA 0x260c52c (thunk 0x1051050).
struct SBroadcastPeer {
  ~SBroadcastPeer();
};

// NRadEngine::CastHashKey<unsigned long long>(unsigned long long const&) returns an unsigned short and is the hash
// function named in the table's template arguments. The compiled Destroy has no call to it: it is the single
// `and x9, x15, #0xfff` at 0x260c3e8, so the low 12 bits of the key select the bucket.
template <typename K>
unsigned short CastHashKey(const K& key);

// NRadEngine::CHashTable<T, K, hash>. ReVault has no type record for it. The storage is the extent the code touches:
// this function and ~CHashTable (0x2613d68) read bytes 0x00-0x63, and AllocPage of the sibling instantiation at
// 0x2613ebc reads the u32 at +0x64, so the modelled object is 0x68 bytes (alignas(8) from its u64 members). The real
// object size is not recorded anywhere, so the static_asserts pin this modelled extent only.
//
// Fields as raw offsets (the roles are inferred from how Destroy and AllocPage use them):
//   +0x00 u32* bucket array, 4096 entries, each the flat index of the first element of its chain or 0xffffffff
//   +0x18 CMemBlock whose first word is the array of page pointers; its allocator (+0x28 of the table) is the one
//         Destroy makes current while it runs
//   +0x50 u32 head of the free list of elements (0xffffffff when empty); ~CHashTable resets it to 0xffffffff
//   +0x54 u32 tail of the doubly linked list of live elements (0xffffffff when empty)
//   +0x58 u32 free-element count: AllocPage adds the page size to it, Destroy adds 1
//   +0x5c u32 live-element count: Destroy subtracts 1
//   +0x60 u32 elements per page
template <typename T, typename K, unsigned short (*HashKey)(const K&)>
class CHashTable {
 public:
  // NRadEngine::CHashTable<...>::Destroy(unsigned long long const&); Quest libr15.so VA 0x260c3b0, reached from the
  // PLT stub 0x1034f90. Removes the element stored under `key`; returns true when one was found. Ghidra shows an
  // undefined4 result: the code leaves 1 or 0 in w0.
  bool Destroy(const K& key);

 private:
  alignas(8) std::byte raw_[0x68];
};

}  // namespace NRadEngine

namespace {

// One element is 0x948 bytes (`mov w13, #0x948`): +0x00 u32 next index in its bucket chain (or, when the element is
// free, in the free list), +0x04 u32 zero, +0x08 u32 previous index in the list of live elements, +0x0c u32 next index
// in that list, +0x10 u64 key, +0x18 the SBroadcastPeer (0x930 bytes). 0xffffffff means "none" in every index field.
constexpr std::uint64_t kElementStride = 0x948;
constexpr std::uint32_t kNone = 0xffffffffu;

std::uint32_t LoadU32(const std::byte* base, std::size_t offset) {
  std::uint32_t value = 0;
  std::memcpy(&value, base + offset, sizeof value);
  return value;
}

std::uint64_t LoadU64(const std::byte* base, std::size_t offset) {
  std::uint64_t value = 0;
  std::memcpy(&value, base + offset, sizeof value);
  return value;
}

std::byte* LoadBytePtr(const std::byte* base, std::size_t offset) {
  std::byte* value = nullptr;
  std::memcpy(&value, base + offset, sizeof value);
  return value;
}

void StoreU32(std::byte* base, std::size_t offset, std::uint32_t value) {
  std::memcpy(base + offset, &value, sizeof value);
}

void StoreU64(std::byte* base, std::size_t offset, std::uint64_t value) {
  std::memcpy(base + offset, &value, sizeof value);
}

// Flat element index -> element address: page = index / per_page, slot = index - page * per_page, then
// pages[page] + slot * 0x948 (udiv, msub, umaddl at 0x260c404-0x260c414). udiv on AArch64 yields 0 for a zero divisor
// and the compiled code has no check, so a zero per_page gives page 0 here instead of a division by zero.
std::byte* ElementAt(const std::byte* pages, std::uint32_t per_page, std::uint32_t index) {
  const std::uint32_t page = per_page == 0 ? 0u : index / per_page;
  const std::uint32_t slot = index - page * per_page;
  return LoadBytePtr(pages, static_cast<std::size_t>(page) * 8) + static_cast<std::uint64_t>(slot) * kElementStride;
}

}  // namespace

namespace NRadEngine {

// @0x260c3b0
template <>
bool CHashTable<SBroadcastPeer, unsigned long long, &CastHashKey<unsigned long long>>::Destroy(
    const unsigned long long& key) {
  std::byte* const self = raw_;

  // 0x260c3c0-0x260c3dc: the allocator of the CMemBlock at +0x18 becomes the current memory context; the previous
  // one is kept in x19 and put back on both exits (0x260c504).
  CAllocator* const allocator = reinterpret_cast<CMemBlock*>(self + 0x18)->Allocator();
  CAllocator* const previous = CMemoryContext::CurrentPtr();
  CMemoryContext::SetCurrent(allocator);

  // 0x260c3e0-0x260c3f4: the bucket slot of the key; an empty chain means the key is absent.
  const std::uint64_t search_key = key;
  std::byte* link = LoadBytePtr(self, 0x00) + (search_key & 0xfffu) * 4;
  std::uint32_t index = LoadU32(link, 0);

  bool destroyed = false;
  if (index != kNone) {
    // 0x260c3fc-0x260c408: elements per page and the page-pointer array are read once.
    const std::uint32_t per_page = LoadU32(self, 0x60);
    const std::byte* const pages = LoadBytePtr(self, 0x18);

    // 0x260c40c-0x260c44c: walk the bucket chain until the key at +0x10 matches. `link` follows along as the address
    // of the u32 that holds the current element's index (the bucket slot, then the previous element's +0x00).
    std::byte* element = ElementAt(pages, per_page, index);
    bool found = true;
    while (LoadU64(element, 0x10) != search_key) {
      link = element;
      index = LoadU32(element, 0x00);
      if (index == kNone) {
        found = false;
        break;
      }
      element = ElementAt(pages, per_page, index);
    }

    if (found) {
      // 0x260c450-0x260c4a4: take the element out of the doubly linked list of live elements (+0x08 previous,
      // +0x0c next, tail at table +0x54).
      const std::uint32_t list_next = LoadU32(element, 0x0c);
      std::uint32_t list_previous = LoadU32(element, 0x08);
      if (list_next == kNone) {
        // 0x260c51c: it was the tail.
        StoreU32(self, 0x54, list_previous);
      } else {
        // 0x260c46c-0x260c484: the next element's previous link now skips this one; the previous link of this
        // element is read again afterwards.
        StoreU32(ElementAt(pages, per_page, list_next), 0x08, list_previous);
        list_previous = LoadU32(element, 0x08);
      }
      if (list_previous != kNone) {
        // 0x260c490-0x260c4a4 (also reached from 0x260c524): the previous element's next link skips this one.
        StoreU32(ElementAt(pages, per_page, list_previous), 0x0c, list_next);
      }

      // 0x260c4a8-0x260c4b0: unlink it from the bucket chain: *link takes the element's chain successor.
      const std::uint32_t chain_next = LoadU32(element, 0x00);
      const std::uint32_t element_index = LoadU32(link, 0);
      StoreU32(link, 0, chain_next);

      // 0x260c4b4-0x260c4c4: push it on the free list: its +0x00 takes the old free-list head.
      const std::uint32_t free_head = LoadU32(self, 0x50);
      StoreU32(element, 0x00, free_head);

      // 0x260c4b8-0x260c4e4: `add v0.2S` of the pair {1, 0xffffffff} (data at 0x2c2f958) to the u32 pair at +0x58:
      // the free count goes up by one and the live count down by one; the free-list head becomes this element.
      const std::uint64_t counts = LoadU64(self, 0x58);
      const std::uint32_t free_count = static_cast<std::uint32_t>(counts) + 1u;
      const std::uint32_t live_count = static_cast<std::uint32_t>(counts >> 32) + 0xffffffffu;
      StoreU32(self, 0x50, element_index);
      StoreU64(self, 0x58, (static_cast<std::uint64_t>(live_count) << 32) | free_count);

      // 0x260c4e8-0x260c4ec: the element's +0x04 becomes 0 and +0x08 becomes 0xffffffff (one 64-bit store at +0x04,
      // value 0xffffffff00000000), and its +0x0c becomes 0xffffffff.
      StoreU64(element, 0x04, 0xffffffff00000000ull);
      StoreU32(element, 0x0c, kNone);

      // 0x260c4d4, 0x260c4f0: ~SBroadcastPeer on the payload at element + 0x18.
      reinterpret_cast<SBroadcastPeer*>(element + 0x18)->~SBroadcastPeer();
      destroyed = true;
    }
  }

  // 0x260c500-0x260c508: the previous memory context is restored and w0 = 1 or 0 is returned.
  CMemoryContext::SetCurrent(previous);
  return destroyed;
}

// The modelled extent declared above (raw_[0x68], alignas(8)); self-consistency of this file, not a recovered size.
static_assert(sizeof(CHashTable<SBroadcastPeer, unsigned long long, &CastHashKey<unsigned long long>>) == 0x68);
static_assert(alignof(CHashTable<SBroadcastPeer, unsigned long long, &CastHashKey<unsigned long long>>) == 8);

}  // namespace NRadEngine
