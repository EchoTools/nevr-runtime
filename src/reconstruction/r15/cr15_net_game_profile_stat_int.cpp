// NRadEngine::NRadGame::CR15NetGame::ProfileStatInt
// Quest libr15.so VA 0x137f890; ReVault binary 576.
// ReVault raw parent 100223975, reconstruction node 100600723; grade M: all 675 instructions of 0x137f890-0x1380318 read (the single ret is at 0x13801ec, the 2700-byte extent ends at 0x138031c, no brk), 0 placeholders (every callee has a ReVault name), the game-type call is CR15NetClientLobby::GameType (vtable 0x3730ab0 slot 5 @ 0x134786c) on the lobby at this+0x40, but CR15NetGame, the lobby's group-name fields at +0x478, +0x480 and +0x4d8, the per-slot stat sets and the stat nodes are raw byte offsets; compile-checked.
// @0x137f890 — NRadEngine::NRadGame::CR15NetGame::ProfileStatInt
/* @confidence: M */

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// 16-byte user id (layout not recovered). Passed by reference; CNSUser::UserID() returns it in x0:x1.
struct SNSUserID {
    alignas(8) std::byte raw[16];
    // NRadEngine::SNSUserID::Compare(SNSUserID const&, SNSUserID const&), libr15 0x13ca74c (thunk 0xfdf420).
    // The listing tests the full 64-bit result against 0.
    static long Compare(const SNSUserID& left, const SNSUserID& right);
};
static_assert(sizeof(SNSUserID) == 0x10);

class CJson {
public:
    // NRadEngine::CJson::IsInt(char const*) const, libr15 0x10a3ab4 (thunk 0xffd320).
    bool IsInt(const char* key) const;
    // NRadEngine::CJson::Int(char const*, long long, unsigned int) const, libr15 0x10a40ec (thunk 0x10152d0),
    // declared as in cjson_int.cpp.
    std::int64_t Int(const char* key, std::int64_t fallback, std::uint32_t flag) const;
};

class CNSUser {
public:
    // NRadEngine::CNSUser::UserID() const, libr15 0x1a19c9c (thunk 0x101afc0); returned in x0:x1.
    SNSUserID UserID() const;
    // NRadEngine::CNSUser::ServerProfile() const, libr15 0x1a353e0 (thunk 0xfe74b0); the body is `add x0, x0, #0x38`.
    CJson* ServerProfile() const;
};

class CNSIUsers {
public:
    // NRadEngine::CNSIUsers::Profile(SNSUserID const&) const, libr15 0x1a1c00c (thunk 0x1001d80).
    CJson* Profile(const SNSUserID& user) const;
};

class CSysString {
public:
    // NRadEngine::CSysString::Compare, libr15 0x10824ac (thunk 0xff0d10), declared as in csys_string_compare.cpp.
    static unsigned long Compare(const char* left, const char* right, unsigned int case_sensitive,
                                 unsigned long long max_length);
};

class CSymbol64 {
public:
    // NRadEngine::CSymbol64::CalculateSymbolValue, libr15 0x12cc580 (thunk 0x1046210), declared as in
    // cjson_navigate_path.cpp. Only this static function is used here.
    static std::uint64_t CalculateSymbolValue(const char* text, std::int64_t length, std::uint32_t arg3,
                                              std::uint64_t arg4, std::uint32_t arg5);
};

namespace CMemory {
// NRadEngine::CMemory::Fill, libr15 0x12cafcc (thunk 0x1047300), declared as in memory_fill.cpp.
void Fill(void* destination, unsigned char value, std::uint64_t count);
}  // namespace CMemory

// CFixedStringT<N>: the N-byte buffer; SPrintF's `this` is that buffer.
template <std::size_t N>
struct CFixedStringT {
    char text[N];
    // CFixedStringT<32>::SPrintF(char const*, ...), libr15 0x108484c (thunk 0x1038480).
    // CFixedStringT<64>::SPrintF(char const*, ...), libr15 0x108da6c (thunk 0x1051860).
    std::uint64_t SPrintF(const char* format, ...);
};

namespace NRadGame {

// The lobby object at CR15NetGame+0x40. Only the virtual slots up to GameType are declared, in vtable order
// (vtable @ 0x3730ab0): the two destructors are slots 0 and 1 (0x13454e8, 0x134570c), Update 0x1345740,
// End 0x1347320, ProcessUI 0x1347868, GameType 0x134786c at +0x28. Never constructed here.
class CR15NetClientLobby {
public:
    CR15NetClientLobby() = delete;
    virtual ~CR15NetClientLobby();
    virtual void Update();
    virtual void End();
    virtual void ProcessUI();
    // NRadEngine::NRadGame::CR15NetClientLobby::GameType() const, libr15 0x134786c; the 64-bit game type symbol
    // (the demangled `long` is 64-bit on AArch64).
    virtual std::int64_t GameType() const;
};

class CR15NetGame {
public:
    // NRadEngine::NRadGame::CR15NetGame::UserId(SNSUserID const&) const, libr15 0x138dab0 (thunk 0xfdafa0).
    // A user handle: bits 0-15 index, bits 16-31 generation, 0xffffffff invalid (see SR15NetUserId elsewhere).
    // This is a different overload from the UserId(session_id, net_user_id) at 0x135a9b8.
    std::uint32_t UserId(const SNSUserID& user) const;
    // NRadEngine::NRadGame::CR15NetGame::DailyStatGroup() const, libr15 0x1354368 (thunk 0xfeed50).
    // Returned through x8: a 32-byte string ("daily_%04hu_%02hu_%02hu" or the name at game+0x498).
    CFixedStringT<32> DailyStatGroup() const;
    // NRadEngine::NRadGame::CR15NetGame::WeeklyStatGroup() const, libr15 0x13544bc (thunk 0x102ad00).
    CFixedStringT<32> WeeklyStatGroup() const;
    // NRadEngine::NRadGame::CR15NetGame::ProfileStatInt(int&, SNSUserID const&, char const*, char const*) const,
    // libr15 0x137f890 (thunk 0x1048d90). Returns 1 when `out` was written, else 0 (w0 is 0 or 1 on every path).
    // merged login_success.cpp declares this as `void ProfileStatInt(int&, const SNSUserID&, const char* stat,
    // const char* fallback)`; the listing returns w0 and its recursive calls test it, and the fourth argument is
    // the stat name (compared with "ArenaGamesPlayed"), the third the stat group ("arena", "combat").
    bool ProfileStatInt(std::int32_t& out, const SNSUserID& user, const char* group, const char* stat) const;
};

namespace {

constexpr unsigned long long kNoLimit = 0xffffffffffffffffULL;

// CSymbol64 values CR15NetClientLobby::GameType() is compared with: the 64-bit words at 0x2ca4b30, 0x2cace80 and
// 0x2cacea0. The names come from the tree's symbol table (src/runtime/log/symcache_data.cpp).
constexpr std::uint64_t kGameTypeSocial20 = 0x042d9cf9cfddcf76ULL;         // "social_2.0"
constexpr std::uint64_t kGameTypeSocial20Private = 0x305d6e37c1589c45ULL;  // "social_2.0_private"
constexpr std::uint64_t kGameTypeSocial20Npe = 0x163956e6e431e617ULL;      // "social_2.0_npe"

// CR15NetGame holds 32 per-slot records of 0x4d08 bytes starting at this+0. Each holds four stat sets, reached as
// slot_base + offset. A set has the bucket array pointer at +0x38, the page table pointer at +0x50 and the entries
// per page (uint32) at +0x98.
constexpr std::size_t kSlotStride = 0x4d08;
constexpr std::size_t kSetForCurrentGroup = 0x2ef8;  // group equals the name at lobby+0x478 / lobby+0x4d8
constexpr std::size_t kSetForDailyGroup = 0x3e98;    // group equals DailyStatGroup()
constexpr std::size_t kSetForWeeklyGroup = 0x4e38;   // group equals WeeklyStatGroup()
constexpr std::size_t kSetForNamedGroup = 0x5dd8;    // group equals the name at lobby+0x480 (or this+0x2a10)

std::uint32_t LoadU16(const void* base, std::size_t offset) {
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

// 0x13802f0-0x1380310: fcvtzs w8, s0 rounds toward zero, saturates, and turns NaN into 0.
std::int32_t FloatToInt32(float value) {
    if (std::isnan(value)) {
        return 0;
    }
    if (value >= 2147483648.0f) {
        return INT32_MAX;
    }
    if (value <= -2147483648.0f) {
        return INT32_MIN;
    }
    return static_cast<std::int32_t>(value);
}

// 0x137f9d4-0x137fa38: the sum of the three counters, wrapping like the 32-bit adds.
std::int32_t SumInt32(std::int32_t first, std::int32_t second, std::int32_t third) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(first) + static_cast<std::uint32_t>(second) +
                                     static_cast<std::uint32_t>(third));
}

// Walks the chain of one stat set for `key` (0x137fbd0-0x137fc0c and its three repeats). The bucket array holds
// 4096 uint32 chain heads indexed by the low 12 bits of the key; an entry is 0x20 bytes: +0x00 uint32 index of the
// next entry (0xffffffff ends the chain), +0x10 uint64 key, +0x18 pointer to the stat node. An entry index is split
// into page (index / entries-per-page) and slot; udiv by zero gives 0 on AArch64, so a zero page size gives page 0.
const std::byte* FindStatEntry(const std::byte* set, std::uint64_t key) {
    const std::byte* link = LoadPtr<const std::byte>(set, 0x38) + (key & 0xfff) * 4;
    for (;;) {
        const std::uint32_t index = LoadU32(link, 0);
        if (index == 0xffffffffu) {
            return nullptr;
        }
        const std::uint32_t entries_per_page = LoadU32(set, 0x98);
        const std::byte* const pages = LoadPtr<const std::byte>(set, 0x50);
        const std::uint32_t page = entries_per_page != 0 ? index / entries_per_page : 0;
        const std::uint32_t within_page = index - page * entries_per_page;
        const std::byte* const entry = LoadPtr<const std::byte>(pages, static_cast<std::size_t>(page) * 8) +
                                       static_cast<std::size_t>(within_page) * 0x20;
        if (LoadU64(entry, 0x10) == key) {
            return entry;
        }
        link = entry;
    }
}

// What the stat-set lookup decided: continue at 0x13801f0 (read the user's JSON profile), return 0, or `out` was
// written and the function returns 1.
enum class StatLookup { kProfileJson, kMissing, kStored };

// 0x137fb30-0x138030c (and the three copies at 0x137fcc8, 0x137fe60, 0x137fff8 that differ only in `set_offset`).
StatLookup LookupStat(const CR15NetGame& game, const SNSUserID& user, std::size_t set_offset, const char* stat,
                      std::int32_t& out) {
    const auto* const self = reinterpret_cast<const std::byte*>(&game);

    // The user handle must be index 0-15 (bits 4-15 clear), generation not 0xffff and equal to the one stored for
    // that index; the index maps to an entry whose uint16 at +0x178 gives the slot (low 5 bits).
    const std::uint32_t user_id = game.UserId(user);
    if ((user_id & 0xfff0u) != 0) {
        return StatLookup::kProfileJson;
    }
    const std::uint32_t generation = (user_id >> 16) & 0xffffu;
    if (generation == 0xffffu) {
        return StatLookup::kProfileJson;
    }
    const std::size_t index = user_id & 0xffffu;
    if (generation != LoadU16(self, index * 4 + 0x13a)) {
        return StatLookup::kProfileJson;
    }
    const std::size_t entry_index = LoadU16(self, index * 4 + 0x138);
    if (entry_index == 0xffffu) {
        return StatLookup::kProfileJson;
    }
    const std::size_t slot = LoadU16(self, entry_index * 0x250 + 0x178) & 0x1fu;

    // The listing compares slot_base + set_offset with 0 (cmp x8, #-set_offset) before using the set.
    const std::uintptr_t slot_base = reinterpret_cast<std::uintptr_t>(self) + slot * kSlotStride;
    if (slot_base + set_offset == 0) {
        return StatLookup::kProfileJson;
    }
    const auto* const set = reinterpret_cast<const std::byte*>(slot_base + set_offset);

    const std::uint64_t key = CSymbol64::CalculateSymbolValue(stat, -1, 0, kNoLimit, 1);

    // Find 1: no entry means the user's JSON profile is read instead.
    if (FindStatEntry(set, key) == nullptr) {
        return StatLookup::kProfileJson;
    }

    // Find 2: the node must exist and have a non-zero divisor at +0x24.
    const std::byte* const second = FindStatEntry(set, key);
    if (second == nullptr) {
        return StatLookup::kMissing;
    }
    const std::byte* const second_node = LoadPtr<const std::byte>(second, 0x18);
    if (second_node == nullptr) {
        return StatLookup::kMissing;
    }
    if (LoadU32(second_node, 0x24) == 0) {
        return StatLookup::kMissing;
    }

    // Find 3: the node must exist and have kind 1 at +0x18.
    const std::byte* const third = FindStatEntry(set, key);
    if (third == nullptr) {
        return StatLookup::kMissing;
    }
    const std::byte* const third_node = LoadPtr<const std::byte>(third, 0x18);
    if (third_node == nullptr) {
        return StatLookup::kMissing;
    }
    if (LoadU32(third_node, 0x18) != 1) {
        return StatLookup::kMissing;
    }

    // Find 4: decode the node. Kind at +0x18 (1 integer, 2 float), representation at +0x1c (5 means value divided by
    // the uint32 at +0x24), value at +0x20.
    const std::byte* const last = FindStatEntry(set, key);
    if (last == nullptr) {
        out = 0;
        return StatLookup::kStored;
    }
    const std::byte* const node = LoadPtr<const std::byte>(last, 0x18);
    if (node == nullptr) {
        out = 0;
        return StatLookup::kStored;
    }
    const std::uint32_t kind = LoadU32(node, 0x18);
    if (kind == 2) {
        float value;
        if (LoadU32(node, 0x1c) == 5) {
            const std::uint32_t divisor = LoadU32(node, 0x24);
            value = divisor == 0 ? 0.0f : LoadF32(node, 0x20) / static_cast<float>(divisor);
        } else {
            value = LoadF32(node, 0x20);
        }
        out = FloatToInt32(value);
        return StatLookup::kStored;
    }
    if (kind != 1) {
        out = 0;
        return StatLookup::kStored;
    }
    if (LoadU32(node, 0x1c) != 5) {
        out = static_cast<std::int32_t>(LoadU32(node, 0x20));
        return StatLookup::kStored;
    }
    const std::uint32_t divisor = LoadU32(node, 0x24);
    if (divisor == 0) {
        out = 0;
        return StatLookup::kStored;
    }
    out = static_cast<std::int32_t>(LoadU32(node, 0x20) / divisor);
    return StatLookup::kStored;
}

}  // namespace

bool CR15NetGame::ProfileStatInt(std::int32_t& out, const SNSUserID& user, const char* group,
                                 const char* stat) const {
    const auto* const self = reinterpret_cast<const std::byte*>(this);

    // 0x137f8a4-0x137f8c8: a null or empty group or stat name returns 0.
    if (group == nullptr) {
        return false;
    }
    if (stat == nullptr) {
        return false;
    }
    if (*group == '\0') {
        return false;
    }
    if (*stat == '\0') {
        return false;
    }

    // 0x137f8d0-0x137fa40: "ArenaGamesPlayed" is the sum of three stats; the locals sit at sp+0x00, sp+0x40 and
    // sp+0x6c and are zeroed before the first call. The result is 1 when any of the three calls returned non-zero.
    if (CSysString::Compare(stat, "ArenaGamesPlayed", 0, kNoLimit) == 0) {
        std::int32_t wins = 0;
        std::int32_t losses = 0;
        std::int32_t ties = 0;
        const bool found_wins = ProfileStatInt(wins, user, group, "ArenaWins");
        const bool found_losses = ProfileStatInt(losses, user, group, "ArenaLosses");
        const bool found_ties = ProfileStatInt(ties, user, group, "ArenaTies");
        out = SumInt32(wins, losses, ties);
        return found_wins | found_losses | found_ties;
    }
    // 0x137f8f0-0x137fa40: "CombatGamesPlayed", same shape.
    if (CSysString::Compare(stat, "CombatGamesPlayed", 0, kNoLimit) == 0) {
        std::int32_t wins = 0;
        std::int32_t losses = 0;
        std::int32_t ties = 0;
        const bool found_wins = ProfileStatInt(wins, user, group, "CombatWins");
        const bool found_losses = ProfileStatInt(losses, user, group, "CombatLosses");
        const bool found_ties = ProfileStatInt(ties, user, group, "CombatTies");
        out = SumInt32(wins, losses, ties);
        return found_wins | found_losses | found_ties;
    }

    // 0x137f90c-0x137fa6c: the group name of the lobby (lobby = *(this + 0x40)) that applies to this stat. Level and
    // XP use the pointer at lobby+0x4d8, every other stat the pointer at lobby+0x478; with no lobby the name is "".
    // The pointer at this+0x40 is loaded again before the second read, as in the listing.
    const char* current_group;
    if (LoadPtr<const std::byte>(self, 0x40) == nullptr) {
        current_group = "";
    } else {
        const bool level_or_xp =
            CSysString::Compare(stat, "Level", 1, kNoLimit) == 0 || CSysString::Compare(stat, "XP", 1, kNoLimit) == 0;
        const std::byte* const lobby = LoadPtr<const std::byte>(self, 0x40);
        current_group = LoadPtr<const char>(lobby, level_or_xp ? 0x4d8 : 0x478);
    }

    // 0x137fa6c-0x137fb2c: pick the stat set the group names, or none (then the JSON profile is read).
    std::size_t set_offset = 0;
    if (CSysString::Compare(current_group, group, 1, kNoLimit) == 0) {
        set_offset = kSetForCurrentGroup;
    } else {
        CFixedStringT<32> period_group = DailyStatGroup();  // sret buffer at sp+0x00
        if (CSysString::Compare(period_group.text, group, 1, kNoLimit) == 0) {
            set_offset = kSetForDailyGroup;
        } else {
            period_group = WeeklyStatGroup();
            if (CSysString::Compare(period_group.text, group, 1, kNoLimit) == 0) {
                set_offset = kSetForWeeklyGroup;
            } else {
                // 0x137fab8-0x137fb2c: with no lobby, or one of the three social_2.0 game types, the name is the
                // string at this+0x2a10; otherwise the string at lobby+0x480.
                bool own_group = LoadPtr<const std::byte>(self, 0x40) == nullptr;
                if (!own_group) {
                    const auto game_type =
                        static_cast<std::uint64_t>(LoadPtr<const CR15NetClientLobby>(self, 0x40)->GameType());
                    own_group = game_type == kGameTypeSocial20 || game_type == kGameTypeSocial20Private ||
                                game_type == kGameTypeSocial20Npe;
                }
                const char* const named_group = own_group
                                                    ? reinterpret_cast<const char*>(self + 0x2a10)
                                                    : reinterpret_cast<const char*>(LoadPtr<const std::byte>(self, 0x40) + 0x480);
                if (CSysString::Compare(named_group, group, 1, kNoLimit) == 0) {
                    set_offset = kSetForNamedGroup;
                }
            }
        }
    }

    if (set_offset != 0) {
        switch (LookupStat(*this, user, set_offset, stat, out)) {
            case StatLookup::kMissing:
                return false;
            case StatLookup::kStored:
                return true;
            case StatLookup::kProfileJson:
                break;
        }
    }

    // 0x13801f0-0x1380238: the JSON profile of the user. When the local CNSUser (pointer at this+0x28d0) has the
    // same id the server profile is used, without a null test on the reloaded pointer; otherwise the profile comes
    // from the CNSIUsers at this+0x64770, and a missing one returns 0.
    bool use_server_profile = false;
    if (CNSUser* const local_user = LoadPtr<CNSUser>(self, 0x28d0)) {
        const SNSUserID local_id = local_user->UserID();
        use_server_profile = SNSUserID::Compare(local_id, user) == 0;
    }
    CJson* profile;
    if (use_server_profile) {
        profile = LoadPtr<CNSUser>(self, 0x28d0)->ServerProfile();
    } else {
        CNSIUsers* const users = LoadPtr<CNSIUsers>(self, 0x64770);
        if (users == nullptr) {
            return false;
        }
        profile = users->Profile(user);
        if (profile == nullptr) {
            return false;
        }
    }

    // 0x1380240-0x13802b8: key "stats|<group>|<stat>|val" built in a 32-byte and a 64-byte buffer (sp+0x40, sp+0x00).
    CFixedStringT<32> stats_key;
    CMemory::Fill(&stats_key, 0, 0x20);
    stats_key.SPrintF("stats|%s", group);
    CFixedStringT<64> value_key;
    CMemory::Fill(&value_key, 0, 0x40);
    value_key.SPrintF("%s|%s|val", stats_key.text, stat);
    if (profile->IsInt(value_key.text)) {
        out = static_cast<std::int32_t>(profile->Int(value_key.text, 0, 0));
        return true;
    }
    return false;
}

}  // namespace NRadGame
}  // namespace NRadEngine
