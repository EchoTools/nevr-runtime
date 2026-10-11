#include "runtime/patch/evrp_package.h"

#include <cstring>

#include "core/logging.h"

namespace nevr_evrp {

namespace {

// Wire layout; the 28-byte size is asserted below.
#pragma pack(push, 1)
struct EvrpHeader {
    uint32_t magic;
    uint32_t format_version;
    int64_t  symbol_id;
    uint8_t  slot_type;
    uint8_t  reserved[7];
    uint32_t data_length;
};
#pragma pack(pop)
static_assert(sizeof(EvrpHeader) == kHeaderSize, "EvrpHeader must be 28 bytes");

}  // namespace

bool ParseTint(const std::vector<uint8_t>& data, const std::string& context,
               int64_t& out_symbol_id, TintData& out_tint) {
    if (data.size() < kHeaderSize) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.CDN] .evrp package skipped — file too small: file=%s size=%zu min=%zu",
            context.c_str(), data.size(), kHeaderSize);
        return false;
    }

    EvrpHeader header;
    memcpy(&header, data.data(), sizeof(header));

    // Validate magic
    if (header.magic != kMagic) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.CDN] .evrp package skipped — bad magic: file=%s magic=0x%08X want=0x%08X('EVRP')",
            context.c_str(), header.magic, kMagic);
        return false;
    }

    // Validate format version
    if (header.format_version != kFormatVersion) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.CDN] .evrp package skipped — unsupported format version: file=%s got=%u want=%u",
            context.c_str(), header.format_version, kFormatVersion);
        return false;
    }

    // Validate slot type
    if (header.slot_type != kSlotTypeTint) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.CDN] .evrp package skipped — unknown slot type: file=%s got=0x%02X want=0x%02X(tint)",
            context.c_str(), header.slot_type, kSlotTypeTint);
        return false;
    }

    // Validate reserved bytes are zero
    for (int i = 0; i < 7; i++) {
        if (header.reserved[i] != 0) {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.CDN] .evrp package skipped — reserved byte nonzero: file=%s index=%d value=0x%02X",
                context.c_str(), i, header.reserved[i]);
            return false;
        }
    }

    // Validate data length for tint
    if (header.data_length != kTintDataLength) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.CDN] .evrp package skipped — data_length mismatch: file=%s got=%u want=%u",
            context.c_str(), header.data_length, kTintDataLength);
        return false;
    }

    // Validate total file size
    if (data.size() != kHeaderSize + header.data_length) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.CDN] .evrp package skipped — size mismatch: file=%s got=%zu want=%zu",
            context.c_str(), data.size(), static_cast<size_t>(kHeaderSize + header.data_length));
        return false;
    }

    out_symbol_id = header.symbol_id;
    memcpy(out_tint.colors, data.data() + kHeaderSize, kTintDataLength);
    return true;
}

}  // namespace nevr_evrp
