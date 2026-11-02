// CR15NetGame::LogInSuccess
// Quest libr15.so VA 0x136cb20; ReVault binary 576.
// ReVault raw parent 100223778, reconstruction node 100600467; grade M: control flow checked branch by branch against 0x136cb20-0x136ce9c; callees named from libr15 symbols; loader-filled slots, vtable slots and frame layouts left raw.
// @0x136cb20 — CR15NetGame::LogInSuccess
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace NRadEngine {

struct CSymbol64 {
    std::uint64_t value;
};

// 16-byte value returned in x0:x1 by CNSUser::UserID(); field layout not recovered.
struct SNSUserID {
    std::byte raw[16];
};

class CJson {
public:
    // libr15 0x10a4370 (thunk 0x103e540). The return width is not in the decompile header; the caller tests w0.
    bool Boolean(const char* key, std::uint32_t fallback, std::uint32_t flags) const;
    // libr15 0x10a2e7c (thunk 0x1028e40). Ghidra returns ulong; the result is passed on as the next default.
    const char* TString(const char* key, const char* fallback, std::uint32_t flags) const;
};

class CNSUser {
public:
    // libr15 0x1a35310 (thunk 0x1043100)
    void Clear(const char* key);
    // libr15 0x1a353d0 (thunk 0xff50f0)
    CJson* ClientProfile();
    // libr15 0x1a19c9c (thunk 0x101afc0)
    SNSUserID UserID() const;
    // libr15 0x1a1b000 (thunk 0x1027900)
    std::uint64_t LocalID() const;
    // libr15 0x1a34f20 (thunk 0xfead30)
    void SetBoolean(const char* key, std::uint32_t value);
    // libr15 0x1a34e2c (thunk 0x1043590)
    void SetInt(const char* key, std::int64_t value);
};

class CUriContainer {
public:
    // libr15 0x1a39654 (thunk 0x103eb10)
    CUriContainer();
    // libr15 0x1372140 (thunk 0x104f310)
    ~CUriContainer();
    // libr15 0x1a397e8 (thunk 0xffdec0). The return type is not in the decompile header; the caller passes it as int.
    std::int32_t Parse(const char* url);
};

class CMemory {
public:
    // libr15 0x12cb908 (thunk 0x100cc90)
    static void Copy(void* dst, const void* src, std::uint64_t bytes);
};

class CMemoryContext {
public:
    // libr15 0x12c8dc4 (thunk 0x1028430)
    static CMemoryContext* Current();
};

class CTcpBroadcaster {
public:
    // libr15 0x2607090 (thunk 0xfd8360)
    void Send(CSymbol64 first, CSymbol64 second, const void* payload, std::uint64_t payload_count,
              const void* extra, std::uint64_t extra_count, std::uint32_t flags);
};

namespace NWriteLog {
enum class ELogLevel : std::int32_t {};  // enumerators not recovered; LogInSuccess passes 8
}

// libr15 0x12d1b38 (thunk 0x104e4c0). Ghidra shows an undefined8 return; the caller tests w0 only.
std::int32_t OutputError(std::int32_t status, NWriteLog::ELogLevel level);

namespace NRadGame {

class CR15NetGame {
public:
    enum class EState : std::int32_t {};  // values 3 and 4 are passed here; enumerators not recovered

    // libr15 0x135b8b4 (thunk 0xfe6330)
    void SwitchTo(EState state);
    // libr15 0x137f890 (thunk 0x1048d90)
    void ProfileStatInt(std::int32_t& out, const SNSUserID& user, const char* stat,
                        const char* fallback) const;
    // libr15 0x136cb20 (this function)
    void LogInSuccess();
};

class CR15NetStoreTransactions {
public:
    // Callback table passed by reference. From the stores: {CR15NetGame*, fn, 0, fn} at +0x00 and at +0x40; the rest is zero.
    struct SCallbacks {
        std::byte raw[0x80];
    };
    // libr15 0x13b537c (thunk 0x101fc00)
    CR15NetStoreTransactions(CR15NetGame& game, CUriContainer& uri, const SCallbacks& callbacks);
};

// UNRESOLVED: loader-filled slots (zero in the file) and one global; none is named in libr15.
// Slots 0x3817a88, 0x380b890, 0x381d560 and 0x3818398 hold the four callback code pointers.
extern void* const libr15_slot_0x3817a88;
extern void* const libr15_slot_0x380b890;
extern void* const libr15_slot_0x381d560;
extern void* const libr15_slot_0x3818398;
// Global 0x386d208: the first CSymbol64 argument of CTcpBroadcaster::Send.
extern const CSymbol64 libr15_global_0x386d208;

namespace {

std::uint8_t LoadU8(const void* base, std::size_t offset) {
    std::uint8_t value;
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof value);
    return value;
}

std::uint32_t LoadU32(const void* base, std::size_t offset) {
    std::uint32_t value;
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof value);
    return value;
}

std::uint64_t LoadU64(const void* base, std::size_t offset) {
    std::uint64_t value;
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof value);
    return value;
}

template <typename T>
T* LoadPtr(const void* base, std::size_t offset) {
    return reinterpret_cast<T*>(static_cast<std::uintptr_t>(LoadU64(base, offset)));
}

void StoreU32(void* base, std::size_t offset, std::uint32_t value) {
    std::memcpy(static_cast<std::byte*>(base) + offset, &value, sizeof value);
}

void StoreU64(void* base, std::size_t offset, std::uint64_t value) {
    std::memcpy(static_cast<std::byte*>(base) + offset, &value, sizeof value);
}

// The disassembly reloads the CNSUser pointer at +0x28d0 before each call and skips the call when it is null.
void SetBooleanIfProfile(const void* self, const char* key) {
    if (CNSUser* profile = LoadPtr<CNSUser>(self, 0x28d0)) {
        profile->SetBoolean(key, 1);
    }
}

}  // namespace

void CR15NetGame::LogInSuccess() {
    auto* const self = reinterpret_cast<std::byte*>(this);
    // Frame slot sp+0xa8 (0x120 bytes): the 16-byte SNSUserID, then the CUriContainer, then the send buffer.
    alignas(8) std::byte uri_slot[0x120];

    if (static_cast<std::int32_t>(LoadU32(self, 0)) == 2) {
        // The view is the CJson at +0x28d8, or the client profile once the profile is set up.
        CJson* view = reinterpret_cast<CJson*>(self + 0x28d8);
        if (CNSUser* profile = LoadPtr<CNSUser>(self, 0x28d0)) {
            profile->Clear("gametype");
            if (CNSUser* profile2 = LoadPtr<CNSUser>(self, 0x28d0)) {
                profile2->Clear("bodytype");
                if (CNSUser* profile3 = LoadPtr<CNSUser>(self, 0x28d0)) {
                    view = profile3->ClientProfile();
                }
            }
        }

        if (view->Boolean("npecompleted", 0, 0)) {
            if (CNSUser* profile = LoadPtr<CNSUser>(self, 0x28d0)) {
                profile->Clear("npecompleted");
                if (CNSUser* profile2 = LoadPtr<CNSUser>(self, 0x28d0)) {
                    profile2->SetBoolean("npe|movement|completed", 1);
                }
            }
        }

        // Frame slots sp+0x1cc (arena) and sp+0x1c8 (combat), zeroed together in the disassembly.
        std::int32_t arena = 0;
        std::int32_t combat = 0;
        // No null test on the profile before UserID(), as in the disassembly.
        const SNSUserID arena_user = LoadPtr<CNSUser>(self, 0x28d0)->UserID();
        std::memcpy(uri_slot, &arena_user, sizeof arena_user);
        ProfileStatInt(arena, arena_user, "arena", "ArenaGamesPlayed");
        const SNSUserID combat_user = LoadPtr<CNSUser>(self, 0x28d0)->UserID();
        std::memcpy(uri_slot, &combat_user, sizeof combat_user);
        ProfileStatInt(combat, combat_user, "combat", "CombatGamesPlayed");

        // Byte 1, bit 0 of *flags (bit 8), tested only when the two counters sum to zero.
        if (static_cast<std::uint32_t>(arena) + static_cast<std::uint32_t>(combat) != 0 ||
            (LoadU8(LoadPtr<const std::uint32_t>(self, 0x2da0), 1) & 1) != 0) {
            SetBooleanIfProfile(self, "npe|movement|completed");
            SetBooleanIfProfile(self, "npe|lobby|completed");
            SetBooleanIfProfile(self, "npe|arenabasics|completed");
            SetBooleanIfProfile(self, "npe|firstmatch|completed");
        }

        // Byte 1, bit 1 of *flags (bit 9).
        if ((LoadU8(LoadPtr<const std::uint32_t>(self, 0x2da0), 1) & 2) != 0) {
            if (CNSUser* profile = LoadPtr<CNSUser>(self, 0x28d0)) {
                profile->SetInt("social|community_values_version", 1);
            }
        }

        // Interface at +0x647c8: vtable slot +0x78 (name not recovered) takes LocalID() truncated to 32 bits.
        if (void* const iface = LoadPtr<void>(self, 0x647c8)) {
            const std::uint64_t local_id = LoadPtr<CNSUser>(self, 0x28d0)->LocalID();
            void* const* const vtable = *reinterpret_cast<void* const* const*>(iface);
            auto set_local_id =
                reinterpret_cast<void (*)(void*, std::uint32_t)>(vtable[0x78 / sizeof(void*)]);
            set_local_id(iface, static_cast<std::uint32_t>(local_id));
        }

        // Transaction store, built only while the field at +0x2aa0 is null.
        if (LoadPtr<void>(self, 0x2aa0) == nullptr) {
            CUriContainer& uri = *new (uri_slot) CUriContainer();
            CJson* const config = reinterpret_cast<CJson*>(self + 0x63240);
            const char* url =
                config->TString("transaction_host", "wss://transaction.readyatdawn.com/rad/rad15_live", 0);
            url = config->TString("transactionservice_host", url, 0);
            if (OutputError(uri.Parse(url), static_cast<NWriteLog::ELogLevel>(8)) == 0) {
                alignas(8) std::byte functor[0x20];  // sp+0x00: {this, fnA, 0, fnB}, then {this, fnC, 0, fnD}
                alignas(8) std::byte pair[16];       // sp+0x1d0: the 16-byte value CMemory::Copy moves
                CR15NetStoreTransactions::SCallbacks callbacks{};

                // Entry at +0x00.
                StoreU64(pair, 0, reinterpret_cast<std::uintptr_t>(libr15_slot_0x3817a88));
                StoreU64(pair, 8, 0);
                StoreU64(functor, 0, reinterpret_cast<std::uintptr_t>(this));
                StoreU64(functor, 0x18, reinterpret_cast<std::uintptr_t>(libr15_slot_0x380b890));
                CMemory::Copy(functor + 8, pair, sizeof pair);
                std::memcpy(callbacks.raw + 0x00, functor + 0x00, 16);
                std::memcpy(callbacks.raw + 0x10, functor + 0x10, 16);

                // Entry at +0x40.
                StoreU64(pair, 0, reinterpret_cast<std::uintptr_t>(libr15_slot_0x381d560));
                StoreU64(pair, 8, 0);
                StoreU64(functor, 0x18, reinterpret_cast<std::uintptr_t>(libr15_slot_0x3818398));
                CMemory::Copy(functor + 8, pair, sizeof pair);
                std::memcpy(callbacks.raw + 0x40, functor + 0x00, 16);
                std::memcpy(callbacks.raw + 0x50, functor + 0x10, 16);

                // Allocate 0x1c8 bytes from the memory context (vtable slot +0x10, name not recovered).
                CMemoryContext* const memory = CMemoryContext::Current();
                void* const* const memory_vtable = *reinterpret_cast<void* const* const*>(memory);
                auto allocate =
                    reinterpret_cast<void* (*)(void*, std::uint32_t)>(memory_vtable[0x10 / sizeof(void*)]);
                void* const object = allocate(memory, 0x1c8);
                new (object) CR15NetStoreTransactions(*this, uri, callbacks);
                StoreU64(self, 0x2aa0, reinterpret_cast<std::uintptr_t>(object));
                StoreU64(object, 0, 5);
            }
            uri.~CUriContainer();
        }

        // Bit 14 of *flags (0x4000) selects state 4, otherwise 3.
        const bool bit14 = (LoadU32(LoadPtr<const std::uint32_t>(self, 0x2da0), 0) & 0x4000u) != 0;
        SwitchTo(static_cast<EState>(bit14 ? 4 : 3));
    }

    // Social groups (CR15NetSocialGroups): one CTcpBroadcaster::Send when the connection state is above 2
    // and bit 0 of the word at +8 is clear; the bit is then set.
    if (std::byte* const groups = LoadPtr<std::byte>(self, 0x2a00)) {
        const std::byte* const conn = LoadPtr<const std::byte>(groups, 0);
        if (static_cast<std::int32_t>(LoadU32(conn, 0)) > 2 && (LoadU8(groups, 8) & 1) == 0) {
            CTcpBroadcaster* const broadcaster = LoadPtr<CTcpBroadcaster>(conn, 0x28c0);
            broadcaster->Send(libr15_global_0x386d208, CSymbol64{0x90758e58515724e0}, uri_slot, 1, nullptr, 0, 1);
            StoreU32(groups, 8, LoadU32(groups, 8) | 1u);
        }
    }
}

}  // namespace NRadGame
}  // namespace NRadEngine
