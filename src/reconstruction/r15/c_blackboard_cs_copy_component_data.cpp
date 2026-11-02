// NRadEngine::CBlackboardCS::CopyComponentData
// Quest libr15.so VA 0x14fe938; ReVault binary 576.
// ReVault raw parent 100233064, reconstruction node 100600479; grade M: the node text carries @confidence M with no further prose; its comments name the table, the copy and the block growth.
// @0x14fe938 — NRadEngine::CBlackboardCS::CopyComponentData
/* @confidence: M */
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "cmemory_context.hpp"

namespace NRadEngine {

struct SActor {
  std::uint64_t bits;
};

// Passed as two 64-bit registers (x3, x4). The body reads the low 16 bits of word0 and all of word1.
struct SComponentID {
  std::uint64_t word0;
  std::uint64_t word1;
};

namespace CMemBlock {
// NRadEngine::CMemBlock::Resize, 0x12dc30c (thunk 0x103e400). The call site passes w2 = 1 as a third argument;
// destroy_peer_record.cpp declares the same three-argument form.
void Resize(void* block, std::uint64_t bytes, std::uint32_t copy_existing);
}  // namespace CMemBlock

namespace SBlackboardCD {
// One 0x250-byte entry of the property table. The fields the function reads are addressed by byte offset.
struct SProperties {
  // NRadEngine::SBlackboardCD::SProperties::SProperties, 0x1528be8 (thunk 0xfd8900).
  SProperties(const SProperties& source);
  std::byte storage[0x250];
};
static_assert(sizeof(SProperties) == 0x250);
}  // namespace SBlackboardCD

class CBlackboardCS {
 public:
  // The destination CMemBlock is x1, the actor is x2 and the component id is x3:x4.
  bool CopyComponentData(void* dst, SActor actor, SComponentID component) const;
};

}  // namespace NRadEngine

bool NRadEngine::CBlackboardCS::CopyComponentData(void* dst, SActor actor,
                                                  SComponentID component) const {
  constexpr std::size_t kEntrySize = 0x250;
  const auto* self = reinterpret_cast<const std::byte*>(this);
  auto* block = static_cast<std::byte*>(dst);

  // this+0xb8 holds the table; table+0x00 is its first entry and table+0x30 its entry count.
  std::uintptr_t table = 0;
  std::memcpy(&table, self + 0xb8, sizeof(table));
  std::uint64_t remaining = 0;
  std::memcpy(&remaining, reinterpret_cast<const std::byte*>(table) + 0x30, sizeof(remaining));
  if (remaining == 0) {
    return false;
  }
  std::uintptr_t first = 0;
  std::memcpy(&first, reinterpret_cast<const std::byte*>(table), sizeof(first));

  // Scan for the entry whose key (+0x00) equals word1 and whose halfword (+0x10) equals word0's low 16 bits.
  const std::byte* found = reinterpret_cast<const std::byte*>(first);
  for (;;) {
    std::uint64_t key = 0;
    std::memcpy(&key, found, sizeof(key));
    std::uint16_t halfword = 0;
    std::memcpy(&halfword, found + 0x10, sizeof(halfword));
    if (key == component.word1 && halfword == static_cast<std::uint16_t>(component.word0)) {
      break;
    }
    --remaining;
    found += kEntrySize;
    if (remaining == 0) {
      return false;
    }
  }

  // block+0x20: minimum growth; block+0x28: capacity; block+0x30: entries in use.
  std::uint64_t capacity = 0;
  std::memcpy(&capacity, block + 0x28, sizeof(capacity));
  std::uint64_t used = 0;
  std::memcpy(&used, block + 0x30, sizeof(used));
  if (capacity <= used) {
    std::uint64_t minimum = 0;
    std::memcpy(&minimum, block + 0x20, sizeof(minimum));
    const std::uint64_t growth = minimum < capacity ? capacity : minimum;
    CMemBlock::Resize(block, (growth + capacity) * kEntrySize, 1);
    std::memcpy(&capacity, block + 0x28, sizeof(capacity));
    capacity += growth;
    std::memcpy(block + 0x28, &capacity, sizeof(capacity));
    std::memcpy(&used, block + 0x30, sizeof(used));
  }

  std::uintptr_t data = 0;
  std::memcpy(&data, block, sizeof(data));
  std::uintptr_t allocator = 0;
  std::memcpy(&allocator, block + 0x10, sizeof(allocator));
  std::byte* const entry = reinterpret_cast<std::byte*>(data) + used * kEntrySize;
  used += 1;
  std::memcpy(block + 0x30, &used, sizeof(used));

  // Copy the found entry into the new slot while the block's allocator is the current one.
  // NRadEngine::CMemoryContext::CurrentPtr, 0x12cae90 (thunk 0xffbc50).
  CAllocator* const previous = CMemoryContext::CurrentPtr();
  // NRadEngine::CMemoryContext::SetCurrent, 0x12cafac (thunk 0xfd3590).
  CMemoryContext::SetCurrent(reinterpret_cast<CAllocator*>(allocator));
  // NRadEngine::SBlackboardCD::SProperties::SProperties, 0x1528be8 (thunk 0xfd8900).
  ::new (static_cast<void*>(entry))
      SBlackboardCD::SProperties(*reinterpret_cast<const SBlackboardCD::SProperties*>(found));
  // NRadEngine::CMemoryContext::SetCurrent, 0x12cafac (thunk 0xfd3590).
  CMemoryContext::SetCurrent(previous);

  // Re-read the block (0x14fea10): the new entry's halfwords sit at +0x10 and +0x12 of its slot.
  std::uint64_t used_now = 0;
  std::memcpy(&used_now, block + 0x30, sizeof(used_now));
  std::uintptr_t data_now = 0;
  std::memcpy(&data_now, block, sizeof(data_now));
  std::byte* const tail = reinterpret_cast<std::byte*>(data_now) + used_now * kEntrySize;
  const auto slot = static_cast<std::uint16_t>(actor.bits);
  std::memcpy(tail - 0x240, &slot, sizeof(slot));
  const std::uint16_t unset = 0xffff;
  std::memcpy(tail - 0x23e, &unset, sizeof(unset));
  return true;
}
