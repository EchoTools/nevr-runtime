// @0x21e8e0c — NRadEngine::NRadGame::ApplyRuntimeGfxSettings(const SR15GfxSettings&, CR15Game&, uint32_t)
/* @confidence: M — render-setting calls, LOD branches, UI-refresh gate and log order match the disassembly.
   The CMemBlock helpers and relocated enum-string tables retain inferred interfaces. */
#include <cstddef>
#include <cstdint>

namespace NRadEngine {
struct C2Vector { float x, y; };
static_assert(offsetof(C2Vector, x) == 0x0);
static_assert(offsetof(C2Vector, y) == 0x4);
static_assert(sizeof(C2Vector) == 0x8);
struct SR15GfxSettings {
    std::uint32_t flags;                  // +0x00
    std::uint32_t display;                // +0x04
    float resolutionScale;                // +0x08
    float autoResMinScale;                // +0x0c
    float autoResMaxScale;                // +0x10
    std::int32_t autoResFrameRate;        // +0x14
    float autoResHeadroom;                // +0x18
    std::int32_t msaa;                    // +0x1c
    float sharpening;                     // +0x20
    std::int32_t qualityLevel;            // +0x24
    std::uint32_t spotShadowSize;         // +0x28
    std::uint32_t cascadeShadowSize;      // +0x2c
    float cascadeDistanceScale;           // +0x30
    std::int32_t shadowMsaaMode;          // +0x34
    float shadowFilteringScale;           // +0x38
    std::uint32_t effectFlags;            // +0x3c
    std::int32_t lightQuality;            // +0x40
    std::byte reserved44[4];
    std::int32_t textureQuality;           // +0x48
    std::int32_t meshQuality;              // +0x4c
    std::byte reserved50[4];
    std::int32_t fxQuality;                // +0x54
    float displayGamma;                    // +0x58
    std::byte reserved5c[4];
    std::uint16_t field_60;                // +0x60
};
static_assert(offsetof(SR15GfxSettings, flags) == 0x00);
static_assert(offsetof(SR15GfxSettings, display) == 0x04);
static_assert(offsetof(SR15GfxSettings, resolutionScale) == 0x08);
static_assert(offsetof(SR15GfxSettings, autoResMinScale) == 0x0c);
static_assert(offsetof(SR15GfxSettings, autoResMaxScale) == 0x10);
static_assert(offsetof(SR15GfxSettings, autoResFrameRate) == 0x14);
static_assert(offsetof(SR15GfxSettings, autoResHeadroom) == 0x18);
static_assert(offsetof(SR15GfxSettings, msaa) == 0x1c);
static_assert(offsetof(SR15GfxSettings, sharpening) == 0x20);
static_assert(offsetof(SR15GfxSettings, qualityLevel) == 0x24);
static_assert(offsetof(SR15GfxSettings, spotShadowSize) == 0x28);
static_assert(offsetof(SR15GfxSettings, cascadeShadowSize) == 0x2c);
static_assert(offsetof(SR15GfxSettings, cascadeDistanceScale) == 0x30);
static_assert(offsetof(SR15GfxSettings, shadowMsaaMode) == 0x34);
static_assert(offsetof(SR15GfxSettings, shadowFilteringScale) == 0x38);
static_assert(offsetof(SR15GfxSettings, effectFlags) == 0x3c);
static_assert(offsetof(SR15GfxSettings, lightQuality) == 0x40);
static_assert(offsetof(SR15GfxSettings, textureQuality) == 0x48);
static_assert(offsetof(SR15GfxSettings, meshQuality) == 0x4c);
static_assert(offsetof(SR15GfxSettings, fxQuality) == 0x54);
static_assert(offsetof(SR15GfxSettings, displayGamma) == 0x58);
static_assert(offsetof(SR15GfxSettings, field_60) == 0x60);
static_assert(sizeof(SR15GfxSettings) == 0x64);

struct CR15Game {
    std::byte reserved0[0x1f78];
    void* settingsTranslatorComponentSystems; // +0x1f78; only tested for null here
    std::byte reserved1[0x7ad8 - 0x1f80];
    std::uint32_t flags_7ad8;             // +0x7ad8
};
static_assert(offsetof(CR15Game, settingsTranslatorComponentSystems) == 0x1f78);
static_assert(offsetof(CR15Game, flags_7ad8) == 0x7ad8);
static_assert(sizeof(CR15Game) == 0x7ae0);

namespace CGRenderConfig {
void SetEnableMultiResVR(std::uint32_t);
void SetDisplayGamma(float);
void SetEnableTemporalAA(std::uint32_t);
void SetTemporalAABlendWeight(float);
void SetSharpeningAmount(float);
void SetShadowCascadeMapSize(std::uint32_t);
void SetShadowMapSize(std::uint32_t);
void SetShadowMSAAMode(std::int32_t);
void SetShadowFilteringScale(float);
void SetShadowCascadeDistanceScale(float);
void SetStaggerCascadeUpdates(std::uint32_t);
void SetEnableVolumetrics(std::uint32_t);
void SetEnableBloom(std::uint32_t);
void SetLightQualityLevel(std::int32_t);
void SetParticleQualityLevel(std::int32_t);
void SetAutoResolutionMinScale(float);
void SetAutoResolutionMaxScale(float);
void SetAutoResolutionFrameRate(std::int32_t);
void SetAutoResolutionHeadRoom(float);
void SetCustomViewportScale(std::uint32_t, C2Vector);
void SetMSAAMode(std::int32_t);
void SetMaxStreamedTextureMips(std::uint32_t);
void SetMaxHiPriorityStreamedTextureMips(std::uint32_t);
void SetTextureHeapSize(std::uint32_t);
void SetEnableAutoResolution(std::uint32_t);
} // namespace CGRenderConfig

namespace CVR {
std::uint32_t QuestHeadsetType();
std::uint32_t Initialized();
void SetEnableDynamicFoveation(std::uint32_t);
void RefreshRTSize();
extern const float kR15Quest1ResolutionScale;
extern const float kR15Quest2ResolutionScale;
extern const std::int64_t kR15Quest1MSAALevel;
extern const std::int64_t kR15Quest2MSAALevel;
} // namespace CVR

namespace NRadEngine::NRadGame::reconstruction_detail {
using LogStringTable = const char* const[];
// The binary accesses relocated pointer tables; their string contents are not
// guessed here. These references name the tables by their observed role.
extern LogStringTable kGraphicsQualityNames; // table base + 0x90 in the image
extern LogStringTable kTextureQualityNames; // table base in the image
extern LogStringTable kGraphicsSettingNames;
extern const char* kEnabledText;
extern const char* kDisabledText;

void SetLodScales(float distance, float modelDistance, float modelOffset);
void* GameSpace(CR15Game*, void*);
void* CurrentMemoryContext();
struct CMemBlock {
    std::byte reserved0[0x1c];
    std::uint8_t flags;
    std::byte reserved1[3];
    std::uint64_t allocationSize;
};
static_assert(sizeof(CMemBlock) == 0x28);
static_assert(offsetof(CMemBlock, flags) == 0x1c);
static_assert(offsetof(CMemBlock, allocationSize) == 0x20);
std::int32_t CurrentTlsSlot();
void* GetTls(std::int32_t);
void CMemBlockConstruct(CMemBlock*, void*, void*, void*);
void CMemBlockDetach(CMemBlock*);
void CMemBlockDestructor_(CMemBlock*);
void CMemBlockSetAllocator(CMemBlock*, void*);
void CMemBlockConstructFrom(CMemBlock*, std::uintptr_t, std::uintptr_t);
struct ComponentSystemRange {
    void** begin;
    std::uintptr_t count;
};
static_assert(offsetof(ComponentSystemRange, begin) == 0x0);
static_assert(offsetof(ComponentSystemRange, count) == 0x8);
static_assert(sizeof(ComponentSystemRange) == 0x10);
std::uint64_t SettingsTranslatorComponentSystemTypeId();
void FindComponentSystems(void* gameSpace, std::uint64_t typeId, CMemBlock*,
                          ComponentSystemRange* result);
void WriteSettingsToUI(void* settingsTranslatorComponentSystem);
void CMemBlockDestroy(CMemBlock*);
} // namespace reconstruction_detail

namespace NWriteLog {
void WriteLog(std::int32_t level, std::int32_t category, const char* format, ...);
}

namespace NRadEngine::NRadGame {
using namespace reconstruction_detail;

void ApplyRuntimeGfxSettings(const SR15GfxSettings& g, CR15Game& game,
                             std::uint32_t refreshUI) {
    CGRenderConfig::SetEnableMultiResVR(
        g.field_60 == 0 ? ((g.flags >> 3) & 1U) : 0U);
    CGRenderConfig::SetDisplayGamma(g.displayGamma);
    CGRenderConfig::SetEnableTemporalAA((g.flags >> 2) & 1U);
    CGRenderConfig::SetTemporalAABlendWeight(0.9F);
    CGRenderConfig::SetSharpeningAmount(g.sharpening);
    CGRenderConfig::SetShadowCascadeMapSize(g.cascadeShadowSize);
    CGRenderConfig::SetShadowMapSize(g.spotShadowSize);
    CGRenderConfig::SetShadowMSAAMode(g.shadowMsaaMode);
    CGRenderConfig::SetShadowFilteringScale(g.shadowFilteringScale);
    CGRenderConfig::SetShadowCascadeDistanceScale(g.cascadeDistanceScale);
    CGRenderConfig::SetStaggerCascadeUpdates(g.effectFlags & 1U);
    CGRenderConfig::SetEnableVolumetrics((g.effectFlags >> 1) & 1U);
    CGRenderConfig::SetEnableBloom((g.effectFlags >> 2) & 1U);
    CGRenderConfig::SetLightQualityLevel(1 << (g.lightQuality & 31));
    CGRenderConfig::SetParticleQualityLevel(
        g.fxQuality < 0 ? 0 : 1 << (g.fxQuality & 31));
    CGRenderConfig::SetAutoResolutionMinScale(g.autoResMinScale);
    CGRenderConfig::SetAutoResolutionMaxScale(g.autoResMaxScale);
    CGRenderConfig::SetAutoResolutionFrameRate(g.autoResFrameRate);
    CGRenderConfig::SetAutoResolutionHeadRoom(g.autoResHeadroom);

    const bool quest2Class = (CVR::QuestHeadsetType() | 1U) == 3U;
    const float viewportScale =
        quest2Class ? CVR::kR15Quest2ResolutionScale
                    : CVR::kR15Quest1ResolutionScale;
    CGRenderConfig::SetCustomViewportScale(0, {viewportScale, viewportScale});
    const std::int64_t msaaLevel =
        quest2Class ? CVR::kR15Quest2MSAALevel : CVR::kR15Quest1MSAALevel;
    CGRenderConfig::SetMSAAMode(msaaLevel == 4 ? 2 : (msaaLevel == 2 ? 1 : 0));
    CGRenderConfig::SetMaxStreamedTextureMips(16);
    CGRenderConfig::SetMaxHiPriorityStreamedTextureMips(16);
    CGRenderConfig::SetTextureHeapSize(0x10000000);
    CGRenderConfig::SetEnableAutoResolution(0);
    CVR::SetEnableDynamicFoveation(1);
    if (CVR::Initialized() == 0) {
        // The disassembly branches around RefreshRTSize when this is false.
    } else {
        CVR::RefreshRTSize();
    }

    if ((game.flags_7ad8 & 0x80000000U) != 0) {
        SetLodScales(1.0F, 1.0F, 0.0F);
    } else if (g.meshQuality >= 0 && g.meshQuality <= 2) {
        constexpr float kDistance[] = {1.42F, 1.0F, 0.5F};
        constexpr float kModelDistance[] = {2.0F, 1.0F, 1.0F};
        constexpr float kModelOffset[] = {5.0F, 0.0F, 0.0F};
        const auto index = static_cast<std::size_t>(g.meshQuality);
        SetLodScales(kDistance[index], kModelDistance[index], kModelOffset[index]);
    }

    if (refreshUI != 0) {
        if (game.settingsTranslatorComponentSystems == nullptr) {
            return; // This branch returns before every log call.
        }
        const auto* gameSpace = GameSpace(&game, nullptr);
        CMemBlock block{};
        CMemBlockConstruct(&block, nullptr, nullptr, CurrentMemoryContext());
        const auto tlsSlot = CurrentTlsSlot();
        void* tls = GetTls(tlsSlot);
        if ((block.flags & 6U) != 0) {
            CMemBlockDetach(&block);
            block.flags &= static_cast<std::uint8_t>(~6U);
        } else {
            CMemBlockDestructor_(&block);
        }
        CMemBlockSetAllocator(&block, tls);
        CMemBlockConstructFrom(&block,
            reinterpret_cast<std::uintptr_t>(game.settingsTranslatorComponentSystems) << 3,
            0);
        ComponentSystemRange systems{};
        FindComponentSystems(const_cast<void*>(gameSpace),
                             SettingsTranslatorComponentSystemTypeId(),
                             &block, &systems);
        for (std::uintptr_t i = 0; i < systems.count; ++i) {
            WriteSettingsToUI(systems.begin[i]);
        }
        if ((block.flags & 6U) != 0) {
            CMemBlockDetach(&block);
        }
        CMemBlockDestroy(&block);
    }

    NWriteLog::WriteLog(2, 0, "### Applying new graphics settings ###");
    NWriteLog::WriteLog(2, 0, "    Graphics quality: %s",
                        kGraphicsQualityNames[g.qualityLevel]);

    if (g.qualityLevel == 0) {
        NWriteLog::WriteLog(2, 0, "    Texture quality: %s",
                            kTextureQualityNames[g.textureQuality]);
        NWriteLog::WriteLog(2, 0, "    Cascade shadow resolution: %u",
                            g.cascadeShadowSize);
        NWriteLog::WriteLog(2, 0, "    Spot shadow resolution: %u",
                            g.spotShadowSize);
        NWriteLog::WriteLog(2, 0, "    Shadow MSAA: %s",
                            kGraphicsSettingNames[g.shadowMsaaMode]);
        NWriteLog::WriteLog(2, 0, "    Volumetrics: %s",
                            (g.effectFlags & 2U) ? kEnabledText : kDisabledText);
        NWriteLog::WriteLog(2, 0, "    Bloom: %s",
                            (g.effectFlags & 4U) ? kEnabledText : kDisabledText);
        NWriteLog::WriteLog(2, 0, "    Light Quality: %s",
                            kGraphicsSettingNames[g.lightQuality + 3]);
        NWriteLog::WriteLog(2, 0, "    FX Quality: %s",
                            kGraphicsSettingNames[g.fxQuality + 6]);
        NWriteLog::WriteLog(2, 0, "    Mesh Quality: %s",
                            kGraphicsSettingNames[g.meshQuality + 9]);
    }

    NWriteLog::WriteLog(2, 0, "    MSAA mode: %s", kGraphicsSettingNames[g.msaa]);
    NWriteLog::WriteLog(2, 0, "    TAA: %s",
                        (g.flags & 4U) ? kEnabledText : kDisabledText);
    NWriteLog::WriteLog(2, 0, "    Multi-Res: %s",
                        (g.flags & 8U) ? kEnabledText : kDisabledText);
    NWriteLog::WriteLog(2, 0, "    Adaptive Res: %s",
                        (g.flags & 2U) ? kEnabledText : kDisabledText);
    NWriteLog::WriteLog(2, 0, "    Resolution Scale: %f",
                        static_cast<double>(g.resolutionScale));
    NWriteLog::WriteLog(2, 0, "    Fullscreen: %s",
                        (g.flags & 1U) ? kEnabledText : kDisabledText);
}

} // namespace NRadEngine::NRadGame
