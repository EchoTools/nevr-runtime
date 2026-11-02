// NRadEngine::NRadGame::CR15NetGame::SetErrorMessage
// Quest libr15.so VA 0x1341430; ReVault binary 576.
// ReVault raw parent 100223043, reconstruction node 100600718; grade M: all 282 instructions read to the ret at 0x1341894 and checked; 0 placeholders: the local is a NRadEngine::NRadGame::CR15NetJsonLog (vtable 0x3731918, slot 1 WriteLog @0x136a304, chain CNSJsonLog, CJsonLog, CTempJsonString) declared as in cr15_net_client_lobby_switch_to.cpp, and the 0x1341858 call is its virtual WriteLog(2); 0xff4b80 keeps the name CSysString::Length, which login_failed.cpp disputes; compiled with the mingw check.
// @0x1341430 — NRadEngine::NRadGame::CR15NetGame::SetErrorMessage
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// NRadEngine::NWriteLog::WriteLog(ELogLevel, unsigned long long, char const*, ...) @0x01088c20 (thunk 0x0104b960).
namespace NWriteLog {
enum ELogLevel : int;
void WriteLog(ELogLevel level, unsigned long long channel, const char* format, ...);
}  // namespace NWriteLog

// NRadEngine::CMemory::Fill @0x12cafcc (thunk 0x1047300); NRadEngine::CMemory::Move @0x12cb90c (thunk 0x1010a90).
namespace CMemory {
void Fill(void* destination, unsigned char value, std::uint64_t count);
void Move(void* destination, const void* source, std::uint64_t count);
}  // namespace CMemory

class CSysString {
public:
    // 0xff4b80: a PLT-form jump through GOT 0x37d1ca8, which no import names. One pointer argument; the caller uses the
    // result as an unsigned length. login_failed.cpp disputes this name.
    static std::size_t Length(const char* text);
};

// NRadEngine::CStackAllocator::DirectFree @0x12df3fc (thunk 0x1003d40).
struct CStackAllocator {
    void DirectFree();
};

// STempString<N>: the temporary that MakeTStrPrintF returns in the caller's frame.
template <std::size_t N>
struct STempString {
    unsigned char storage[N];
};

// MakeTStrPrintF<N>(char const*, ...) @0x1086b9c (thunk 0x102b370).
template <std::size_t N>
STempString<N> MakeTStrPrintF(const char* format, ...);

// NRadEngine::CTempJsonString: 0x40 bytes, vtable_va 0x36fb2b8. +0x00 is the vtable pointer, +0x18 the CStackAllocator to
// free (zero until a call stores one), +0x2c a u32 that the construction sets to 1. Everything else is zero.
class CTempJsonString {
public:
    enum class ERootType : std::int32_t {};  // enumerators not recovered; 0 is passed at both calls here

    // The default construction and destruction are inlined at 0x1341718..0x134174c and 0x134185c..0x1341874: zero the
    // fields and store 1 at +0x2c; at the end the vtable pointer becomes the CTempJsonString one (0x36fb2b8) and the
    // allocator at +0x18 is freed when one is stored.
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

    // Vtable slot 0 Finalize @0x10abba8 and slot 1 WriteLog @0x10ad3f8.
    virtual char* Finalize(std::uint32_t flags);
    virtual void WriteLog(NWriteLog::ELogLevel level);

    // NRadEngine::CTempJsonString::EnsureRoot(ERootType) @0x12e5258 (thunk 0x10321f0).
    void EnsureRoot(ERootType root);
    // NRadEngine::CTempJsonString::NextElement(unsigned long long) @0x12e54d4 (thunk 0xfe0660): the length reserved.
    void NextElement(std::uint64_t length);
    // NRadEngine::CTempJsonString::SAppendF(char const*, ...) @0x12e53f4 (thunk 0xfe5000).
    void SAppendF(const char* format, ...);
    // NRadEngine::CTempJsonString::EscapeAppend(char const*, unsigned long long) @0x12e55b0 (thunk 0x104af70).
    void EscapeAppend(const char* text, std::uint64_t length);
    // NRadEngine::CTempJsonString::Append(char) @0x12e52d4 (thunk 0xffc120).
    void Append(char c);

private:
    alignas(8) std::byte fields_[0x38];
};
static_assert(sizeof(CTempJsonString) == 0x40);

// NRadEngine::CJsonLog (vtable_va 0x36fb2f8) adds nothing to the layout.
class CJsonLog : public CTempJsonString {};

// NRadEngine::CNSJsonLog (vtable_va 0x37322e8) adds nothing this function uses.
class CNSJsonLog : public CJsonLog {};

namespace {

// The error block at CR15NetGame + kErrorBlockOffset: one flag byte, then four lines of kErrorLineBytes each.
constexpr std::size_t kErrorBlockOffset = 0x63308;
constexpr std::size_t kErrorLineBytes = 0x40;

// One line of the error block, written as the disassembly writes it: NULL fills it with zeros; the line's own
// address is left alone; otherwise the first min(length, 0x40) bytes are copied, a NUL follows a short text, the
// line's last byte is forced to NUL, and the bytes after the text are zeroed.
void StoreErrorLine(std::byte* line, const char* text) {
    if (text == nullptr) {
        CMemory::Fill(line, 0, kErrorLineBytes);
        return;
    }
    if (reinterpret_cast<const char*>(line) == text) {
        return;
    }
    const std::uint64_t length = CSysString::Length(text);
    const std::uint64_t copied = length < kErrorLineBytes ? length : kErrorLineBytes;
    CMemory::Move(line, text, copied);
    if (length < kErrorLineBytes) {
        line[copied] = std::byte{0};
    }
    line[kErrorLineBytes - 1] = std::byte{0};
    if (length + 1 < kErrorLineBytes) {
        std::memset(line + length + 1, 0, kErrorLineBytes - 1 - length);
    }
}

}  // namespace

namespace NRadGame {

// NRadEngine::NRadGame::CR15NetJsonLog: vtable_va 0x3731918; slot 0 Finalize @0x10abba8, slot 1 WriteLog @0x136a304.
class CR15NetJsonLog : public CNSJsonLog {
public:
    void WriteLog(NWriteLog::ELogLevel level) override;
};
static_assert(sizeof(CR15NetJsonLog) == 0x40);

class CR15NetGame {
public:
    // NRadEngine::NRadGame::CR15NetGame::SetErrorMessage(char const*, char const*, char const*, char const*)
    // @0x01341430 (thunk 0x010473d0).
    void SetErrorMessage(const char* line0, const char* line1, const char* line2, const char* line3);
};

}  // namespace NRadGame

void NRadGame::CR15NetGame::SetErrorMessage(const char* line0, const char* line1, const char* line2,
                                            const char* line3) {
    std::byte* const block = reinterpret_cast<std::byte*>(this) + kErrorBlockOffset;
    // 0x1341474: the flag byte at block+0 is set first, unconditionally.
    block[0] = std::byte{1};
    // The four lines: block+0x01 (0x63309), +0x41 (0x63349), +0x81 (0x63389), +0xc1 (0x633c9).
    StoreErrorLine(block + 0x01, line0);
    StoreErrorLine(block + 0x41, line1);
    StoreErrorLine(block + 0x81, line2);
    StoreErrorLine(block + 0xc1, line3);

    // 0x1341700..0x1341714: the log line at level 8, channel 0.
    NWriteLog::WriteLog(NWriteLog::ELogLevel(8), 0, "[NETGAME] %s %s %s %s", line0, line1, line2, line3);

    // 0x1341718..0x134174c: the object is constructed in place (zeroed, u32 at +0x2c is 1, vtable pointer stored), then
    // 0x1341750 calls EnsureRoot(0).
    CR15NetJsonLog json;
    json.EnsureRoot(CTempJsonString::ERootType(0));

    // 0x1341754..0x1341794: the first element, with the message name and its value.
    const std::uint64_t length_message = CSysString::Length("message");
    const std::uint64_t length_r15 = CSysString::Length("r15 net game error message");
    json.NextElement(length_message + length_r15 + 5);
    json.SAppendF("\"%s\":\"", "message");
    json.EscapeAppend("r15 net game error message", length_r15);
    json.Append('"');

    // 0x13417c0..0x13417dc: the formatted text, returned in the caller's frame.
    const STempString<512> message = MakeTStrPrintF<512>("%s %s %s %s", line0, line1, line2, line3);
    const char* const text = reinterpret_cast<const char*>(message.storage);

    // 0x13417e0..0x1341844: the second element, with the error_message name and the formatted text.
    json.EnsureRoot(CTempJsonString::ERootType(0));
    const std::uint64_t length_error_message = CSysString::Length("error_message");
    const std::uint64_t length_text = CSysString::Length(text);
    json.NextElement(length_error_message + length_text + 5);
    json.SAppendF("\"%s\":\"", "error_message");
    json.EscapeAppend(text, length_text);
    json.Append('"');

    // 0x1341848..0x1341858: vtable slot 1 (+0x8), WriteLog @0x136a304, called with (object, 2).
    json.WriteLog(NWriteLog::ELogLevel(2));
    // 0x134185c..0x1341874: the destructor of json runs here (vtable pointer becomes CTempJsonString's, the allocator at
    // +0x18 is freed when one was stored).
}

}  // namespace NRadEngine
