// NRadEngine::NRadGame::CR15NetClientLobby::Find
// Quest libr15.so VA 0x1343890; ReVault binary 576.
// ReVault raw parent 100223104, reconstruction node 100600733; grade M: all 366 instructions (0x1343890-0x1343e44) traced to the ret; every callee has a ReVault name; 28 globals stay placeholders (DAT_03731b20, DAT_03807e38, DAT_038196d8, DAT_038611b8 and the 24 symbol words DAT_02cac900, DAT_02cac920, DAT_02ca0950, DAT_02ca0970, DAT_02ca4af0, DAT_02ca4b18, DAT_02cace30, DAT_02cace58, DAT_02cacec0, DAT_02cacee8, DAT_02cacf08, DAT_02cacf30, DAT_02cacf58, DAT_02cacf70, DAT_02cacf98, DAT_02cacfc0, DAT_02cacfe8, DAT_02ca2418, DAT_02ca2428, DAT_02ca2440, DAT_02ca2450, DAT_02ca2468, DAT_02ca11c0, DAT_02ca1260); match-service vtable slot 0xd8 unnamed; syntax-checked.
// @0x1343890 — NRadEngine::NRadGame::CR15NetClientLobby::Find
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

// UNRESOLVED globals: ReVault names none of these 28 words, so each keeps its placeholder name (its VA).
//
// Label table at VA 0x3731b20: three char* entries. The stored words are unrelocated addends (0x2baceff,
// 0x2b6f177, 0x31e1742); at image VA + 0x100000 they read "public", "private" and "unassigned".
extern const char* const DAT_03731b20[3];
// GOT slot at VA 0x3807e38 (loader-filled, zero in the stored image): its target is a char const* variable
// whose value is the format string passed to SUuid::ToString.
extern const char* const* const DAT_03807e38;
// GOT slot at VA 0x38196d8 (loader-filled, zero in the stored image): its target is the u64 passed as the
// first argument of CNSLobby::RequestFindSession (the same slot SwitchTo reads).
extern const std::uint64_t* const DAT_038196d8;
// VA 0x38611b8: the 32-bit TLS index word read before CSysThread::GetTLS.
extern std::uint32_t DAT_038611b8;

// Symbol words in rodata (8-byte CSymbol64 hashes), compared with the lobby's match-type symbol at +0x6a0.
// In the image each word is followed by a pad of 1 to 8 bytes that is a suffix of "OlPrEfIx" (seen: "x",
// "fIx", "EfIx", "rEfIx", "PrEfIx", "lPrEfIx", "OlPrEfIx"), and then by the NUL-terminated text; the text is
// followed by a pad that is a prefix of "RaDsSyM" (seen: "Ra", "RaD", "RaDs", "RaDsS", "RaDsSyM") before the
// next word. Each comment below gives the stored value and that text (the pad bytes are not part of it).
extern const std::uint64_t DAT_02cac900;  // 0xea8d9b0bbdaa1c6a, "echo_arena_first_match"
extern const std::uint64_t DAT_02cac920;  // 0x47c693e5ebaddea6, "echo_combat_first_match"
extern const std::uint64_t DAT_02ca0950;  // 0xcb60a4de7e1caf73, "echo_arena"
extern const std::uint64_t DAT_02ca0970;  // 0x3d5c3976578a321a, "echo_combat"
extern const std::uint64_t DAT_02ca4af0;  // 0x09990965f4db8c03, "echo_arena_private"
extern const std::uint64_t DAT_02ca4b18;  // 0x4dc1ddbd89b28531, "echo_demo"
extern const std::uint64_t DAT_02cace30;  // 0x888ac0cfd102345f, "echo_arena_practice_ai"
extern const std::uint64_t DAT_02cace58;  // 0x25c1c8218705f693, "echo_combat_practice_ai"
extern const std::uint64_t DAT_02cacec0;  // 0xd53a9c73b8b2c760, "echo_arena_tournament"
extern const std::uint64_t DAT_02cacee8;  // 0x339c5dc66c03b81f, "echo_demo_public"
extern const std::uint64_t DAT_02cacf08;  // 0xd54d62c40852c2f9, "echo_arena_public_ai"
extern const std::uint64_t DAT_02cacf30;  // 0xdf8258f63e878e9d, "echo_arena_private_ai"
extern const std::uint64_t DAT_02cacf58;  // 0xd894afea8246bc6b, "echo_arena_npe"
extern const std::uint64_t DAT_02cacf70;  // 0x33bbf6842df97a3f, "echo_combat_private"
extern const std::uint64_t DAT_02cacf98;  // 0x6b44ed1ad5234fbc, "echo_combat_tournament"
extern const std::uint64_t DAT_02cacfc0;  // 0x4311cb2c132e9299, "echo_combat_public_ai"
extern const std::uint64_t DAT_02cacfe8;  // 0x61fc299f53160641, "echo_combat_private_ai"

// Symbol words in rodata compared with -1 when the game has no user object (cmn x8, #1).
extern const std::uint64_t DAT_02ca2418;  // 0xc8e8d0b1a883f6e7, "STM"
extern const std::uint64_t DAT_02ca2428;  // 0xc8e8d0b1a880f1e4, "PSN"
extern const std::uint64_t DAT_02ca2440;  // 0xc8e8d0b1a888e0f2, "XBX"
extern const std::uint64_t DAT_02ca11c0;  // 0xc8e8d0b1a89ff4f8, "OVR"
extern const std::uint64_t DAT_02ca1260;  // 0xc8e8d0b1a894efe5, "DMO"
extern const std::uint64_t DAT_02ca2450;  // 0xc8e8d0b1a897f1fe, "GST"
extern const std::uint64_t DAT_02ca2468;  // 0xc8e8d0b1a892edfe, "BOT"

namespace NRadEngine {

// One 8-byte hash word, passed by value in one register.
struct CSymbol64 {
    std::uint64_t bits;
    // NRadEngine::CSymbol64::String() const @ 0x12ccb48 (thunk 0x1038ec0); this is the address of the word.
    const char* String() const;
};
static_assert(sizeof(CSymbol64) == 0x08);
static_assert(offsetof(CSymbol64, bits) == 0x00);

// 16 bytes (two words, as in begin_multiplayer.cpp).
struct SUuid {
    std::uint64_t words[2];

    // NRadEngine::SUuid::kInvalid: the loader-filled GOT slot at VA 0x382adc8 holds its address.
    static const SUuid kInvalid;

    // NRadEngine::SUuid::ToString(char*, unsigned long long, char const*) const @ 0x12f1650 (thunk 0x100bf30).
    // this is x0, the buffer x1, its size x2 and the format x3.
    void ToString(char* buffer, unsigned long long size, const char* format) const;
};
static_assert(sizeof(SUuid) == 0x10);

// 16-byte value returned in x0:x1 by CNSUser::UserID(); the first word is the user object's field at +0x90.
struct SNSUserID {
    alignas(8) std::byte raw[16];
};
static_assert(sizeof(SNSUserID) == 0x10);

// Find copies 16 bytes of it, from CNSUser::LoginId() or from SUuid::kInvalid; the layout is not recovered.
struct SNSLoginId {
    alignas(8) std::byte raw[16];
};
static_assert(sizeof(SNSLoginId) == 0x10);

// NRadEngine::CArrayView<T, 0ull>: a data pointer and a 64-bit element count (stp x25, x19).
template <typename T, unsigned long long Extent>
struct CArrayView {
    T* data;
    std::uint64_t count;
};
static_assert(sizeof(CArrayView<std::uint16_t, 0>) == 0x10);

class CJson {
public:
    // NRadEngine::CJson::CJson() @ 0x10a2250 (thunk 0xfd7760)
    CJson();
    // NRadEngine::CJson::~CJson() @ 0x10a2258 (thunk 0xfdcae0)
    ~CJson();
    // NRadEngine::CJson::TString @ 0x10a2e7c (thunk 0x1028e40); the same shape as in login_success.cpp.
    const char* TString(const char* key, const char* fallback, std::uint32_t flags) const;

private:
    // Two words: the json root and the path cache pointer.
    alignas(8) std::byte raw[0x10];
};
static_assert(sizeof(CJson) == 0x10);

class CMemory {
public:
    // NRadEngine::CMemory::Fill @ 0x12cafcc (thunk 0x1047300): a tail jump to memset.
    static void Fill(void* destination, unsigned char value, std::uint64_t count);
};

class CSysThread {
public:
    // NRadEngine::CSysThread::GetTLS @ 0x108a22c (thunk 0x104b720): static, the key is w0.
    static void* GetTLS(int key);
};

struct CStackAllocator {
    // NRadEngine::CStackAllocator::DirectAlloc @ 0x12df278 (thunk 0x10412a0): the allocator is this (x0),
    // the byte count x1 and the alignment x2.
    void* DirectAlloc(std::size_t bytes, std::size_t alignment);
    // NRadEngine::CStackAllocator::DirectFree @ 0x12df3fc (thunk 0x1003d40).
    void DirectFree();
};

namespace NWriteLog {
// Enumerators not established; Find passes 2.
enum ELogLevel : std::int32_t {};
// NRadEngine::NWriteLog::WriteLog @ 0x1088c20 (thunk 0x104b960). Variadic.
void WriteLog(ELogLevel level, unsigned long long channel, const char* format, ...);
}  // namespace NWriteLog

class CNSUser {
public:
    // NRadEngine::CNSUser::UserID() const @ 0x1a19c9c (thunk 0x101afc0); returns x0:x1.
    SNSUserID UserID() const;
    // NRadEngine::CNSUser::LoginId() const @ 0x1a32c24 (thunk 0x1042e70); returns user->[0x48] + 0x358.
    const SNSLoginId& LoginId() const;
};

class CNSProvider {
public:
    // NRadEngine::CNSProvider::ProviderID(unsigned long) @ 0x1a2f27c (thunk 0x102b4e0); the argument is
    // the u64 at game + 0x10 and the result is a CSymbol64 in x0.
    static CSymbol64 ProviderID(std::uint64_t module);
};

class CNSLobby {
public:
    // NRadEngine::CNSLobby::RequestFindSession(unsigned long long, char const*, CSymbol64, CSymbol64,
    // CSymbol64, SNSLoginId const&, CArrayView<SNSUserID const, 0ull> const&,
    // CArrayView<unsigned short const, 0ull> const&, unsigned int, CJson const&, SUuid const&,
    // SUuid const&) @ 0x1a29dd8 (thunk 0x104f6e0). this is the CNSLobby at lobby + 0x18; the last five
    // arguments after the 8th are passed on the stack.
    void RequestFindSession(unsigned long long id, const char* publisher_lock, CSymbol64 match_type,
                            CSymbol64 level, CSymbol64 provider, const SNSLoginId& login,
                            const CArrayView<const SNSUserID, 0>& user_ids,
                            const CArrayView<const std::uint16_t, 0>& match_ids, unsigned int flag,
                            const CJson& request, const SUuid& session, const SUuid& group);
};

namespace NRadGame {

class CR15NetMatchmakerQueue;

class CR15NetMatchmakerQueues {
public:
    // NRadEngine::NRadGame::CR15NetMatchmakerQueues::SwitchToQueueForMatchType(CSymbol64) @ 0x133e04c
    // (thunk 0x103f650). Returns the queue record; Find reads its u32 at +0x58.
    CR15NetMatchmakerQueue* SwitchToQueueForMatchType(CSymbol64 match_type);
};

class CR15NetGame {
public:
    // NRadEngine::NRadGame::CR15NetGame::PartyCount(unsigned short) const @ 0x138e71c (thunk 0x1016460),
    // declared as in create_match.cpp.
    std::uint64_t PartyCount(std::uint16_t index) const;
    // NRadEngine::NRadGame::CR15NetGame::Party(CArrayView<SNSUserID, 0ull> const&) const @ 0x1342e8c
    // (thunk 0xfd6e60). Find discards the result.
    std::uint64_t Party(const CArrayView<SNSUserID, 0>& users) const;
};

class CR15NetClientLobby {
public:
    // NRadEngine::NRadGame::CR15NetClientLobby::Find(SUuid const&, SUuid const&) @ 0x1343890 (thunk 0xfeb930).
    void Find(const SUuid& session, const SUuid& group);
    // NRadEngine::NRadGame::CR15NetClientLobby::SettingsForRequest(CJson&) @ 0x13418fc (thunk 0x100a6e0).
    void SettingsForRequest(CJson& request);
    // NRadEngine::NRadGame::CR15NetClientLobby::SwitchTo(int) @ 0x133e21c (thunk 0xff52f0).
    void SwitchTo(std::int32_t state);
};

}  // namespace NRadGame
}  // namespace NRadEngine

namespace {

// ReVault has no layout for the lobby, the game, the matchmaker queues or the match service, so each field is
// reached by its byte offset (the offset and what it holds are in the comment at each use).
template <typename T>
T LoadAt(const std::byte* base, std::size_t offset) {
    T value;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

template <typename T>
void StoreAt(std::byte* base, std::size_t offset, T value) {
    std::memcpy(base + offset, &value, sizeof value);
}

template <typename T>
T* LoadPtr(const std::byte* base, std::size_t offset) {
    return reinterpret_cast<T*>(static_cast<std::uintptr_t>(LoadAt<std::uint64_t>(base, offset)));
}

// Raw vtable slot call: the object's vtable entry at byte_offset, called with the object as this.
template <typename Result>
Result InvokeUnaryVirtualAt(void* object, std::size_t byte_offset) {
    using Function = Result (*)(void*);
    auto** vtable = *reinterpret_cast<void***>(object);
    auto function = reinterpret_cast<Function>(vtable[byte_offset / sizeof(void*)]);
    return function(object);
}

constexpr std::uint64_t kAllOnes = ~std::uint64_t{0};

}  // namespace

namespace NRadEngine::NRadGame {

void CR15NetClientLobby::Find(const SUuid& session, const SUuid& group) {
    // Lobby fields used: +0x08 CR15NetGame*, +0x18 the embedded CNSLobby, +0x698 settings byte,
    // +0x6a0 match-type CSymbol64, +0x6a8 level CSymbol64, +0x6b8 match ids (u16[16]), +0x6d8 match count,
    // +0x910 CR15NetMatchmakerQueues*.
    std::byte* const self = reinterpret_cast<std::byte*>(this);
    const auto game = [self]() { return LoadPtr<std::byte>(self, 0x08); };

    // 0x13438ac-0x13438d8: the signed settings byte at +0x698 indexes the label table; outside 0..2 it is "invalid".
    const std::int64_t kind_index = LoadAt<std::int8_t>(self, 0x698);
    const char* const kind_label =
        static_cast<std::uint64_t>(kind_index) < 3 ? DAT_03731b20[kind_index] : "invalid";

    // 0x13438dc-0x13438f0: both symbol words are turned into text before the group is.
    const char* const match_type_text = reinterpret_cast<const CSymbol64*>(self + 0x6a0)->String();
    const char* const level_text = reinterpret_cast<const CSymbol64*>(self + 0x6a8)->String();

    // 0x13438f4-0x134391c: the group UUID as text, in a zeroed 0x25-byte buffer.
    char group_text[0x25];
    CMemory::Fill(group_text, 0, sizeof group_text);
    group.ToString(group_text, sizeof group_text, *DAT_03807e38);

    // 0x1343920-0x1343940
    NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(2), 0,
                        "[NETGAME] Requesting search for a %s session of gametype %s, level %s, group %s",
                        kind_label, match_type_text, level_text, group_text);

    // 0x1343944-0x1343afc: only the two first-match words (0x2cac900, 0x2cac920) enter the lookup. It then
    // compares 17 symbol words in this order: a hit among the first ten (the arena first-match word is the
    // eighth) takes the word at 0x2cace30, a hit among the last seven (the combat first-match word is the
    // last) takes the word at 0x2cace58; the asm loads each word in turn.
    std::uint64_t match_type = LoadAt<std::uint64_t>(self, 0x6a0);
    if (match_type == DAT_02cac900 || match_type == DAT_02cac920) {
        if (match_type == DAT_02ca0950 || match_type == DAT_02ca4af0 || match_type == DAT_02cacec0 ||
            match_type == DAT_02ca4b18 || match_type == DAT_02cacee8 || match_type == DAT_02cacf08 ||
            match_type == DAT_02cace30 || match_type == DAT_02cac900 || match_type == DAT_02cacf30 ||
            match_type == DAT_02cacf58) {
            match_type = DAT_02cace30;
        } else if (match_type == DAT_02ca0970 || match_type == DAT_02cacf70 ||
                   match_type == DAT_02cacf98 || match_type == DAT_02cacfc0 ||
                   match_type == DAT_02cace58 || match_type == DAT_02cacfe8 ||
                   match_type == DAT_02cac920) {
            match_type = DAT_02cace58;
        }
    }

    // 0x1343b00-0x1343c1c: queue switch. queues = lobby + 0x910; *queues is the CR15NetGame (its +0x28d0 is
    // the CNSUser pointer); queues + 0xf0 is a u32.
    CR15NetMatchmakerQueues* const queues = LoadPtr<CR15NetMatchmakerQueues>(self, 0x910);
    const std::byte* const queues_bytes = reinterpret_cast<const std::byte*>(queues);
    const CNSUser* const queues_user = LoadPtr<const CNSUser>(LoadPtr<const std::byte>(queues_bytes, 0x00), 0x28d0);

    bool queue_candidate;
    if (queues_user != nullptr) {
        // 0x1343b10-0x1343b20: low word of UserID(); (word & 0xe | 1) == 5.
        const SNSUserID user_id = queues_user->UserID();
        const std::uint64_t user_word = LoadAt<std::uint64_t>(user_id.raw, 0);
        queue_candidate = ((user_word & 0xe) | 1) == 5;
    } else {
        // 0x1343b64-0x1343c1c: no user object. Seven symbol words are compared with -1 in this order; the
        // OVR word (0x2ca11c0) equal to -1 jumps straight to the queue test at 0x1343b24, any other -1 word
        // skips it. With none of them -1 the last word picks 6 or 0 and neither passes (word & 0xe | 1) == 5.
        queue_candidate = false;
        if (DAT_02ca2418 != kAllOnes && DAT_02ca2428 != kAllOnes && DAT_02ca2440 != kAllOnes) {
            if (DAT_02ca11c0 == kAllOnes) {
                queue_candidate = true;
            } else if (DAT_02ca1260 != kAllOnes && DAT_02ca2450 != kAllOnes) {
                const std::uint64_t word = DAT_02ca2468 == kAllOnes ? 6 : 0;
                queue_candidate = ((word & 0xe) | 1) == 5;
            }
        }
    }

    if (queue_candidate) {
        // 0x1343b24-0x1343b5c
        const std::uint32_t queues_state = LoadAt<std::uint32_t>(queues_bytes, 0xf0);
        if (queues_state != 0) {
            const std::byte* const queue = reinterpret_cast<const std::byte*>(
                LoadPtr<CR15NetMatchmakerQueues>(self, 0x910)->SwitchToQueueForMatchType(CSymbol64{match_type}));
            // The queue record's u32 at +0x58 must not be 2, and the state must be 2 or 3.
            if (queue != nullptr && (queues_state | 1) == 3 && LoadAt<std::uint32_t>(queue, 0x58) != 2) {
                SwitchTo(3);
                return;
            }
        }
    }

    // 0x1343c20-0x1343c30: the request JSON, filled by SettingsForRequest.
    CJson request;
    SettingsForRequest(request);

    // 0x1343c34-0x1343c60: game + 0x63240 is a CJson; the "publisher_lock" text (empty fallback, flags 0)
    // is kept for the request.
    const char* const publisher_lock =
        reinterpret_cast<const CJson*>(game() + 0x63240)->TString("publisher_lock", "", 0);

    // 0x1343c64-0x1343d14: the party id array. party_ids (x25) and party_count (x19) stay 0 unless the party
    // is counted; allocator (x24) is the TLS stack allocator the array came from.
    std::uint64_t party_count = 0;
    CStackAllocator* allocator = nullptr;
    SNSUserID* party_ids = nullptr;

    // game + 0x2da0 points at the mode flags; bit 2 of the first byte skips the count.
    if ((LoadAt<std::uint8_t>(LoadPtr<const std::byte>(game(), 0x2da0), 0) & 0x04U) == 0) {
        std::uint32_t wanted;
        if (LoadAt<std::uint16_t>(game(), 0xde) != 0) {
            // 0x1343c78-0x1343c84: PartyCount(0).
            wanted = static_cast<std::uint32_t>(reinterpret_cast<const CR15NetGame*>(game())->PartyCount(0));
        } else if (void* const service = LoadPtr<void>(game(), 0x647c8)) {
            // 0x1343cb4-0x1343cc8: the match service at game + 0x647c8, vtable slot 0xd8 (not named).
            wanted = InvokeUnaryVirtualAt<std::uint32_t>(service, 0xd8);
        } else {
            // 0x1343cd8-0x1343ce8: game + 0x28d0 is the CNSUser pointer; one id when it is set.
            wanted = LoadPtr<const CNSUser>(game(), 0x28d0) != nullptr ? 1U : 0U;
        }
        if (wanted != 0) {
            // 0x1343cf0-0x1343d14: wanted * 16 bytes, 8-aligned, from the TLS stack allocator.
            party_count = wanted;
            allocator = static_cast<CStackAllocator*>(CSysThread::GetTLS(static_cast<int>(DAT_038611b8)));
            party_ids = static_cast<SNSUserID*>(allocator->DirectAlloc(static_cast<std::size_t>(wanted) << 4, 8));
        }
    }

    // 0x1343d18-0x1343d24: fill the id array from the game's party.
    const CArrayView<SNSUserID, 0> party_out{party_ids, party_count};
    reinterpret_cast<const CR15NetGame*>(game())->Party(party_out);

    // 0x1343d28-0x1343d60: set bit 1 of the byte at game + 0x647ea; when it was 0 and a match service is
    // set, clear bit 1 of the u32 at service + 0x27c.
    {
        std::byte* const g = game();
        const std::uint8_t old_flags = LoadAt<std::uint8_t>(g, 0x647ea);
        StoreAt<std::uint8_t>(g, 0x647ea, static_cast<std::uint8_t>(old_flags | 0x02U));
        if (old_flags == 0) {
            if (std::byte* const service = LoadPtr<std::byte>(g, 0x647c8)) {
                StoreAt<std::uint32_t>(service, 0x27c, LoadAt<std::uint32_t>(service, 0x27c) & 0xfffffffdU);
            }
        }
    }

    // 0x1343d64-0x1343d6c
    SwitchTo(5);

    // 0x1343d70-0x1343d8c: the request id (u64 behind the GOT slot), the level word, the provider symbol.
    const std::uint64_t request_id = *DAT_038196d8;
    const std::uint64_t level = LoadAt<std::uint64_t>(self, 0x6a8);
    const CSymbol64 provider = CNSProvider::ProviderID(LoadAt<std::uint64_t>(game(), 0x10));

    // 0x1343d90-0x1343dcc: the login id is 16 bytes of the user's LoginId(), or of SUuid::kInvalid.
    SNSLoginId login;
    if (const CNSUser* const user = LoadPtr<const CNSUser>(game(), 0x28d0)) {
        std::memcpy(&login, &user->LoginId(), sizeof login);
    } else {
        std::memcpy(&login, &SUuid::kInvalid, sizeof login);
    }

    // 0x1343dc0-0x1343e10: the two views and the call. user_ids is the id array, match_ids is the lobby's
    // match id array with its count at +0x6d8.
    const CArrayView<const SNSUserID, 0> user_ids{party_ids, party_count};
    const CArrayView<const std::uint16_t, 0> match_ids{reinterpret_cast<const std::uint16_t*>(self + 0x6b8),
                                                       LoadAt<std::uint32_t>(self, 0x6d8)};
    reinterpret_cast<CNSLobby*>(self + 0x18)
        ->RequestFindSession(request_id, publisher_lock, CSymbol64{match_type}, CSymbol64{level}, provider,
                             login, user_ids, match_ids, 1, request, session, group);

    // 0x1343e14-0x1343e1c: free the id array; ~CJson (0x1343e24) runs at the end of the scope.
    if (allocator != nullptr) {
        allocator->DirectFree();
    }
}

}  // namespace NRadEngine::NRadGame
