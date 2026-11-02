// NRadEngine::NRadGame::CR15NetClientLobby::Create
// Quest libr15.so VA 0x13428e0; ReVault binary 576.
// ReVault raw parent 100223091, reconstruction node 100600731; grade M: all 363 instructions (0x13428e0-0x1342e88) checked; 3 globals stay placeholders (DAT_03731b20 VA 0x3731b20, DAT_038611b8 VA 0x38611b8, DAT_038196d8 VA 0x38196d8); every callee name is real; syntax-checked.
// @0x13428e0 — NRadEngine::NRadGame::CR15NetClientLobby::Create
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

// UNRESOLVED globals: ReVault names none of these, so each keeps its placeholder name (the name is the VA).
// VA 0x3731b20: three C string pointers indexed by the lobby type byte (relocated targets, ReVault VA:
// 0x2caceff "public", 0x2c6f177 "private", 0x32e1742 "unassigned").
extern const char* const DAT_03731b20[3];
// VA 0x38611b8: the 32-bit TLS index word passed to CSysThread::GetTLS (the same global as in
// cnca_game_space_send_component_event.cpp).
extern std::uint32_t DAT_038611b8;
// VA 0x38196d8: loader-filled slot holding a pointer to a 64-bit word. The stored target 0x2c9cca8 holds
// 0x642c62c3. It is the 64-bit argument of CNSLobby::RequestCreateSession; cr15_net_game_switch_to.cpp
// uses the same slot as the first RequestRegistration argument.
extern const std::uint64_t* const DAT_038196d8;

namespace {

// Raw access at a byte offset. ReVault has no layout for the objects these reach.
template <typename T>
T LoadAt(const std::byte* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

template <typename T>
void StoreAt(std::byte* base, std::size_t offset, T value) {
    std::memcpy(base + offset, &value, sizeof value);
}

// Raw vtable slot call: the object's vtable entry at byte_offset, called with the object as this.
template <typename Result>
Result InvokeUnaryVirtualAt(void* object, std::size_t byte_offset) {
    using Function = Result (*)(void*);
    auto** vtable = *reinterpret_cast<void***>(object);
    auto function = reinterpret_cast<Function>(vtable[byte_offset / sizeof(void*)]);
    return function(object);
}

// 64-bit symbol values the gametype is compared with. Each is the 8-byte value stored in libr15.so rodata at
// the VA shown; the symbol text it names follows it in the same record (the text is quoted in each comment).
// The identifier spells that text.
constexpr std::uint64_t kEchoArena = 0xcb60a4de7e1caf73ULL;              // 0x2ca0950 "echo_arena"
constexpr std::uint64_t kEchoCombat = 0x3d5c3976578a321aULL;             // 0x2ca0970 "echo_combat"
constexpr std::uint64_t kEchoArenaPrivate = 0x09990965f4db8c03ULL;       // 0x2ca4af0 "echo_arena_private"
constexpr std::uint64_t kEchoDemo = 0x4dc1ddbd89b28531ULL;               // 0x2ca4b18 "echo_demo"
constexpr std::uint64_t kEchoArenaFirstMatch = 0xea8d9b0bbdaa1c6aULL;    // 0x2cac900 "echo_arena_first_match"
constexpr std::uint64_t kEchoCombatFirstMatch = 0x47c693e5ebaddea6ULL;   // 0x2cac920 "echo_combat_first_match"
constexpr std::uint64_t kEchoArenaPracticeAi = 0x888ac0cfd102345fULL;    // 0x2cace30 "echo_arena_practice_ai"
constexpr std::uint64_t kEchoCombatPracticeAi = 0x25c1c8218705f693ULL;   // 0x2cace58 "echo_combat_practice_ai"
constexpr std::uint64_t kEchoArenaTournament = 0xd53a9c73b8b2c760ULL;    // 0x2cacec0 "echo_arena_tournament"
constexpr std::uint64_t kEchoDemoPublic = 0x339c5dc66c03b81fULL;         // 0x2cacee8 "echo_demo_public"
constexpr std::uint64_t kEchoArenaPublicAi = 0xd54d62c40852c2f9ULL;      // 0x2cacf08 "echo_arena_public_ai"
constexpr std::uint64_t kEchoArenaPrivateAi = 0xdf8258f63e878e9dULL;     // 0x2cacf30 "echo_arena_private_ai"
constexpr std::uint64_t kEchoArenaNpe = 0xd894afea8246bc6bULL;           // 0x2cacf58 "echo_arena_npe"
constexpr std::uint64_t kEchoCombatPrivate = 0x33bbf6842df97a3fULL;      // 0x2cacf70 "echo_combat_private"
constexpr std::uint64_t kEchoCombatTournament = 0x6b44ed1ad5234fbcULL;   // 0x2cacf98 "echo_combat_tournament"
constexpr std::uint64_t kEchoCombatPublicAi = 0x4311cb2c132e9299ULL;     // 0x2cacfc0 "echo_combat_public_ai"
constexpr std::uint64_t kEchoCombatPrivateAi = 0x61fc299f53160641ULL;    // 0x2cacfe8 "echo_combat_private_ai"

// 0x1342954-0x1342b68: the gametype compare chain. Returns the symbol the request is made with (x20) and sets
// practice (w22), which selects the practice-AI user list. The machine code loads every constant from rodata
// and compares them in this order.
std::uint64_t MapRequestGametype(std::uint64_t kind, bool& practice) {
    // 0x1342988-0x13429d4: x20 = kEchoArenaPracticeAi was loaded at 0x1342964; then five compares.
    if (kind == kEchoArenaPracticeAi || kind == kEchoCombatPracticeAi || kind == kEchoArenaPrivateAi ||
        kind == kEchoCombatPrivateAi || kind == kEchoArenaFirstMatch) {
        practice = true;  // 0x13429d8: mov w22, #1
    } else {
        practice = kind == kEchoCombatFirstMatch;  // 0x1342e54-0x1342e60: cset w22, eq
    }
    // 0x13429dc-0x13429f8 and 0x1342e64-0x1342e74: only the two first-match symbols reach the mapping chain;
    // any other symbol is passed on unchanged (0x1342b64: mov x20, x8).
    if (kind != kEchoArenaFirstMatch && kind != kEchoCombatFirstMatch) {
        return kind;
    }
    // 0x13429fc-0x1342ac0: a symbol in this group maps to kEchoArenaPracticeAi (x20 keeps its first value).
    if (kind == kEchoArena || kind == kEchoArenaPrivate || kind == kEchoArenaTournament ||
        kind == kEchoDemo || kind == kEchoDemoPublic || kind == kEchoArenaPracticeAi ||
        kind == kEchoArenaPublicAi || kind == kEchoArenaFirstMatch || kind == kEchoArenaPrivateAi ||
        kind == kEchoArenaNpe) {
        return kEchoArenaPracticeAi;  // 0x1342b68
    }
    // 0x1342ac4-0x1342b54: a symbol in this group maps to kEchoCombatPracticeAi (0x1342b58: x20 = [0x2cace58]).
    if (kind == kEchoCombat || kind == kEchoCombatPrivate || kind == kEchoCombatTournament ||
        kind == kEchoCombatPublicAi || kind == kEchoCombatPracticeAi || kind == kEchoCombatPrivateAi ||
        kind == kEchoCombatFirstMatch) {
        return kEchoCombatPracticeAi;
    }
    return kind;  // 0x1342b4c: mov x20, x8
}

}  // namespace

namespace NRadEngine {

// NRadEngine::CSymbol64: one 64-bit value (same layout as csymbol64_string.cpp).
struct CSymbol64 {
    std::uint64_t bits;  // +0x00

    // NRadEngine::CSymbol64::String() const, VA 0x12ccb48 (thunk 0x1038ec0).
    const char* String() const;
};
static_assert(sizeof(CSymbol64) == 0x08);

// NRadEngine::SUuid: two qwords. kInvalid is reached through the GOT slot at 0x382adc8 (target 0x386c3b8).
struct SUuid {
    std::uint64_t words[2];
    static const SUuid kInvalid;
};
static_assert(sizeof(SUuid) == 0x10);

// NRadEngine::SNSUserID: 16 bytes; ReVault has no layout for it.
struct SNSUserID {
    alignas(8) std::byte raw[16];
};
static_assert(sizeof(SNSUserID) == 0x10);

// NRadEngine::SNSLoginId: 16 bytes copied from CNSUser::LoginId() or SUuid::kInvalid; no layout in ReVault.
struct SNSLoginId {
    alignas(8) std::byte raw[16];
};
static_assert(sizeof(SNSLoginId) == 0x10);

// NRadEngine::ENSLobbyType: one byte at lobby + 0x698; 0, 1 and 2 index the name table
// (public, private, unassigned). The enumerators are not recovered.
enum class ENSLobbyType : std::uint8_t {};

// NRadEngine::CArrayView<T, 0ull>: ReVault has no layout; the observed words are the data pointer at +0x00
// and the element count at +0x08.
template <typename T, unsigned long long Extent>
class CArrayView {
public:
    alignas(8) std::byte raw[0x10];
};
static_assert(sizeof(CArrayView<SNSUserID, 0ull>) == 0x10);

// NRadEngine::CTempArray<T, 4294967295u>: ReVault has no layout; the observed words are the data pointer at
// +0x00, the element count at +0x08 and the TLS stack allocator at +0x10 (freed with DirectFree when set).
template <typename T, unsigned int Max>
class CTempArray {
public:
    alignas(8) std::byte raw[0x18];
};
static_assert(sizeof(CTempArray<SNSUserID, 4294967295u>) == 0x18);

class CStackAllocator {
public:
    // NRadEngine::CStackAllocator::DirectAlloc @ 0x12df278 (thunk 0x10412a0): a member; x1 bytes, x2 alignment.
    void* DirectAlloc(std::size_t bytes, std::size_t alignment);
    // NRadEngine::CStackAllocator::DirectFree @ 0x12df3fc (thunk 0x1003d40).
    void DirectFree();
};

class CSysThread {
public:
    // NRadEngine::CSysThread::GetTLS(int) @ 0x108a22c (thunk 0x104b720).
    static void* GetTLS(int index);
};

// NRadEngine::CJson: two words (root pointer, path cache); same layout as cjson_constructor.cpp.
class CJson {
public:
    // NRadEngine::CJson::CJson() @ 0x10a2250 (thunk 0xfd7760).
    CJson();
    // NRadEngine::CJson::~CJson() @ 0x10a2258 (thunk 0xfdcae0).
    ~CJson();
    // NRadEngine::CJson::TString(char const*, char const*, unsigned int) const @ 0x10a2e7c (thunk 0x1028e40).
    const char* TString(const char* key, const char* fallback, std::uint32_t flag) const;

private:
    alignas(8) std::byte raw[0x10];
};
static_assert(sizeof(CJson) == 0x10);

namespace NWriteLog {
enum class ELogLevel : std::int32_t {};  // enumerators not recovered; Create passes 2
// NRadEngine::NWriteLog::WriteLog(ELogLevel, unsigned long long, char const*, ...) @ 0x1088c20 (thunk 0x104b960).
void WriteLog(ELogLevel level, unsigned long long channel, const char* format, ...);
}  // namespace NWriteLog

class CNSUser {
public:
    // NRadEngine::CNSUser::LoginId() const @ 0x1a32c24 (thunk 0x1042e70): returns this->[0x48] + 0x358.
    const SNSLoginId& LoginId() const;
};

class CNSProvider {
public:
    // NRadEngine::CNSProvider::ProviderID(unsigned long) @ 0x1a2f27c (thunk 0x102b4e0): the argument is the
    // provider module handle; the result is passed on as a CSymbol64.
    static CSymbol64 ProviderID(std::uint64_t module);
};

class CNSLobby {
public:
    // NRadEngine::CNSLobby::RequestCreateSession(CSymbol64, unsigned long long, char const*, CSymbol64,
    // CSymbol64, CSymbol64, SNSLoginId const&, CArrayView<SNSUserID const, 0ull> const&,
    // CArrayView<unsigned short const, 0ull> const&, ENSLobbyType, unsigned int, CJson const&, SUuid const&)
    // @ 0x1a29a1c (thunk 0x1009150).
    void RequestCreateSession(CSymbol64 first_symbol, unsigned long long id, const char* publisher_lock,
                              CSymbol64 gametype, CSymbol64 level, CSymbol64 provider,
                              const SNSLoginId& login, const CArrayView<const SNSUserID, 0ull>& users,
                              const CArrayView<const unsigned short, 0ull>& match_ids,
                              ENSLobbyType lobby_type, unsigned int flag, const CJson& settings,
                              const SUuid& group);
    // NRadEngine::CNSLobby::RequestCreateSession(unsigned long long, char const*, CSymbol64, CSymbol64,
    // CSymbol64, SNSLoginId const&, CArrayView<SNSUserID const, 0ull> const&,
    // CArrayView<unsigned short const, 0ull> const&, ENSLobbyType, unsigned int, CJson const&, SUuid const&)
    // @ 0x1a29bb0 (thunk 0xfed440).
    void RequestCreateSession(unsigned long long id, const char* publisher_lock, CSymbol64 gametype,
                              CSymbol64 level, CSymbol64 provider, const SNSLoginId& login,
                              const CArrayView<const SNSUserID, 0ull>& users,
                              const CArrayView<const unsigned short, 0ull>& match_ids,
                              ENSLobbyType lobby_type, unsigned int flag, const CJson& settings,
                              const SUuid& group);
};

namespace NRadGame {

class CR15NetGame {
public:
    // NRadEngine::NRadGame::CR15NetGame::PartyCount(unsigned short) const @ 0x138e71c (thunk 0x1016460).
    std::uint64_t PartyCount(std::uint16_t index) const;
    // NRadEngine::NRadGame::CR15NetGame::Party(CArrayView<SNSUserID, 0ull> const&) const @ 0x1342e8c
    // (thunk 0xfd6e60); fills the view and returns the entry count.
    std::uint64_t Party(const CArrayView<SNSUserID, 0ull>& out) const;
};

// Layout view of the CR15NetGame fields Create reads (offsets as in create_match.cpp and find_match.cpp).
struct alignas(8) CR15NetGameLayout {
    std::byte unknown_0000[0x10];
    std::uint64_t provider_module;   // +0x10: argument of CNSProvider::ProviderID
    std::byte unknown_0018[0xc6];
    std::uint16_t state_count;       // +0xde
    std::byte unknown_00e0[0x27f0];
    CNSUser* fallback_state;         // +0x28d0
    std::byte unknown_28d8[0x4c8];
    const std::uint8_t* mode_flags;  // +0x2da0: bit 2 of its first byte skips the party lookup
    std::byte unknown_2da8[0x60498];
    std::byte json_63240[0x10];      // +0x63240: a CJson (TString "publisher_lock")
    std::byte unknown_63250[0x1578];
    void* match_service;             // +0x647c8: vtable slot 0xd8 is called; the class is not recovered
    std::byte unknown_647d0[0x1a];
    std::uint8_t flags_647ea;        // +0x647ea: bit 1 is set here
};
static_assert(offsetof(CR15NetGameLayout, provider_module) == 0x10);
static_assert(offsetof(CR15NetGameLayout, state_count) == 0xde);
static_assert(offsetof(CR15NetGameLayout, fallback_state) == 0x28d0);
static_assert(offsetof(CR15NetGameLayout, mode_flags) == 0x2da0);
static_assert(offsetof(CR15NetGameLayout, json_63240) == 0x63240);
static_assert(offsetof(CR15NetGameLayout, match_service) == 0x647c8);
static_assert(offsetof(CR15NetGameLayout, flags_647ea) == 0x647ea);
static_assert(sizeof(CR15NetGameLayout) == 0x647f0);

class CR15NetClientLobby {
public:
    // NRadEngine::NRadGame::CR15NetClientLobby::Create() @ 0x13428e0 (thunk 0x1038090): this function.
    void Create();
    // NRadEngine::NRadGame::CR15NetClientLobby::SettingsForRequest(CJson&) @ 0x13418fc (thunk 0x100a6e0).
    void SettingsForRequest(CJson& settings);
    // NRadEngine::NRadGame::CR15NetClientLobby::UsersForPracticeAIRequest(CTempArray<SNSUserID, 4294967295u>&)
    // @ 0x1341cd0 (thunk 0xfe7c40).
    void UsersForPracticeAIRequest(CTempArray<SNSUserID, 4294967295u>& users);
    // NRadEngine::NRadGame::CR15NetClientLobby::SwitchTo(int) @ 0x133e21c (thunk 0xff52f0).
    void SwitchTo(std::int32_t state);
};

// Layout view of the lobby fields Create reads. The settings block at +0x698 is the one in create_match.cpp
// and find_match.cpp (match_ids at +0x6b8, match_count at +0x6d8); the view ends at match_count and is not a
// claim about the complete object size.
struct alignas(8) CR15NetClientLobbyLayout {
    std::byte unknown_0000[0x08];
    CR15NetGame* game;                  // +0x08
    std::byte unknown_0010[0x08];
    std::byte ns_lobby[0x430];          // +0x18: the embedded CNSLobby; its extent is not recovered
    std::int32_t lobby_status;          // +0x448: compared with 14 on the practice-AI path
    std::byte unknown_044c[0x24c];
    std::int8_t lobby_type;             // +0x698: low byte of the settings' first word; ENSLobbyType
    std::byte unknown_0699[7];
    CSymbol64 gametype;                 // +0x6a0: first %s argument after the type name
    CSymbol64 level;                    // +0x6a8: second %s argument
    CSymbol64 symbol_6b0;               // +0x6b0: -1 selects the RequestCreateSession overload without it
    std::uint16_t match_ids[16];        // +0x6b8
    std::uint32_t match_count;          // +0x6d8
    std::byte unknown_06dc[4];
};
static_assert(offsetof(CR15NetClientLobbyLayout, game) == 0x08);
static_assert(offsetof(CR15NetClientLobbyLayout, ns_lobby) == 0x18);
static_assert(offsetof(CR15NetClientLobbyLayout, lobby_status) == 0x448);
static_assert(offsetof(CR15NetClientLobbyLayout, lobby_type) == 0x698);
static_assert(offsetof(CR15NetClientLobbyLayout, gametype) == 0x6a0);
static_assert(offsetof(CR15NetClientLobbyLayout, level) == 0x6a8);
static_assert(offsetof(CR15NetClientLobbyLayout, symbol_6b0) == 0x6b0);
static_assert(offsetof(CR15NetClientLobbyLayout, match_ids) == 0x6b8);
static_assert(offsetof(CR15NetClientLobbyLayout, match_count) == 0x6d8);
static_assert(sizeof(CR15NetClientLobbyLayout) == 0x6e0);

namespace {

CR15NetGameLayout* LayoutOf(CR15NetGame* game) {
    return reinterpret_cast<CR15NetGameLayout*>(game);
}

}  // namespace

void CR15NetClientLobby::Create() {
    auto* const lobby = reinterpret_cast<CR15NetClientLobbyLayout*>(this);

    // 0x13428f8-0x1342918: ldrsb of the lobby type; an index above 2 (unsigned compare) selects "invalid".
    const auto type_index = static_cast<std::uint64_t>(static_cast<std::int64_t>(lobby->lobby_type));
    const char* const type_name = type_index <= 2 ? DAT_03731b20[type_index] : "invalid";

    // 0x1342920-0x1342950: both symbols as text, then the log line at level 2, channel 0.
    const char* const gametype_name = lobby->gametype.String();
    const char* const level_name = lobby->level.String();
    NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(2), 0,
                        "[NETGAME] Requesting creation of a %s session of gametype %s, level %s", type_name,
                        gametype_name, level_name);

    // 0x1342954-0x1342b68: x20 (the request gametype) and w22 (practice-AI request).
    bool practice_request = false;
    const std::uint64_t request_gametype = MapRequestGametype(lobby->gametype.bits, practice_request);

    // 0x1342b68-0x1342b78: the settings JSON on the stack (sp+0x90), filled for this request.
    CJson settings;
    SettingsForRequest(settings);

    // 0x1342b7c-0x1342ba0: game + 0x63240 is a CJson; the fallback is the empty string, flag 0.
    const char* const publisher_lock =
        reinterpret_cast<const CJson*>(LayoutOf(lobby->game)->json_63240)->TString("publisher_lock", "", 0);

    // 0x1342ba8-0x1342bac: the temp array (sp+0x70) starts as three zero words.
    CTempArray<SNSUserID, 4294967295u> users{};

    bool send_request = true;
    if (practice_request) {
        // 0x1342bb4-0x1342bc8: the practice-AI list; a lobby status above 13 skips the request (blt 0x1342c90).
        UsersForPracticeAIRequest(users);
        send_request = lobby->lobby_status < 14;
    } else {
        // 0x1342bfc-0x1342c8c: the party list, with the temp array filled in place.
        CR15NetGame* const game = lobby->game;
        std::uint64_t count = 0;  // x22
        void* data = nullptr;     // x0 / [sp+0x70]
        // 0x1342c00-0x1342c08: bit 2 of *mode_flags forces an empty list (tbnz -> 0x1342c70).
        if ((*LayoutOf(game)->mode_flags & 0x04U) == 0) {
            std::uint32_t entries;  // w0
            if (LayoutOf(game)->state_count != 0) {
                // 0x1342c0c-0x1342c1c: PartyCount(0), low 32 bits tested with cbz.
                entries = static_cast<std::uint32_t>(game->PartyCount(0));
            } else if (LayoutOf(game)->match_service != nullptr) {
                // 0x1342c58-0x1342c6c: match_service vtable slot 0xd8.
                entries = InvokeUnaryVirtualAt<std::uint32_t>(LayoutOf(game)->match_service, 0xd8);
            } else {
                // 0x1342e78-0x1342e84: cset w0, ne on the CNSUser pointer at +0x28d0.
                entries = LayoutOf(game)->fallback_state != nullptr ? 1U : 0U;
            }
            if (entries != 0) {
                // 0x1342c20-0x1342c48: allocator from the TLS slot, 16 bytes per entry, alignment 8.
                count = entries;
                auto* const allocator = static_cast<CStackAllocator*>(CSysThread::GetTLS(static_cast<int>(DAT_038611b8)));
                StoreAt(users.raw, 0x10, allocator);  // str x0, [sp, #0x80]
                data = allocator->DirectAlloc(count << 4, 8);
                StoreAt(users.raw, 0x00, data);  // str x0, [sp, #0x70]
            }
        }
        // 0x1342c78: the count word, then Party fills a view over {data, count}.
        StoreAt(users.raw, 0x08, count);
        CArrayView<SNSUserID, 0ull> party_view{};
        StoreAt(party_view.raw, 0x00, data);  // stp x0, x22, [sp, #0x60]
        StoreAt(party_view.raw, 0x08, count);
        lobby->game->Party(party_view);  // 0x1342c7c-0x1342c8c: the game pointer is read again
    }

    if (send_request) {
        // 0x1342c90-0x1342cc8: set bit 1 of the flag byte; when the byte was zero, clear bit 1 of the match
        // service's word at +0x27c (the service pointer is read again).
        CR15NetGameLayout* game = LayoutOf(lobby->game);
        const std::uint8_t previous_flags = game->flags_647ea;
        game->flags_647ea = static_cast<std::uint8_t>(previous_flags | 0x02U);
        if (previous_flags == 0) {
            if (auto* const service = static_cast<std::byte*>(game->match_service)) {
                StoreAt(service, 0x27c, LoadAt<std::uint32_t>(service, 0x27c) & 0xfffffffdU);
            }
        }

        // 0x1342ccc-0x1342cd4: SwitchTo(4).
        SwitchTo(4);

        // 0x1342cd8-0x1342cf8: operands for the request, then the provider id.
        game = LayoutOf(lobby->game);
        const std::uint64_t symbol_6b0 = lobby->symbol_6b0.bits;  // x26
        const std::uint64_t level_bits = lobby->level.bits;       // x22
        const std::uint64_t request_id = *DAT_038196d8;           // x23
        auto* const net_lobby = reinterpret_cast<CNSLobby*>(lobby->ns_lobby);  // x24 = this + 0x18
        const CSymbol64 provider = CNSProvider::ProviderID(game->provider_module);  // x25

        // 0x1342d08-0x1342d20 and 0x1342d28-0x1342d3c (the same read on both overload paths): the login id is
        // the CNSUser's when the pointer at +0x28d0 is set, otherwise SUuid::kInvalid.
        const CNSUser* const user = LayoutOf(lobby->game)->fallback_state;
        const std::byte* login_source = reinterpret_cast<const std::byte*>(&SUuid::kInvalid);
        if (user != nullptr) {
            login_source = user->LoginId().raw;
        }
        SNSLoginId login;  // sp+0x60
        std::memcpy(login.raw, login_source, sizeof login.raw);

        // 0x1342d58 / 0x1342de0: ldr q0, [sp, #0x70]: the temp array's data pointer and count as a view.
        CArrayView<const SNSUserID, 0ull> users_view{};  // sp+0x50
        std::memcpy(users_view.raw, users.raw, sizeof users_view.raw);
        // 0x1342d5c / 0x1342d6c: the match id array at lobby + 0x6b8 with the 32-bit count at +0x6d8.
        CArrayView<const unsigned short, 0ull> match_ids_view{};  // sp+0x40
        StoreAt(match_ids_view.raw, 0x00, static_cast<const std::uint16_t*>(lobby->match_ids));
        StoreAt(match_ids_view.raw, 0x08, static_cast<std::uint64_t>(lobby->match_count));
        // 0x1342d70 / 0x1342dfc: ldr q0, [kInvalid]: the group id argument is a copy of SUuid::kInvalid.
        const SUuid group = SUuid::kInvalid;  // sp+0x30
        // 0x1342d80 / 0x1342e08: the lobby type byte is read again.
        const auto lobby_type = static_cast<ENSLobbyType>(static_cast<std::uint8_t>(lobby->lobby_type));

        if (symbol_6b0 != ~std::uint64_t{0}) {
            // 0x1342d8c-0x1342dc0: cmn x26, #1 not equal; the overload with the leading CSymbol64.
            net_lobby->RequestCreateSession(CSymbol64{symbol_6b0}, request_id, publisher_lock,
                                            CSymbol64{request_gametype}, CSymbol64{level_bits}, provider, login,
                                            users_view, match_ids_view, lobby_type, 1U, settings, group);
        } else {
            // 0x1342e18-0x1342e44: symbol_6b0 == -1; the overload without it.
            net_lobby->RequestCreateSession(request_id, publisher_lock, CSymbol64{request_gametype},
                                            CSymbol64{level_bits}, provider, login, users_view,
                                            match_ids_view, lobby_type, 1U, settings, group);
        }
    }

    // 0x1342bcc-0x1342bd4 and 0x1342dc4 / 0x1342e48: free the temp array's allocator when it is set.
    if (auto* const allocator = LoadAt<CStackAllocator*>(users.raw, 0x10)) {
        allocator->DirectFree();
    }
    // 0x1342bd8-0x1342bdc: ~CJson on sp+0x90 runs when `settings` goes out of scope, then the epilogue.
}

}  // namespace NRadGame
}  // namespace NRadEngine
