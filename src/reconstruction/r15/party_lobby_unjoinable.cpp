// NRadEngine::NRadGame::CR15NetGame::PartyLobbyUnjoinable
// Quest libr15.so VA 0x13593a4; ReVault binary 576.
// ReVault raw parent 100223448, reconstruction node 100600725; grade M: all 184 instructions traced; placeholders DAT_0386d300, DAT_0386d3f0, PTR_LOOP_036fac50 and the unnamed vtable slots +0xc8 and +0xd0 of CNSISocial; syntax-checked.
// @0x13593a4 — NRadEngine::NRadGame::CR15NetGame::PartyLobbyUnjoinable
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

// Itanium C++ ABI runtime entry points used by the function-local static (ReVault imports).
extern "C" int __cxa_guard_acquire(std::byte* guard);   // import __cxa_guard_acquire, thunk 0xff6770
extern "C" void __cxa_guard_release(std::byte* guard);  // import __cxa_guard_release, thunk 0x1051c60
extern "C" int __cxa_atexit(void (*destructor)(void*), void* object, void* dso_handle);  // import __cxa_atexit, thunk 0xfefa30

namespace NRadEngine {

// An 8-byte symbol value (begin_multiplayer.cpp, bot_actor_id.cpp).
struct CSymbol64 {
    std::uint64_t bits;
};
static_assert(sizeof(CSymbol64) == 0x08);

// Two qwords, returned in x0:x1 by CNSISocial::LobbyId and compared 16 bytes at a time (begin_multiplayer.cpp).
struct SUuid {
    // NRadEngine::SUuid::kInvalid, 16 bytes at libr15.so 0x386c3b8; reached through the slot at 0x382adc8, which holds its address.
    static const SUuid kInvalid;
    std::uint64_t words[2];
};
static_assert(sizeof(SUuid) == 0x10);

namespace CMemory {
// libr15.so VA 0x12cae74 (thunk 0x1051e10): memcmp; zero when the ranges match (c_memory_compare.cpp).
std::int64_t Compare(const void* lhs, const void* rhs, std::uint64_t count);
}  // namespace CMemory

// libr15.so VA 0x10a2250 (thunk 0xfd7760): CJson::CJson(). Two words: root at +0x00, path cache at +0x08.
class CJson {
public:
    CJson();
    // libr15.so VA 0x10a2258 (thunk 0xfdcae0): CJson::~CJson(); registered with __cxa_atexit by the function-local static.
    ~CJson();
    // Demangled: CJson::Boolean(char const*, unsigned int, unsigned int) const (create_match.cpp).
    std::uint32_t Boolean(const char* key, std::uint32_t default_value, std::uint32_t log_missing) const;
    // Demangled: CJson::Boolean(NRadEngine::CSymbol64, unsigned int) const (0x10a53d8, thunk 0xffb9e0).
    std::uint32_t Boolean(CSymbol64 symbol, std::uint32_t default_value) const;
    // Demangled: CJson::Int(char const*, long long, unsigned int) const (cjson_int.cpp).
    std::int64_t Int(const char* key, std::int64_t fallback, std::uint32_t flag) const;
    // Demangled: CJson::Symbol(char const*, NRadEngine::CSymbol64, unsigned int) const (0x10a3e5c, thunk 0x1053480).
    CSymbol64 Symbol(const char* key, CSymbol64 default_value, std::uint32_t flag) const;

private:
    alignas(8) std::byte raw[0x10];
};
static_assert(sizeof(CJson) == 0x10);

// libr15.so VA 0x1a353d0 (thunk 0xff50f0): CNSUser::ClientProfile() (create_match.cpp).
class CNSUser {
public:
    CJson* ClientProfile();
};

// CNSLobby::LobbyId() (thunk 0xfe4dd0), called on the CNSLobby at lobby + 0x18; returns a pointer to 16 bytes.
class CNSLobby {
public:
    const void* LobbyId() const;
};

// ReVault class NRadEngine::CNSISocial (vtable 0x375e648: slots +0xc8 and +0xd0 are PURE_VIRTUAL there, unnamed).
// CNSISocial::LobbyId() (thunk 0x100bbb0) is a direct call, not a virtual one; it returns the 16-byte id in x0:x1.
class CNSISocial {
public:
    SUuid LobbyId() const;
};

// NRadEngine::CVR::HeadsetType() (thunk 0x10451e0; bot_actor_id.cpp).
class CVR {
public:
    static int HeadsetType();
};

}  // namespace NRadEngine

// Placeholders: data with no name recoverable from libr15.so; the Ghidra address name is used.
extern std::byte DAT_0386d300[8];              // guard word of the static CJson; bit 0 of byte 0 is the done flag
extern NRadEngine::CJson DAT_0386d3f0;         // the function-local static CJson, built under DAT_0386d300
extern std::byte PTR_LOOP_036fac50;            // address passed as dso_handle (no __dso_handle import in ReVault)

namespace NRadEngine::NRadGame {

// Layout as in create_match.cpp: lobby at +0x40, CNSUser at +0x28d0, embedded CJson at +0x28d8.
class CR15NetGame {
public:
    bool PartyLobbyUnjoinable() const;
    // NRadEngine::NRadGame::CR15NetGame::PartyLobbyLocked() (thunk 0x1052ac0); signature from this call site only.
    std::int32_t PartyLobbyLocked() const;
};

namespace {

// Field read at a byte offset; no layout is recovered for these offsets.
template <typename T>
T LoadAt(const std::byte* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

// Raw vtable slot call: the entry at byte_offset of the object's vtable, called with the object as this.
template <typename Result>
Result InvokeUnaryVirtualAt(void* object, std::size_t byte_offset) {
    using Function = Result (*)(void*);
    auto** vtable = *reinterpret_cast<void***>(object);
    auto function = reinterpret_cast<Function>(vtable[byte_offset / sizeof(void*)]);
    return function(object);
}

// The original passes CJson::~CJson itself to __cxa_atexit; C++ cannot take a destructor's address, so this forwards to it.
void DestroyCJson(void* object) {
    static_cast<NRadEngine::CJson*>(object)->~CJson();
}

}  // namespace

bool CR15NetGame::PartyLobbyUnjoinable() const {
    const std::byte* const self = reinterpret_cast<const std::byte*>(this);

    // 0x13593b4-0x13593c4: the social object at this + 0x647c8; null returns false.
    CNSISocial* const social = LoadAt<CNSISocial*>(self, 0x647c8);
    if (social == nullptr) {
        return false;
    }

    // 0x13593c8-0x13593e0: slot 0xd0 of the social object against the 64-bit value at this + 0x647d0.
    if (InvokeUnaryVirtualAt<std::int64_t>(social, 0xd0) == LoadAt<std::int64_t>(self, 0x647d0)) {
        // 0x13593ec-0x135940c: the social object's id against kInvalid; a nonzero result continues.
        const SUuid social_id = LoadAt<CNSISocial*>(self, 0x647c8)->LobbyId();
        if (NRadEngine::CMemory::Compare(&social_id, &SUuid::kInvalid, 16) != 0) {
            // 0x1359410-0x1359434: the social object's id against the 16 bytes at this + 0x647d8; zero returns true.
            const SUuid social_id_again = LoadAt<CNSISocial*>(self, 0x647c8)->LobbyId();
            if (NRadEngine::CMemory::Compare(&social_id_again, self + 0x647d8, 16) == 0) {
                return true;
            }
        }
    }

    // 0x1359438-0x135944c: a null social object, or a nonzero slot 0xc8 result, returns false.
    CNSISocial* const social_c8 = LoadAt<CNSISocial*>(self, 0x647c8);
    if (social_c8 == nullptr || InvokeUnaryVirtualAt<std::uint32_t>(social_c8, 0xc8) != 0) {
        return false;
    }

    // 0x1359458-0x1359480: the social object's id against the id of the CNSLobby at lobby + 0x18; zero returns false.
    const SUuid social_id_lobby = LoadAt<CNSISocial*>(self, 0x647c8)->LobbyId();
    const std::byte* const lobby = LoadAt<const std::byte*>(self, 0x40);
    const void* const lobby_id = reinterpret_cast<const CNSLobby*>(lobby + 0x18)->LobbyId();
    if (NRadEngine::CMemory::Compare(&social_id_lobby, lobby_id, 16) == 0) {
        return false;
    }

    // 0x1359484-0x135949c and 0x13594b4-0x13594c8: profile() is the CJson at this + 0x28d8, or the client profile
    // of the CNSUser at this + 0x28d0 when that pointer is set. The original evaluates it again for each call.
    const auto profile = [self]() -> const CJson* {
        CNSUser* const user = LoadAt<CNSUser*>(self, 0x28d0);
        return user == nullptr ? reinterpret_cast<const CJson*>(self + 0x28d8) : user->ClientProfile();
    };

    // 0x135949c-0x13594b0: Boolean("npe|firstmatch|completed", 0, 0) == 0 returns true.
    if (profile()->Boolean("npe|firstmatch|completed", 0, 0) == 0) {
        return true;
    }
    // 0x13594b4-0x13594e0: Int("social|community|_values_version", 0, 0) == 0 returns true.
    if (profile()->Int("social|community|_values_version", 0, 0) == 0) {
        return true;
    }

    // 0x13594e4-0x13594f4: DAT_0386d3f0 is built once under the guard DAT_0386d300 (bit 0 clear means not yet built).
    std::uint8_t guard_byte = 0;
    std::memcpy(&guard_byte, DAT_0386d300, sizeof guard_byte);
    if ((guard_byte & 1u) == 0 && __cxa_guard_acquire(DAT_0386d300) != 0) {
        ::new (static_cast<void*>(&DAT_0386d3f0)) CJson();  // CJson::CJson, thunk 0xfd7760
        __cxa_atexit(DestroyCJson, static_cast<void*>(&DAT_0386d3f0), static_cast<void*>(&PTR_LOOP_036fac50));
        __cxa_guard_release(DAT_0386d300);
    }

    // 0x13594f8-0x1359518: the config pointer at this + 0x64770 selects its CJson at + 0x3a0, else DAT_0386d3f0.
    const std::byte* const config = LoadAt<const std::byte*>(self, 0x64770);
    const CJson* const json = config != nullptr ? reinterpret_cast<const CJson*>(config + 0x3a0) : &DAT_0386d3f0;

    // 0x1359500-0x1359524: Boolean(json, 0xdc501f5c717426ba, 0); the key is the 8 bytes stored at 0x2ca5f70.
    // A nonzero result goes to PartyLobbyLocked.
    if (json->Boolean(CSymbol64{0xdc501f5c717426baULL}, 0) == 0) {
        // 0x1359528-0x135952c: CVR::HeadsetType() == 3 enters the match-type test.
        if (CVR::HeadsetType() == 3) {
            // 0x1359534-0x1359568: slot 0xc8 of the social object (re-read) selects Symbol("matchtype", -1, 0) on
            // the CJson at social + 0x1f0, or else the 64-bit value at social + 0x270.
            CNSISocial* const social_match = LoadAt<CNSISocial*>(self, 0x647c8);
            CSymbol64 match_type{};
            if (InvokeUnaryVirtualAt<std::uint32_t>(social_match, 0xc8) == 0) {
                const CJson* const social_json =
                    reinterpret_cast<const CJson*>(reinterpret_cast<const std::byte*>(social_match) + 0x1f0);
                match_type = social_json->Symbol("matchtype", CSymbol64{~std::uint64_t{0}}, 0);
            } else {
                match_type.bits = LoadAt<std::uint64_t>(reinterpret_cast<const std::byte*>(social_match), 0x270);
            }

            // 0x135956c-0x1359610: a match type equal to one of seven stored 64-bit values (libr15.so 0x2ca0970,
            // 0x2cacf70, 0x2cacf98, 0x2cacfc0, 0x2cace58, 0x2cacfe8, 0x2cac920) returns true.
            if (match_type.bits == 0x3d5c3976578a321aULL) {
                return true;
            }
            if (match_type.bits == 0x33bbf6842df97a3fULL) {
                return true;
            }
            if (match_type.bits == 0x6b44ed1ad5234fbcULL) {
                return true;
            }
            if (match_type.bits == 0x4311cb2c132e9299ULL) {
                return true;
            }
            if (match_type.bits == 0x25c1c8218705f693ULL) {
                return true;
            }
            if (match_type.bits == 0x61fc299f53160641ULL) {
                return true;
            }
            if (match_type.bits == 0x47c693e5ebaddea6ULL) {
                return true;
            }
        }
    }

    // 0x135962c-0x1359634: PartyLobbyLocked() != 0.
    return PartyLobbyLocked() != 0;
}

}  // namespace NRadEngine::NRadGame
