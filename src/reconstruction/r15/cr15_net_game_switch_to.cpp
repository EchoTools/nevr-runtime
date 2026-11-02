// NRadEngine::NRadGame::CR15NetGame::SwitchTo
// Quest libr15.so VA 0x135b8b4; ReVault binary 576.
// ReVault raw parent 100223509, reconstruction node 100600615; grade M: all 482 instructions checked against the disassembly; callees resolved to thunk targets; eight loader slots (DAT_0380cb28, DAT_03817060, DAT_0381c0f8, DAT_0382ab90, DAT_038196d8, DAT_03821268, DAT_038280a8, DAT_0382be80) unresolved; virtual slots unnamed; list loops are compare-and-swap rewrites.
// @0x135b8b4 — NRadEngine::NRadGame::CR15NetGame::SwitchTo
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

// UNRESOLVED: loader-filled slots. Each name is the slot's address; the slot holds a pointer or a code pointer,
// and the code reads the value stored there.
// DAT_0380cb28: pointer to C3Vector::realmax (u64 at +0, u32 at +8).
extern void* const DAT_0380cb28;
// DAT_03817060: pointer to the u32 passed to SetFoveationLevel in state 9.
extern void* const DAT_03817060;
// DAT_0381c0f8: pointer to the float passed to the case-4 update call.
extern void* const DAT_0381c0f8;
// DAT_0382ab90: pointer to the float fallback passed to CJson::Real("load_timeout", ...).
extern void* const DAT_0382ab90;
// DAT_038196d8: pointer to the u64 passed as the first RequestRegistration argument.
extern void* const DAT_038196d8;
// DAT_03821268: CR15NetLobby::AcceptSlotsCB (code pointer, copied into a delegate).
extern void* const DAT_03821268;
// DAT_038280a8: CDelegate<...>::MemberProxy<...> for AcceptSlotsCB (code pointer).
extern void* const DAT_038280a8;
// DAT_0382be80: CncaGame::InvokeExclusiveUpdate<NRadEngine::NRadGame::CR15NetGame, &EnableGameSpaces> (code pointer).
extern void* const DAT_0382be80;

namespace NRadEngine {

namespace NWriteLog {
enum class ELogLevel : std::int32_t {};  // enumerators not recovered; SwitchTo passes 2
// libr15 0x1088c20 (thunk 0x104b960)
void WriteLog(ELogLevel level, std::uint64_t channel, const char* format, ...);
}  // namespace NWriteLog

// STempString<N>: the temporary that MakeTStrPrintF returns in the caller's frame.
template <std::size_t N>
struct STempString {
    unsigned char storage[N];
};

// libr15 0x1086b9c (thunk 0x102b370)
template <std::size_t N>
STempString<N> MakeTStrPrintF(const char* format, ...);

class CSysTime {
public:
    // libr15 0x1084e20 (thunk 0x1048d70)
    static std::uint64_t GetTick();
    // libr15 0x1084e64 (thunk 0x1013390)
    static std::uint64_t GetTicksPerSecond();
};

namespace CVR {
// libr15 0x1b71294 (thunk 0xff6cd0)
void SetFoveationLevel(std::uint32_t level);
}  // namespace CVR

class CLockReadWrite {
public:
    // libr15 0x1091a40 (thunk 0x102da20): NRadEngine::CLockReadWrite::WriteLock(); this is the lock.
    void WriteLock();
    // libr15 0x1091a50 (thunk 0x101c780): NRadEngine::CLockReadWrite::WriteUnlock(); this is the lock.
    void WriteUnlock();
};

class CBroadcaster {
public:
    // libr15 0x25eda88 (thunk 0x1010540)
    void Unlisten(std::uint16_t id);
};

class CJson {
public:
    // libr15 0x10a4358 (thunk 0x10094e0). Key in x1, fallback in s0, flags in w2.
    float Real(const char* key, float fallback, std::uint32_t flags) const;
};

class CMemory {
public:
    // libr15 0x12cb908 (thunk 0x100cc90)
    static void Copy(void* dst, const void* src, std::uint64_t bytes);
};

class CNSUser {
public:
    // libr15 0x1a1b000 (thunk 0x1027900)
    std::uint64_t LocalID() const;
};

class CNSLobby {
public:
    // libr15 0x1a2d0c4 (thunk 0x100f250). Nonzero when the lobby is owned.
    std::int32_t Owned();
    // libr15 0x1a2e540 (thunk 0x1039000). The key is the string at 0x2c9be89 ("ingame"); w2 is the flag word.
    void SetOwnerBoolean(const char* key, std::uint32_t value);
    // libr15 0x1a29564 (thunk 0x10176d0). Five arguments after this, in x1..x5. The ReVault prototype is
    // (ull, ull, CJson const&, CFlagsT, CDelegate const&); the call site passes x4 as a pointer to the kind word,
    // and this declaration follows the call site.
    void RequestRegistration(std::uint64_t id, std::uint64_t second, const void* json,
                             const std::uint32_t* kind, const void* delegate);
};

namespace NR15RestAPI {
// libr15 0x135de18 (reached through 0x135de58 and thunk 0x100b5b0)
void RegisterHTTPCallbacks(void* listener, void* game);
// libr15 0x135cce8 (thunk 0xfeeda0)
void UnregisterHTTPCallbacks(void* listener);
}  // namespace NR15RestAPI

namespace NRadGame {

class CR15Game {
public:
    // libr15 0x13005fc (thunk 0x1032b90)
    void EnableTransitionMenu();
    // libr15 0x1301a84 (thunk 0x102c780)
    void BeginMultiplayerGameplay();
    // libr15 0x1301aa4 (thunk 0x103f070)
    void EndMultiplayerGameplay();
};

class CR15NetClientLobby {
public:
    // libr15 0x13468c8 (thunk 0x1031d50)
    void PickLevel();
};

class CR15NetGameplay {
public:
    // libr15 0x1352458 (thunk 0x1052db0). Nonzero when the spawn point was set.
    std::int32_t SetSpawnPoint();
    // libr15 0x135e268 (thunk 0xff19f0)
    void InitFrisbee();
};

class CR15NetGame {
public:
    enum class EState : std::int32_t {};  // values 0..9 are switched on here; enumerators not recovered

    // libr15 0x134d478 (thunk 0x1036eb0): static (state in w0, no this); the name of a state, passed to %s.
    static const char* GameStateString(EState state);
    // libr15 0x1344758 (thunk 0x1021530): this is the CR15NetGame; the event id is the argument.
    void SendComponentEventGlobal(std::uint64_t event);

    // libr15 0x135b8b4 (thunk 0xfe6330): this function.
    void SwitchTo(EState state);
    // libr15 0x135cc00 (thunk 0x1007130)
    void UnbindInputs();
    // libr15 0x135db9c (thunk 0x1032820)
    void BindInputs();
    // libr15 0x135c03c (thunk 0xfe4120). The ReVault row at 0xfe4120 is named OnAbilityRecycled; its body only calls this.
    void RegisterLobbyCallbacks();
    // libr15 0x135d614 (thunk 0x1051410)
    void FindLobby();
    // libr15 0x135d2e4 (thunk 0xfd6c20)
    void RequestLeaderboards();
    // libr15 0x135e00c (thunk 0x104e7a0)
    void RegisterGameEvents();
    // libr15 0x1344a10 (thunk 0xfdf7b0)
    void ScheduleQuitOnError();
    // libr15 0x1341050 (thunk 0x10461b0)
    void SetErrorMessage(const char* line0);
    // libr15 0x135ce34 (thunk 0x101c570)
    void SetErrorMessage(const char* line0, const char* line1);
    // libr15 0x1341430 (thunk 0x10473d0)
    void SetErrorMessage(const char* line0, const char* line1, const char* line2, const char* line3);
};

namespace {

std::uint8_t LoadU8(const void* base, std::size_t offset) {
    std::uint8_t value;
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof value);
    return value;
}

std::uint16_t LoadU16(const void* base, std::size_t offset) {
    std::uint16_t value;
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

float LoadF32(const void* base, std::size_t offset) {
    float value;
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

void StoreF32(void* base, std::size_t offset, float value) {
    std::memcpy(static_cast<std::byte*>(base) + offset, &value, sizeof value);
}

// **(this + 0x2da0): the flags. The byte form gates state 9 and the tails; the word form selects in the switch.
std::uint8_t FlagByte(const std::byte* self) {
    return LoadU8(LoadPtr<const void>(self, 0x2da0), 0);
}

std::uint64_t FlagWord(const std::byte* self) {
    return LoadU64(LoadPtr<const void>(self, 0x2da0), 0);
}

// The ldar/ldaxr/stlxr spin of case 0: wait while the word is nonzero, then take it from 0 to 1.
void AcquireSpinLock(std::uint32_t* const lock) {
    for (;;) {
        while (__atomic_load_n(lock, __ATOMIC_ACQUIRE) != 0) {
        }
        std::uint32_t expected = 0;
        if (__atomic_compare_exchange_n(lock, &expected, 1u, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return;
        }
    }
}

// The ldaxr/stlxr pair of the binary: store desired when the 64-bit word still equals expected. A changed
// word returns false (the caller restarts); a failed store alone is retried.
bool CasWord(std::byte* const base, std::size_t offset, std::uint64_t expected, std::uint64_t desired) {
    auto* const word = reinterpret_cast<std::uint64_t*>(base + offset);
    for (;;) {
        std::uint64_t observed = expected;
        if (__atomic_compare_exchange_n(word, &observed, desired, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return true;
        }
        if (observed != expected) {
            return false;
        }
    }
}

// Lines 0x135bd8c..0x135bf04 of the disassembly (state 9). Takes the head of the free list at pool+0x80
// (index in the low word, tag in the high word), counts it at pool+0xc0, fills its 0x40-byte entry (this,
// InvokeExclusiveUpdate), then runs the transfer loop on the list at pool+0x40. Every ldaxr/stlxr pair is a
// CasWord call. Returns where the binary reaches the tail (0x135bfc8), with or without a slot taken.
void TakeSlot(std::byte* const obj8, void* const game) {
    std::byte* const pool = LoadPtr<std::byte>(obj8, 0x08);
    std::byte* const table = LoadPtr<std::byte>(obj8, 0x150);

    std::uint64_t taken = 0;
    std::uint64_t next = 0;
    for (;;) {
        taken = LoadU64(pool, 0x80);
        if (static_cast<std::uint32_t>(taken) == 0xffffffffu) {
            return;  // empty list: 0x135bd9c branches to the tail
        }
        next = LoadU64(table, static_cast<std::size_t>(static_cast<std::uint32_t>(taken)) * 0x40u);
        if (CasWord(pool, 0x80, taken, next)) {
            break;
        }
    }
    __atomic_fetch_add(reinterpret_cast<std::uint64_t*>(pool + 0xc0), std::uint64_t{1}, __ATOMIC_ACQ_REL);

    const auto slot = static_cast<std::uint32_t>(taken);
    std::byte* const entry = table + static_cast<std::size_t>(slot) * 0x40u;
    StoreU64(entry, 0x08, reinterpret_cast<std::uintptr_t>(game));
    StoreU64(entry, 0x10, reinterpret_cast<std::uintptr_t>(DAT_0382be80));
    StoreU32(entry, 0x00, 0xffffffffu);

    // Transfer loop (0x135be2c..0x135bf04). word is the stack word: its high half is rewritten each pass.
    std::uint64_t word = taken;
    for (;;) {
        const std::uint64_t head2 = LoadU64(pool, 0x40);
        const auto idx2 = static_cast<std::uint32_t>(head2);
        const auto tag2 = static_cast<std::uint32_t>(head2 >> 32);
        const std::uint64_t next2 = LoadU64(table, static_cast<std::size_t>(idx2) * 0x40u);
        const auto low_word = static_cast<std::uint32_t>(word);
        StoreU32(table, static_cast<std::size_t>(low_word) * 0x40u + 0x04u, tag2 + 2u);
        word = (static_cast<std::uint64_t>(tag2 + 1u) << 32) | low_word;
        if (LoadU64(pool, 0x40) != head2) {
            continue;  // 0x135be6c..0x135be7c: the head moved, read it again
        }
        if (static_cast<std::uint32_t>(next2) != 0xffffffffu) {
            CasWord(pool, 0x40, head2, next2);  // 0x135be94..0x135bea8: either outcome runs the loop again
            continue;
        }
        if (!CasWord(table, static_cast<std::size_t>(idx2) * 0x40u, next2, word)) {
            continue;  // 0x135bed8: restart the transfer loop
        }
        CasWord(pool, 0x40, head2, word);  // 0x135bee0..0x135bf04: either outcome reaches the tail
        return;
    }
}

}  // namespace

void CR15NetGame::SwitchTo(EState state) {
    auto* const self = reinterpret_cast<std::byte*>(this);
    const auto requested = static_cast<std::uint32_t>(state);

    // 0x135b8d8..0x135b8e0: already in the requested state, nothing to do.
    if (LoadU32(self, 0x00) == requested) {
        return;
    }
    // 0x135b8e8..0x135b908: entering state 9 runs SetSpawnPoint unless flag bit 2 is set.
    if (requested == 9 && ((FlagByte(self) >> 2) & 1u) == 0) {
        if (LoadPtr<CR15NetGameplay>(self, 0x2aa8)->SetSpawnPoint() == 0) {
            return;
        }
    }

    const std::uint32_t previous = LoadU32(self, 0x00);
    const char* const from_name = GameStateString(static_cast<EState>(previous));
    const char* const to_name = GameStateString(state);
    NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(2), 0ull,
                        "[NETGAME] NetGame switching state (from %s, to %s)", from_name, to_name);

    if (previous == 9) {
        // 0x135b958..0x135b9e4: leaving state 9.
        CVR::SetFoveationLevel(0u);
        std::byte* const lobby = LoadPtr<std::byte>(self, 0x40);
        if (reinterpret_cast<CNSLobby*>(lobby + 0x18)->Owned() != 0) {
            reinterpret_cast<CNSLobby*>(lobby + 0x18)->SetOwnerBoolean("ingame", 0u);
        }
        // Registry at this+0x2b08: array pointer at +0xbd8 (element pointer at array+0x8+0x40*k), count at
        // +0xc08, lock at +0xc10.
        std::byte* const registry = LoadPtr<std::byte>(self, 0x2b08);
        reinterpret_cast<CLockReadWrite*>(registry + 0xc10)->WriteLock();
        std::uint64_t remaining = LoadU64(registry, 0xc08);
        std::uint64_t offset = 0x08;
        while (remaining != 0) {
            void* const element =
                LoadPtr<void>(LoadPtr<const std::byte>(registry, 0xbd8), static_cast<std::size_t>(offset));
            void* const* const vtable = *reinterpret_cast<void* const* const*>(element);
            reinterpret_cast<void (*)(void*, void*)>(vtable[0])(element, self);  // slot 0; name not recovered
            --remaining;
            offset += 0x40;
        }
        reinterpret_cast<CLockReadWrite*>(registry + 0xc10)->WriteUnlock();
        if (((FlagByte(self) >> 2) & 1u) == 0) {
            UnbindInputs();
        }
        NR15RestAPI::UnregisterHTTPCallbacks(self + 0x64750);
    } else if (previous == 4) {
        RegisterLobbyCallbacks();
    }

    StoreU32(self, 0x00, requested);

    // 0x135b9f8..0x135ba70: time in seconds (low 32 bits) and the clamped timeout float.
    const std::uint64_t tick = CSysTime::GetTick();
    const std::uint64_t ticks_per_second = CSysTime::GetTicksPerSecond();
    StoreU32(self, 0x04, ticks_per_second == 0 ? 0u : static_cast<std::uint32_t>(tick / ticks_per_second));

    std::uint32_t current = LoadU32(self, 0x00);
    if (current == 1 || current == 4 || (current | 1u) == 7u) {
        // 0x135ba2c..0x135ba70: the fallback is the float at the pointer in DAT_0382ab90. The result is stored
        // at this+0x6306c; a negative result becomes +inf, and a NaN is kept (fcsel on lt).
        const float timeout =
            reinterpret_cast<CJson*>(self + 0x63280)->Real("load_timeout", LoadF32(DAT_0382ab90, 0), 0u);
        StoreF32(self, 0x6306c, timeout < 0.0f ? std::numeric_limits<float>::infinity() : timeout);
        current = LoadU32(self, 0x00);
    }

    // First switch: the table at 0x2ca3e46 (index = state + 100) selects the arms below.
    switch (current) {
        case 0xffffff9cu: {  // -100: 0x135baa8
            const auto message = MakeTStrPrintF<512>(
                "Please update to play %s.", reinterpret_cast<const char*>(LoadPtr<std::byte>(self, 0x08) + 0x838));
            SetErrorMessage("Your Oculus Quest OS is out-of-date.", reinterpret_cast<const char*>(message.storage));
            ScheduleQuitOnError();
            break;
        }
        case 0xffffff9du: {  // -99: 0x135bb7c
            const auto message = MakeTStrPrintF<512>(
                "Data is missing from your %s installation.",
                reinterpret_cast<const char*>(LoadPtr<std::byte>(self, 0x08) + 0x838));
            SetErrorMessage(reinterpret_cast<const char*>(message.storage), "Please reinstall from the Oculus store.");
            ScheduleQuitOnError();
            break;
        }
        case 0xffffff9eu:  // -98: 0x135bbac
            SetErrorMessage("No network connection detected");
            ScheduleQuitOnError();
            break;
        case 0xffffff9fu:  // -97: 0x135bbb8
            SetErrorMessage("Broadcaster initialization failed");
            ScheduleQuitOnError();
            break;
        case 0xffffffa0u:  // -96: 0x135bbc4
            // The fourth string is at 0x2c419ad (adrp 0x2c41000 + add 0x9ad). Its first byte is 00, so it is the empty string;
            // "user=%s" starts at 0x2c419ae. Passed as "", as set_delimited_error_message does.
            SetErrorMessage("Certificate validation failed.", "If you have anti-virus software, try disabling it.",
                            "Some replace valid certificates with their own.", "");
            ScheduleQuitOnError();
            break;
        case 0xffffffa1u:  // -95: 0x135bbf0; flags bit 22 picks the text
            SetErrorMessage((FlagWord(self) & 0x400000u) != 0 ? "Matchmaker is unavailable"
                                                              : "Service is unavailable");
            ScheduleQuitOnError();
            break;
        case 0xffffffa2u:  // -94: 0x135bc48
        case 0xffffffa4u:  // -92: 0x135bc48
        case 0xffffffa5u:  // -91: 0x135bc48
            ScheduleQuitOnError();
            break;
        case 0xffffffa3u: {  // -93: 0x135bc14
            void* const iface = LoadPtr<void>(self, 0x647c8);
            if (iface != nullptr) {
                const auto local_id = static_cast<std::uint32_t>(LoadPtr<CNSUser>(self, 0x28d0)->LocalID());
                void* const* const vtable = *reinterpret_cast<void* const* const*>(iface);
                reinterpret_cast<void (*)(void*, std::uint32_t)>(vtable[0x80 / sizeof(void*)])(iface, local_id);
            }
            SetErrorMessage("Another client logged in using the same account");
            ScheduleQuitOnError();
            break;
        }
        case 0u: {  // 0x135bad0: unlist the recorded ids, each under the spin lock at *(this+0x30)
            if (LoadU32(self, 0x2d84) != 0) {
                std::uint64_t index = 0;
                do {
                    const std::uint16_t id = LoadU16(self, static_cast<std::size_t>(0x2d6cu + index * 2u));
                    auto* const lock = LoadPtr<std::uint32_t>(self, 0x30);
                    AcquireSpinLock(lock);
                    reinterpret_cast<CBroadcaster*>(LoadPtr<void>(self, 0x28c8))->Unlisten(id);
                    __atomic_store_n(lock, 0u, __ATOMIC_RELEASE);
                    ++index;
                } while (index < LoadU32(self, 0x2d84));
            }
            StoreU32(self, 0x2d84, 0u);
            break;
        }
        case 4u: {  // 0x135bd00: argument block at sp, then the update call at vtable slot 0x338
            alignas(8) std::byte args[0x38];
            StoreU64(args, 0x00, 0x3f9915d3001dc28eull);  // u64 at 0x2ca02e0
            StoreU64(args, 0x08, ~std::uint64_t{0});
            StoreU64(args, 0x10, ~std::uint64_t{0});
            const auto* const realmax = static_cast<const std::byte*>(DAT_0380cb28);
            StoreU64(args, 0x18, LoadU64(realmax, 0));
            StoreU32(args, 0x20, LoadU32(realmax, 8));
            StoreU32(args, 0x24, 2u);
            StoreU64(args, 0x28, 0);
            StoreU64(args, 0x30, 0);
            void* const game = LoadPtr<void>(self, 0x08);
            void* const* const vtable = *reinterpret_cast<void* const* const*>(game);
            using Update = std::uint64_t (*)(void*, const void*, std::uint32_t, float);
            const auto update = reinterpret_cast<Update>(vtable[0x338 / sizeof(void*)]);
            const std::uint64_t handle = update(game, args, 1u, LoadF32(DAT_0381c0f8, 0));
            StoreU64(self, 0x2b08, handle);
            reinterpret_cast<CR15Game*>(game)->EndMultiplayerGameplay();
            break;
        }
        case 5u: {  // 0x135bb34
            const std::uint64_t flags = FlagWord(self);
            if ((flags & 0x4u) == 0) {
                if ((flags & 0x12u) == 0 && (flags & 0x44u) != 0x40u) {
                    FindLobby();
                    LoadPtr<CR15Game>(self, 0x08)->EnableTransitionMenu();
                } else {
                    LoadPtr<CR15NetClientLobby>(self, 0x40)->PickLevel();
                }
                break;
            }
            // 0x135bf08: lobby registration, when flag bit 2 is set and the lobby's bit 0 at +0x44c is clear.
            std::byte* const lobby = LoadPtr<std::byte>(self, 0x40);
            if ((LoadU8(lobby, 0x44c) & 1u) == 0) {
                std::byte* const peer = LoadPtr<std::byte>(lobby, 0x08);
                const std::uint64_t peer_flags = LoadU64(LoadPtr<const void>(peer, 0x2da0), 0);
                std::uint32_t kind = 6u;
                if (((peer_flags & 0x2u) != 0 || (peer_flags & 0x44u) == 0x40u) && (peer_flags & 0x8u) != 0) {
                    kind = 7u;
                }
                // CDelegate on the stack: {lobby, {AcceptSlotsCB, 0}, MemberProxy}.
                alignas(8) std::byte delegate[0x20];
                StoreU64(delegate, 0x00, reinterpret_cast<std::uintptr_t>(lobby));
                alignas(8) std::byte pair[0x10];
                StoreU64(pair, 0x00, reinterpret_cast<std::uintptr_t>(DAT_03821268));
                StoreU64(pair, 0x08, 0);
                CMemory::Copy(delegate + 0x08, pair, sizeof pair);
                StoreU64(delegate, 0x18, reinterpret_cast<std::uintptr_t>(DAT_038280a8));
                reinterpret_cast<CNSLobby*>(lobby + 0x18)
                    ->RequestRegistration(LoadU64(DAT_038196d8, 0), 0x10u, peer + 0x63240, &kind, delegate);
                StoreU32(lobby, 0x448, 2u);
            }
            break;
        }
        case 7u:  // 0x135bb70
            RequestLeaderboards();
            break;
        case 9u: {  // 0x135bd70
            CVR::SetFoveationLevel(LoadU32(DAT_03817060, 0));
            LoadPtr<CR15Game>(self, 0x08)->BeginMultiplayerGameplay();
            TakeSlot(LoadPtr<std::byte>(self, 0x08), this);
            // 0x135bfc8..0x135c02c: the tail of state 9.
            if (((FlagByte(self) >> 2) & 1u) == 0) {
                BindInputs();
            }
            NR15RestAPI::RegisterHTTPCallbacks(self + 0x64750, this);
            RegisterGameEvents();
            StoreU32(self, 0x64848, 0u);  // param_1[0x19212] = 0
            std::byte* const lobby = LoadPtr<std::byte>(self, 0x40);
            if (reinterpret_cast<CNSLobby*>(lobby + 0x18)->Owned() != 0) {
                reinterpret_cast<CNSLobby*>(lobby + 0x18)->SetOwnerBoolean("ingame", 1u);
            }
            LoadPtr<CR15NetGameplay>(self, 0x2aa8)->InitFrisbee();
            break;
        }
        default:
            break;
    }

    // Second switch: the event for each state 2..9 is the 8-byte constant at its rodata address.
    switch (LoadU32(self, 0x00)) {
        case 2u:  // 0x2ca68d8
            SendComponentEventGlobal(0x84244a28bfa226bcull);
            break;
        case 3u:  // 0x2ca6900
            SendComponentEventGlobal(0xad9f67246712e2bcull);
            break;
        case 4u:  // 0x2ca6930
            SendComponentEventGlobal(0x60433466edf3937aull);
            break;
        case 5u:  // 0x2ca6960
            SendComponentEventGlobal(0xad9f62217b1fe5b5ull);
            break;
        case 6u:  // 0x2ca6990
            SendComponentEventGlobal(0x2e0c4a4adbc510a7ull);
            break;
        case 7u:  // 0x2ca69c0
            SendComponentEventGlobal(0x23be985e8bb54bd8ull);
            break;
        case 8u:  // 0x2ca69e8
            SendComponentEventGlobal(0xe0e81b25f76105a3ull);
            break;
        case 9u:  // 0x2ca6a18
            SendComponentEventGlobal(0x077d3c2eb1ea2ea5ull);
            break;
        default:
            break;
    }
}

}  // namespace NRadGame
}  // namespace NRadEngine
