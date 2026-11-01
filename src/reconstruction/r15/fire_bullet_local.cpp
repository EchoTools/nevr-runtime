// CR15NetAutoTargetCS::FireBulletLocal
// Quest libr15.so VA 0x220bff8; ReVault binary 576.
// ReVault raw parent 100302367, reconstruction node 100600469; grade M: callees and branch structure from the asm; vtable slots 0x1e0 and 0x1e8 unresolved; the Broadcast flags word (x5, loaded through loader-filled slot 0x3813e10) is the decompiler's fold 0xffffffffffffffff, not byte-checked.
// @0x220bff8 — NRadEngine::NRadGame::CR15NetAutoTargetCS::FireBulletLocal
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NWriteLog {
// VA 0x1088c20 (thunk 0x104b960).
void WriteLog(std::int32_t level, unsigned long long channel, const char* format, ...);
}  // namespace NWriteLog

namespace NRadEngine {

// SCompactPoolHandle arrives in x1. Its low 16 bits index the slot map; the code uses its low 32 bits.
using SCompactPoolHandle = std::uint64_t;

struct C3Vector {
    float x;
    float y;
    float z;
};
static_assert(sizeof(C3Vector) == 0x0c);

struct CSymbol64 {
    std::uint64_t bits;
    const char* String() const;  // VA 0x12ccb48 (thunk 0x1038ec0)
};
static_assert(sizeof(CSymbol64) == 0x08);

// 16-byte result (x0:x1) of AllocatePooledActor and of vtable slot 0x1e0.
struct SActorId {
    std::uint64_t low;
    std::uint64_t high;
};
static_assert(sizeof(SActorId) == 0x10);

struct SActor {
    std::uint64_t bits;
};
static_assert(sizeof(SActor) == 0x08);

struct CTransfQ {
    float values[8];
};
static_assert(sizeof(CTransfQ) == 0x20);

enum class EComponentTeleportReason : std::uint32_t {};

// One 16-byte entry of CTable<CSimpleKey<CComponentSystem*, CSymbol64>>: key bits, then the system pointer.
struct CSimpleKey {
    std::uint64_t key;
    std::uint64_t value;
};
static_assert(sizeof(CSimpleKey) == 0x10);

// Empty comparator; the decompiler passes its stack slot to Sort by address.
struct CSimpleKeyLess {};

// NRadEngine::Sort<512ull, CSimpleKey*, ...>, VA 0x130d850 (thunk 0xff0b30).
template <std::uint64_t Block, class Key, class Less>
void Sort(Key* keys, std::uint64_t count, Key* scratch, Less* less);

class CncaGameSpace {
public:
    CncaGameSpace* GetST();                                 // VA 0x18ff2fc (thunk 0xfe2f50): returns this
    void* GameMT();                                         // VA 0x18ff33c (thunk 0x103f6f0): returns *(this + 0x8)
    void* AvailablePooledActors(CSymbol64 symbol);          // VA 0x190400c (thunk 0xfdb6a0)
    SActorId AllocatePooledActor(CSymbol64 symbol);         // VA 0x19037c4 (thunk 0xfeb620)
    void WorldTeleport(SActor actor, const CTransfQ& pose,  // VA 0x190a384 (thunk 0xff0660)
                       EComponentTeleportReason reason);
    void SendComponentEventGlobal(std::uint64_t event_id,   // VA 0x190735c (thunk 0x104a800)
                                  std::uint64_t low, std::uint64_t high, std::uint32_t flags);
};

class CTransformCS {
public:
    // VA 0x17f6ef0 (thunk 0x103d910). The 32-byte result is returned through x8.
    CTransfQ ComputeLocalToWorldTransfQ(std::uint32_t transform_id) const;
};

namespace NRadGame {
class CR15NetBulletCS;
class CR15NetActorCS;
}  // namespace NRadGame

// A CComponentHandle<T> object of 0x50 bytes; its fields are reached through raw offsets.
template <class T>
class CComponentHandle {
public:
    CComponentHandle() = default;
    // VA 0x220c4a8 (thunk 0x1023660): constructor used for CR15NetActorCS.
    CComponentHandle(CncaGameSpace* game_space, const SActorId& actor, std::uint64_t mask);
    // FindInternal: VA 0x23e1270 (thunk 0xfd6240) for CR15NetBulletCS; VA 0x23e1910 (thunk 0xfe60f0) for CR15NetActorCS.
    void FindInternal(CncaGameSpace* game_space, std::uint64_t key, std::uint64_t mask);
    // RefreshCache: VA 0x23e14e8 (thunk 0xfe8600) for CR15NetBulletCS; VA 0x23dfbf4 (thunk 0xff0740) for CR15NetActorCS.
    void RefreshCache();

    alignas(8) std::byte raw[0x50];
};
static_assert(sizeof(CComponentHandle<NRadGame::CR15NetBulletCS>) == 0x50);
static_assert(sizeof(CComponentHandle<NRadGame::CR15NetActorCS>) == 0x50);

namespace {
using Address = std::uintptr_t;

template <class T>
T Load(const std::byte* base, std::size_t offset) {
    T value;
    std::memcpy(&value, base + offset, sizeof(T));
    return value;
}

template <class T>
void Store(std::byte* base, std::size_t offset, T value) {
    std::memcpy(base + offset, &value, sizeof(T));
}

// The function pointer stored at byte_offset of the object's vtable.
template <class Fn>
Fn VirtualSlot(const std::byte* object, std::size_t byte_offset) {
    const Address table = Load<Address>(object, 0);
    return reinterpret_cast<Fn>(Load<Address>(reinterpret_cast<const std::byte*>(table), byte_offset));
}
}  // namespace

namespace NRadGame {

class CR15NetBulletCS {
public:
    // VA 0x220c5b0 (thunk 0x1017ad0).
    void SetBulletOwner(std::uint32_t bullet, std::uint32_t owner);
    // VA 0x220c9c4 (thunk 0x1038c20). The float is the s0 argument.
    void FireBullet(std::uint32_t bullet, const void* spawn, const CTransfQ* transform,
                    const C3Vector* direction, std::uint32_t arg5, std::uint32_t arg6, float speed);
};

class CR15NetGame {
public:
    // VA 0x133ca70 (thunk 0xfeaab0).
    std::uint64_t Server();
};

class CR15Game {
public:
    // VA 0x13027d4 (thunk 0x103a070): returns *(this + 0x8510).
    CR15NetGame* NetGameMT();
};

// CR15NetGame::Broadcast<SR15NetAutoTargetFireBullet>, VA 0x23e1aa8 (thunk 0x102f930).
void CR15NetGame_Broadcast_SR15NetAutoTargetFireBullet(
    CR15NetGame* game, const void* message, const void* data, std::uint64_t size,
    std::uint64_t peer, std::uint64_t flags);

// u64 at VA 0x3283f18: the CSymbol64 key looked up in the space table at ST+0x420.
constexpr std::uint64_t kBulletSystemKey = 0x48037cde0e07c1b2ULL;
// u64 at VA 0x32cefe8: the component event sent after the two slot-0x1e0 calls.
constexpr std::uint64_t kFireBulletEvent = 0x41552625c376e30eULL;
// The Broadcast flags argument (x5) is loaded at 0x220c464-0x220c478 as *(*(u64*)0x3813e10): the slot is zero in the file and
// filled by the loader, so the pointed-to word is not readable from the file. 0xffffffffffffffff is the decompiler's fold of
// that load, not a byte-checked value.

class CR15NetAutoTargetCS {
public:
    // NRadEngine::NRadGame::CR15NetAutoTargetCS::FireBulletLocal(SCompactPoolHandle, C3Vector, unsigned int)
    void FireBulletLocal(SCompactPoolHandle handle, C3Vector dir, std::uint32_t flags);
};

void CR15NetAutoTargetCS::FireBulletLocal(SCompactPoolHandle handle, C3Vector dir, std::uint32_t flags) {
    auto* const self = reinterpret_cast<std::byte*>(this);

    // 0x220c020-0x220c040: record index = u16 at slot_map + 4*(handle & 0xffff); record = records + index*0x90.
    const std::byte* const slot_map = Load<const std::byte*>(self, 0xc8);
    const std::uint16_t record_index = Load<std::uint16_t>(slot_map, (handle & 0xffffULL) * 4);
    std::byte* const rec = Load<std::byte*>(self, 0xf8) + static_cast<std::size_t>(record_index) * 0x90;

    // 0x220c044-0x220c05c: bullet type = *(this+0xb8)[0] + rec.u16[0x60] * 0x68; its symbol is at +0x50.
    const std::uint16_t type_index = Load<std::uint16_t>(rec, 0x60);
    const std::byte* const type_table = Load<const std::byte*>(Load<const std::byte*>(self, 0xb8), 0);
    const std::byte* const bullet_type = type_table + static_cast<std::size_t>(type_index) * 0x68;
    const CSymbol64 symbol{Load<std::uint64_t>(bullet_type, 0x50)};

    // 0x220c060-0x220c068: ST = this->[0x80]->GetST(), which returns its argument.
    CncaGameSpace* const game = Load<CncaGameSpace*>(self, 0x80);
    auto* const game_bytes = reinterpret_cast<std::byte*>(game);
    CncaGameSpace* const st = game->GetST();
    auto* const st_bytes = reinterpret_cast<std::byte*>(st);

    // 0x220c06c-0x220c0ec: bullet system = value of kBulletSystemKey in ST's table, else 0.
    // Bit 0 of ST+0x458 marks an unsorted table, sorted first (0x220c07c-0x220c0a8).
    std::uint64_t bullet_system = 0;  // x22
    std::uint64_t count = Load<std::uint64_t>(st_bytes, 0x450);
    if (count != 0) {
        if ((Load<std::uint32_t>(st_bytes, 0x458) & 1U) != 0) {
            CSimpleKeyLess less;
            CSimpleKey sort_scratch[5];  // sp+0x128, 0x50 bytes
            Sort<512>(Load<CSimpleKey*>(st_bytes, 0x420), count, sort_scratch, &less);
            Store<std::uint32_t>(st_bytes, 0x458, Load<std::uint32_t>(st_bytes, 0x458) & ~1U);
            count = Load<std::uint64_t>(st_bytes, 0x450);
        }
    }
    if (count != 0) {
        // 0x220c0ac-0x220c0e8: binary search; lo = 0, hi = count.
        const std::byte* const table = Load<const std::byte*>(st_bytes, 0x420);
        std::uint64_t lo = 0;
        std::uint64_t hi = count;
        for (;;) {
            const std::uint64_t mid = lo + ((hi - lo) >> 1);
            const auto probe = static_cast<std::int64_t>(Load<std::uint64_t>(table, mid * 0x10));
            const auto wanted = static_cast<std::int64_t>(kBulletSystemKey);
            if (probe > wanted) {                                  // b.gt 0x220c0b8
                hi = mid;
            } else if (probe == wanted) {                          // b.ge 0x220c3c8
                bullet_system = Load<std::uint64_t>(table, mid * 0x10 + 8);
                break;
            } else {                                               // 0x220c0e0
                lo = mid + 1;
            }
            if (hi <= lo) {                                        // 0x220c0c0: not found
                break;
            }
        }
    }

    // 0x220c0f0-0x220c184: no pooled actor of this type is an error (log and return).
    if (st->AvailablePooledActors(symbol) == nullptr) {
        NWriteLog::WriteLog(8, 0ULL, "No available bullets of type '%s'; add more to their pool", symbol.String());
        return;
    }
    const SActorId actor = st->AllocatePooledActor(symbol);     // 0x220c11c; actor at sp+0x1c8
    const std::uint64_t handle32 = handle & 0xffffffffULL;      // 0x220c114 (x23)

    // Bullet handle at sp+0x178: preamble stores of 0x220c12c-0x220c148 and 0x220c150.
    CComponentHandle<CR15NetBulletCS> bullet;
    Store<std::uint64_t>(bullet.raw, 0x00, 0);
    Store<std::uint64_t>(bullet.raw, 0x08, ~0ULL);
    Store<std::uint64_t>(bullet.raw, 0x10, ~0ULL);
    Store<std::uint64_t>(bullet.raw, 0x16, ~0ULL);   // sp+0x18e..0x195
    Store<std::uint16_t>(bullet.raw, 0x1e, 0);       // sp+0x196
    Store<std::uint32_t>(bullet.raw, 0x20, 0xffffU); // sp+0x198
    Store<std::uint32_t>(bullet.raw, 0x24, 0);       // sp+0x19c
    Store<std::uint64_t>(bullet.raw, 0x28, ~0ULL);   // sp+0x1a0
    Store<std::uint64_t>(bullet.raw, 0x30, ~0ULL);   // sp+0x1a8
    Store<std::uint64_t>(bullet.raw, 0x38, ~0ULL);   // sp+0x1b0
    Store<std::uint64_t>(bullet.raw, 0x48, 0xffffULL);  // sp+0x1c0: the bullet's u32 handle, read as w1 below
    if (game != nullptr) {                           // cbz x25 at 0x220c154
        // 0x220c158-0x220c184
        Store<void*>(bullet.raw, 0x00, game->GameMT());
        Store<std::uint64_t>(bullet.raw, 0x08, Load<std::uint64_t>(game_bytes, 0x58));
        Store<std::uint64_t>(bullet.raw, 0x18, actor.high);
        bullet.FindInternal(game, actor.high, ~0ULL);
    } else {
        // 0x220c1ac-0x220c1d8
        Store<std::uint64_t>(bullet.raw, 0x40, 0);   // sp+0x1b8
    }

    // 0x220c1dc-0x220c1f0: actor handle at sp+0x128, built from the space and the actor id (result unused).
    CComponentHandle<CR15NetActorCS> actor_handle(st, actor, ~0ULL);

    // 0x220c1f4-0x220c230: transform of the record, copy to pose, world teleport, then bullet refresh.
    CTransformCS* const transform_cs = Load<CTransformCS*>(self, 0x130);
    const CTransfQ transform = transform_cs->ComputeLocalToWorldTransfQ(Load<std::uint32_t>(rec, 0x7c));
    const CTransfQ pose = transform;                                   // 0x220c20c-0x220c224
    st->WorldTeleport(SActor{actor.high}, pose, EComponentTeleportReason{0});  // 0x220c228
    bullet.RefreshCache();                                             // 0x220c22c-0x220c230

    // 0x220c234-0x220c24c: owner of the bullet, then refresh.
    auto* const bullet_system_obj = reinterpret_cast<CR15NetBulletCS*>(bullet_system);
    bullet_system_obj->SetBulletOwner(Load<std::uint32_t>(bullet.raw, 0x48), Load<std::uint32_t>(rec, 0x64));
    bullet.RefreshCache();

    // 0x220c250-0x220c2a0: spawn record = rec[0x00..0x48) followed by {rec[0x48], 0}; then FireBullet.
    alignas(8) std::byte spawn[0x50];                                  // sp+0x90
    std::memcpy(spawn, rec, 0x48);
    Store<std::uint32_t>(spawn, 0x48, Load<std::uint32_t>(rec, 0x48));
    Store<std::uint32_t>(spawn, 0x4c, 0);
    bullet_system_obj->FireBullet(Load<std::uint32_t>(bullet.raw, 0x48), spawn, &transform, &dir, 0, 0,
                                  Load<float>(rec, 0x70));

    // 0x220c2a4-0x220c2f8: two calls through vtable slot 0x1e0 (UNRESOLVED), then the component event.
    using ActorIdSlot = SActorId (*)(CR15NetAutoTargetCS*, std::uint64_t);
    const auto actor_id_slot = VirtualSlot<ActorIdSlot>(self, 0x1e0);
    const SActorId first = actor_id_slot(this, handle32);   // x0 kept
    const SActorId second = actor_id_slot(this, handle32);  // x1 kept
    game->SendComponentEventGlobal(kFireBulletEvent, first.low, second.high, 1);

    // 0x220c2fc-0x220c300: a nonzero flags argument ends the function here.
    if (flags != 0) {
        return;
    }

    // 0x220c304-0x220c36c: broadcast payload at sp+0x60 (0x30 bytes).
    // The 32-bit load through GOT slot 0x381a350 at 0x220c318-0x220c32c is overwritten at 0x220c33c and not modeled.
    std::byte message[0x30];
    Store<std::uint64_t>(message, 0x00, ~0ULL);  // 16-byte constant at VA 0x2c2f960 = {-1, 0}
    Store<std::uint64_t>(message, 0x08, 0);
    Store<std::uint32_t>(message, 0x10, Load<std::uint32_t>(rec, 0x64));
    Store<float>(message, 0x14, dir.x);
    Store<float>(message, 0x18, dir.y);
    Store<float>(message, 0x1c, dir.z);
    Store<std::uint32_t>(message, 0x28, Load<std::uint32_t>(rec, 0x70));
    Store<std::uint32_t>(message, 0x2c, 0);

    // 0x220c348-0x220c36c: compact id = table_d8[record] | (slot_map[that].u16[+2] << 16); vtable slot 0x1e8 (UNRESOLVED).
    const std::byte* const table_d8 = Load<const std::byte*>(self, 0xd8);
    const std::uint16_t compact_low = Load<std::uint16_t>(table_d8, static_cast<std::size_t>(record_index) * 2);
    const std::uint16_t compact_high =
        Load<std::uint16_t>(slot_map, static_cast<std::size_t>(compact_low) * 4 + 2);
    const std::uint64_t compact =
        static_cast<std::uint64_t>(compact_low) | (static_cast<std::uint64_t>(compact_high) << 16);
    using KeySlot = std::uint64_t (*)(CR15NetAutoTargetCS*, std::uint64_t);
    const std::uint64_t key = VirtualSlot<KeySlot>(self, 0x1e8)(this, compact);

    // 0x220c370-0x220c3c4 (game != 0) and 0x220c3d4-0x220c3e8 (game == 0): game handle at sp+0x10.
    CComponentHandle<CR15NetActorCS> game_handle;
    Store<std::uint64_t>(game_handle.raw, 0x00, 0);
    Store<std::uint64_t>(game_handle.raw, 0x08, ~0ULL);
    Store<std::uint64_t>(game_handle.raw, 0x10, ~0ULL);
    Store<std::uint64_t>(game_handle.raw, 0x16, ~0ULL);
    Store<std::uint16_t>(game_handle.raw, 0x1e, 0);
    Store<std::uint64_t>(game_handle.raw, 0x20, 0xffffULL);
    Store<std::uint64_t>(game_handle.raw, 0x28, ~0ULL);
    Store<std::uint64_t>(game_handle.raw, 0x30, ~0ULL);
    Store<std::uint64_t>(game_handle.raw, 0x38, ~0ULL);
    Store<std::uint64_t>(game_handle.raw, 0x48, 0xffffULL);
    if (game != nullptr) {
        Store<void*>(game_handle.raw, 0x00, game->GameMT());
        Store<std::uint64_t>(game_handle.raw, 0x08, Load<std::uint64_t>(game_bytes, 0x58));
        Store<std::uint64_t>(game_handle.raw, 0x18, key);
        game_handle.FindInternal(game, key, ~0ULL);
    } else {
        Store<std::uint32_t>(game_handle.raw, 0x24, 0);  // sp+0x34
        Store<std::uint64_t>(game_handle.raw, 0x40, 0);  // sp+0x50
    }
    game_handle.RefreshCache();                          // 0x220c408-0x220c40c

    // 0x220c410-0x220c44c: actor record from the handle's u16 at +0x48 in this+0x150's table, else -1.
    const std::uint32_t game_index = Load<std::uint32_t>(game_handle.raw, 0x48);
    std::uint64_t actor_value;
    if ((~game_index & 0xffffU) == 0) {
        actor_value = ~0ULL;                             // 0x220c448
    } else {
        const std::byte* const pool = Load<const std::byte*>(self, 0x150);
        const std::uint16_t pool_record =
            Load<std::uint16_t>(Load<const std::byte*>(pool, 0xc8), (game_index & 0xffffU) * 4);
        const std::byte* const pool_entry =
            Load<const std::byte*>(pool, 0xf8) + static_cast<std::size_t>(pool_record) * 0x18;
        actor_value = Load<std::uint64_t>(pool_entry, 0x08);
    }
    Store<std::uint64_t>(message, 0x08, actor_value);    // sp+0x68

    // 0x220c450-0x220c484: net game of the CR15Game object, its server peer, then the broadcast.
    CR15Game* const cr15_game = static_cast<CR15Game*>(game->GameMT());  // 0x220c450: x0 = [this+0x80]+8
    CR15NetGame* const net_game = cr15_game->NetGameMT();                 // 0x220c458
    const std::uint64_t peer = net_game->Server();                        // 0x220c460
    CR15NetGame_Broadcast_SR15NetAutoTargetFireBullet(net_game, message, nullptr, 0, peer,
                                                      0xffffffffffffffffULL);  // 0x220c484
}

}  // namespace NRadGame
}  // namespace NRadEngine
