// FUN_018db678
// Quest libr15.so VA 0x19db678; ReVault binary 576.
// ReVault raw parent 100259910, reconstruction node 100600771; grade M: all 104 instructions of 0x19db678..0x19db814 (through the ret) are matched to the block map and code below; M because 3 globals stay DAT_ placeholders (0x2d65a5c 0x38754c0 0x38759a0), the function itself is unnamed in symbols and SGTex3DInit is read by raw offset; 0 callee placeholders (WriteLog is named).
// @0x19db678 — FUN_018db678
/* @confidence: M */

#include <cstddef>
#include <cstdint>

// Overview. The function turns a 3D texture init record (NRadEngine::SGTex3DInit, x1) into the 88-byte
// image create-info record (x0) that CreateTexture3D (0x1941f58) and MeasureTexture3D (0x19b2ddc) pass on,
// and that CGEngineResources::Initialize (0x193e52c) builds for its two default 3D textures.
//   1. the init's first word indexes a format table (0x2d65a5c); six values of the result are replaced
//      (the jump table at 0x2d575cb and the compares at 0x19db6d0);
//   2. the image usage is 4, or 0x17 when init+0x14 is set, | 8 when init+0x18 is set, | 2 when the data
//      pointer at init+0x20 is set;
//   3. the graphics function table entry +0x38 (0x38754c0) is called with the physical device (table +0x518,
//      0x38759a0), the format, image type 2, tiling 0, the usage and flags 0; the log text of the
//      failure paths names it vkGetPhysicalDeviceImageFormatProperties;
//   4. on VK_SUCCESS the record is filled; on any other result the cause is logged and the record is zeroed.
// The name: this is the 3D image create-info builder, unnamed in symbols. ReVault stores the function as
// FUN_018db678 (its names carry the base-0 address), the decompiler and the merged callers call it
// FUN_019db678, and cg_engine_resources_initialize.cpp declares it as FUN_019db678(void*, const void*) at
// global scope, which is the name and signature defined here; the body casts the two pointers to the local
// VkImageCreateInfo and SGTex3DInit types.
// Block map (instruction counts add up to the 104 of the listing):
//   0x19db678-0x19db684  4  prologue (x19..x22, x30 saved, 0x50-byte frame)
//   0x19db688-0x19db6c4 16  format table lookup, range check, byte-table jump (w21 preset to 0x4a)
//   0x19db6c8-0x19db6cc  2  case 0x3a   -> 0x40
//   0x19db6d0-0x19db6ec  8  outside the table range: 0x10 -> 0x25, 0x7a -> 0x6d, else the table value
//   0x19db6f0-0x19db6fc  4  case 0x5b -> 0x5f, case 0x5c -> 0x61
//   0x19db700            1  0x10 -> 0x25
//   0x19db704-0x19db75c 23  usage, function table loads, the call (blr x11)
//   0x19db760-0x19db794 14  non-zero result: -11 logs the unsupported format, then joins the zeroing
//   0x19db798-0x19db7cc 14  result 0: fill the create-info record
//   0x19db7d0-0x19db7e0  5  other result: log it
//   0x19db7e4-0x19db800  8  zero the record
//   0x19db804-0x19db814  5  epilogue, ret

// Vulkan API types, declared locally because this build has no Vulkan headers. The layouts are the
// Vulkan specification's; the evidence that the 0x58-byte record is a VkImageCreateInfo is in the offsets
// asserted below (every store of the listing lands on the field of that name), the structure type 14
// written at +0 (VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO) and CreateTexture3D's 88-byte buffer.
using VkResult = std::int32_t;
using VkFormat = std::int32_t;
using VkImageType = std::int32_t;
using VkImageTiling = std::int32_t;
using VkStructureType = std::int32_t;
using VkSampleCountFlagBits = std::int32_t;
using VkSharingMode = std::int32_t;
using VkImageLayout = std::int32_t;
using VkImageCreateFlags = std::uint32_t;
using VkImageUsageFlags = std::uint32_t;
struct VkPhysicalDevice_T;
using VkPhysicalDevice = VkPhysicalDevice_T*;

constexpr VkResult VK_SUCCESS = 0;
constexpr VkResult VK_ERROR_FORMAT_NOT_SUPPORTED = -11;
constexpr VkStructureType VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO = 14;
constexpr VkImageType VK_IMAGE_TYPE_3D = 2;
constexpr VkImageTiling VK_IMAGE_TILING_OPTIMAL = 0;
constexpr VkSampleCountFlagBits VK_SAMPLE_COUNT_1_BIT = 1;
constexpr VkSharingMode VK_SHARING_MODE_EXCLUSIVE = 0;
constexpr VkImageLayout VK_IMAGE_LAYOUT_UNDEFINED = 0;
constexpr VkImageUsageFlags VK_IMAGE_USAGE_TRANSFER_SRC_BIT = 0x1;
constexpr VkImageUsageFlags VK_IMAGE_USAGE_TRANSFER_DST_BIT = 0x2;
constexpr VkImageUsageFlags VK_IMAGE_USAGE_SAMPLED_BIT = 0x4;
constexpr VkImageUsageFlags VK_IMAGE_USAGE_STORAGE_BIT = 0x8;
constexpr VkImageUsageFlags VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT = 0x10;

struct VkExtent3D {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t depth;
};

struct VkImageCreateInfo {
    VkStructureType sType;                     // +0x00
    const void* pNext;                         // +0x08
    VkImageCreateFlags flags;                  // +0x10
    VkImageType imageType;                     // +0x14
    VkFormat format;                           // +0x18
    VkExtent3D extent;                         // +0x1c
    std::uint32_t mipLevels;                   // +0x28
    std::uint32_t arrayLayers;                 // +0x2c
    VkSampleCountFlagBits samples;             // +0x30
    VkImageTiling tiling;                      // +0x34
    VkImageUsageFlags usage;                   // +0x38
    VkSharingMode sharingMode;                 // +0x3c
    std::uint32_t queueFamilyIndexCount;       // +0x40
    const std::uint32_t* pQueueFamilyIndices;  // +0x48
    VkImageLayout initialLayout;               // +0x50
};
static_assert(sizeof(VkImageCreateInfo) == 0x58, "CreateTexture3D's record buffer is 88 bytes");
static_assert(offsetof(VkImageCreateInfo, sType) == 0x00, "str w8 (14), [x19]");
static_assert(offsetof(VkImageCreateInfo, pNext) == 0x08, "stp xzr, x9, [x19, #8]");
static_assert(offsetof(VkImageCreateInfo, flags) == 0x10, "low half of the 0x200000000 store at [x19, #0x10]");
static_assert(offsetof(VkImageCreateInfo, imageType) == 0x14, "high half of the 0x200000000 store");
static_assert(offsetof(VkImageCreateInfo, format) == 0x18, "str w21, [x19, #0x18]");
static_assert(offsetof(VkImageCreateInfo, extent) == 0x1c, "stur q0, [x19, #0x1c]");
static_assert(offsetof(VkImageCreateInfo, mipLevels) == 0x28, "last word of the q0 store");
static_assert(offsetof(VkImageCreateInfo, arrayLayers) == 0x2c, "low half of the 0x100000001 store at [x19, #0x2c]");
static_assert(offsetof(VkImageCreateInfo, samples) == 0x30, "high half of the 0x100000001 store");
static_assert(offsetof(VkImageCreateInfo, tiling) == 0x34, "stp wzr, w22, [x19, #0x34]");
static_assert(offsetof(VkImageCreateInfo, usage) == 0x38, "stp wzr, w22, [x19, #0x34]");
static_assert(offsetof(VkImageCreateInfo, sharingMode) == 0x3c, "stur xzr, [x19, #0x3c]");
static_assert(offsetof(VkImageCreateInfo, queueFamilyIndexCount) == 0x40, "stur xzr, [x19, #0x3c]");
static_assert(offsetof(VkImageCreateInfo, pQueueFamilyIndices) == 0x48, "str xzr, [x19, #0x48]");
static_assert(offsetof(VkImageCreateInfo, initialLayout) == 0x50, "str wzr, [x19, #0x50]");

// The 32-byte block the query writes to the listing's [sp, #0] .. [sp, #0x20) (x6 = sp).
struct VkImageFormatProperties {
    VkExtent3D maxExtent;
    std::uint32_t maxMipLevels;
    std::uint32_t maxArrayLayers;
    std::uint32_t sampleCounts;
    std::uint64_t maxResourceSize;
};
static_assert(sizeof(VkImageFormatProperties) == 0x20, "the frame holds 32 bytes below the saved registers at sp+0x20");

using PFN_vkGetPhysicalDeviceImageFormatProperties = VkResult (*)(
    VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkImageTiling tiling,
    VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties* pImageFormatProperties);

namespace NRadEngine {

struct SGTex3DInit;  // NRadEngine::SGTex3DInit (CreateTexture3D's parameter type); ReVault has no layout

namespace NWriteLog {
enum ELogLevel : int;
// NRadEngine::NWriteLog::WriteLog(ELogLevel, unsigned long long, char const*, ...) @0x1088c20 (thunk 0x104b960);
// the same declaration as nwrite_log_write_log.cpp.
void WriteLog(ELogLevel level, unsigned long long channel, const char* format, ...);
}  // namespace NWriteLog

}  // namespace NRadEngine

// UNRESOLVED globals: ReVault and the decompiler name none of these.
// 0x2d65a5c: table of 32-bit values indexed by the init's first word (read at 0x19db698); its length is not
// recovered; the entry at index 4 is 0xffffffff, read here as -1.
/* UNRESOLVED: DAT_02d65a5c */
extern const std::int32_t DAT_02d65a5c[];
// 0x38754c0: slot +0x38 of the graphics function table whose base 0x3875488 the listing reaches through
// the GOT word 0x380f360. The failure log names vkGetPhysicalDeviceImageFormatProperties.
/* UNRESOLVED: DAT_038754c0 */
extern PFN_vkGetPhysicalDeviceImageFormatProperties DAT_038754c0;
// 0x38759a0: slot +0x518 of the same table, passed as the first argument of that call.
/* UNRESOLVED: DAT_038759a0 */
extern VkPhysicalDevice DAT_038759a0;

namespace {

// Reads a word of the init record. Fields read (the record's other members are not touched):
//   +0x00 index into DAT_02d65a5c      +0x04 width    +0x08 height    +0x0c depth    +0x10 mip level count
//   +0x14 non-zero selects usage 0x17  +0x18 non-zero adds usage 8     +0x20 data pointer, non-zero adds usage 2
std::uint32_t LoadU32(const NRadEngine::SGTex3DInit* init, std::size_t offset) {
    return *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const unsigned char*>(init) + offset);
}

std::uint64_t LoadU64(const NRadEngine::SGTex3DInit* init, std::size_t offset) {
    return *reinterpret_cast<const std::uint64_t*>(reinterpret_cast<const unsigned char*>(init) + offset);
}

}  // namespace

// @0x19db678 — FUN_018db678
/* @confidence: M */
void FUN_019db678(void* image_record, const void* image_desc) {
    VkImageCreateInfo* create_info = reinterpret_cast<VkImageCreateInfo*>(image_record);
    const NRadEngine::SGTex3DInit* init = reinterpret_cast<const NRadEngine::SGTex3DInit*>(image_desc);

    // 0x19db688-0x19db6c4: ldr w8, [x1]; the table value is the Vulkan format before the replacement below.
    const std::int32_t table_format = DAT_02d65a5c[LoadU32(init, 0x00)];

    VkFormat format = 0x4a;  // 0x19db6c0: mov w21, #0x4a, executed only on the in-range path
    const std::uint32_t biased = static_cast<std::uint32_t>(table_format) - 0x3a;  // sub w9, w8, #0x3a
    if (biased <= 0x22) {  // cmp w9, #0x22; b.hi 0x19db6d0
        switch (table_format) {  // br x11 through the 35-byte table at 0x2d575cb
            case 0x3a:
                format = 0x40;  // 0x19db6c8
                break;
            case 0x46:
                break;  // 0x19db704 with w21 still 0x4a
            case 0x5b:
                format = 0x5f;  // 0x19db6f0
                break;
            case 0x5c:
                format = 0x61;  // 0x19db6f8
                break;
            default:
                format = table_format;  // the other 31 entries (0x3b-0x45, 0x47-0x5a): 0x19db6e8, mov w21, w8
                break;
        }
    } else if (table_format == 0x10) {
        format = 0x25;  // 0x19db700
    } else if (table_format == 0x7a) {
        format = 0x6d;  // 0x19db6e0
    } else {
        format = table_format;  // 0x19db6e8
    }

    // 0x19db704-0x19db744: ldp w8, w11, [x20, #0x14]; ldr x8, [x20, #0x20]; csel chain.
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;  // 4
    if (LoadU32(init, 0x14) != 0) {
        usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;  // 0x17
    }
    if (LoadU32(init, 0x18) != 0) {
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;  // orr w11, w9, #8
    }
    if (LoadU64(init, 0x20) != 0) {
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;  // orr w8, w9, #2
    }

    // 0x19db748-0x19db75c: blr x11 with x0 = physical device, w1 = format, w2 = 2, w3 = 0, w4 = usage,
    // w5 = 0, x6 = sp.
    VkImageFormatProperties properties;
    const VkResult result = DAT_038754c0(DAT_038759a0, format, VK_IMAGE_TYPE_3D, VK_IMAGE_TILING_OPTIMAL, usage, 0,
                                         &properties);

    if (result == VK_SUCCESS) {  // 0x19db760: cbz w0, 0x19db798
        // 0x19db798-0x19db7cc. ldur q0, [x20, #4] reads width, height, depth and the mip count before any store.
        const std::uint32_t width = LoadU32(init, 0x04);
        const std::uint32_t height = LoadU32(init, 0x08);
        const std::uint32_t depth = LoadU32(init, 0x0c);
        const std::uint32_t mip_levels = LoadU32(init, 0x10);
        create_info->format = format;                         // str w21, [x19, #0x18]
        create_info->tiling = VK_IMAGE_TILING_OPTIMAL;        // stp wzr, w22, [x19, #0x34]
        create_info->usage = usage;
        create_info->sharingMode = VK_SHARING_MODE_EXCLUSIVE;  // stur xzr, [x19, #0x3c]
        create_info->queueFamilyIndexCount = 0;
        create_info->pQueueFamilyIndices = nullptr;            // str xzr, [x19, #0x48]
        create_info->sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;  // str w8 (14), [x19]
        create_info->pNext = nullptr;                          // stp xzr, x9, [x19, #8]
        create_info->flags = 0;
        create_info->imageType = VK_IMAGE_TYPE_3D;             // 0x200000000
        create_info->arrayLayers = 1;                          // stur x10 (0x100000001), [x19, #0x2c]
        create_info->samples = VK_SAMPLE_COUNT_1_BIT;
        create_info->extent.width = width;                     // stur q0, [x19, #0x1c]
        create_info->extent.height = height;
        create_info->extent.depth = depth;
        create_info->mipLevels = mip_levels;
        create_info->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // str wzr, [x19, #0x50]
        return;                                                // b 0x19db804
    }

    if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) {  // 0x19db768: cmn w0, #0xb
        // 0x19db770-0x19db794: WriteLog(8, 0, text, format, 2, usage, 0).
        NRadEngine::NWriteLog::WriteLog(
            static_cast<NRadEngine::NWriteLog::ELogLevel>(8), 0,
            "Unsupported texture format '%d' (image type '%d', usage: %d, flags:%d)", static_cast<int>(format),
            static_cast<int>(VK_IMAGE_TYPE_3D), static_cast<int>(usage), 0);
    } else {
        // 0x19db7d0-0x19db7e0: WriteLog(8, 0, text, result).
        NRadEngine::NWriteLog::WriteLog(static_cast<NRadEngine::NWriteLog::ELogLevel>(8), 0,
                                        "vkGetPhysicalDeviceImageFormatProperties returned error: %d",
                                        static_cast<int>(result));
    }

    // 0x19db7e4-0x19db800: the record is cleared; +4, +0x44 and +0x54 (padding) are not written.
    create_info->sType = 0;
    create_info->pNext = nullptr;
    create_info->flags = 0;
    create_info->imageType = 0;
    create_info->format = 0;
    create_info->extent.width = 0;
    create_info->extent.height = 0;
    create_info->extent.depth = 0;
    create_info->mipLevels = 0;
    create_info->arrayLayers = 0;
    create_info->samples = 0;
    create_info->tiling = 0;
    create_info->usage = 0;
    create_info->sharingMode = 0;
    create_info->queueFamilyIndexCount = 0;
    create_info->pQueueFamilyIndices = nullptr;
    create_info->initialLayout = 0;
}
