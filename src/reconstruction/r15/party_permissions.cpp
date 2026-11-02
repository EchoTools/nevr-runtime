// NRadEngine::NRadGame::CR15NetPartyPermissionsExpression::operator()
// Quest libr15.so VA 0x242c794; ReVault binary 576.
// ReVault raw parent 100313119, reconstruction node 100600483; grade H: all 48 instructions checked branch by branch, all seven callees resolved to named ReVault functions.
// @0x242c794 — NRadEngine::NRadGame::CR15NetPartyPermissionsExpression::operator()
/* @confidence: H */
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

struct SScriptThread;

}  // namespace NRadEngine

namespace NRadEngine::NRadGame {

struct SNSUserID {
    // 16 bytes: PartyMemberId returns them in x0 and x1, and the caller stores them to the stack
    // before PurchasedCombat takes their address. The fields are not recovered.
    alignas(8) std::byte raw[16];
};

class CR15NetGame {
  public:
    // CR15NetGame::PartyCount() const; ReVault 0x1341f90 (thunk 0x101e530).
    std::uint32_t PartyCount() const;
    // CR15NetGame::PartyMemberMatchType(unsigned int) const; ReVault 0x139b1e8 (thunk 0x1045330).
    // Returns a 64-bit CSymbol64 in x0; all ones is the no-match value.
    std::uint64_t PartyMemberMatchType(std::uint32_t member) const;
    // CR15NetGame::PartyMemberHeadsetType(unsigned int) const; ReVault 0x139b168 (thunk 0x104c330).
    std::int32_t PartyMemberHeadsetType(std::uint32_t member) const;
    // CR15NetGame::PartyMemberId(unsigned int) const; ReVault 0x1341fe4 (thunk 0x1048950).
    SNSUserID PartyMemberId(std::uint32_t member) const;
    // CR15NetGame::PurchasedCombat(NRadEngine::SNSUserID const&) const; ReVault 0x13991a4 (thunk 0x104f4a0).
    bool PurchasedCombat(const SNSUserID& user_id) const;
};

class CR15Game {
  public:
    // CR15Game::NetGameMT() const; ReVault 0x13027d4 (thunk 0x103a070). Its result is the this (x0)
    // of every CR15NetGame call in the expression, so it is declared as the CR15NetGame object.
    CR15NetGame* NetGameMT() const;
};

class CR15NetLobby {
  public:
    // CR15NetLobby::Social(NRadEngine::CSymbol64); ReVault 0x13afab4 (thunk 0x104bc40).
    // The match type is passed in x0 with no this, so the member is static.
    static bool Social(std::uint64_t match_type);
};

class CR15NetPartyPermissionsExpression {
  public:
    struct SOutput;
    // Clears the flag bytes of SOutput at +0 and +1; it never sets them.
    void operator()(SOutput& output, NRadEngine::SScriptThread& thread) const;
};

}  // namespace NRadEngine::NRadGame

namespace {

// Pointer stored at base + offset (a raw load; the object layouts are not recovered).
const std::byte* LoadPtr(const std::byte* base, std::size_t offset) {
    const std::byte* value = nullptr;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

// One-byte store at base + offset (strb of a zero register).
void StoreU8(std::byte* base, std::size_t offset, std::uint8_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

}  // namespace

namespace NRadEngine::NRadGame {

void CR15NetPartyPermissionsExpression::operator()(SOutput& output, NRadEngine::SScriptThread& thread) const {
    // The thread argument (x2) leads to the CR15Game object by raw loads: [thread] -> context,
    // [context+0x30] -> host, [host] -> vptr, [vptr+0x80] -> entry, [entry+8] -> the CR15Game passed as this.
    const auto* script_thread = reinterpret_cast<const std::byte*>(&thread);
    const std::byte* const context = LoadPtr(script_thread, 0x0);
    const std::byte* const host = LoadPtr(context, 0x30);
    const std::byte* const vtable = LoadPtr(host, 0x0);
    const std::byte* const entry = LoadPtr(vtable, 0x80);
    const auto* game = reinterpret_cast<const CR15Game*>(LoadPtr(entry, 0x8));

    CR15NetGame* const net = game->NetGameMT();
    const std::uint32_t count = net->PartyCount();
    auto* const output_bytes = reinterpret_cast<std::byte*>(&output);

    for (std::uint32_t i = 0; i != count; ++i) {
        const std::uint64_t match_type = net->PartyMemberMatchType(i);
        if (match_type != ~std::uint64_t{0} && !CR15NetLobby::Social(match_type)) {
            StoreU8(output_bytes, 0x0, 0);
        }
        if (net->PartyMemberHeadsetType(i) == 3) {
            StoreU8(output_bytes, 0x1, 0);
        } else {
            const SNSUserID user_id = net->PartyMemberId(i);
            if (!net->PurchasedCombat(user_id)) {
                StoreU8(output_bytes, 0x1, 0);
            }
        }
    }
}

}  // namespace NRadEngine::NRadGame
