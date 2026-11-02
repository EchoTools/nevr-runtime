// NRadEngine::NRadGame::CR15NetClientLobby::SwitchTo
// Quest libr15.so VA 0x133e21c; ReVault binary 576.
// ReVault raw parent 100222977, reconstruction node 100600742; grade H: all 1031 instructions of 0x133e21c-0x133f234 (ret at 0x133eeec) checked against the disassembly, both jump tables (0x2ca3d60, 0x2ca3d8a) decoded, every callee, global and type has a real ReVault or Ghidra name (0 placeholders), the eight-byte event, game-type and platform words are read from the file at the addresses cited, syntax-checked.
// @0x133e21c — NRadEngine::NRadGame::CR15NetClientLobby::SwitchTo
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// 16 bytes. NRadEngine::SUuid::kInvalid is read through the GOT slot 0x382adc8.
struct SUuid {
    std::uint64_t words[2];
    static const SUuid kInvalid;
};
static_assert(sizeof(SUuid) == 0x10);

// NRadEngine::SNSLoginId: 16 bytes, copied by value into the frame before the two Cancel calls.
struct SNSLoginId {
    std::uint64_t words[2];
};
static_assert(sizeof(SNSLoginId) == 0x10);

// NRadEngine::SNSUserID: 16 bytes returned in x0:x1 by CNSUser::UserID(). words[0] carries the platform code in its
// low four bits (CNSJsonLog::SetUserId indexes its prefix table with (words[0] & 0xf) - 1); words[1] is the number.
struct SNSUserID {
    std::uint64_t words[2];
};
static_assert(sizeof(SNSUserID) == 0x10);

// NRadEngine::SPeer: a peer is one 64-bit word. kSelf is read through the GOT slot 0x381aae0, kInvalidPeer through
// 0x380fc48.
struct SPeer {
    static const std::uint64_t kSelf;
    static const std::uint64_t kInvalidPeer;
};

namespace NWriteLog {
enum class ELogLevel : std::int32_t {};  // enumerators not recovered; this function passes 2
// NRadEngine::NWriteLog::WriteLog @0x1088c20, called through the stub at 0x104b960.
void WriteLog(ELogLevel level, std::uint64_t category, const char* format, ...);
}  // namespace NWriteLog

// STempString<N>: the temporary that MakeTStrPrintF returns in the caller's frame (x8 carries its address).
template <std::size_t N>
struct STempString {
    unsigned char storage[N];
};

// NRadEngine::MakeTStrPrintF<512ull>(char const*, ...) @0x1086b9c, thunk 0x102b370.
template <std::size_t N>
STempString<N> MakeTStrPrintF(const char* format, ...);

// NRadEngine::CStackAllocator::DirectFree @0x12df3fc, thunk 0x1003d40: a member, this = the allocator.
struct CStackAllocator {
    void DirectFree();
};

class CSysString {
public:
    // NRadEngine::CSysString::Length(char const*), reached through the stub at 0xff4b80.
    static std::int64_t Length(const char* text);
};

class CSysTime {
public:
    // NRadEngine::CSysTime::GetTime() @0x1084d28, thunk 0xff2ab0: Unix seconds, or -1 when the clock call fails.
    static std::int64_t GetTime();
};

class CBroadcaster {
public:
    // All of them take an SPeer (one 64-bit word) in x1; the four metrics return a float in s0.
    // CBroadcaster::PeerDisplayName(SPeer) const @0x25e88a4, thunk 0x102b780.
    const char* PeerDisplayName(std::uint64_t peer) const;
    // CBroadcaster::Ping(SPeer) const @0x25f1f58, thunk 0xfe9f40.
    float Ping(std::uint64_t peer) const;
    // CBroadcaster::TimeSinceLastReceived(SPeer) const @0x25f13bc, thunk 0x1050670.
    float TimeSinceLastReceived(std::uint64_t peer) const;
    // CBroadcaster::DecodeFailureRate(SPeer) const @0x25f11b8, thunk 0x1052140.
    float DecodeFailureRate(std::uint64_t peer) const;
    // CBroadcaster::DropPercent(SPeer) const @0x25f14f0, thunk 0x100cfa0.
    float DropPercent(std::uint64_t peer) const;
};

class CNSUser {
public:
    // CNSUser::UserID() const @0x1a19c9c, thunk 0x101afc0.
    SNSUserID UserID() const;
    // CNSUser::LoginId() const @0x1a32c24, thunk 0x1042e70: the address *(this + 0x48) + 0x358.
    const SNSLoginId& LoginId() const;
};

class CNSLobby {
public:
    // CNSLobby::HostPeer() const @0x1a2d0b4, thunk 0x1027cf0.
    std::uint64_t HostPeer() const;
    // CNSLobby::LobbyId() const @0x1a18c74, thunk 0xfe4dd0: the address this + 0x1cc.
    const SUuid& LobbyId() const;
    // CNSLobby::CancelFindingSession(SNSLoginId const&) @0x1a2a264, thunk 0x104b7e0.
    void CancelFindingSession(const SNSLoginId& login);
    // CNSLobby::CancelPendingSession(SNSLoginId const&) @0x1a2a1d8, thunk 0x100a6b0.
    void CancelPendingSession(const SNSLoginId& login);
};

// NRadEngine::CTempJsonString: 0x40 bytes, vtable_va 0x36fb2b8. +0x00 is the vtable pointer, +0x08 the character
// array pointer, +0x18 the CStackAllocator to free (zero until a call stores one), +0x20 the length, +0x2c a u32
// that the construction sets to 1, +0x38 the flag CNSJsonLog::SetSessionId sets. Everything else is zero.
class CTempJsonString {
public:
    enum class ERootType : std::int32_t {};  // enumerators not recovered; 0 is passed at every call here

    // The default construction and destruction are inlined at 0x133e6fc..0x133e724 and 0x133e928..0x133e940 (and at
    // 0x133e484 and 0x133f0dc..0x133f0f4): zero the fields and store 1 at +0x2c; at the end the vtable pointer
    // becomes the CTempJsonString one (036fb2b8) and the allocator at +0x18 is freed when one is stored.
    CTempJsonString() : fields_{} {
        const std::uint32_t one = 1;
        std::memcpy(fields_ + 0x24, &one, sizeof one);
    }
    ~CTempJsonString() {
        CStackAllocator* allocator = nullptr;
        std::memcpy(&allocator, fields_ + 0x10, sizeof allocator);
        if (allocator != nullptr) {
            allocator->DirectFree();
        }
    }

    // Vtable slot 0 @0x10abba8 and slot 1 @0x10ad3f8.
    virtual char* Finalize(std::uint32_t flags);
    virtual void WriteLog(NWriteLog::ELogLevel level);

    // CTempJsonString::EnsureRoot(ERootType) @0x12e5258, thunk 0x10321f0.
    void EnsureRoot(ERootType root);
    // CTempJsonString::NextElement(unsigned long long) @0x12e54d4, thunk 0xfe0660: the length reserved for the element.
    void NextElement(std::uint64_t length);
    // CTempJsonString::SAppendF(char const*, ...) @0x12e53f4, thunk 0xfe5000.
    void SAppendF(const char* format, ...);
    // CTempJsonString::SetFormatted(char const*, ...) @0x12e5790, thunk 0xfd67b0.
    void SetFormatted(const char* format, ...);
    // CTempJsonString::EscapeAppend(char const*, unsigned long long) @0x12e55b0, thunk 0x104af70.
    void EscapeAppend(const char* text, std::uint64_t length);
    // CTempJsonString::Append(char) @0x12e52d4, thunk 0xffc120 (ReVault names that stub ListenProxy<...>).
    void Append(char c);

private:
    alignas(8) std::byte fields_[0x38];
};
static_assert(sizeof(CTempJsonString) == 0x40);

// NRadEngine::CJsonLog (vtable_va 0x36fb2f8) adds nothing to the layout.
class CJsonLog : public CTempJsonString {};

// NRadEngine::CNSJsonLog (vtable_va 0x37322e8).
class CNSJsonLog : public CJsonLog {
public:
    // CNSJsonLog::SetUserId(char const*, SNSUserID const&) @0x263b47c, thunk 0xff4f10: returns this.
    CNSJsonLog* SetUserId(const char* key, const SNSUserID& user);
    // CNSJsonLog::SetSessionId(SUuid const&) @0x263b5f4, thunk 0xfe1e70: returns this.
    CNSJsonLog* SetSessionId(const SUuid& session);
};

namespace NRadGame {

// NRadEngine::NRadGame::CR15NetJsonLog: vtable_va 0x3731918 (the Ghidra label PTR_Finalize_03731918); slot 1 is
// WriteLog @0x136a304.
class CR15NetJsonLog : public CNSJsonLog {
public:
    void WriteLog(NWriteLog::ELogLevel level) override;
};
static_assert(sizeof(CR15NetJsonLog) == 0x40);

class CR15NetGame {
public:
    enum class EState : std::int32_t {};  // values 0..9 are switched on in CR15NetGame::SwitchTo; not recovered here

    // CR15NetGame::GameStateString(EState) @0x134d478, thunk 0x1036eb0: static, the state is in w0.
    static const char* GameStateString(EState state);
    // CR15NetGame::SendComponentEventGlobal @0x1344758, thunk 0x1021530: this is the game, the event id the argument.
    void SendComponentEventGlobal(std::uint64_t event);
    // CR15NetGame::ScheduleQuitOnError() @0x1344a10, thunk 0xfdf7b0.
    void ScheduleQuitOnError();
    // CR15NetGame::SetErrorMessage(char const*) @0x1341050, thunk 0x10461b0.
    void SetErrorMessage(const char* line0);
};

class CR15NetSpectatorCameraCS {
public:
    // CR15NetSpectatorCameraCS::SetPersonOfInterest(SNSUserID const&) @0x22a6524, thunk 0x1026f10: static, it copies
    // the 16 bytes into two globals.
    static void SetPersonOfInterest(const SNSUserID& user);
};

class CR15NetLobby {
public:
    // CR15NetLobby::StatusString(int) @0x1344570: the name of a lobby status, "unknown" outside -1..32. The
    // compiler inlined it twice at 0x133e248..0x133e298 (the table is at 0x3731b38).
    static const char* StatusString(std::int32_t status);
};

// The object's own layout is reached by raw offsets: +0x08 CR15NetGame*, +0x10 CBroadcaster*, +0x18 the CNSLobby
// part, +0x448 the status (i32), +0x44c a flag word, +0x8e0 the failed game type (i64), +0x8e8 a time (i64).
class CR15NetClientLobby : public CR15NetLobby {
public:
    // The ten vtable slots of vtable_va 0x3730ab0.
    virtual ~CR15NetClientLobby();
    virtual void Update(float);
    virtual void End(std::uint32_t flags);
    virtual void ProcessUI();
    virtual std::uint64_t GameType() const;
    virtual std::uint64_t MatchmakingGameType() const;
    virtual std::uint64_t JoinQueuedGameType() const;
    virtual std::uint64_t FailedGameType() const;
    virtual std::uint64_t LevelName() const;

    // @0x133e21c, thunk 0xff52f0.
    void SwitchTo(std::int32_t state);
};

}  // namespace NRadGame

namespace {

template <typename T>
T* LoadPtr(const std::byte* base, std::size_t offset) {
    T* value = nullptr;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

std::uint16_t LoadU16(const std::byte* base, std::size_t offset) {
    std::uint16_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

std::int32_t LoadI32(const std::byte* base, std::size_t offset) {
    std::int32_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

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

float LoadF32(const std::byte* base, std::size_t offset) {
    float value = 0.0f;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

void StoreI32(std::byte* base, std::size_t offset, std::int32_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

void StoreU32(std::byte* base, std::size_t offset, std::uint32_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

void StoreI64(std::byte* base, std::size_t offset, std::int64_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

// 0x133ecac..0x133edc4 and 0x133ef18..0x133efb4 (the same chain, inlined twice): seven eight-byte symbol words of the
// image, each compared with -1; the first one that equals -1 picks the kind. The words are the hashes in front of the
// texts "STM", "PSN", "XBX", "OVR", "DMO", "GST" and "BOT". None equals -1 in the file, so the result is 0.
std::uint64_t SymbolChainKind() {
    constexpr std::uint64_t kUnset = ~std::uint64_t{0};
    if (0xc8e8d0b1a883f6e7ull == kUnset) return 1;     // 0x2ca2418, "STM"
    if (0xc8e8d0b1a880f1e4ull == kUnset) return 2;     // 0x2ca2428, "PSN"
    if (0xc8e8d0b1a888e0f2ull == kUnset) return 3;     // 0x2ca2440, "XBX"
    if (0xc8e8d0b1a89ff4f8ull == kUnset) return 4;     // 0x2ca11c0, "OVR"
    if (0xc8e8d0b1a894efe5ull == kUnset) return 7;     // 0x2ca1260, "DMO"
    if (0xc8e8d0b1a897f1feull == kUnset) return 0x10;  // 0x2ca2450, "GST"
    if (0xc8e8d0b1a892edfeull == kUnset) return 6;     // 0x2ca2468, "BOT"
    return 0;
}

}  // namespace

void NRadGame::CR15NetClientLobby::SwitchTo(std::int32_t state) {
    std::byte* const self = reinterpret_cast<std::byte*>(this);

    // 0x133e23c..0x133e244: nothing happens when the lobby is already in the requested state (b.eq 0x133eecc is the
    // epilogue).
    const std::int32_t status_before = LoadI32(self, 0x448);
    if (status_before == state) {
        return;
    }

    // 0x133e248..0x133e2a8: both names come from the StatusString table, the old status first.
    NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(2), 0,
                        "[NETGAME] NetClientLobby switching state (from %s, to %s)",
                        StatusString(status_before), StatusString(state));

    std::byte* const game_bytes = LoadPtr<std::byte>(self, 0x08);
    CR15NetGame* const game = reinterpret_cast<CR15NetGame*>(game_bytes);
    CNSLobby* const session = reinterpret_cast<CNSLobby*>(self + 0x18);
    const CBroadcaster* const broadcaster = LoadPtr<CBroadcaster>(self, 0x10);

    // 0x133e2ac..0x133e2e4 and 0x133e35c..0x133e480: what leaving the old status undoes.
    const std::int32_t current = LoadI32(self, 0x448);
    if (static_cast<std::uint32_t>(current) - 0x12u < 0xfu || current == 0xc) {
        // Statuses 0x12..0x20 and 0xc: end the lobby unless the game is in state 9, and forget the failed game type.
        if (LoadI32(game_bytes, 0x00) != 9) {
            End(0);  // vtable slot 3 (+0x18), 0x133e2cc..0x133e2dc
        }
        StoreI64(self, 0x8e0, -1);  // 0x133e2e0
    } else if (current == 5) {
        // 0x133e440..0x133e480: a new status of 7 or 8 keeps the session; any other cancels the find.
        if (static_cast<std::uint32_t>(state) - 7u >= 2u) {
            const CNSUser* const user = LoadPtr<const CNSUser>(game_bytes, 0x28d0);
            SNSLoginId login;
            std::memcpy(&login, user != nullptr ? static_cast<const void*>(&user->LoginId()) : &SUuid::kInvalid,
                        sizeof login);
            session->CancelFindingSession(login);
        }
    } else if (current == 4) {
        // 0x133e37c..0x133e3cc: the same test; the game state read before LoginId() decides which cancel runs.
        if (static_cast<std::uint32_t>(state) - 7u >= 2u) {
            const CNSUser* const user = LoadPtr<const CNSUser>(game_bytes, 0x28d0);
            const std::int32_t game_state = LoadI32(game_bytes, 0x00);
            SNSLoginId login;
            std::memcpy(&login, user != nullptr ? static_cast<const void*>(&user->LoginId()) : &SUuid::kInvalid,
                        sizeof login);
            if (game_state == 9) {
                session->CancelFindingSession(login);  // 0x133e470
            } else {
                session->CancelPendingSession(login);  // 0x133e3c8
            }
        }
    }

    // 0x133e2e8..0x133e31c: the new status is stored, bit 0 of the flag word cleared, and the old status kept.
    const std::uint32_t flags_before = LoadU32(self, 0x44c);
    const std::int32_t from = LoadI32(self, 0x448);
    StoreI32(self, 0x448, state);
    StoreU32(self, 0x44c, flags_before & 0xfffffffeu);

    // 0x133e304..0x133e31c: w28 = from is 4 or 5, w29 = from is 6.
    const std::uint64_t net_flags = LoadU64(LoadPtr<std::byte>(game_bytes, 0x2da0), 0);
    const bool from_4_or_5 = (from | 1) == 5;
    const bool from_6 = from == 6;

    // 0x133e320..0x133e3dc: the peer the log records are about.
    std::int32_t entered = state;
    std::uint64_t peer;
    if (((net_flags >> 1) & 1u) != 0 || (net_flags & 0x44u) == 0x40u) {
        peer = SPeer::kSelf;  // 0x133e350
    } else if (LoadPtr<std::byte>(game_bytes, 0x40) == nullptr) {
        peer = SPeer::kInvalidPeer;  // 0x133e3d0
    } else {
        peer = reinterpret_cast<const CNSLobby*>(LoadPtr<std::byte>(game_bytes, 0x40) + 0x18)->HostPeer();  // 0x133e340
        entered = LoadI32(self, 0x448);  // 0x133e344
    }

    // First switch (jump table 0x2ca3d60, halfword offsets, states 0xc..0x20): the message for the new status.
    // State 0xd and everything outside 0xc..0x20 go straight to the second switch (0x133ec48); every other path
    // reloads the status first (0x133ec44).
    bool reload = true;
    switch (entered) {
        case 0xc:  // 0x133e430
            game->SetErrorMessage("Server was shut down");  // 0x2ca493f
            break;
        case 0xe:  // 0x133e6dc
            game->SetErrorMessage("Server registration failed");  // 0x2ca4506
            break;
        case 0xf:  // 0x133e6ec
            game->SetErrorMessage("Server find failed");  // 0x2ca4521
            break;
        case 0x10: {  // 0x133e6fc..0x133e950
            {
                CR15NetJsonLog json;
                json.EnsureRoot(static_cast<CTempJsonString::ERootType>(0));
                // The message field (0x133e72c..0x133e790).
                const std::int64_t message_key_length = CSysString::Length("message");
                const std::int64_t message_length = CSysString::Length("Server connection failed");  // 0x2ca4534
                json.NextElement(static_cast<std::uint64_t>(message_key_length + message_length + 5));
                json.SAppendF("\"%s\":\"", "message");
                json.EscapeAppend("Server connection failed", static_cast<std::uint64_t>(message_length));
                json.Append('"');
                // The user (0x133e794..0x133e7bc): a missing CNSUser yields the kind the symbol chain picks.
                const CNSUser* const user = LoadPtr<const CNSUser>(game_bytes, 0x28d0);
                SNSUserID user_id;
                if (user != nullptr) {
                    user_id = user->UserID();  // 0x133e7a0
                } else {
                    user_id = SNSUserID{{SymbolChainKind(), 0}};  // 0x133ef18..0x133efb8
                }
                CNSJsonLog* const with_user = json.SetUserId("userid", user_id);  // key at 0x2ca8aea
                CNSJsonLog* const log = with_user->SetSessionId(session->LobbyId());  // 0x133e7c8, 0x133e7d4
                // The game state field (0x133e7d8..0x133e85c).
                const char* const game_state_text =
                    CR15NetGame::GameStateString(static_cast<CR15NetGame::EState>(LoadI32(game_bytes, 0x00)));
                log->EnsureRoot(static_cast<CTempJsonString::ERootType>(0));
                const std::int64_t game_state_key_length = CSysString::Length("game_state");  // 0x2ca7423
                const std::int64_t game_state_length = CSysString::Length(game_state_text);
                log->NextElement(static_cast<std::uint64_t>(game_state_key_length + game_state_length + 5));
                log->SAppendF("\"%s\":\"", "game_state");
                log->EscapeAppend(game_state_text, static_cast<std::uint64_t>(game_state_length));
                log->Append('"');
                // The server address field (0x133e860..0x133e8d4).
                const char* const server_address = broadcaster->PeerDisplayName(peer);
                log->EnsureRoot(static_cast<CTempJsonString::ERootType>(0));
                const std::int64_t address_key_length = CSysString::Length("server_address");  // 0x2ca454d
                const std::int64_t address_length = CSysString::Length(server_address);
                log->NextElement(static_cast<std::uint64_t>(address_key_length + address_length + 5));
                log->SAppendF("\"%s\":\"", "server_address");
                log->EscapeAppend(server_address, static_cast<std::uint64_t>(address_length));
                log->Append('"');
                // The ping field (0x133e8d8..0x133e910).
                const float server_ping = broadcaster->Ping(peer);
                log->EnsureRoot(static_cast<CTempJsonString::ERootType>(0));
                log->SetFormatted("\"%s\":%.*f", "server_ping", 6, static_cast<double>(server_ping));  // 0x2c32ec2, 0x2ca455c
                log->WriteLog(static_cast<NWriteLog::ELogLevel>(2));  // vtable slot 1 (+0x8), 0x133e914..0x133e924
            }  // 0x133e928..0x133e940: the destruction
            game->SetErrorMessage("Server connection failed");  // 0x133e944..0x133e950
            break;
        }
        case 0x11: {  // 0x133e980..0x133e9a4
            const STempString<512> text = MakeTStrPrintF<512>(
                "Please update %s", reinterpret_cast<const char*>(LoadPtr<std::byte>(game_bytes, 0x08) + 0x838));
            game->SetErrorMessage(reinterpret_cast<const char*>(text.storage));
            break;
        }
        case 0x12:  // 0x133e9a8
            game->SetErrorMessage("Internal error");  // 0x2c3ab00
            game->SendComponentEventGlobal(0x50beedceed097932ull);  // 0x2ca4580, delegate_onlobbyjoinerrorinternal
            break;
        case 0x13:  // 0x133e9c8
            game->SetErrorMessage("Bad request");  // 0x2ca45ab
            game->SendComponentEventGlobal(0x2621dbad3b9657d6ull);  // 0x2ca45b8, delegate_onlobbyjoinerrorbadrequest
            break;
        case 0x14:  // 0x133e9e8
            game->SetErrorMessage("Timeout");  // 0x2c30c34
            game->SendComponentEventGlobal(0x33eee43165822182ull);  // 0x2ca45f0, delegate_onlobbyjoinerrortimeout
            break;
        case 0x15:  // 0x133ea08
            game->SetErrorMessage("Server does not exist");  // 0x2ca461c
            game->SendComponentEventGlobal(0xa05ffa5ab02b543eull);  // 0x2ca4638, delegate_onlobbyjoinerrorlobbyinvalid
            break;
        case 0x16:  // 0x133ea28
            game->SetErrorMessage("Server is incompatible");  // 0x2ca4668
            game->SendComponentEventGlobal(0xcc9972e9937e610dull);  // 0x2ca4680, delegate_onlobbyjoinerrorlobbyincompatible
            break;
        case 0x17:  // 0x133ea44
            game->SetErrorMessage("Server is solo");  // 0x2ca46ba
            game->SendComponentEventGlobal(0xa059f24abd2e533full);  // 0x2ca46d0, delegate_onlobbyjoinerrorlobbyoffline
            break;
        case 0x18:  // 0x133ea64
            game->SetErrorMessage("Server is locked");  // 0x2ca46ff
            game->SendComponentEventGlobal(0x79d935d40e9866d0ull);  // 0x2ca4710, delegate_onlobbyjoinerrorlobbylocked
            break;
        case 0x19:  // 0x133ea84
            game->SetErrorMessage("Server is full");  // 0x2ca4745
            game->SendComponentEventGlobal(0x5a1d871b9739008eull);  // 0x2ca4758, delegate_onlobbyjoinerrorlobbyfull
            break;
        case 0x1a: {  // 0x133eaa4: FailedGameType() picks the message; the event is the same for all three
            const char* message;
            const std::uint64_t failed_game_type = FailedGameType();  // vtable slot 8 (+0x40)
            if (failed_game_type == 0xcb60a4de7e1caf73ull ||  // 0x2ca0950, echo_arena
                failed_game_type == 0x09990965f4db8c03ull ||  // 0x2ca4af0, echo_arena_private
                failed_game_type == 0xd53a9c73b8b2c760ull ||  // 0x2cacec0, echo_arena_tournament
                failed_game_type == 0x4dc1ddbd89b28531ull ||  // 0x2ca4b18, echo_demo
                failed_game_type == 0x339c5dc66c03b81full ||  // 0x2cacee8, echo_demo_public
                failed_game_type == 0xd54d62c40852c2f9ull ||  // 0x2cacf08, echo_arena_public_ai
                failed_game_type == 0x888ac0cfd102345full ||  // 0x2cace30, echo_arena_practice_ai
                failed_game_type == 0xdf8258f63e878e9dull ||  // 0x2cacf30, echo_arena_private_ai
                failed_game_type == 0xea8d9b0bbdaa1c6aull ||  // 0x2cac900, echo_arena_first_match
                failed_game_type == 0xd894afea8246bc6bull) {  // 0x2cacf58, echo_arena_npe
                message = "Echo Arena required";  // 0x2ca4787
            } else {
                const std::uint64_t failed_again = FailedGameType();  // 0x133f15c
                if (failed_again == 0x3d5c3976578a321aull ||  // 0x2ca0970, echo_combat
                    failed_again == 0x33bbf6842df97a3full ||  // 0x2cacf70, echo_combat_private
                    failed_again == 0x6b44ed1ad5234fbcull ||  // 0x2cacf98, echo_combat_tournament
                    failed_again == 0x4311cb2c132e9299ull ||  // 0x2cacfc0, echo_combat_public_ai
                    failed_again == 0x25c1c8218705f693ull ||  // 0x2cace58, echo_combat_practice_ai
                    failed_again == 0x61fc299f53160641ull ||  // 0x2cacfe8, echo_combat_private_ai
                    failed_again == 0x47c693e5ebaddea6ull) {  // 0x2cac920, echo_combat_first_match
                    message = "Echo Combat required";  // 0x2ca479b
                } else {
                    message = "Missing entitlement";  // 0x2ca47b0
                }
            }
            game->SetErrorMessage(message);
            game->SendComponentEventGlobal(0x275446b9b6a72906ull);  // 0x2ca47c8, delegate_onlobbyjoinerrormissingentitlement
            break;
        }
        case 0x1b:  // 0x133ebd0
            game->SetErrorMessage("Party changed");  // 0x2ca4800
            game->SendComponentEventGlobal(0xe41bf3bafd04ecfdull);  // 0x2ca4810, delegate_onlobbyjoinerrorpartychange
            [[fallthrough]];
        case 0x1c:  // 0x133ebf8: no message, only the event
            game->SendComponentEventGlobal(0x8eb76583e5910a9eull);  // 0x2ca4848, delegate_onlobbyjoinerroraccessdenied
            break;
        case 0x1d:  // 0x133ec14
            game->SetErrorMessage("Kicked from server");  // 0x2ca4879
            break;
        case 0x1e:  // 0x133ec24
            game->SetErrorMessage("Kicked from server due to inactivity");  // 0x2ca488c
            break;
        case 0x1f: {  // 0x133e484..0x133f110
            {
                CR15NetJsonLog json;
                json.EnsureRoot(static_cast<CTempJsonString::ERootType>(0));
                // The message field (0x133e4b4..0x133e51c).
                const std::int64_t message_key_length = CSysString::Length("message");
                const std::int64_t message_length = CSysString::Length("Disconnected from server due to timeout");  // 0x2ca48b1
                json.NextElement(static_cast<std::uint64_t>(message_key_length + message_length + 5));
                json.SAppendF("\"%s\":\"", "message");
                json.EscapeAppend("Disconnected from server due to timeout", static_cast<std::uint64_t>(message_length));
                json.Append('"');
                // No user here (0x133e520..0x133e530): the session id only.
                CNSJsonLog* const log = json.SetSessionId(session->LobbyId());
                // The game state field (0x133e534..0x133e5b0).
                const char* const game_state_text =
                    CR15NetGame::GameStateString(static_cast<CR15NetGame::EState>(LoadI32(game_bytes, 0x00)));
                log->EnsureRoot(static_cast<CTempJsonString::ERootType>(0));
                const std::int64_t game_state_key_length = CSysString::Length("game_state");  // 0x2ca7423
                const std::int64_t game_state_length = CSysString::Length(game_state_text);
                log->NextElement(static_cast<std::uint64_t>(game_state_key_length + game_state_length + 5));
                log->SAppendF("\"%s\":\"", "game_state");
                log->EscapeAppend(game_state_text, static_cast<std::uint64_t>(game_state_length));
                log->Append('"');
                // The server address field (0x133e5b4..0x133e628).
                const char* const server_address = broadcaster->PeerDisplayName(peer);
                log->EnsureRoot(static_cast<CTempJsonString::ERootType>(0));
                const std::int64_t address_key_length = CSysString::Length("server_address");  // 0x2ca454d
                const std::int64_t address_length = CSysString::Length(server_address);
                log->NextElement(static_cast<std::uint64_t>(address_key_length + address_length + 5));
                log->SAppendF("\"%s\":\"", "server_address");
                log->EscapeAppend(server_address, static_cast<std::uint64_t>(address_length));
                log->Append('"');
                // The ping field (0x133e62c..0x133e664).
                const float server_ping = broadcaster->Ping(peer);
                log->EnsureRoot(static_cast<CTempJsonString::ERootType>(0));
                log->SetFormatted("\"%s\":%.*f", "server_ping", 6, static_cast<double>(server_ping));  // 0x2c32ec2, 0x2ca455c
                // The real ping estimate (0x133e668..0x133e6d8, 0x133efbc..0x133eff0).
                float real_ping_estimate;
                if (LoadU16(game_bytes, 0xde) == 0) {
                    real_ping_estimate = 0.0f;  // 0x133eef0
                } else {
                    float sample;
                    if (LoadU16(game_bytes, 0xe2) == 0) {
                        sample = 0.0f;  // 0x133efbc
                    } else {
                        const std::uint64_t slot = LoadU16(game_bytes, 0x178) & 0x1fu;
                        const std::uint64_t flags = LoadU64(LoadPtr<std::byte>(game_bytes, 0x2da0), 0);
                        if (((flags >> 1) & 1u) != 0 || (flags & 0x44u) == 0x40u) {
                            sample = LoadF32(game_bytes, slot * 0xe78 + 0x541e0);
                        } else {
                            sample = LoadF32(game_bytes, slot * 0x70 + 0x62960);
                        }
                    }
                    real_ping_estimate = sample * 0.001f;  // 0x3a83126f
                }
                log->EnsureRoot(static_cast<CTempJsonString::ERootType>(0));
                log->SetFormatted("\"%s\":%.*f", "real_ping_estimate", 6, static_cast<double>(real_ping_estimate));  // 0x2ca48d9
                // The three metrics that follow (0x133f020..0x133f0c4).
                const float time_since_last_received = broadcaster->TimeSinceLastReceived(peer);
                log->EnsureRoot(static_cast<CTempJsonString::ERootType>(0));
                log->SetFormatted("\"%s\":%.*f", "time_since_last_received", 6, static_cast<double>(time_since_last_received));  // 0x2ca48ec
                const float decode_failure_rate = broadcaster->DecodeFailureRate(peer);
                log->EnsureRoot(static_cast<CTempJsonString::ERootType>(0));
                log->SetFormatted("\"%s\":%.*f", "decode_failure_rate", 6, static_cast<double>(decode_failure_rate));  // 0x2ca4905
                const float drop_percent = broadcaster->DropPercent(peer);
                log->EnsureRoot(static_cast<CTempJsonString::ERootType>(0));
                log->SetFormatted("\"%s\":%.*f", "drop_percent", 6, static_cast<double>(drop_percent));  // 0x2ca4919
                log->WriteLog(static_cast<NWriteLog::ELogLevel>(2));  // vtable slot 1 (+0x8), 0x133f0c8..0x133f0d8
            }  // 0x133f0dc..0x133f0f4: the destruction
            game->SetErrorMessage("Disconnected from server due to timeout");  // 0x133f0f8..0x133f104
            break;
        }
        case 0x20:  // 0x133ec34
            game->SetErrorMessage("Disconnected from server");  // 0x2ca4926
            break;
        case 0xd:
        default:
            reload = false;
            break;
    }

    // 0x133ec44: reload the status, then the second switch (jump table 0x2ca3d8a, byte offsets, states 3..0x20).
    const std::int32_t second = reload ? LoadI32(self, 0x448) : entered;
    switch (second) {
        case 3:  // 0x133ed58
            StoreI64(self, 0x8e8, CSysTime::GetTime());  // 0x133ed58, 0x133ed64
            game->SendComponentEventGlobal(0x24bd4d4f0d0485aaull);  // 0x2ca4958, delegate_onmatchmakingqueued
            break;
        case 0xc:
        case 0x11:
        case 0x1d:
        case 0x1e:
        case 0x1f:
        case 0x20:  // 0x133ec94
            game->ScheduleQuitOnError();
            break;
        case 0xd:  // 0x133ed8c
            End(0);  // vtable slot 3 (+0x18)
            break;
        case 0xe:
        case 0xf:
        case 0x10:
        case 0x12:
        case 0x13:
        case 0x14:
        case 0x15:
        case 0x16:
        case 0x17:
        case 0x18:
        case 0x19:
        case 0x1a:
        case 0x1b:
        case 0x1c:  // 0x133ec6c
            if (LoadI32(game_bytes, 0x00) != 9) {
                game->ScheduleQuitOnError();
            }
            break;
        default:
            break;
    }

    // 0x133ec80 and its siblings: the status is read again, w20 = from was 4, 5 or 6.
    const bool from_search = from_4_or_5 || from_6;
    std::int32_t after = LoadI32(self, 0x448);
    if (after >= 0xe) {
        // 0x133ecac..0x133ede8: the person of interest is cleared (kind picked by the symbol chain, number 0) and bit 1
        // of the game's byte at +0x647e9 is cleared.
        const SNSUserID person_of_interest{{SymbolChainKind(), 0}};
        CR15NetSpectatorCameraCS::SetPersonOfInterest(person_of_interest);
        LoadPtr<std::byte>(self, 0x08)[0x647e9] &= std::byte{0xfd};
        after = LoadI32(self, 0x448);
    }

    // 0x133edec..0x133ee0c: only a change between "searching" (status 4, 5 or 6) and not searching goes on.
    const bool now_search = after == 6 || (after | 1) == 5;
    if (from_search == now_search) {
        return;
    }

    // 0x133ee10..0x133eec8: the matchmaking event for the new status.
    if (static_cast<std::uint32_t>(after) - 4u <= 2u) {
        StoreI64(self, 0x8e8, CSysTime::GetTime());  // 0x133ee1c, 0x133ee24
        const std::int32_t stamped = LoadI32(self, 0x448);  // 0x133ee20
        if (stamped == 6) {
            game->SendComponentEventGlobal(0x4dec8dc7d9737805ull);  // 0x2ca49f0, delegate_onmatchmakingstartedjoining
        } else if (stamped == 5) {
            game->SendComponentEventGlobal(0x4de08bc0d3737805ull);  // 0x2ca49b8, delegate_onmatchmakingstartedfinding
        } else if (stamped == 4) {
            game->SendComponentEventGlobal(0xef2dd284b5ee9375ull);  // 0x2ca4988, delegate_onmatchmakingstartedcreating
        }
    } else if (after == 8) {
        game->SendComponentEventGlobal(0xf65debd252bbaefbull);  // 0x2ca4a60, delegate_onmatchmakingsucceededjoining
    } else if (after == 7) {
        game->SendComponentEventGlobal(0x775ed06b02a94972ull);  // 0x2ca4a28, delegate_onmatchmakingsucceededjoinqueued
    } else if (after >= 0xe) {
        game->SendComponentEventGlobal(0x24bd5a5b011d85aaull);  // 0x2ca4a98, delegate_onmatchmakingfailed
    } else {
        game->SendComponentEventGlobal(0x30d54385bae922eaull);  // 0x2ca4ac0, delegate_onmatchmakingcanceled
    }
}

}  // namespace NRadEngine
