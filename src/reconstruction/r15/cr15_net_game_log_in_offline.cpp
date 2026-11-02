// NRadEngine::NRadGame::CR15NetGame::LogInOffline
// Quest libr15.so VA 0x135ed54; ReVault binary 576.
// ReVault raw parent 100223547, reconstruction node 100600590; grade M: all 60 instructions checked to the ret at 0x135ee40, no placeholder remains, but the 0xff4b80 name CSysString::Length is disputed by login_failed.cpp and the LoadFromProjectSourceDb return type is taken from its caller.
// @0x135ed54 — NRadEngine::NRadGame::CR15NetGame::LogInOffline
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// CFixedStringT<512>: the 512-byte buffer the caller keeps on its stack (Ghidra: CFixedStringT aCStack_240[512]).
template <std::size_t N>
struct CFixedStringT {
    char text[N];
};
static_assert(sizeof(CFixedStringT<512>) == 0x200);

class CSysString {
public:
    // 0xff4b80: a PLT-form jump through GOT 0x37d1ca8, which no import names. One pointer argument;
    // the caller uses the result as an unsigned length.
    static std::size_t Length(const char* text);
    // 0x108188c (thunk 0xfd1370): the characters before the first NUL, looking at no more than size of them.
    static long Length(const char* buffer, unsigned long long size);
};

namespace CMemory {
// 0x12cb90c (thunk 0x1010a90): a tail jump to memmove; the caller's arguments pass through unchanged.
void Move(void* destination, const void* source, std::uint64_t count);
}  // namespace CMemory

namespace NWriteLog {
enum class ELogLevel : std::int32_t {};  // enumerators not recovered; LogInOffline passes 8
}  // namespace NWriteLog

// 0x12d202c (thunk 0xff6a00). Ghidra shows an undefined8 return; this function does not use it.
std::uint64_t OutputError(std::int32_t status, const char* message, NWriteLog::ELogLevel level);

class CJson {
public:
    // 0x10a82f8 (thunk 0x1004730): bool CJson::Empty() const.
    bool Empty() const;
    // 0x10a84e4 (thunk 0x1000140). ReVault's decompile header says void; the function returns the x0 of
    // its last call (0x102fe90), and LogInOffline passes that value on to OutputError.
    static std::int32_t LoadFromProjectSourceDb(CJson& json, const CFixedStringT<512>& path);
};

class CNSUser {
public:
    // 0x1a35740 (thunk 0xfffb80): NRadEngine::CNSUser::SetOffline(CJson const&). This is the CNSUser at +0x28d0.
    void SetOffline(const CJson& json);
};

namespace NRadGame {

class CR15NetGame {
public:
    // NRadEngine::NRadGame::CR15NetGame::LogInOffline(); this function (0x135ed54, stub 0x103f6e0).
    void LogInOffline();
    // 0x136cb20 (thunk 0xfedbc0): CR15NetGame::LogInSuccess().
    void LogInSuccess();
};

namespace {

// Offsets recovered from the disassembly; the rest of the object layout is not recovered, and this function writes none of it.
constexpr std::size_t kJsonOffset = 0x63250;   // add x20, x0, #0x63250: the CJson that the load and the Empty test use
constexpr std::size_t kNSUserOffset = 0x28d0;  // ldr x0, [x19, #0x28d0]: the CNSUser pointer that SetOffline is called on
constexpr char kDefaultProfilePath[] = "json|r14|defaultserverprofile.json";  // 0x2ca89a4, 34 characters
constexpr char kOfflineModeMessage[] = "entering offline mode";              // 0x2caa692, 21 characters

template <typename T>
T* LoadPtr(const std::byte* base, std::size_t offset) {
    T* value = nullptr;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

}  // namespace

void CR15NetGame::LogInOffline() {
    auto* const self = reinterpret_cast<std::byte*>(this);
    CJson& json = *reinterpret_cast<CJson*>(self + kJsonOffset);

    // 0x135ed7c-0x135ed80: when Empty() returns nonzero, load the default server profile.
    if (json.Empty()) {
        // The 512-byte buffer (sp+0x000 .. sp+0x1ff). It is zero-filled here; the original leaves it undefined,
        // but every byte is written below before any read.
        CFixedStringT<512> path{};
        constexpr std::size_t kPathBytes = 0x200;

        // 0x135ed90-0x135ed9c: length of the literal, copied = length < 0x200 ? length : 0x200.
        const std::size_t path_length = CSysString::Length(kDefaultProfilePath);
        const std::size_t copied = path_length < kPathBytes ? path_length : kPathBytes;
        // 0x135edb4: CMemory::Move(path, literal, copied).
        CMemory::Move(path.text, kDefaultProfilePath, copied);
        // 0x135edb8-0x135edc0: the NUL after the copied bytes, when the literal was shorter than 0x200.
        if (path_length < kPathBytes) {
            path.text[copied] = '\0';
        }
        // 0x135edc4-0x135edcc: the last byte is always NUL.
        path.text[kPathBytes - 1] = '\0';
        // 0x135edd4: bounded length of the buffer.
        const long text_length = CSysString::Length(path.text, kPathBytes);
        // 0x135edd8-0x135ede0: zero the rest of the buffer when text_length + 1 < 0x200.
        if (static_cast<unsigned long long>(text_length) + 1ULL < kPathBytes) {
            std::memset(path.text + text_length + 1, 0, static_cast<std::size_t>(0x1ff - text_length));
        }

        // 0x135ee00-0x135ee04: LoadFromProjectSourceDb(json, path); its result is the status passed on below.
        const std::int32_t load_status = CJson::LoadFromProjectSourceDb(json, path);
        // 0x135ee08-0x135ee14: OutputError(status, "entering offline mode", 8).
        OutputError(load_status, kOfflineModeMessage, static_cast<NWriteLog::ELogLevel>(8));
    }

    // 0x135ee18-0x135ee20: SetOffline on the CNSUser at +0x28d0, with the offline CJson in x1 (mov x1, x20 at 0x135ee1c).
    // Ghidra's caller decompile shows one argument; the disassembly passes two, and the demangled signature takes CJson const&.
    // There is no null test on the CNSUser pointer in the disassembly.
    CNSUser* const profile = LoadPtr<CNSUser>(self, kNSUserOffset);
    profile->SetOffline(json);
    // 0x135ee24-0x135ee28: LogInSuccess() on this.
    LogInSuccess();
}

}  // namespace NRadGame
}  // namespace NRadEngine
