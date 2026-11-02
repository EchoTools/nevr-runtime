// NRadEngine::NRadGame::CR15NetGame::BeginMultiplayer
// Quest libr15.so VA 0x138960c; ReVault binary 576.
// ReVault raw parent 100224048, reconstruction node 100600747; grade M: all 2428 instructions to the stored extent end 0x138bbfc checked (hot path 0x138960c-0x138b7d8, 22 cold TypeId stubs 0x138b7dc-0x138bbfc); 4 DAT_ placeholders stay (DAT_02c9dc50, DAT_02c9db18, DAT_02c9d9e0, DAT_0386ce10), no FUN_ placeholder; one more global-scope extern, g_vptr_CR15NetClientLobby (vptr value 0x3730ab0, a vtable address point, not a placeholder name); sizes asserted only for CMemBlock 0x38, CJson 0x10, SUuid 0x10 and SCallbacks 0x80, taken from the merged tree; SPeer and CTableXT<SR15NetLeaderboard> sizes unconfirmed; compiled with mingw g++ -c.
// @0x138960c — NRadEngine::NRadGame::CR15NetGame::BeginMultiplayer
/* @confidence: M */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "cmemory_context.hpp"

// Placeholders: no name is recoverable for these. Each is reached through a GOT slot (zero in the file) except
// DAT_0386ce10, which the code addresses directly.
// DAT_02c9dc50: .rodata, first 8 bytes 0 on disk (GOT slot 0x3828448, read at 0x138a818). Guards the leaderboard block.
extern const std::uint64_t DAT_02c9dc50;
// DAT_02c9db18: .rodata, first 8 bytes 0 on disk (GOT slot 0x382aec0, read at 0x138aaf0). Selects bit 0x1000 of the flags.
extern const std::uint64_t DAT_02c9db18;
// DAT_02c9d9e0: .rodata, first 8 bytes 0x5a on disk (GOT slot 0x3812958, read at 0x138b598). Size of the first range taken.
extern const std::uint64_t DAT_02c9d9e0;
// DAT_0386ce10: .bss, an 8-byte pointer to the input device table; read at 0x138ac74 and 0x138ac9c.
extern std::byte* DAT_0386ce10;

// Vptr value of NRadEngine::NRadGame::CR15NetClientLobby: the vtable address point 0x3730ab0 (ReVault vtable), which the
// code forms as GOT slot 0x380b2e8 (vtable symbol 0x3730aa0) plus 0x10. Merged convention g_vptr_<Class>, global scope.
extern const std::byte g_vptr_CR15NetClientLobby[];

namespace NRadEngine {

namespace NWriteLog {
enum class ELogLevel : std::int32_t {};  // enumerators not recovered; this function passes 8
}

// NRadEngine::OutputError(int, NWriteLog::ELogLevel) @ 0x12d1b38 (thunk 0x104e4c0).
std::int32_t OutputError(std::int32_t status, NWriteLog::ELogLevel level);

namespace CMemory {
// NRadEngine::CMemory::Copy @ 0x12cb908 (thunk 0x100cc90) and CMemory::Move @ 0x12cb90c (thunk 0x1010a90).
void Copy(void* destination, const void* source, std::uint64_t count);
void Move(void* destination, const void* source, std::uint64_t count);
}  // namespace CMemory

// NRadEngine::CMatSym::Hash(char const*) @ 0x13106f8 (thunk 0xfddeb0). The 64-bit symbol that identifies a message type.
struct CMatSym {
    std::uint64_t value;
    static std::uint64_t Hash(const char* text);
};

// NRadEngine::SPeer: only named in the ListenProxy signature here, so it is never constructed or read by this function.
// The two words are an assumption (two registers, x5 and x6 of the proxy); neither ReVault, the disassembly of this
// function nor the merged tree gives its size, so no static_assert pins it.
struct SPeer {
    std::uint64_t words[2];
};

// NRadEngine::SMsg<T>: the message wrapper that every callback takes by reference.
template <typename T>
struct SMsg;

// NRadEngine::CDelegate<void (void const*, unsigned long long, unsigned long long, SPeer)>, 0x20 bytes as built on the
// stack at sp+0x90: the object, the 16-byte member function pointer copied by CMemory::Copy, the proxy function.
using ListenProxyFn = void (*)(void*, const void*, const void*, std::uint64_t, std::uint64_t, SPeer);
struct CDelegate {
    void* object;                       // +0x00
    alignas(8) std::byte method[16];    // +0x08 member function pointer {code address, adjustment}
    ListenProxyFn proxy;                // +0x18
};
static_assert(sizeof(CDelegate) == 0x20);
static_assert(offsetof(CDelegate, object) == 0x00);
static_assert(offsetof(CDelegate, method) == 0x08);
static_assert(offsetof(CDelegate, proxy) == 0x18);

class CBroadcaster {
public:
    // NRadEngine::CBroadcaster::Listen(CMatSym, unsigned int, CDelegate<...> const&, unsigned int) @ 0x25e8a70
    // (thunk 0x1009e90). The caller keeps the low 16 bits of the result (the listener handle).
    std::uint16_t Listen(CMatSym type, std::uint32_t one, const CDelegate& delegate, std::uint32_t zero);
    // NRadEngine::CBroadcaster::EnableDefragTimeout(unsigned int) @ thunk 0x1024a70; the argument is 1 here.
    void EnableDefragTimeout(std::uint32_t enable);
    // NRadEngine::CBroadcaster::ListenProxy<C, T>(void*, void const*, void const*, unsigned long long,
    // unsigned long long, SPeer): the function whose address goes into the delegate.
    template <typename C, typename T>
    static void ListenProxy(void*, const void*, const void*, std::uint64_t, std::uint64_t, SPeer);
};

// NRadEngine::CMemBlock: addressed here only as a byte range inside the game object (+0x2ab8, +0x63120, +0x63158). Raw
// storage of 0x38 bytes, the size the merged cmem_block_constructor.cpp and cmem_block_destructor.cpp use (not from a
// ReVault type record). The two table blocks at +0x63120 and +0x63158 are 0x38 apart (next member at +0x63190); the range
// table at +0x2ab8 keeps two more words (+0x38, +0x40) after its first 0x38 bytes.
class CMemBlock {
public:
    alignas(8) std::byte raw_[0x38];

    // @ 0x12dc618 (thunk 0xfd8590)
    void SetAllocator(CAllocator* allocator);
    // @ 0x12dc2f8 (thunk 0xfe2470)
    void Detach();
    // @ 0x12dbc04 (thunk 0x103ad00)
    void Destructor_();
    // @ 0x12dbb58 (thunk 0xfed2a0): size and alignment, this in x0.
    void Constructor_(std::uint64_t size, std::uint64_t alignment);
    // @ 0x12dc30c (thunk 0x103e400)
    void Resize(std::uint64_t new_size, std::uint32_t keep_contents);
};
static_assert(alignof(CMemBlock) == 8);
static_assert(offsetof(CMemBlock, raw_) == 0x00);
static_assert(sizeof(CMemBlock::raw_) == 0x38);
static_assert(sizeof(CMemBlock) == 0x38);

// The 24-byte parameter block CTableXT<T>::Setup reads: its first word is the element count.
struct CMemParms {
    std::uint64_t words[2];
    CAllocator* current;
};
static_assert(sizeof(CMemParms) == 0x18);

// NRadEngine::CTableXT<NRadGame::SR15NetLeaderboard>, addressed as a byte range in the game object at +0x63208. Its size
// is not given by ReVault or the merged tree, so it has no storage and no static_assert here.
namespace NRadGame {
struct SR15NetLeaderboard;
}
template <typename T>
class CTableXT;
template <>
class CTableXT<NRadGame::SR15NetLeaderboard> {
public:
    // @ 0x138bcec (thunk 0x1013220)
    void Setup(const CMemParms& parms);
    // @ 0x13e5618 (thunk 0x101c830)
    std::uint64_t GrowBy(std::uint64_t count);
};

class CJson {
public:
    // NRadEngine::CJson::CJson() @ 0x10a2250 (thunk 0xfd7760).
    CJson();
    // NRadEngine::CJson::TString(char const*, char const*, unsigned int) const @ 0x10a2e7c (thunk 0x1028e40).
    const char* TString(const char* key, const char* fallback, std::uint32_t flags) const;

private:
    // Two words (root json_t* and path cache pointer), 0x10 bytes as in the merged cjson_constructor.cpp.
    alignas(8) std::byte raw_[0x10];
};
static_assert(sizeof(CJson) == 0x10);

class CUriContainer {
public:
    // @ 0x1a39654 (thunk 0x103eb10), ~CUriContainer @ 0x1372140 (thunk 0x104f310), Parse @ 0x1a397e8 (thunk 0xffdec0).
    CUriContainer();
    ~CUriContainer();
    std::int32_t Parse(const char* url);
};

class CInputDevice;

class CInputListener {
public:
    // NRadEngine::CInputListener::SetDevice(CInputDevice*, unsigned long long) @ 0x2637ad8 (thunk 0x1050660).
    void SetDevice(CInputDevice* device, std::uint64_t index);
};

class CVR {
public:
    // NRadEngine::CVR::SetHUDVisible(unsigned int) @ thunk 0xff9470.
    static void SetHUDVisible(std::uint32_t visible);
};

class CGRenderConfig {
public:
    // @ thunks 0xfd6fa0, 0x10445a0, 0xff9640. Ghidra shows SetForcedUIResolution(void); the call passes x0.
    static void SetEnableAutoExposure(std::uint32_t enable);
    static std::uint64_t ForcedUIResolution();
    static void SetForcedUIResolution(std::uint64_t resolution);
};

// NRadEngine::SUuid::kInvalid @ 0x386c3b8: two words, copied as 16 bytes into the lobby.
struct SUuid {
    std::uint64_t words[2];
    static const SUuid kInvalid;
};
static_assert(sizeof(SUuid) == 0x10);  // as in the merged begin_multiplayer.cpp and find_match.cpp

// Message types the game listens for. TypeId() is the function-local static the original inlines at every Listen:
// SR15NetSaveLoadoutRequest::TypeId()::id = CMatSym::Hash("SR15NetSaveLoadoutRequest"), guarded by __cxa_guard_acquire
// (import thunk 0xff6770) and __cxa_guard_release (import thunk 0x1051c60). The cold half of each sits at
// 0x138b7dc-0x138bbfc (22 stubs of 12 instructions).
#define R15_MESSAGE_TYPE(Name)                                      \
    struct Name {                                                   \
        static CMatSym TypeId() {                                   \
            static CMatSym id{CMatSym::Hash(#Name)};                \
            return id;                                              \
        }                                                           \
    }

R15_MESSAGE_TYPE(SNSLobbySettingsResponse);
R15_MESSAGE_TYPE(SNSLobbySendClientLobbySettings);
R15_MESSAGE_TYPE(SNSRefreshProfileForUser);
R15_MESSAGE_TYPE(SNSRefreshProfileFromServer);

namespace NRadGame {

R15_MESSAGE_TYPE(SR15NetSaveLoadoutRequest);
R15_MESSAGE_TYPE(SR15NetCurrentLoadoutRequest);
R15_MESSAGE_TYPE(SR15NetTierRewardMsg);
R15_MESSAGE_TYPE(SR15NetTopAwardsMsg);
R15_MESSAGE_TYPE(SR15NetReliableStatUpdate);
R15_MESSAGE_TYPE(SR15NetReliableTeamStatUpdate);
R15_MESSAGE_TYPE(SR15NetNewUnlocks);
R15_MESSAGE_TYPE(SR15NetSaveLoadoutSuccess);
R15_MESSAGE_TYPE(SR15NetSaveLoadoutPartial);
R15_MESSAGE_TYPE(SR15NetSaveLoadoutFailure);
R15_MESSAGE_TYPE(SR15NetLeaderboardMsg);
R15_MESSAGE_TYPE(SR15NetReliablePostScoreUpdate);
R15_MESSAGE_TYPE(SR15NetCurrentLoadoutResponse);
R15_MESSAGE_TYPE(SR15NetItemEquippedNotification);
R15_MESSAGE_TYPE(SR15NetLoadoutNumberNotification);
R15_MESSAGE_TYPE(SR15NetComponentEvent);
R15_MESSAGE_TYPE(SR15NetPlayEmote);
R15_MESSAGE_TYPE(SR15NetAbilityActivate);
// Listened for through the out-of-line Listen<SR15NetAbilityCooldown, CR15NetGame> at 0x138bbfc, which owns its TypeId.
struct SR15NetAbilityCooldown;

#undef R15_MESSAGE_TYPE

// The 0x48-byte match settings block, copied unchanged into the lobby at +0x698.
struct SR15NetMatchSettings {
    alignas(8) std::byte raw[0x48];
};
static_assert(sizeof(SR15NetMatchSettings) == 0x48);

class CR15NetGame;
class CR15NetGameMT;

class CR15NetGameplay {
public:
    // @ 0x13ab0e0 (thunk 0x100f3d0)
    explicit CR15NetGameplay(CR15NetGame* game);
};

class CR15NetAIScenarios {
public:
    // @ 0x133b930 (thunk 0xfeede0). Ghidra types the parameter CR15NetGameMT*; the game object is passed unchanged.
    explicit CR15NetAIScenarios(CR15NetGameMT* game);
};

class CR15NetStoreTransactions {
public:
    // Callback table passed by reference; BeginMultiplayer passes it all zero.
    struct SCallbacks {
        alignas(8) std::byte raw[0x80];
    };
    static_assert(sizeof(SCallbacks) == 0x80);  // as in the merged login_success.cpp; BeginMultiplayer zeroes all of it
    // @ 0x13b537c (thunk 0x101fc00)
    CR15NetStoreTransactions(CR15NetGame& game, CUriContainer& uri, const SCallbacks& callbacks);
};

class SR15NetScreenStatsConfig {
public:
    // @ 0x134b31c (thunk 0xffc560): the game object keeps one at +0x63190.
    std::int32_t Load();
};

class SR15NetProgression {
public:
    // @ 0x134b210 (thunk 0xfe55f0): the game object keeps one at +0x63070.
    std::int32_t Load();
};

class SR15NetLeaderboardsData {
public:
    // @ 0x220042c (thunk 0x1000980): the game object keeps one at +0x631c8.
    std::int32_t Load();
};

class CR15NetLobby {
public:
    // @ 0x1344d58 (thunk 0x10252d0): the base constructor of the client lobby.
    explicit CR15NetLobby(CR15NetGame& game);
};

class CR15NetClientLobby {
public:
    // @ 0x1343e48 (thunk 0x1001a70) and 0x13479f4 (thunk 0xffc3c0).
    void RegisterEvents();
    void LoginSettingsChanged();
};

class CR15NetDedicatedLobby {
public:
    // @ 0x134a3dc (thunk 0x10478e0)
    explicit CR15NetDedicatedLobby(CR15NetGame& game);
};

class CR15NetSpectatorCameraCS {
public:
    // @ 0x22a5074 (thunk 0xfe64c0)
    static void LoadCameraData();
};

class CR15NetGame {
public:
    enum class EState : std::int32_t {};  // as in the merged cr15_net_game_switch_to.cpp; enumerators not recovered

    // This function: 0x138960c (thunk 0xfe0f00). The merged CR15Game::BeginMultiplayer calls it with the same signature.
    std::int32_t BeginMultiplayer(const SR15NetMatchSettings& settings);

    // NRadEngine::NRadGame::CR15NetGame::Listen<T, C>(C*, void (C::*)(SMsg<T> const&)). Inlined for 22 messages here;
    // the SR15NetAbilityCooldown instance is the out-of-line function at 0x138bbfc (thunk 0x1007f10), called twice.
    template <typename T, typename C>
    std::uint16_t Listen(C* object, void (C::*callback)(const SMsg<T>&));

    // Members called from this function (thunks in brackets).
    void RegisterServerCallbacks();        // 0x13711d8 [0x103a3c0]
    void ResetPlayerData();                // 0x1371c68 [0x10539c0]
    void EstablishConnections();           // 0x135e840 [0xfd8e00]
    void SwitchTo(EState state);           // 0x135b8b4 [0xfe6330]
    void InitializeHttpListener();         // 0x1382df8 [0x1045b40]

    // Message callbacks whose member function pointers are registered below.
    void SaveLoadoutRequestCB(const SMsg<SR15NetSaveLoadoutRequest>& message);  // 0x1365598
    void LobbySettingsCB(const SMsg<SNSLobbySettingsResponse>& message);
    void CurrentLoadoutRequestCB(const SMsg<SR15NetCurrentLoadoutRequest>& message);
    void SendLobbySettingsCB(const SMsg<SNSLobbySendClientLobbySettings>& message);
    void RefreshProfileForUserCB(const SMsg<SNSRefreshProfileForUser>& message);
    void RefreshProfileFromServerCB(const SMsg<SNSRefreshProfileFromServer>& message);
    void TierRewardCB(const SMsg<SR15NetTierRewardMsg>& message);
    void TopAwardsCB(const SMsg<SR15NetTopAwardsMsg>& message);
    void ReliableStatUpdateCB(const SMsg<SR15NetReliableStatUpdate>& message);
    void ReliableTeamStatUpdateCB(const SMsg<SR15NetReliableTeamStatUpdate>& message);
    void NewUnlocksCB(const SMsg<SR15NetNewUnlocks>& message);
    void SaveLoadoutSuccessCB(const SMsg<SR15NetSaveLoadoutSuccess>& message);
    void SaveLoadoutPartialCB(const SMsg<SR15NetSaveLoadoutPartial>& message);
    void SaveLoadoutFailureCB(const SMsg<SR15NetSaveLoadoutFailure>& message);
    void ServerLeaderboardCB(const SMsg<SR15NetLeaderboardMsg>& message);
    void ReliablePostScoreUpdateCB(const SMsg<SR15NetReliablePostScoreUpdate>& message);
    void CurrentLoadoutResponseCB(const SMsg<SR15NetCurrentLoadoutResponse>& message);
    void ItemEquippedNotificationCB(const SMsg<SR15NetItemEquippedNotification>& message);
    void LoadoutNumberNotificationCB(const SMsg<SR15NetLoadoutNumberNotification>& message);
    void ComponentEventCB(const SMsg<SR15NetComponentEvent>& message);
    void PlayEmoteCB(const SMsg<SR15NetPlayEmote>& message);
    void AbilityActivateCB(const SMsg<SR15NetAbilityActivate>& message);
    void AbilityCooldownCB(const SMsg<SR15NetAbilityCooldown>& message);  // 0x1362e70
};

namespace {

// Offsets recovered from the disassembly; the rest of the object layout is not recovered.
constexpr std::size_t kLockPointerOffset = 0x30;       // ldr x20, [x19, #0x30]: pointer to the 32-bit spin lock word
constexpr std::size_t kBroadcasterOffset = 0x28c8;     // ldr x21, [x19, #0x28c8]: the CBroadcaster
constexpr std::size_t kFlagsPointerOffset = 0x2da0;    // ldr x8, [x19, #0x2da0]: pointer to the 64-bit flags word
constexpr std::size_t kListenCountOffset = 0x2d50;     // u32: number of handles recorded
constexpr std::size_t kListenHandlesOffset = 0x2d20;   // u16 handles, indexed by the count
constexpr std::size_t kRangeTableOffset = 0x2ab8;      // table of 16-byte {first, last} free ranges
constexpr std::size_t kRangeDataOffset = 0x2ab8;       // pointer to the ranges (the CMemBlock data word)
constexpr std::size_t kRangeGrowByOffset = 0x2ad8;     // table +0x20
constexpr std::size_t kRangeCapacityOffset = 0x2ae0;   // table +0x28
constexpr std::size_t kRangeCountOffset = 0x2ae8;      // table +0x30
constexpr std::size_t kRangeLimitOffset = 0x2af0;      // table +0x38: 0x10000
constexpr std::size_t kRangeTotalOffset = 0x2af8;      // table +0x40: entries handed out so far

template <typename T>
T Load(const void* base, std::size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof value);
    return value;
}

template <typename T>
void Store(void* base, std::size_t offset, T value) {
    std::memcpy(static_cast<std::byte*>(base) + offset, &value, sizeof value);
}

template <typename T>
T* LoadPtr(const void* base, std::size_t offset) {
    return Load<T*>(base, offset);
}

// The function address in the vtable slot at byte `offset` of `object` (vtable pointer at +0). ReVault names no slot of
// CAllocator: +0x10 is PURE_VIRTUAL in its vtable.
void* VirtualSlot(const void* object, std::size_t offset) {
    return Load<void*>(Load<const void*>(object, 0), offset);
}

void ZeroBytes(void* base, std::size_t offset, std::size_t count) {
    std::memset(static_cast<std::byte*>(base) + offset, 0, count);
}

}  // namespace

template <typename T, typename C>
std::uint16_t CR15NetGame::Listen(C* object, void (C::*callback)(const SMsg<T>&)) {
    auto* const self = reinterpret_cast<std::byte*>(this);

    // The spin lock word at *(this + 0x30): wait for 0 (ldar), then 0 -> 1 (ldaxr / stlxr); released with stlr.
    auto* const lock = LoadPtr<std::atomic<std::int32_t>>(self, kLockPointerOffset);
    for (;;) {
        while (lock->load(std::memory_order_acquire) != 0) {
        }
        std::int32_t expected = 0;
        if (lock->compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
            break;
        }
    }

    CBroadcaster* const broadcaster = LoadPtr<CBroadcaster>(self, kBroadcasterOffset);
    const CMatSym type = T::TypeId();

    CDelegate delegate;
    delegate.object = object;
    delegate.proxy = &CBroadcaster::ListenProxy<C, T>;
    static_assert(sizeof callback == sizeof delegate.method);
    CMemory::Copy(delegate.method, &callback, sizeof callback);
    const std::uint16_t handle = broadcaster->Listen(type, 1, delegate, 0);

    lock->store(0, std::memory_order_release);
    return handle;
}

// Out-of-line instance @ 0x138bbfc (thunk 0x1007f10): x0 this, x1 the object, x2:x3 the member function pointer.
template <>
std::uint16_t CR15NetGame::Listen<SR15NetAbilityCooldown, CR15NetGame>(
    CR15NetGame* object, void (CR15NetGame::*callback)(const SMsg<SR15NetAbilityCooldown>&));

std::int32_t CR15NetGame::BeginMultiplayer(const SR15NetMatchSettings& settings) {
    auto* const self = reinterpret_cast<std::byte*>(this);

    // 0x1389634-0x1389658: HUD off, auto exposure off, the forced UI resolution is saved at +0x6484c and replaced.
    CVR::SetHUDVisible(0);
    CGRenderConfig::SetEnableAutoExposure(0);
    Store<std::uint64_t>(self, 0x6484c, CGRenderConfig::ForcedUIResolution());
    CGRenderConfig::SetForcedUIResolution(0x36000000600ULL);

    // 0x138965c-0x1389690: the default allocator is the first word of the memory context.
    if (CMemoryContext::context == nullptr) {
        NWriteLog::WriteLog(8, nullptr, "Trying to use memory context before it's initialized!");
        SoftwareBreakpoint(1, 0x138968c)();
    }
    CAllocator* const default_allocator = *static_cast<CAllocator**>(CMemoryContext::context);

    // 0x13896b8-0x1389728: the free-range table at +0x2ab8 is given a limit of 0x10000 and one range [0, 0xffff].
    Store<std::uint64_t>(self, kRangeLimitOffset, 0x10000);
    auto* const range_table = reinterpret_cast<CMemBlock*>(self + kRangeTableOffset);
    if (default_allocator != nullptr) {
        range_table->SetAllocator(default_allocator);
    }
    const std::uint64_t last_free = Load<std::uint64_t>(self, kRangeLimitOffset) - 1;
    std::uint64_t range_count = Load<std::uint64_t>(self, kRangeCountOffset);
    const std::uint64_t range_capacity = Load<std::uint64_t>(self, kRangeCapacityOffset);
    if (range_count >= range_capacity) {
        const std::uint64_t grow_by = Load<std::uint64_t>(self, kRangeGrowByOffset);
        const std::uint64_t extra = grow_by < range_capacity ? range_capacity : grow_by;  // csel: the larger of the two
        range_table->Resize((extra + range_capacity) << 4, 1);
        Store<std::uint64_t>(self, kRangeCapacityOffset, Load<std::uint64_t>(self, kRangeCapacityOffset) + extra);
        range_count = Load<std::uint64_t>(self, kRangeCountOffset);
    }
    range_count += 1;
    Store<std::uint64_t>(self, kRangeCountOffset, range_count);
    {
        std::byte* const entry = LoadPtr<std::byte>(self, kRangeDataOffset) + range_count * 16 - 16;
        Store<std::uint64_t>(entry, 0, 0);
        Store<std::uint64_t>(entry, 8, last_free);
    }

    // Records a listener handle: handles[count++] = handle (0x1389858-0x1389870 and the same eight instructions per Listen).
    auto record_handle = [self](std::uint16_t handle) {
        const std::uint32_t index = Load<std::uint32_t>(self, kListenCountOffset);
        Store<std::uint32_t>(self, kListenCountOffset, index + 1);
        Store<std::uint16_t>(self, kListenHandlesOffset + static_cast<std::size_t>(index) * 2, handle);
    };

    // 0x138972c-0x13897b0: the flags word. Bit 1, or (flags & 0x44) == 0x40, selects the server callbacks.
    const std::uint64_t start_flags = Load<std::uint64_t>(LoadPtr<std::byte>(self, kFlagsPointerOffset), 0);
    if ((start_flags & 2) != 0 || (start_flags & 0x44) == 0x40) {
        RegisterServerCallbacks();  // 0x13897b8
        // 0x13897bc-0x1389870: the first inlined Listen; TypeId guard 0x381c2b0, cold stub 0x138b98c.
        record_handle(Listen<SR15NetSaveLoadoutRequest, CR15NetGame>(this, &CR15NetGame::SaveLoadoutRequestCB));
        // 0x1389874-0x1389924: guard 0x38286e8, cold stub 0x138b9bc.
        record_handle(Listen<SNSLobbySendClientLobbySettings, CR15NetGame>(this, &CR15NetGame::SendLobbySettingsCB));
        // 0x1389928-0x1389978, tail 0x138997c-0x13899d4 (shared with the client branch): guard 0x381f028, stub 0x138b9ec.
        record_handle(Listen<SNSRefreshProfileForUser, CR15NetGame>(this, &CR15NetGame::RefreshProfileForUserCB));
    } else {
        // 0x1389758-0x13897b0, then the shared tail 0x138997c-0x13899d4: guard 0x3824530, cold stub 0x138b95c.
        record_handle(Listen<SNSLobbySettingsResponse, CR15NetGame>(this, &CR15NetGame::LobbySettingsCB));
    }

    // 0x13899d8-0x1389a88: guard 0x380a158, stub 0x138b7dc.
    record_handle(Listen<SR15NetCurrentLoadoutRequest, CR15NetGame>(this, &CR15NetGame::CurrentLoadoutRequestCB));

    // 0x1389a8c-0x1389a94: bit 2 of the first flags byte (a dedicated server) skips the next ten registrations.
    if ((Load<std::uint8_t>(LoadPtr<std::byte>(self, kFlagsPointerOffset), 0) & 4) == 0) {
        // 0x1389a98-0x1389b4c: guard 0x381f5c0, stub 0x138ba1c.
        record_handle(
            Listen<SNSRefreshProfileFromServer, CR15NetGame>(this, &CR15NetGame::RefreshProfileFromServerCB));
        // 0x1389b50-0x1389c00: guard 0x3826320, stub 0x138ba4c.
        record_handle(Listen<SR15NetTierRewardMsg, CR15NetGame>(this, &CR15NetGame::TierRewardCB));
        // 0x1389c04-0x1389cb4: guard 0x38036e0, stub 0x138ba7c.
        record_handle(Listen<SR15NetTopAwardsMsg, CR15NetGame>(this, &CR15NetGame::TopAwardsCB));
        // 0x1389cb8-0x1389d68: guard 0x381c1d8, stub 0x138baac.
        record_handle(Listen<SR15NetReliableStatUpdate, CR15NetGame>(this, &CR15NetGame::ReliableStatUpdateCB));
        // 0x1389d6c-0x1389e1c: guard 0x3807a20, stub 0x138badc.
        record_handle(
            Listen<SR15NetReliableTeamStatUpdate, CR15NetGame>(this, &CR15NetGame::ReliableTeamStatUpdateCB));
        // 0x1389e20-0x1389ed0: guard 0x3828540, stub 0x138bb0c.
        record_handle(Listen<SR15NetNewUnlocks, CR15NetGame>(this, &CR15NetGame::NewUnlocksCB));
        // 0x1389ed4-0x1389f84: guard 0x381e0d8, stub 0x138bb3c.
        record_handle(Listen<SR15NetSaveLoadoutSuccess, CR15NetGame>(this, &CR15NetGame::SaveLoadoutSuccessCB));
        // 0x1389f88-0x138a038: guard 0x381b300, stub 0x138bb6c.
        record_handle(Listen<SR15NetSaveLoadoutPartial, CR15NetGame>(this, &CR15NetGame::SaveLoadoutPartialCB));
        // 0x138a03c-0x138a0ec: guard 0x380b510, stub 0x138bb9c.
        record_handle(Listen<SR15NetSaveLoadoutFailure, CR15NetGame>(this, &CR15NetGame::SaveLoadoutFailureCB));
        // 0x138a0f0-0x138a19c: guard 0x380e930, stub 0x138bbcc.
        record_handle(Listen<SR15NetLeaderboardMsg, CR15NetGame>(this, &CR15NetGame::ServerLeaderboardCB));
    }

    // 0x138a1a0-0x138a254: guard 0x3802308, stub 0x138b80c.
    record_handle(Listen<SR15NetReliablePostScoreUpdate, CR15NetGame>(this, &CR15NetGame::ReliablePostScoreUpdateCB));
    // 0x138a258-0x138a308: guard 0x3814070, stub 0x138b83c.
    record_handle(Listen<SR15NetCurrentLoadoutResponse, CR15NetGame>(this, &CR15NetGame::CurrentLoadoutResponseCB));
    // 0x138a30c-0x138a3bc: guard 0x38084e8, stub 0x138b86c.
    record_handle(
        Listen<SR15NetItemEquippedNotification, CR15NetGame>(this, &CR15NetGame::ItemEquippedNotificationCB));
    // 0x138a3c0-0x138a470: guard 0x3822258, stub 0x138b89c.
    record_handle(
        Listen<SR15NetLoadoutNumberNotification, CR15NetGame>(this, &CR15NetGame::LoadoutNumberNotificationCB));
    // 0x138a474-0x138a524: guard 0x382af48, stub 0x138b8cc.
    record_handle(Listen<SR15NetComponentEvent, CR15NetGame>(this, &CR15NetGame::ComponentEventCB));
    // 0x138a528-0x138a5dc: guard 0x3815368, stub 0x138b8fc.
    record_handle(Listen<SR15NetPlayEmote, CR15NetGame>(this, &CR15NetGame::PlayEmoteCB));
    // 0x138a5e0-0x138a600: the out-of-line instance, called with (this, this, the callback at GOT slot 0x3820ef8, 0).
    record_handle(Listen<SR15NetAbilityCooldown, CR15NetGame>(this, &CR15NetGame::AbilityCooldownCB));
    // 0x138a604-0x138a6b4: guard 0x3814320, stub 0x138b92c.
    record_handle(Listen<SR15NetAbilityActivate, CR15NetGame>(this, &CR15NetGame::AbilityActivateCB));
    // 0x138a6b8-0x138a6d8: the same out-of-line instance a second time, registering the same callback again.
    record_handle(Listen<SR15NetAbilityCooldown, CR15NetGame>(this, &CR15NetGame::AbilityCooldownCB));

    // Allocation through the current allocator's vtable slot +0x10 (no name recovered); the size goes in w1.
    using AllocateFn = void* (*)(CAllocator*, std::uint32_t);

    // 0x138a6dc-0x138a6fc: the gameplay object, 0x4d8 bytes, stored at +0x2aa8.
    {
        CAllocator* const allocator = CMemoryContext::Current();
        void* const storage = reinterpret_cast<AllocateFn>(VirtualSlot(allocator, 0x10))(allocator, 0x4d8);
        auto* const gameplay = ::new (storage) CR15NetGameplay(this);
        Store<CR15NetGameplay*>(self, 0x2aa8, gameplay);
    }
    // 0x138a700-0x138a71c: the AI scenarios object, 0x570 bytes, stored at +0x2ab0 after the next constructor returns.
    CR15NetAIScenarios* scenarios;
    {
        CAllocator* const allocator = CMemoryContext::Current();
        void* const storage = reinterpret_cast<AllocateFn>(VirtualSlot(allocator, 0x10))(allocator, 0x570);
        scenarios = ::new (storage) CR15NetAIScenarios(reinterpret_cast<CR15NetGameMT*>(this));
    }
    Store<CR15NetAIScenarios*>(self, 0x2ab0, scenarios);  // 0x138a72c, before the Load call below
    // 0x138a730: SR15NetScreenStatsConfig::Load on the member at +0x63190; a nonzero result is returned.
    if (const std::int32_t status = reinterpret_cast<SR15NetScreenStatsConfig*>(self + 0x63190)->Load()) {
        return status;
    }

    // 0x138a758-0x138a814: bit 2 of the first flags byte (dedicated server) builds the transaction store.
    if ((Load<std::uint8_t>(LoadPtr<std::byte>(self, kFlagsPointerOffset), 0) & 4) != 0) {
        alignas(8) std::byte uri_slot[0x120];  // sp+0x90: the CUriContainer; its extent is not recovered
        CUriContainer& uri = *::new (uri_slot) CUriContainer();
        const CJson* const config = reinterpret_cast<const CJson*>(self + 0x63240);
        // The default is the string at 0x2caecf7; its text is the production URL below.
        const char* url = config->TString("transaction_host", "wss://transaction.readyatdawn.com/rad/rad15_live", 0);
        url = config->TString("transactionservice_host", url, 0);
        if (OutputError(uri.Parse(url), static_cast<NWriteLog::ELogLevel>(8)) == 0) {
            CAllocator* const allocator = CMemoryContext::Current();
            void* const storage = reinterpret_cast<AllocateFn>(VirtualSlot(allocator, 0x10))(allocator, 0x1c8);
            CR15NetStoreTransactions::SCallbacks callbacks;
            std::memset(&callbacks, 0, sizeof callbacks);  // four zeroing stores of 32 bytes at sp+0x10
            auto* const store = ::new (storage) CR15NetStoreTransactions(*this, uri, callbacks);
            Store<CR15NetStoreTransactions*>(self, 0x2aa0, store);
            Store<std::uint64_t>(store, 0, 5);
        }
        uri.~CUriContainer();
    }

    // 0x138a814-0x138a874: DAT_02c9dc50 is zero in the file, so this block is dead on this binary; Ghidra removed it.
    if (DAT_02c9dc50 != 0) {
        if (const std::int32_t status = reinterpret_cast<SR15NetLeaderboardsData*>(self + 0x631c8)->Load()) {
            return status;
        }
        CMemParms parms;
        parms.words[0] = Load<std::uint64_t>(self, 0x631f8);
        parms.words[1] = 0;
        parms.current = CMemoryContext::CurrentPtr();
        auto* const leaderboards = reinterpret_cast<CTableXT<SR15NetLeaderboard>*>(self + 0x63208);
        leaderboards->Setup(parms);
        leaderboards->GrowBy(Load<std::uint64_t>(self, 0x63230) - Load<std::uint64_t>(self, 0x63238));
    }

    // 0x138a874-0x138a884: SR15NetProgression::Load on the member at +0x63070; a nonzero result is returned.
    if (const std::int32_t status = reinterpret_cast<SR15NetProgression*>(self + 0x63070)->Load()) {
        return status;
    }

    // 0x138a888-0x138a968: over the progression rows (pointer +0x630e8, count +0x63118, stride 0x90) find the largest
    // entry.field(+0x40) * row.count(+0x50) over the entries (pointer row+0x20, stride 0x48) and the largest row.field(+8).
    // The compiler vectorized the inner loop by two; this is the scalar form.
    std::uint64_t largest_product = 0;
    std::uint64_t largest_row_value = 0;
    {
        const std::uint64_t row_count = Load<std::uint64_t>(self, 0x63118);
        const std::byte* const rows = LoadPtr<const std::byte>(self, 0x630e8);
        for (std::uint64_t row_index = 0; row_index < row_count; ++row_index) {
            const std::byte* const row = rows + row_index * 0x90;
            const std::uint64_t entry_count = Load<std::uint64_t>(row, 0x50);
            const std::byte* const entries = LoadPtr<const std::byte>(row, 0x20);
            for (std::uint64_t entry_index = 0; entry_index < entry_count; ++entry_index) {
                const std::uint64_t product = Load<std::uint64_t>(entries + entry_index * 0x48, 0x40) * entry_count;
                if (product > largest_product) {
                    largest_product = product;
                }
            }
            const std::uint64_t row_value = Load<std::uint64_t>(row, 8);
            if (row_value > largest_row_value) {
                largest_row_value = row_value;
            }
        }
    }

    // 0x138a96c-0x138aa28: the table block at +0x63120 (flags word +0x6313c, allocator +0x63130, count +0x63150) is rebuilt
    // to hold largest_product elements of 0x20 bytes. This is CTableXT<T>::Setup(CMemParms const&) inlined for an element
    // type that is not recovered; the instance for SR15NetLeaderboard is the function at 0x138bcec.
    if (CMemoryContext::context == nullptr) {
        NWriteLog::WriteLog(8, nullptr, "Trying to use memory context before it's initialized!");
        SoftwareBreakpoint(1, 0x138a998)();
    }
    {
        CAllocator* const allocator = *static_cast<CAllocator**>(CMemoryContext::context);
        std::byte* const block = self + 0x63120;
        std::uint32_t block_flags = Load<std::uint32_t>(self, 0x6313c);
        if ((block_flags & 4) == 0) {
            CAllocator* const owner = LoadPtr<CAllocator>(self, 0x63130);
            CAllocator* const saved = CMemoryContext::CurrentPtr();
            CMemoryContext::SetCurrent(owner);
            Store<std::uint64_t>(self, 0x63150, 0);
            CMemoryContext::SetCurrent(saved);
            block_flags = Load<std::uint32_t>(self, 0x6313c);
        }
        if ((block_flags & 6) == 0) {
            reinterpret_cast<CMemBlock*>(block)->Destructor_();
        } else {
            reinterpret_cast<CMemBlock*>(block)->Detach();
            Store<std::uint32_t>(self, 0x6313c, Load<std::uint32_t>(self, 0x6313c) & 0xfffffff9u);
        }
        reinterpret_cast<CMemBlock*>(block)->SetAllocator(allocator);
        reinterpret_cast<CMemBlock*>(block)->Constructor_(largest_product << 5, 0);
        Store<std::uint64_t>(self, 0x63148, largest_product);
        Store<std::uint64_t>(self, 0x63150, 0);
        Store<std::uint64_t>(self, 0x63140, 0);
    }

    // 0x138aa28-0x138aae4: the same rebuild for the block at +0x63158 (flags word +0x63174, allocator +0x63168, count
    // +0x63188) with largest_row_value + 1 elements of 0x48 bytes.
    if (CMemoryContext::context == nullptr) {
        NWriteLog::WriteLog(8, nullptr, "Trying to use memory context before it's initialized!");
        SoftwareBreakpoint(1, 0x138aa50)();
    }
    {
        CAllocator* const allocator = *static_cast<CAllocator**>(CMemoryContext::context);
        const std::uint64_t row_slots = largest_row_value + 1;
        std::byte* const block = self + 0x63158;
        std::uint32_t block_flags = Load<std::uint32_t>(self, 0x63174);
        if ((block_flags & 4) == 0) {
            CAllocator* const owner = LoadPtr<CAllocator>(self, 0x63168);
            CAllocator* const saved = CMemoryContext::CurrentPtr();
            CMemoryContext::SetCurrent(owner);
            Store<std::uint64_t>(self, 0x63188, 0);
            CMemoryContext::SetCurrent(saved);
            block_flags = Load<std::uint32_t>(self, 0x63174);
        }
        if ((block_flags & 6) == 0) {
            reinterpret_cast<CMemBlock*>(block)->Destructor_();
        } else {
            reinterpret_cast<CMemBlock*>(block)->Detach();
            Store<std::uint32_t>(self, 0x63174, Load<std::uint32_t>(self, 0x63174) & 0xfffffff9u);
        }
        reinterpret_cast<CMemBlock*>(block)->SetAllocator(allocator);
        reinterpret_cast<CMemBlock*>(block)->Constructor_(row_slots * 0x48, 0);
        Store<std::uint64_t>(self, 0x63180, row_slots);
        Store<std::uint64_t>(self, 0x63188, 0);
        Store<std::uint64_t>(self, 0x63178, 0);
    }

    // 0x138aae8-0x138ab14: bit 0x1000 of the flags word follows DAT_02c9db18 (zero in the file: cleared); +0x48 = -1.
    std::uint64_t* const flags_word = LoadPtr<std::uint64_t>(self, kFlagsPointerOffset);
    const std::uint64_t flags_before = *flags_word;
    const std::uint64_t flags_after = DAT_02c9db18 == 0 ? (flags_before & ~0x1000ULL) : (flags_before | 0x1000ULL);
    *flags_word = flags_after;
    Store<std::int32_t>(self, 0x48, -1);

    // 0x138ab18-0x138ac48: the lobby. Bit 2 of the flags selects the dedicated lobby (0x690 bytes) or the client lobby
    // (0x918 bytes); the allocator slot is loaded before the test.
    {
        CAllocator* const allocator = CMemoryContext::Current();
        const auto allocate = reinterpret_cast<AllocateFn>(VirtualSlot(allocator, 0x10));
        if ((flags_after & 4) == 0) {
            auto* const lobby = static_cast<std::byte*>(allocate(allocator, 0x918));
            const std::uint64_t game_value = Load<std::uint64_t>(self, 0x38);
            ::new (lobby) CR15NetLobby(*this);  // 0x138ab3c: the base constructor
            // 0x138ab40-0x138ab50: the derived constructor, inlined. Vptr, then the CJson at +0x688.
            Store<const std::byte*>(lobby, 0, g_vptr_CR15NetClientLobby);
            ::new (lobby + 0x688) CJson();  // 0x138ab54
            // 0x138ab58-0x138ab84: the 0x48-byte settings block at +0x698.
            std::memcpy(lobby + 0x698, &settings, sizeof settings);
            // 0x138ab98-0x138ac18: +0x6e0 up to +0x8df zeroed (32 stores of 16 bytes), +0x8e0 = -1.
            ZeroBytes(lobby, 0x6e0, 0x200);
            Store<std::uint64_t>(lobby, 0x8e0, ~0ULL);
            // 0x138ab88-0x138ac28: +0x8f0 = SUuid::kInvalid, +0x900 = {0, 1} (the constant at 0x2c30580), +0x910 = game +0x38.
            std::memcpy(lobby + 0x8f0, &SUuid::kInvalid, sizeof(SUuid));
            Store<std::uint64_t>(lobby, 0x900, 0);
            Store<std::uint64_t>(lobby, 0x908, 1);
            Store<std::uint64_t>(lobby, 0x910, game_value);
            reinterpret_cast<CR15NetClientLobby*>(lobby)->RegisterEvents();  // 0x138ac2c
            Store<std::byte*>(self, 0x40, lobby);
            reinterpret_cast<CR15NetClientLobby*>(lobby)->LoginSettingsChanged();  // 0x138ac38
        } else {
            void* const storage = allocate(allocator, 0x690);
            auto* const lobby = ::new (storage) CR15NetDedicatedLobby(*this);  // 0x138ac5c
            Store<CR15NetDedicatedLobby*>(self, 0x40, lobby);
        }
    }

    // 0x138ac3c-0x138acc4: with bit 2 of the first flags byte clear, the input listener at +0x2e50 gets the devices of
    // the table at DAT_0386ce10: index 0 at +0, index 1 at +0x1400 unless bit 4 of byte 0x7ada of the object at +8 is set,
    // and index 2 at +0x640.
    if ((Load<std::uint8_t>(LoadPtr<std::byte>(self, kFlagsPointerOffset), 0) & 4) == 0) {
        auto* const listener = reinterpret_cast<CInputListener*>(self + 0x2e50);
        listener->SetDevice(reinterpret_cast<CInputDevice*>(DAT_0386ce10), 0);
        if ((Load<std::uint8_t>(LoadPtr<std::byte>(self, 8), 0x7ada) & 0x10) == 0) {
            listener->SetDevice(reinterpret_cast<CInputDevice*>(DAT_0386ce10 + 0x1400), 1);
        }
        listener->SetDevice(reinterpret_cast<CInputDevice*>(DAT_0386ce10 + 0x640), 2);
    }

    // 0x138acc8-0x138ad04: reset the player data, set bit 0x4000, and go to state 4 when the server flag (bit 2) is set
    // or the connections came up with the state at +0 equal to 3.
    ResetPlayerData();
    {
        std::uint64_t* const flags = LoadPtr<std::uint64_t>(self, kFlagsPointerOffset);
        const std::uint64_t before = *flags;
        *flags = before | 0x4000;
        if ((before & 4) != 0 || (EstablishConnections(), Load<std::int32_t>(self, 0) == 3)) {
            SwitchTo(static_cast<EState>(4));
        }
    }

    // 0x138ad04-0x138ad24: the broadcaster at +0x28c8 enables its defrag timeout.
    LoadPtr<CBroadcaster>(self, kBroadcasterOffset)->EnableDefragTimeout(1);

    NWriteLog::WriteLog(2, nullptr, "[NETGAME] Resetting local connection stats");  // 0x138ad24

    // 0x138ad28-0x138b580: reset of the connection statistics, as stores of zero. The original writes them with 16-byte
    // vector stores; the byte ranges below are the union (checked against a simulation of every store in the range).
    // One slot of the 0xe78-byte array of sixteen slots at +0x55018: 0x64 bytes at its head.
    auto reset_slot = [](std::byte* slot) {
        ZeroBytes(slot, 0x00, 0x0c);
        ZeroBytes(slot, 0x10, 0x10);
        Store<std::uint32_t>(slot, 0x20, 1);
        ZeroBytes(slot, 0x24, 0x40);
    };
    ZeroBytes(self, 0x53a28, 0x0c);
    ZeroBytes(self, 0x54140, 0x0c);
    ZeroBytes(self, 0x54150, 0x04);
    ZeroBytes(self, 0x54158, 0x38);
    ZeroBytes(self, 0x541d0, 0x34);
    reset_slot(self + 0x55018);
    reset_slot(self + 0x55e90);
    reset_slot(self + 0x56d08);
    reset_slot(self + 0x57b80);
    reset_slot(self + 0x589f8);
    reset_slot(self + 0x59870);
    reset_slot(self + 0x5a6e8);
    reset_slot(self + 0x5b560);
    reset_slot(self + 0x5c3d8);
    reset_slot(self + 0x5d250);
    reset_slot(self + 0x5e0c8);
    reset_slot(self + 0x5ef40);
    reset_slot(self + 0x5fdb8);
    reset_slot(self + 0x60c30);
    reset_slot(self + 0x61aa8);
    reset_slot(self + 0x62920);
    // Fifteen elements of 0x70 bytes from +0x62988, the first 0x6c bytes of each.
    ZeroBytes(self, 0x62988, 0x6c);
    ZeroBytes(self, 0x629f8, 0x6c);
    ZeroBytes(self, 0x62a68, 0x6c);
    ZeroBytes(self, 0x62ad8, 0x6c);
    ZeroBytes(self, 0x62b48, 0x6c);
    ZeroBytes(self, 0x62bb8, 0x6c);
    ZeroBytes(self, 0x62c28, 0x6c);
    ZeroBytes(self, 0x62c98, 0x6c);
    ZeroBytes(self, 0x62d08, 0x6c);
    ZeroBytes(self, 0x62d78, 0x6c);
    ZeroBytes(self, 0x62de8, 0x6c);
    ZeroBytes(self, 0x62e58, 0x6c);
    ZeroBytes(self, 0x62ec8, 0x6c);
    ZeroBytes(self, 0x62f38, 0x6c);
    ZeroBytes(self, 0x62fa8, 0x6c);
    ZeroBytes(self, 0x63018, 0x38);

    // 0x138b580-0x138b788: three runs are taken from the free-range table at +0x2ab8. Each takes the first range that is
    // long enough: a longer range loses `size` entries from its start, an exact fit is removed (CMemory::Move closes the
    // gap), the total at +0x2af8 grows by `size`, and the old start is the result; ~0 when nothing fits or the table is
    // empty. The compiler inlined this three times.
    auto take_range = [self](std::uint64_t size) -> std::uint64_t {
        std::uint64_t count = Load<std::uint64_t>(self, kRangeCountOffset);
        std::byte* const ranges = LoadPtr<std::byte>(self, kRangeDataOffset);
        for (std::uint64_t index = 0; index < count; ++index) {
            std::byte* const range = ranges + index * 16;
            const std::uint64_t first = Load<std::uint64_t>(range, 0);
            const std::uint64_t length = Load<std::uint64_t>(range, 8) - first + 1;
            if (length < size) {
                continue;
            }
            if (length > size) {
                Store<std::uint64_t>(range, 0, first + size);
            } else if (index < count) {
                if (count - 1 != index) {
                    CMemory::Move(range, range + 16, count * 16 - index * 16 - 16);
                    count = Load<std::uint64_t>(self, kRangeCountOffset);
                }
                Store<std::uint64_t>(self, kRangeCountOffset, count - 1);
            }
            Store<std::uint64_t>(self, kRangeTotalOffset, Load<std::uint64_t>(self, kRangeTotalOffset) + size);
            return first;
        }
        return ~0ULL;
    };
    const std::uint64_t short_run = DAT_02c9d9e0;  // 0x5a on disk
    Store<std::uint64_t>(self, 0x63050, take_range(short_run));
    Store<std::uint64_t>(self, 0x63058, take_range(short_run << 4));
    Store<std::uint64_t>(self, 0x63060, take_range(short_run << 4));

    // 0x138b78c-0x138b7b4: the AFK message follows bit 0x20000000 of the flags word.
    const std::uint64_t final_flags = Load<std::uint64_t>(LoadPtr<std::byte>(self, kFlagsPointerOffset), 0);
    NWriteLog::WriteLog(2, nullptr,
                        (final_flags & 0x20000000) != 0 ? "[NETGAME] AFK detection enabled (by init)"
                                                        : "[NETGAME] AFK detection disabled (by init)");

    // 0x138b7b8-0x138b7c4: with bit 2 of the first flags byte clear, the spectator camera data is loaded.
    if ((Load<std::uint8_t>(LoadPtr<std::byte>(self, kFlagsPointerOffset), 0) & 4) == 0) {
        CR15NetSpectatorCameraCS::LoadCameraData();
    }
    InitializeHttpListener();                   // 0x138b7cc
    Store<std::uint16_t>(self, 0x26f8, 0);      // 0x138b7d4
    return 0;                                   // 0x138b7d0, then the epilogue at 0x138a738-0x138a754
}

}  // namespace NRadGame
}  // namespace NRadEngine
