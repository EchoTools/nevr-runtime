// CR15Game::BeginMultiplayer
// Quest libr15.so VA 0x1301d88; ReVault binary 576.
// ReVault raw parent 100221541, reconstruction node 100600517; grade M: every branch and offset checked against the 281-instruction listing; the vtable slots 0x340/0x358 and the route status type are inferred, not named.
// @0x1301d88 — CR15Game::BeginMultiplayer
/* @confidence: M */
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

struct CSymbol64 {
    std::uint64_t bits;
};
static_assert(sizeof(CSymbol64) == 0x08);

// Two qwords: returned in x0:x1 by CR15NetGame::SocialGroupId and compared 16 bytes at a time.
struct SUuid {
    std::uint64_t words[2];
};
static_assert(sizeof(SUuid) == 0x10);

// Passed by address to CR15NetGame::BeginMultiplayerWithLobbyUser; its layout is not established.
struct SNSUserID {
    alignas(8) std::byte raw[16];
};

// The 0x38-byte level-load block built in place here. Only the bytes this function writes are
// modelled; the extent the callee reads is not established.
struct SLevelLoad {
    alignas(8) std::byte raw[0x38];
};

// Five qwords built in place (0x28 bytes) and passed by address.
struct SComponentEvent {
    std::uint64_t words[5];
};
static_assert(sizeof(SComponentEvent) == 0x28);

namespace NWriteLog {
// Enumerators are not established. This function uses 2 (WriteLog) and 8 (OutputError).
enum ELogLevel : std::int32_t {};
// NRadEngine::NWriteLog::WriteLog @ 0x1088c20 (thunk 0x104b960). Variadic.
void WriteLog(ELogLevel level, unsigned long long channel, const char* format, ...);
}  // namespace NWriteLog

// NRadEngine::OutputError(int, NRadEngine::NWriteLog::ELogLevel) @ 0x12d1b38 (thunk 0x104e4c0).
// The int is the status left in x0 by the route call made just before it.
void OutputError(std::int32_t status, NWriteLog::ELogLevel level);

namespace CMemory {
// NRadEngine::CMemory::Fill @ 0x12cafcc (thunk 0x1047300): a tail jump to memset.
void Fill(void* destination, unsigned char value, std::uint64_t count);
// NRadEngine::CMemory::Compare @ 0x12cae74 (thunk 0x1051e10): memcmp; zero when the ranges match.
std::int64_t Compare(const void* lhs, const void* rhs, std::uint64_t count);
}  // namespace CMemory

class CncaGameSpace {
  public:
    // CncaGameSpace::SendComponentEvent(SComponentEvent const&, unsigned int) @ 0x1907264 (thunk 0x10090b0).
    void SendComponentEvent(const SComponentEvent& event, unsigned int count);
};

class CncaGame {
  public:
    // CncaGame::EnableGameSpace(CncaGameSpace*) @ 0x18dc124 (thunk 0x101ed30).
    void EnableGameSpace(CncaGameSpace* space);
    // CncaGame::RequireGameSpace(SLevelLoad const&, unsigned int) @ 0x18d3e88 (thunk 0x104b030).
    void RequireGameSpace(const SLevelLoad& load, unsigned int flag);
    // CncaGame::GameSpace(CSymbol64) const @ 0x18d3414 (thunk 0x1031ef0).
    CncaGameSpace* GameSpace(CSymbol64 symbol) const;
};

namespace NRadGame {

struct SR15NetMatchSettings;

// Six 4-byte floats; this function writes 1.0f into all six.
struct SR15IronmanMultipliers {
    alignas(4) std::byte raw[0x18];
};

class CR15NetGame {
  public:
    // CR15NetGame::BeginMultiplayer(SR15NetMatchSettings const&) @ 0x138960c (thunk 0xfe0f00).
    std::int32_t BeginMultiplayer(const SR15NetMatchSettings& settings);
    // CR15NetGame::BeginMultiplayerWithLobbyUser(SNSUserID const&, unsigned short) @ 0x138bde4 (thunk 0xff81f0).
    std::int32_t BeginMultiplayerWithLobbyUser(const SNSUserID& user, std::uint16_t mode);
    // CR15NetGame::BeginMultiplayerWithLobbyGroup(SUuid const&, unsigned short) @ 0x138bea4 (thunk 0x10024c0).
    std::int32_t BeginMultiplayerWithLobbyGroup(const SUuid& group, std::uint16_t mode);
    // CR15NetGame::BeginMultiplayerWithLobbyId(SUuid const&, unsigned short) @ 0x138bf64 (thunk 0xfd3ee0).
    std::int32_t BeginMultiplayerWithLobbyId(const SUuid& lobby, std::uint16_t mode);
    // CR15NetGame::BeginMultiplayerWithLobbyAddr(CSymbol64, TIpv4, TPort, unsigned short) @ 0x138c024 (thunk 0x103e750).
    // TIpv4 is passed in w2 (32 bits) and TPort in w3 (16 bits).
    std::int32_t BeginMultiplayerWithLobbyAddr(CSymbol64 symbol, std::uint32_t ipv4, std::uint16_t port,
                                               std::uint16_t mode);
    // CR15NetGame::BeginMultiplayerWithRegion(CSymbol64, unsigned short) @ 0x138c144 (thunk 0x10460e0).
    std::int32_t BeginMultiplayerWithRegion(CSymbol64 region, std::uint16_t mode);
    // CR15NetGame::SocialGroupId() const @ 0x138f968 (thunk 0xfec5d0).
    SUuid SocialGroupId() const;
    // CR15NetGame::BeginMultiplayerAsSpectator() @ 0x138c200 (thunk 0xfe97d0).
    std::int32_t BeginMultiplayerAsSpectator();
};

class CR15PlayerNavCS {
  public:
    // CR15PlayerNavCS::SetIronManMultipliers(SR15IronmanMultipliers const&) @ 0x1dd5c58 (thunk 0x100ee70).
    // Static: the record is the only argument (x0).
    static void SetIronManMultipliers(const SR15IronmanMultipliers& multipliers);
};

// The no-argument overload BeginMultiplayer() @ 0x1301be4 builds the settings block and calls this function.
class CR15Game : public CncaGame {
  public:
    // CR15Game::BeginMultiplayer(SR15NetMatchSettings const&) @ 0x1301d88: the function reconstructed here.
    void BeginMultiplayer(const SR15NetMatchSettings& settings);
    // CR15Game::LoadSettings() @ 0x12f3e88 (thunk 0xfed290).
    void LoadSettings();
};

}  // namespace NRadGame
}  // namespace NRadEngine

// Fixed-address globals, named by VA as in the sibling reconstruction.
extern std::uint64_t global_03832080;
extern std::uint64_t global_03832088;
extern std::uint64_t global_0386c570;
extern std::uint64_t global_0386c578;
extern std::uint64_t global_0386c5c8;
// Route sentinels: -1 until something stores a value (see ObservedRouteType).
extern std::uint64_t global_02ca2418;
extern std::uint64_t global_02ca2428;
extern std::uint64_t global_02ca2440;
extern std::uint64_t global_02ca11c0;
extern std::uint64_t global_02ca1260;
extern std::uint64_t global_02ca2450;
extern std::uint64_t global_02ca2468;
// Pointer slots. The loader fills each one (the file bytes are zero), so their targets are not in the file.
extern const std::byte* global_0380cb28;          // -> 16-byte record: qword at +0, dword at +8
extern const float* global_0381c0f8;              // -> the float passed in s0
extern const NRadEngine::SUuid* global_0382adc8;  // -> NRadEngine::SUuid::kInvalid (16 bytes)

namespace NRadEngine {
namespace NRadGame {

namespace {

constexpr std::uint64_t kUnset = ~std::uint64_t{0};
constexpr unsigned char kFillByte = 0xff;
// The 16 bytes at 0x2c2f960 (-1, then 0), copied to the lobby address fields at this+0x8548.
constexpr std::uint64_t kLobbyAddressDefault[2] = {~std::uint64_t{0}, 0};
// Six 1.0f values written as the iron-man multipliers.
constexpr float kUnitMultipliers[6] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

template <typename T>
T Load(const void* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof value);
    return value;
}

template <typename T>
void Store(void* base, std::size_t offset, const T& value) {
    std::memcpy(static_cast<std::byte*>(base) + offset, &value, sizeof value);
}

template <typename T>
const T* FieldAt(const void* base, std::size_t offset) {
    return reinterpret_cast<const T*>(static_cast<const std::byte*>(base) + offset);
}

// The vtable pointer sits at +0 of the object; the slot is read from it.
template <typename Fn>
Fn VirtualSlot(const void* object, std::size_t slot_offset) {
    const std::byte* const vtable = Load<const std::byte*>(object, 0);
    Fn fn = nullptr;
    std::memcpy(&fn, vtable + slot_offset, sizeof fn);
    return fn;
}

// Vtable slot +0x340, called as (this, 4, &level load, 1, realmax) and returning the new game space.
// The slot's name is not recovered in ReVault.
using CreateSpaceSlot = CncaGameSpace* (*)(CR15Game* self, std::uint32_t w1, const SLevelLoad* load,
                                           std::uint32_t w3, float s0);
// Vtable slot +0x358, called as (this, game space, 0, 1). The slot's name is not recovered in ReVault.
using SetSpaceSlot = void (*)(CR15Game* self, CncaGameSpace* space, std::uint32_t w2, std::uint32_t w3);

// The 0x38-byte level-load block: the symbol, two -1 qwords, the record's qword and dword, the constant 2,
// zeros, and four 0xff bytes at +0x30 (CMemory::Fill).
void BuildLevelLoad(SLevelLoad& block, std::uint64_t symbol, const std::byte* record) {
    Store(block.raw, 0x00, symbol);
    Store(block.raw, 0x08, kUnset);
    Store(block.raw, 0x10, kUnset);
    Store(block.raw, 0x18, Load<std::uint64_t>(record, 0x00));
    Store(block.raw, 0x20, Load<std::uint32_t>(record, 0x08));
    Store(block.raw, 0x24, std::uint32_t{2});
    Store(block.raw, 0x28, std::uint64_t{0});
    Store(block.raw, 0x30, std::uint64_t{0});
    CMemory::Fill(block.raw + 0x30, kFillByte, 4);
}

// The route type stored at this+0x8518 after OutputError. The first of seven sentinels still at -1
// selects its code (g0..g5 give 1, 2, 3, 4, 7, 0x10); g6 gives 6, and otherwise the result is 0.
std::uint64_t ObservedRouteType() {
    if (global_02ca2418 == kUnset) return 1;
    if (global_02ca2428 == kUnset) return 2;
    if (global_02ca2440 == kUnset) return 3;
    if (global_02ca11c0 == kUnset) return 4;
    if (global_02ca1260 == kUnset) return 7;
    if (global_02ca2450 == kUnset) return 0x10;
    return global_02ca2468 == kUnset ? 6 : 0;
}

// The route dispatch. Each branch returns the status of the one call it makes; the binary passes that
// x0 straight to OutputError. The lobby user needs a nonzero flag nibble at this+0x8518 and a nonzero
// qword at +0x8520; the lobby group and id need a value other than SUuid::kInvalid; the address needs a
// nonzero dword at +0x8550; the region needs a qword at +0x8568 other than -1; mode 4 is the social group
// and mode 2 the spectator; anything else is the ordinary path.
std::int32_t StartRoute(CR15NetGame& net, const void* game, const SR15NetMatchSettings& settings) {
    const std::uint16_t mode = Load<std::uint16_t>(game, 0x8558);
    if ((Load<std::uint8_t>(game, 0x8518) & 0x0fu) != 0 && Load<std::uint64_t>(game, 0x8520) != 0) {
        return net.BeginMultiplayerWithLobbyUser(*FieldAt<SNSUserID>(game, 0x8518), mode);
    }
    if (CMemory::Compare(FieldAt<SUuid>(game, 0x8528), global_0382adc8, sizeof(SUuid)) != 0) {
        return net.BeginMultiplayerWithLobbyGroup(*FieldAt<SUuid>(game, 0x8528), mode);
    }
    if (CMemory::Compare(FieldAt<SUuid>(game, 0x8538), global_0382adc8, sizeof(SUuid)) != 0) {
        return net.BeginMultiplayerWithLobbyId(*FieldAt<SUuid>(game, 0x8538), mode);
    }
    if (Load<std::uint32_t>(game, 0x8550) != 0) {
        return net.BeginMultiplayerWithLobbyAddr(CSymbol64{Load<std::uint64_t>(game, 0x8548)},
                                                 Load<std::uint32_t>(game, 0x8550),
                                                 Load<std::uint16_t>(game, 0x8554), mode);
    }
    if (Load<std::uint64_t>(game, 0x8568) != kUnset) {
        return net.BeginMultiplayerWithRegion(CSymbol64{Load<std::uint64_t>(game, 0x8568)}, mode);
    }
    if (mode == 4) {
        const SUuid social_group = net.SocialGroupId();
        return net.BeginMultiplayerWithLobbyGroup(social_group, mode);
    }
    if (mode == 2) {
        return net.BeginMultiplayerAsSpectator();
    }
    return net.BeginMultiplayer(settings);
}

}  // namespace

void CR15Game::BeginMultiplayer(const SR15NetMatchSettings& settings) {
    NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(2), 0ull, "Beginning multiplayer");

    CR15NetGame* const net = Load<CR15NetGame*>(this, 0x8510);
    const std::byte* const net_flags = Load<const std::byte*>(net, 0x2da0);
    if ((Load<std::uint8_t>(net_flags, 0) & 0x04u) == 0) {
        CncaGameSpace* space = Load<CncaGameSpace*>(this, 0x7af0);
        if (space != nullptr) {
            for (; space != nullptr; space = Load<CncaGameSpace*>(space, 0x18)) {
                EnableGameSpace(space);
            }
        } else {
            const std::byte* const record = global_0380cb28;
            SLevelLoad first{};
            BuildLevelLoad(first, global_03832088, record);
            // The binary fills the same four bytes a second time (0x1301e44 and 0x1301e54).
            CMemory::Fill(first.raw + 0x30, kFillByte, 4);
            const float realmax = *global_0381c0f8;
            CncaGameSpace* const created =
                VirtualSlot<CreateSpaceSlot>(this, 0x340)(this, 4u, &first, 1u, realmax);
            Store(this, 0x7af0, created);
            Store(created, 0x10, Load<std::uint64_t>(this, 0x7af8));
            SLevelLoad second{};
            BuildLevelLoad(second, global_03832088, record);
            RequireGameSpace(second, 1u);
        }
        const SComponentEvent event{{global_0386c5c8, kUnset, 0x0000ffffffffffffull, kUnset, kUnset}};
        Load<CncaGameSpace*>(this, 0x7af0)->SendComponentEvent(event, 1u);
        if (CncaGameSpace* const found = GameSpace(CSymbol64{global_0386c578})) {
            VirtualSlot<SetSpaceSlot>(this, 0x358)(this, found, 0u, 1u);
        }
    }
    if (CncaGameSpace* const found = GameSpace(CSymbol64{global_03832080})) {
        VirtualSlot<SetSpaceSlot>(this, 0x358)(this, found, 0u, 1u);
    }
    global_03832080 = global_0386c570;

    const std::int32_t status = StartRoute(*net, this, settings);
    OutputError(status, static_cast<NWriteLog::ELogLevel>(8));
    Store(this, 0x8518, ObservedRouteType());
    Store(this, 0x8520, std::uint64_t{0});
    Store(this, 0x8528, *global_0382adc8);
    Store(this, 0x8538, *global_0382adc8);
    Store(this, 0x8548, kLobbyAddressDefault);
    Store(this, 0x8558, std::uint16_t{0xffff});
    SR15IronmanMultipliers multipliers{};
    Store(multipliers.raw, 0, kUnitMultipliers);
    CR15PlayerNavCS::SetIronManMultipliers(multipliers);
    Store(this, 0x7b0c, std::uint32_t{6});
    LoadSettings();
}

}  // namespace NRadGame
}  // namespace NRadEngine
