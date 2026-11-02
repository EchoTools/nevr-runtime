// NRadEngine::NRadGame::CR15NetGame::ApplyItemFromLoadout
// Quest libr15.so VA 0x135a6dc; ReVault binary 576.
// ReVault raw parent 100223480, reconstruction node 100600684; grade M: all 183 instructions checked (0x135a6dc-0x135a9b4, returns at 0x135a85c); syntax-checked; 3 placeholder globals remain: DAT_0386d2a0 (VA 0x0386d2a0), DAT_038039d8 (VA 0x038039d8), DAT_0380c378 (VA 0x0380c378).
// @0x135a6dc — NRadEngine::NRadGame::CR15NetGame::ApplyItemFromLoadout
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace {

// Byte-offset access to a region with no recovered layout; the offsets are the listing's.
template <typename T>
T ReadAt(const std::byte* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, base + offset, sizeof value);
  return value;
}

template <typename T>
void WriteAt(std::byte* base, std::size_t offset, T value) {
  std::memcpy(base + offset, &value, sizeof value);
}

// Reads the vtable slot at byte offset `offset` of `object` as a function pointer of type Fn.
template <typename Fn>
Fn VirtualSlot(const void* object, std::size_t offset) {
  const std::byte* const vtable = ReadAt<const std::byte*>(static_cast<const std::byte*>(object), 0);
  return ReadAt<Fn>(vtable, offset);
}

}  // namespace

namespace NRadEngine {

struct CSymbol64 {
  std::uint64_t key;
  // NRadEngine::CSymbol64::String() const @ 0x12ccb48 (thunk 0x1038ec0).
  const char* String() const;
  // NRadEngine::CSymbol64::Compare @ 0x12ccd08 (thunk 0x1045900). The caller tests the result for below, equal and above zero.
  static std::int64_t Compare(const CSymbol64* lhs, const CSymbol64* rhs);
};
static_assert(sizeof(CSymbol64) == 0x08);

// SCompactPoolHandle: a 32-bit pool index, passed in w1.
struct SCompactPoolHandle {
  std::uint32_t index;
};
static_assert(sizeof(SCompactPoolHandle) == 0x04);

class CDeferredMethodQueue {
 public:
  // ReVault names the callee at 0x10313f0 GetLookupDataS; its decompile is
  // NRadEngine::CDeferredMethodQueue::QueueFunctor<NRadEngine::CDeferredMethodQueue::SFunctor3<void, NRadEngine::NRadGame::CR15NetCustomizationCS, NRadEngine::SCompactPoolHandle, NRadEngine::CSymbol64, NRadEngine::CSymbol64>>.
  // Returns the functor storage; written at +0x00 object, +0x08 invoke, +0x10 zero, +0x18 pool handle, +0x20 and +0x28 CSymbol64. Storage size not recovered.
  std::byte* QueueFunctor(void (*invoker)(CDeferredMethodQueue&, char*));
};

// CComponentHandle<T>: built in place. +0x40 caches the T*, +0x48 holds the SCompactPoolHandle (same layout as cr15_net_customization_renderer_load_mesh_data.cpp).
template <typename T>
class CComponentHandle {
 public:
  // NRadEngine::CComponentHandle<NRadEngine::NRadGame::CR15NetCustomizationCS>::CComponentHandle @ 0x1352828 (thunk 0xfe61c0): owner, actor GUID words, index.
  CComponentHandle(std::uint64_t owner, const std::uint64_t* actor_guid, std::uint64_t index);
  // NRadEngine::CComponentHandle<NRadEngine::NRadGame::CR15NetCustomizationCS>::RefreshCache() @ 0x13d9548 (thunk 0xfeb1c0).
  void RefreshCache() const;

  std::byte reserved[0x40];           // +0x00 to +0x3f: not recovered
  T* component;                       // +0x40
  SCompactPoolHandle compact_handle;  // +0x48
};

namespace NWriteLog {
// Enumerators are not established. This function uses 4 (WriteLog) and 8 (the GUID message).
enum ELogLevel : std::int32_t {};
// NRadEngine::NWriteLog::WriteLog @ 0x1088c20 (thunk 0x104b960). Variadic.
void WriteLog(ELogLevel level, unsigned long long channel, const char* format, ...);
}  // namespace NWriteLog

namespace NRadGame {

// SR15NetUserId: a user handle. kInvalid is 0xffffffff. The compare at 0x135a714 loads the 32-bit global kInvalid through
// the slot at 0x0381a350 and tests the UserId result against it. _INIT_162 (0x105b780) stores 0xffffffff through that same slot.
struct SR15NetUserId {
  std::uint32_t value;
  static constexpr std::uint32_t kInvalid = 0xffffffffu;
};
static_assert(sizeof(SR15NetUserId) == 0x04);

class CR15NetCustomizationCS {
 public:
  // NRadEngine::NRadGame::CR15NetCustomizationCS::LoadAndApplyItem @ 0x2234980 (thunk 0xff1d60). Returns nothing.
  void LoadAndApplyItem(SCompactPoolHandle handle, CSymbol64 item, CSymbol64 slot);
};
static_assert(sizeof(CComponentHandle<CR15NetCustomizationCS>) == 0x50);
static_assert(offsetof(CComponentHandle<CR15NetCustomizationCS>, component) == 0x40);
static_assert(offsetof(CComponentHandle<CR15NetCustomizationCS>, compact_handle) == 0x48);

class CR15NetGame {
 public:
  // NRadEngine::NRadGame::CR15NetGame::UserId @ 0x135a9b8 (thunk 0x1044c30). The body reads both arguments.
  std::int32_t UserId(std::uint64_t session_id, std::uint64_t net_user_id);
  // NRadEngine::NRadGame::CR15NetGame::UserName(unsigned short) @ 0x134f73c (thunk 0x1026fe0). Returns a C string.
  const char* UserName(std::uint16_t user) const;
  // NRadEngine::NRadGame::CR15NetGame::ApplyItemFromLoadout(SActorGUID, CSymbol64, CSymbol64) @ 0x135a6dc.
  // SActorGUID arrives as two qwords (x1, x2); its struct layout is not recovered here.
  void ApplyItemFromLoadout(std::uint64_t id, std::uint64_t generation, CSymbol64 loadout_instance,
                            CSymbol64 item_slot);
};

}  // namespace NRadGame
}  // namespace NRadEngine

// Placeholder globals: no name is recoverable. Their VAs are listed on line 3.
extern const std::uint64_t DAT_0386d2a0;                                       // compared with the loadout instance key
extern void (*const DAT_038039d8)(NRadEngine::CDeferredMethodQueue&, char*);  // invoker passed to QueueFunctor
extern const void* const DAT_0380c378;                                        // invoke pointer stored in the functor

namespace NRadEngine {
namespace NRadGame {

void CR15NetGame::ApplyItemFromLoadout(std::uint64_t id, std::uint64_t generation,
                                       CSymbol64 loadout_instance, CSymbol64 item_slot) {
  // The SActorGUID words stored at sp+0x60 and sp+0x68 (Ghidra local_50, uStack_48).
  const std::uint64_t actor_guid[2] = {id, generation};
  const auto* const self = reinterpret_cast<const std::byte*>(this);
  const auto report_no_user = [] {
    NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(8), 0ULL,
                        "[NETGAME] Actor GUID doesn't map to a known user for ApplyItemFromLoadout");
  };

  // 0x135a704: UserId(this, id, generation); the result is a 32-bit user handle.
  std::uint32_t handle = static_cast<std::uint32_t>(UserId(id, generation));
  if (SR15NetUserId::kInvalid == handle) {
    // 0x135a71c-0x135a73c: with both counters at +0xde and +0xdc set, the handle is rebuilt from the table at +0x13a.
    if (ReadAt<std::uint16_t>(self, 0xde) != 0 && ReadAt<std::uint16_t>(self, 0xdc) != 0) {
      const std::uint32_t index = ReadAt<std::uint16_t>(self, 0x178) & 0x1fu;
      handle = index | (static_cast<std::uint32_t>(ReadAt<std::uint16_t>(self, index * 4 + 0x13a)) << 16);
    }
  }

  // 0x135a740-0x135a768 and 0x135a774-0x135a798: the high half must be the generation recorded for the slot.
  const std::uint32_t generation_bits = handle >> 16;
  const std::uint32_t slot = handle & 0xffffu;
  const bool valid = generation_bits != 0xffffu && (handle & 0xfff0u) == 0 &&
                     generation_bits == ReadAt<std::uint16_t>(self, slot * 4 + 0x13a);
  if (!valid) {
    report_no_user();
    return;
  }

  // 0x135a7a4-0x135a7b0: the user slot at +0x138 must be below the count at +0xe2.
  const std::uint16_t user_slot = ReadAt<std::uint16_t>(self, slot * 4 + 0x138);
  if (user_slot >= ReadAt<std::uint16_t>(self, 0xe2)) {
    report_no_user();
    return;
  }

  // 0x135a7b4-0x135a7d4: the user record (stride 0x250) names a 0x40-byte row by its index at +0x178.
  const std::uint32_t row_slot =
      ReadAt<std::uint16_t>(self + static_cast<std::size_t>(user_slot) * 0x250, 0x178) & 0x1fu;
  const std::byte* const row = self + static_cast<std::size_t>(row_slot) * 0x40;
  const std::uint64_t instance_count = ReadAt<std::uint64_t>(row, 0x51450);
  const std::byte* entry = ReadAt<const std::byte*>(row, 0x51420);

  // 0x135a7e8-0x135a7fc: scan the instance list for the key loadout_instance.
  std::uint64_t left = instance_count;
  while (left != 0 && ReadAt<std::uint64_t>(entry, 0) != loadout_instance.key) {
    entry += 0x40;
    --left;
  }
  if (left == 0) {
    // 0x135a800-0x135a858: no instance matches. The log names the instance, the item slot, the user (w1 is user_slot at 0x135a81c; Ghidra prints (ushort)param_1) and the count.
    const char* const instance_name = loadout_instance.String();
    const char* const slot_name = item_slot.String();
    const char* const user_name = UserName(user_slot);
    NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(4), 0ULL,
                        "[NETGAME] Unknown loadout instance name '%s' when equipping loadout item in item slot %s for user '%s'. User has %llu loadout instances",
                        instance_name, slot_name, user_name, instance_count);
    return;
  }

  // 0x135a888-0x135a8d4: the matched instance's item list, searched by item slot.
  const std::uint64_t item_count = ReadAt<std::uint64_t>(entry, 0x38);
  if (item_count == 0) {
    return;
  }
  const std::byte* const items = ReadAt<const std::byte*>(entry, 0x08);
  std::uint64_t lo = 0;
  std::uint64_t hi = item_count;
  std::uint64_t mid = 0;
  while (true) {
    mid = lo + ((hi - lo) >> 1);
    CSymbol64 probe = item_slot;  // the copy Compare reads (local_b0 at sp+0)
    const std::int64_t order =
        CSymbol64::Compare(reinterpret_cast<const CSymbol64*>(items + mid * 0x10), &probe);
    if (order < 1) {
      if (order >= 0) {
        break;  // found
      }
      lo = mid + 1;
      mid = hi;
    }
    hi = mid;
    if (mid <= lo) {
      return;
    }
  }

  // 0x135a8d8-0x135a8e8: the instance must be DAT_0386d2a0.
  if (loadout_instance.key != DAT_0386d2a0) {
    return;
  }

  // 0x135a8ec-0x135a908: the matched item's value (+0x08) and the owner at this+0x08 build the handle.
  const std::uint64_t item_value = ReadAt<std::uint64_t>(items + mid * 0x10, 0x08);
  alignas(8) std::byte handle_storage[0x50];
  static_assert(0x48 + sizeof(std::uint32_t) <= sizeof handle_storage, "the bytes read at +0x40 and +0x48 fit");
  auto* const component = new (handle_storage) CComponentHandle<CR15NetCustomizationCS>(
      ReadAt<std::uint64_t>(self, 0x08), actor_guid, ~std::uint64_t{0});

  // 0x135a90c-0x135a924: RefreshCache; the cached owner at +0x40 and the handle's first word gate the call.
  component->RefreshCache();
  auto* const owner = ReadAt<CR15NetCustomizationCS*>(handle_storage, 0x40);
  if (owner == nullptr) {
    return;
  }
  if (ReadAt<std::uint64_t>(handle_storage, 0) == 0) {
    return;
  }
  component->RefreshCache();

  // 0x135a92c-0x135a940: vtable slot 0x1d0 is IsComponentValid (revault_vtable, 0x230fad4); a false result returns.
  const SCompactPoolHandle pool{ReadAt<std::uint32_t>(handle_storage, 0x48)};
  using IsComponentValidFn = bool (*)(const void*, SCompactPoolHandle);
  if (!VirtualSlot<IsComponentValidFn>(owner, 0x1d0)(owner, pool)) {
    return;
  }

  // 0x135a944-0x135a954: RefreshCache, the owner read again at +0x40, RefreshCache.
  component->RefreshCache();
  auto* const bound = ReadAt<CR15NetCustomizationCS*>(handle_storage, 0x40);
  component->RefreshCache();

  // 0x135a958-0x135a964: the owner's space at +0x80 and its flag at +0x368 choose the direct call or the queue.
  std::byte* const space = ReadAt<std::byte*>(reinterpret_cast<const std::byte*>(bound), 0x80);
  const SCompactPoolHandle pool_now{ReadAt<std::uint32_t>(handle_storage, 0x48)};
  if (ReadAt<std::uint32_t>(space, 0x368) != 0) {
    // 0x135a96c-0x135a980: direct LoadAndApplyItem(owner, pool, item value, item slot).
    bound->LoadAndApplyItem(pool_now, CSymbol64{item_value}, item_slot);
    return;
  }

  // 0x135a984-0x135a9b4: queue a functor at space+0x170 and fill its fields.
  auto* const queue = reinterpret_cast<NRadEngine::CDeferredMethodQueue*>(space + 0x170);
  std::byte* const functor = queue->QueueFunctor(DAT_038039d8);
  WriteAt<CR15NetCustomizationCS*>(functor, 0x00, bound);
  WriteAt<const void*>(functor, 0x08, DAT_0380c378);
  WriteAt<std::uint64_t>(functor, 0x10, 0);
  WriteAt<std::uint32_t>(functor, 0x18, pool_now.index);
  WriteAt<std::uint64_t>(functor, 0x20, item_value);
  WriteAt<std::uint64_t>(functor, 0x28, item_slot.key);
}

}  // namespace NRadGame
}  // namespace NRadEngine
