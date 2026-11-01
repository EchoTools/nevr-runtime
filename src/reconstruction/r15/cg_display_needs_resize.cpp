// @0x1934f88 — NRadEngine::CGDisplay::NeedsResize(unsigned int, unsigned int, EWindowMode) const
/* @confidence: H */
// ReVault binary 576; reconstruction node 100600210.

#include <cstddef>
#include <cstdint>

namespace NRadEngine {

enum EWindowMode : std::uint32_t;

struct CGDisplayNeedsResizeView {
    std::byte unknown_0000[0x3250];
    std::int32_t width_3250;
    std::int32_t height_3254;
    std::byte unknown_3258[0x78];
    std::int32_t window_mode_32d0;
    std::int32_t unknown_32d4;
    std::int32_t setting_32d8;
    std::int32_t setting_32dc;
    std::int32_t setting_32e0;
    std::int32_t setting_32e4;
};
static_assert(offsetof(CGDisplayNeedsResizeView, width_3250) == 0x3250);
static_assert(offsetof(CGDisplayNeedsResizeView, height_3254) == 0x3254);
static_assert(offsetof(CGDisplayNeedsResizeView, window_mode_32d0) == 0x32d0);
static_assert(offsetof(CGDisplayNeedsResizeView, setting_32d8) == 0x32d8);
static_assert(offsetof(CGDisplayNeedsResizeView, setting_32dc) == 0x32dc);
static_assert(offsetof(CGDisplayNeedsResizeView, setting_32e0) == 0x32e0);
static_assert(offsetof(CGDisplayNeedsResizeView, setting_32e4) == 0x32e4);
static_assert(sizeof(CGDisplayNeedsResizeView) == 0x32e8);

struct NeedsResizeGlobalConfiguration {
    std::int32_t setting_0;
    std::int32_t setting_4;
    std::int32_t setting_8;
    std::int32_t setting_c;
};
static_assert(offsetof(NeedsResizeGlobalConfiguration, setting_0) == 0x00);
static_assert(offsetof(NeedsResizeGlobalConfiguration, setting_4) == 0x04);
static_assert(offsetof(NeedsResizeGlobalConfiguration, setting_8) == 0x08);
static_assert(offsetof(NeedsResizeGlobalConfiguration, setting_c) == 0x0c);
static_assert(sizeof(NeedsResizeGlobalConfiguration) == 0x10);

extern const NeedsResizeGlobalConfiguration g_needs_resize_global_configuration;

class CGDisplay {
public:
    bool NeedsResize(unsigned int width, unsigned int height,
                     EWindowMode window_mode) const;
};

bool CGDisplay::NeedsResize(unsigned int width, unsigned int height,
                            EWindowMode window_mode) const {
    const auto* display = reinterpret_cast<const CGDisplayNeedsResizeView*>(this);
    const auto& global = g_needs_resize_global_configuration;

    if (display->width_3250 != static_cast<std::int32_t>(width)) {
        return true;
    }
    if (display->height_3254 != static_cast<std::int32_t>(height)) {
        return true;
    }
    if (display->window_mode_32d0 != static_cast<std::int32_t>(window_mode)) {
        return true;
    }
    if (display->setting_32d8 != global.setting_0) {
        return true;
    }
    if (display->setting_32dc != global.setting_4) {
        return true;
    }
    if (display->setting_32e0 != global.setting_8) {
        return true;
    }
    return display->setting_32e4 != global.setting_c;
}

}  // namespace NRadEngine
