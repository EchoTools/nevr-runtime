// FUN_018dad8c
// Quest libr15.so VA 0x19dad8c; ReVault binary 576.
// ReVault raw parent 100259906, reconstruction node 100600768; grade M: all 129 instructions (0x19dad8c to the ret at 0x19daf8c) read; the body from 0x19dada4 to 0x19daf70 is mapped by the address comments, the prologue and epilogue are frame code; M because 4 globals stay DAT_ placeholders (0x38754c0 0x38759a0 0x2d65a5c 0x2e6e5dc), the function itself is unnamed in symbols (the decompiler and the merged callers call it FUN_019dad8c), and the image descriptor is read by raw offset; 0 callee placeholders (WriteLog is named).
// @0x19dad8c — FUN_018dad8c
/* @confidence: M */
#include <cstddef>
#include <cstdint>
#include <cstring>

// Vulkan 1.0 structures the function fills and receives. The layouts are the Vulkan API's; the
// 0x58-byte VkImageCreateInfo is the 88-byte stack buffer (auStack_b8) the callers hand in, and the
// 32-byte VkImageFormatProperties is the 32-byte buffer at [sp] of the listing (x6 = sp at 0x19dae8c).
struct VkExtent3D {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t depth;
};
struct VkImageCreateInfo {
    std::uint32_t sType;  // +0x00, stored 14 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO (mov w13,#0xe at 0x19daf08)
    const void* pNext;    // +0x08
    std::uint32_t flags;  // +0x10
    std::uint32_t imageType;  // +0x14
    std::uint32_t format;     // +0x18
    VkExtent3D extent;        // +0x1c
    std::uint32_t mipLevels;     // +0x28
    std::uint32_t arrayLayers;   // +0x2c
    std::uint32_t samples;       // +0x30
    std::uint32_t tiling;        // +0x34
    std::uint32_t usage;         // +0x38
    std::uint32_t sharingMode;   // +0x3c
    std::uint32_t queueFamilyIndexCount;       // +0x40
    const std::uint32_t* pQueueFamilyIndices;  // +0x48
    std::uint32_t initialLayout;               // +0x50
};
static_assert(sizeof(VkImageCreateInfo) == 0x58, "VkImageCreateInfo is 0x58 bytes");
static_assert(offsetof(VkImageCreateInfo, sType) == 0x00);
static_assert(offsetof(VkImageCreateInfo, pNext) == 0x08);
static_assert(offsetof(VkImageCreateInfo, flags) == 0x10);
static_assert(offsetof(VkImageCreateInfo, imageType) == 0x14);
static_assert(offsetof(VkImageCreateInfo, format) == 0x18);
static_assert(offsetof(VkImageCreateInfo, extent) == 0x1c);
static_assert(offsetof(VkImageCreateInfo, mipLevels) == 0x28);
static_assert(offsetof(VkImageCreateInfo, arrayLayers) == 0x2c);
static_assert(offsetof(VkImageCreateInfo, samples) == 0x30);
static_assert(offsetof(VkImageCreateInfo, tiling) == 0x34);
static_assert(offsetof(VkImageCreateInfo, usage) == 0x38);
static_assert(offsetof(VkImageCreateInfo, sharingMode) == 0x3c);
static_assert(offsetof(VkImageCreateInfo, queueFamilyIndexCount) == 0x40);
static_assert(offsetof(VkImageCreateInfo, pQueueFamilyIndices) == 0x48);
static_assert(offsetof(VkImageCreateInfo, initialLayout) == 0x50);

struct VkImageFormatProperties {
    VkExtent3D maxExtent;
    std::uint32_t maxMipLevels;
    std::uint32_t maxArrayLayers;
    std::uint32_t sampleCounts;
    std::uint64_t maxResourceSize;
};
static_assert(sizeof(VkImageFormatProperties) == 0x20, "VkImageFormatProperties is 0x20 bytes");

namespace NRadEngine {
namespace NWriteLog {
enum ELogLevel : int;
// thunk_FUN_01088c20 @ 0x104b960 (the two bl at 0x19daedc and 0x19daf50) -> WriteLog @ 0x1088c20.
void WriteLog(ELogLevel level, unsigned long long channel, const char* format, ...);
}  // namespace NWriteLog
}  // namespace NRadEngine

// ---------------------------------------------------------------------------------------------------
// Globals ReVault and the decompiler do not name; each stays a placeholder at global scope.

// DAT_038754c0 and DAT_038759a0 are slots of the graphics function table the listing reaches through
// the GOT slot 0x380f360 (table base 0x3875488, as in cg_engine_resources_initialize.cpp); ReVault
// xrefs: 0x19dae74 reads 0x38754c0 and 0x19dae78 reads 0x38759a0.
/* UNRESOLVED: DAT_038754c0 */
extern int (*DAT_038754c0)(void* physical_device, std::uint32_t format, std::uint32_t image_type,
                           std::uint32_t tiling, std::uint32_t usage, std::uint32_t flags,
                           VkImageFormatProperties* properties);  // 0x38754c0 = table +0x38, loaded into x9 and called with blr; the log text at 0x2d61f9b names it vkGetPhysicalDeviceImageFormatProperties
/* UNRESOLVED: DAT_038759a0 */
extern void* DAT_038759a0;  // 0x38759a0 = table +0x518, loaded into x0: the physical device handle
/* UNRESOLVED: DAT_02d65a5c */
extern const std::int32_t DAT_02d65a5c[];  // 0x2d65a5c, word table indexed by the descriptor's format word (ldr w8,[x9,x8,LSL #2] at 0x19dadb4); holds the engine format number the switch maps to a VkFormat
/* UNRESOLVED: DAT_02e6e5dc */
extern const std::uint32_t DAT_02e6e5dc[];  // 0x2e6e5dc, word table indexed by the descriptor word at +0x38 (0x19daef8); holds the sample count 1, 2, 4, 0, 1, 1, ...

namespace {

// The image descriptor (second argument, x1 = x20) is read by raw offset; ReVault has no layout for it.
constexpr std::size_t kDescFormat = 0x00;        // u32: index into DAT_02d65a5c
constexpr std::size_t kDescWidth = 0x04;         // u32; the 64-bit load at +4 (0x19daef0) copies width and height
constexpr std::size_t kDescHeight = 0x08;        // u32
constexpr std::size_t kDescMipLevels = 0x0c;     // u32 -> mipLevels
constexpr std::size_t kDescArrayLayers = 0x10;   // u32 (w24) -> arrayLayers, times 6 when the cube word is set
constexpr std::size_t kDescRenderTarget = 0x14;  // u32: nonzero selects usage 0x17, zero usage 4
constexpr std::size_t kDescCube = 0x18;          // u32 (w26): nonzero -> create flag 0x10 and 6 layers per array layer
constexpr std::size_t kDescStorage = 0x1c;       // u32: nonzero -> usage bit 8
constexpr std::size_t kDescInputAttachment = 0x28;  // u32: nonzero -> usage bit 0x80
constexpr std::size_t kDescSamplesIndex = 0x38;  // u32: index into DAT_02e6e5dc
constexpr std::size_t kDescInitialData = 0x48;   // u64 (ldr x8,[x20,#0x48]): nonzero -> usage bit 2

template <typename T>
T LoadAt(const void* base, std::size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof value);
    return value;
}

}  // namespace

// Called as (scratch, desc, 0) by CGEngineResources::Initialize and by CGSurfaceManager::CreateTexture2D,
// CreateCubeTexture and the other texture creators (the callers listed by ReVault). Same name and
// signature as the declaration in cg_engine_resources_initialize.cpp. The prologue (0x19dad8c..0x19dada0:
// sub sp,#0x70 and the saves of x19..x27, x30) and the epilogue (0x19daf74..0x19daf8c: the restores and
// the ret) are compiler frame code and are not modelled.
void FUN_019dad8c(void* create_info, const void* image_desc, int flag) {
    VkImageCreateInfo* const info = static_cast<VkImageCreateInfo*>(create_info);

    // 0x19dada4..0x19dae1c: engine format number -> VkFormat. The listing's jump table covers
    // 0x3a..0x5c (bounds check w9 > 0x22 at 0x19dadc0); the entries it routes to "mov w21,w8"
    // (0x19dae04) are the values the default arm copies, so only the four rewritten values and the
    // two rewritten values outside the table (0x10, 0x7a) need their own arm. w21 starts at 0x4a
    // (mov w21,#0x4a at 0x19daddc), which 0x46 keeps.
    const std::int32_t table_format = DAT_02d65a5c[LoadAt<std::uint32_t>(image_desc, kDescFormat)];
    std::int32_t vk_format = 0x4a;
    switch (table_format) {
        case 0x3a:  // 0x19dade4
            vk_format = 0x40;
            break;
        case 0x46:  // 0x19dae20: keeps 0x4a
            break;
        case 0x5b:  // 0x19dae0c
            vk_format = 0x5f;
            break;
        case 0x5c:  // 0x19dae14
            vk_format = 0x61;
            break;
        default:
            if (table_format == 0x10) {  // 0x19dadec..0x19dae1c
                vk_format = 0x25;
            } else if (table_format == 0x7a) {  // 0x19dadf4..0x19dadfc
                vk_format = 0x6d;
            } else {  // 0x19dae04
                vk_format = table_format;
            }
            break;
    }

    // 0x19dae20..0x19dae88: usage and create flags from the descriptor words.
    const std::uint32_t array_layers = LoadAt<std::uint32_t>(image_desc, kDescArrayLayers);  // w24
    const std::uint32_t cube = LoadAt<std::uint32_t>(image_desc, kDescCube);                // w26
    const bool is_cube = cube != 0;                                                          // w12, bVar5
    std::uint32_t usage = LoadAt<std::uint32_t>(image_desc, kDescRenderTarget) == 0 ? 4u : 0x17u;
    usage |= static_cast<std::uint32_t>(flag != 0);  // w25 = (w2 != 0)
    if (LoadAt<std::uint32_t>(image_desc, kDescStorage) != 0) {
        usage |= 8u;
    }
    if (LoadAt<std::uint64_t>(image_desc, kDescInitialData) != 0) {
        usage |= 2u;
    }
    if (LoadAt<std::uint32_t>(image_desc, kDescInputAttachment) != 0) {
        usage |= 0x80u;
    }
    const std::uint32_t create_flags = is_cube ? 0x10u : 0u;  // w23 = w12 << 4

    // 0x19dae8c..0x19daea8: vkGetPhysicalDeviceImageFormatProperties(physical device, format,
    // image type 1 (2D), tiling 0, usage, flags, &properties); the 32-byte result at [sp] is not read.
    VkImageFormatProperties properties;
    const int result = DAT_038754c0(DAT_038759a0, static_cast<std::uint32_t>(vk_format), 1u, 0u, usage,
                                    create_flags, &properties);

    if (result == 0) {
        // 0x19daee4..0x19daf3c: fill the create info.
        const std::uint32_t width = LoadAt<std::uint32_t>(image_desc, kDescWidth);
        const std::uint32_t height = LoadAt<std::uint32_t>(image_desc, kDescHeight);
        const std::uint32_t mip_levels = LoadAt<std::uint32_t>(image_desc, kDescMipLevels);
        const std::uint32_t samples =
            DAT_02e6e5dc[LoadAt<std::uint32_t>(image_desc, kDescSamplesIndex)];
        info->sType = 14;
        info->pNext = nullptr;
        info->flags = create_flags;
        info->imageType = 1;
        info->format = static_cast<std::uint32_t>(vk_format);
        info->extent.width = width;
        info->extent.height = height;
        info->extent.depth = 1;
        info->mipLevels = mip_levels;
        info->arrayLayers = is_cube ? array_layers * 6u : array_layers;  // add w8,w24,w24,LSL #1; lsl #1
        info->samples = samples;
        info->tiling = static_cast<std::uint32_t>(flag != 0);
        info->usage = usage;
        info->sharingMode = 0;
        info->queueFamilyIndexCount = 0;
        info->pQueueFamilyIndices = nullptr;
        info->initialLayout = 0;
        return;
    }

    // 0x19daeb0..0x19daf50: log, then clear the create info (0x19daf54..0x19daf70). -11 is
    // VK_ERROR_FORMAT_NOT_SUPPORTED; the unsupported-format text is logged only for it.
    if (result == -11) {
        NRadEngine::NWriteLog::WriteLog(
            static_cast<NRadEngine::NWriteLog::ELogLevel>(8), 0,
            "Unsupported texture format '%d' (image type '%d', usage: %d, flags:%d)",
            static_cast<std::int32_t>(vk_format), 1, usage, create_flags);
    } else {
        NRadEngine::NWriteLog::WriteLog(
            static_cast<NRadEngine::NWriteLog::ELogLevel>(8), 0,
            "vkGetPhysicalDeviceImageFormatProperties returned error: %d", result);
    }
    info->sType = 0;
    info->pQueueFamilyIndices = nullptr;
    info->initialLayout = 0;
    info->pNext = nullptr;
    info->flags = 0;
    info->imageType = 0;
    info->format = 0;
    info->extent.width = 0;
    info->extent.height = 0;
    info->extent.depth = 0;
    info->mipLevels = 0;
    info->arrayLayers = 0;
    info->samples = 0;
    info->tiling = 0;
    info->usage = 0;
    info->sharingMode = 0;
    info->queueFamilyIndexCount = 0;
}
