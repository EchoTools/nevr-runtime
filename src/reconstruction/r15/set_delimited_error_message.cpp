// NRadEngine::NRadGame::CR15NetGame::SetDelimitedErrorMessage
// Quest libr15.so VA 0x135f768; ReVault binary 576.
// ReVault raw parent 100223564, reconstruction node 100600502; grade H: every branch checked against the raw bytes 0x135f768-0x135f904 (the stored size 68 stops at 0x135f7ac: SIZE-OVERRUN), with the jump table, both strings and the empty string verified.
// @0x135f768 — NRadEngine::NRadGame::CR15NetGame::SetDelimitedErrorMessage
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// NRadEngine::NWriteLog::WriteLog(ELogLevel, unsigned long long, char const*, ...) @0x01088c20 (thunk 0x0104b960).
// The ELogLevel is passed as an int; the enumeration is not recovered.
namespace NWriteLog {
void WriteLog(std::int32_t level, std::uint64_t target, const char* format, ...);
}  // namespace NWriteLog

// 0x38 bytes: +0x00 block pointer, +0x08 size, +0x10 allocator, +0x18 alignment, +0x1c flags (see cmem_block_ctor.cpp).
class CMemBlock {
public:
    // NRadEngine::CMemBlock::Detach() @0x012dc2f8 (thunk 0x00fe2470).
    void Detach();
    // NRadEngine::CMemBlock::~CMemBlock() @0x012dbd40 (thunk 0x0100e200).
    ~CMemBlock();

    alignas(8) std::byte raw[0x38];
};
static_assert(sizeof(CMemBlock) == 0x38);

class CStringTable {
public:
    // NRadEngine::CStringTable::CStringTable(char const*, char) @0x012e3348 (thunk 0x01024ad0). Builds the table in place.
    CStringTable(const char* text, char separator);
    // NRadEngine::CStringTable::GetEntry(unsigned long long) @0x012e3c94 (thunk 0x00fef9e0). Returns the entry text.
    const char* GetEntry(std::uint64_t index);

    // Two CMemBlocks: the line-offset block at +0x00 (its line count u64 at +0x30) and the text block at +0x38.
    alignas(8) std::byte raw[0x70];
};
static_assert(sizeof(CStringTable) == 0x70);

namespace NRadGame {

class CR15NetGame {
public:
    void SetDelimitedErrorMessage(const char* message);
    // NRadEngine::NRadGame::CR15NetGame::SetErrorMessage(char const*) @0x01341050 (thunk 0x010461b0).
    void SetErrorMessage(const char* line0);
    // NRadEngine::NRadGame::CR15NetGame::SetErrorMessage(char const*, char const*) @0x0135ce34 (thunk 0x0101c570).
    void SetErrorMessage(const char* line0, const char* line1);
    // NRadEngine::NRadGame::CR15NetGame::SetErrorMessage(char const*, char const*, char const*, char const*)
    // @0x01341430 (thunk 0x010473d0).
    void SetErrorMessage(const char* line0, const char* line1, const char* line2, const char* line3);
};

namespace {

// The CStringTable is 0x70 bytes: the lines CMemBlock at +0x00 (its flags at +0x1c, its line count u64 at +0x30)
// and the text CMemBlock at +0x38 (its flags at +0x1c of that object, so +0x54).
constexpr std::size_t kBlockOffset = 0x38;
constexpr std::size_t kFlagsOffset = 0x1c;
constexpr std::size_t kLineCountOffset = 0x30;
// CMemBlock flag bits (bits 1 and 2) that make the block detach before it is destroyed.
constexpr std::uint8_t kDetachMask = 0x6;

// Strings in libr15.so rodata: 0x2caa999 (36 characters) and 0x2caa9be (54 characters).
constexpr char kUnknownErrorText[] = "Log in request failed: Unknown error";
constexpr char kTooManyLinesText[] = "[NETGAME] Log in failed error message exceeded 4 lines";

std::uint8_t LoadU8(const std::byte* base, std::size_t offset) {
    std::uint8_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

std::uint64_t LoadU64(const std::byte* base, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

}  // namespace

void CR15NetGame::SetDelimitedErrorMessage(const char* message) {
    // 0x135f76c-0x135f784: the CStringTable is built at the frame base from the message, split on '\n'.
    CStringTable table(message, '\n');

    // 0x135f788-0x135f7a8: the line count selects a case through a byte table at 0x2ca3ebc; a count above 4 takes default.
    switch (LoadU64(table.raw, kLineCountOffset)) {
        case 0:
            // 0x135f7ac-0x135f7b4: the one-line overload with the text at 0x2caa999.
            SetErrorMessage(kUnknownErrorText);
            break;
        case 1: {
            // 0x135f820-0x135f838: line 0 through the one-line overload.
            SetErrorMessage(table.GetEntry(0));
            break;
        }
        case 2: {
            // 0x135f83c-0x135f868: lines 0 and 1 through the two-line overload.
            const char* const line0 = table.GetEntry(0);
            const char* const line1 = table.GetEntry(1);
            SetErrorMessage(line0, line1);
            break;
        }
        case 3: {
            // 0x135f86c-0x135f8b0: lines 0 to 2, and the empty string at 0x2c419ad as the fourth argument.
            const char* const line0 = table.GetEntry(0);
            const char* const line1 = table.GetEntry(1);
            const char* const line2 = table.GetEntry(2);
            SetErrorMessage(line0, line1, line2, "");
            break;
        }
        default:
            // 0x135f7b8-0x135f7c8: the overflow warning, then the four-line case.
            NWriteLog::WriteLog(4, 0, kTooManyLinesText);
            [[fallthrough]];
        case 4: {
            // 0x135f7cc-0x135f81c: lines 0 to 3 through the four-line overload.
            const char* const line0 = table.GetEntry(0);
            const char* const line1 = table.GetEntry(1);
            const char* const line2 = table.GetEntry(2);
            const char* const line3 = table.GetEntry(3);
            SetErrorMessage(line0, line1, line2, line3);
            break;
        }
    }

    // 0x135f8b4-0x135f8f0: the text CMemBlock at +0x38 is detached when bits 1-2 of its flags are set and then destroyed;
    // the lines CMemBlock at +0x00 gets the same treatment, after the block.
    CMemBlock* const text = reinterpret_cast<CMemBlock*>(table.raw + kBlockOffset);
    if ((LoadU8(table.raw, kBlockOffset + kFlagsOffset) & kDetachMask) != 0) {
        text->Detach();
    }
    text->~CMemBlock();
    CMemBlock* const lines = reinterpret_cast<CMemBlock*>(table.raw);
    if ((LoadU8(table.raw, kFlagsOffset) & kDetachMask) != 0) {
        lines->Detach();
    }
    lines->~CMemBlock();
}

}  // namespace NRadGame
}  // namespace NRadEngine
