// CR15NetGame::LogInFailedCB
// Quest libr15.so VA 0x135f298; ReVault binary 576.
// ReVault raw parent 100223552, reconstruction node 100600459; grade H: every branch and both CopyText64 length cases checked against the disassembly; 10 callees stay FUN_ names; the 0xff4b80 name is a suspected ReVault defect.
// @0x135f298 — CR15NetGame::LogInFailedCB
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

class CJson;
class CNSUser;

namespace NRadGame {

class CR15NetGame {
public:
    // Symbol string in libr15.so (0x4c5ac2): _ZN10NRadEngine8NRadGame11CR15NetGame13LogInFailedCBERNS_7CNSUserENS_15ENSResponseCodeEPKcRKNS_5CJsonE
    // Entry registers: x0 this, x1 CNSUser& (never read), w2 response, x3 message, x4 const CJson&.
    void LogInFailedCB(CNSUser& user, int response, const char* message, const CJson& json);
    // NRadEngine::NRadGame::CR15NetGame::LogInOffline(); @0x0135ed54, stub 0x0103f6e0.
    void LogInOffline();
    // NRadEngine::NRadGame::CR15NetGame::SwitchTo(EState); @0x0135b8b4, stub 0x00fe6330.
    void SwitchTo(std::int32_t state);
};

// UNRESOLVED callees: ReVault names none of these. Each keeps its FUN_ name; the VA is the call target.
/* UNRESOLVED: FUN_0135f634 */
void FUN_0135f634(std::byte* reason_table, std::size_t count, std::uint64_t zero);  // 0x135f634, thunk 0x1022600; called as (this + 0x64718, n, 0)
/* UNRESOLVED: FUN_0135f768 */
void FUN_0135f768(CR15NetGame* self, const char* message);  // 0x135f768, thunk 0x1023510; called as (this, message)
/* UNRESOLVED: FUN_010a3dc8 */
bool FUN_010a3dc8(const CJson* json, const char* path);  // 0x10a3dc8, thunk 0x1035610; called as (json, "incident_categories"); the 32-bit result is tested with cbz
/* UNRESOLVED: FUN_010a0d4c */
std::size_t FUN_010a0d4c(const CJson* json, const char* path);  // 0x10a0d4c, thunk 0x1015010; called as (json, "incident_categories"); its result is n
/* UNRESOLVED: FUN_010a2e7c */
const char* FUN_010a2e7c(const CJson* json, const std::byte* key, const char* fallback, int flag);  // 0x10a2e7c, thunk 0x1028e40; text lookup, flag 0
/* UNRESOLVED: FUN_010a40ec */
std::int64_t FUN_010a40ec(const CJson* json, const std::byte* key, std::int64_t fallback, int flag);  // 0x10a40ec, thunk 0x10152d0; integer lookup, fallback 0, flag 0
/* UNRESOLVED: FUN_01086b9c */
void FUN_01086b9c(std::byte* result, const char* fmt, std::uint64_t index);  // 0x1086b9c, thunk 0x102b370; result is returned through x8, fmt in x0, index in x1
/* UNRESOLVED: FUN_00ff4b80 */
std::size_t FUN_00ff4b80(const char* text);  // 0xff4b80; its result is used as the length of text. ReVault names this stub PEM_read_bio_SSL_SESSION, but no import has that name and the strlen import is at thunk 0xfd1970
/* UNRESOLVED: FUN_012cb90c */
void FUN_012cb90c(std::byte* dst, const char* src, std::size_t count);  // 0x12cb90c, thunk 0x1010a90; copies count bytes from src (not the memmove or memcpy import thunks)
/* UNRESOLVED: FUN_012cafcc */
void FUN_012cafcc(std::byte* dst, int value, std::size_t count);  // 0x12cafcc, thunk 0x1047300; called as (dst, 0, 0x40)

namespace {

// Offsets recovered from the disassembly; the rest of the object layout is not recovered.
constexpr std::size_t kFlagsPointerOffset = 0x2da0;  // ldr x8, [x0, #0x2da0]; ldr x8, [x8] reads the uint64 flags
constexpr std::size_t kReasonTableOffset = 0x64718;  // this + 0x64718 is passed to FUN_0135f634; *(this + 0x64718) is the row array
constexpr std::size_t kReasonStride = 0xc8;          // add x20, x20, #0xc8 per row
constexpr std::size_t kIdOffset = 0x00;              // 64-byte text
constexpr std::size_t kNameOffset = 0x40;            // 64-byte text
constexpr std::size_t kMessagingOffset = 0x80;       // 64-byte text
constexpr std::size_t kPointsOffset = 0xc0;          // int64

// Strings in libr15.so rodata: 0x2ca6b30, 0x2ca6b44, 0x2ca6b61, 0x2ca6b80, 0x2ca6ba4.
constexpr char kIncidentCategories[] = "incident_categories";
constexpr char kIdFormat[] = "incident_categories[%llu]|id";
constexpr char kNameFormat[] = "incident_categories[%llu]|name";
constexpr char kMessagingFormat[] = "incident_categories[%llu]|messaging";
constexpr char kPointsFormat[] = "incident_categories[%llu]|points";

std::int32_t LoadI32(const std::byte* base, std::size_t offset) {
    std::int32_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

std::uint64_t LoadU64(const std::byte* base, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

std::byte* LoadPtr(const std::byte* base, std::size_t offset) {
    std::byte* value = nullptr;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

void StoreI64(std::byte* dst, std::int64_t value) {
    std::memcpy(dst, &value, sizeof value);
}

// Field `field` of row `index` of the row array that *(self + kReasonTableOffset) points at.
std::byte* RowField(std::byte* self, std::size_t index, std::size_t field) {
    return LoadPtr(self, kReasonTableOffset) + index * kReasonStride + field;
}

// Text copy into a 64-byte field (the id, name and messaging fields of a row).
void CopyText64(std::byte* dst, const char* src) {
    if (src == nullptr) {  // cbz x0 -> 0x135f474: zero the 64-byte field
        FUN_012cafcc(dst, 0, 0x40);
        return;
    }
    const auto* const src_bytes = reinterpret_cast<const std::byte*>(src);
    if (dst == src_bytes) {  // cmp dst, src; b.eq 0x135f484: text already in place, nothing is copied
        return;
    }
    const std::size_t length = FUN_00ff4b80(src);
    const std::size_t copied = length < 0x40 ? length : 0x40;  // csel x29, x0, x8, cc
    FUN_012cb90c(dst, src, copied);
    if (length < 0x40) {  // cmp length, #0x3f; b.hi 0x135f440
        dst[length] = std::byte{0};
    }
    dst[0x3f] = std::byte{0};  // 0x135f44c, unconditional
    if (length + 1 < 0x40) {  // cmp length+1, #0x3f; b.hi 0x135f484
        std::memset(dst + length + 1, 0, 0x3f - length);  // import memset, thunk 0xfe71f0
    }
}

}  // namespace

void CR15NetGame::LogInFailedCB(CNSUser& user, int response, const char* message, const CJson& json) {
    static_cast<void>(user);  // x1 is overwritten at 0x135f308 before any read
    auto* const self = reinterpret_cast<std::byte*>(this);

    // 0x135f2b4-0x135f2cc: if ((flags & 0x44) == 0x40) return LogInOffline();
    // The branch at 0x135f2f0 is a tail jump, so LogInOffline's return is this function's return.
    const std::uint64_t flags = LoadU64(LoadPtr(self, kFlagsPointerOffset), 0);
    if ((flags & 0x44) == 0x40) {
        LogInOffline();
        return;
    }

    // 0x135f2f4-0x135f2f8: if (*(int32_t*)this < 0) return;
    if (LoadI32(self, 0) < 0) {
        return;
    }

    // 0x135f2fc-0x135f31c: the list is read only for response 0x193, and only when the key is present.
    if (response == 0x193 && FUN_010a3dc8(&json, kIncidentCategories)) {
        // 0x135f320-0x135f34c: n = count of the list; the table at this + kReasonTableOffset is reset to n rows.
        const std::size_t n = FUN_010a0d4c(&json, kIncidentCategories);
        FUN_0135f634(self + kReasonTableOffset, n, 0);

        // FUN_01086b9c writes each key through x8 into the frame at sp+0x20 (the frame reserves up to sp+0x220); its object size is not recovered.
        alignas(8) std::byte key_text[0x200];

        // 0x135f354: n == 0 skips the loop; row i starts at the row array + i * 0xc8.
        for (std::size_t i = 0; i < n; ++i) {
            // Per row, in this order: id, name, messaging (64-byte texts), then points (int64).
            FUN_01086b9c(key_text, kIdFormat, i);
            // The fallback is the empty string at 0x2c419ad (a NUL byte is read there).
            const char* const id = FUN_010a2e7c(&json, key_text, "", 0);
            CopyText64(RowField(self, i, kIdOffset), id);

            FUN_01086b9c(key_text, kNameFormat, i);
            const char* const name = FUN_010a2e7c(&json, key_text, "", 0);
            CopyText64(RowField(self, i, kNameOffset), name);

            FUN_01086b9c(key_text, kMessagingFormat, i);
            const char* const messaging = FUN_010a2e7c(&json, key_text, "", 0);
            CopyText64(RowField(self, i, kMessagingOffset), messaging);

            FUN_01086b9c(key_text, kPointsFormat, i);
            const std::int64_t points = FUN_010a40ec(&json, key_text, 0, 0);
            StoreI64(RowField(self, i, kPointsOffset), points);
        }
    }

    // 0x135f5fc-0x135f614: FUN_0135f768(this, message); SwitchTo(this, -94);
    FUN_0135f768(this, message);
    SwitchTo(-94);
}

}  // namespace NRadGame
}  // namespace NRadEngine
