// NRadEngine::CCanvasDisplay::NeedsResize
// Quest libr15.so VA 0x26ee06c; ReVault binary 576.
// ReVault raw parent 100323516, reconstruction node 100600780; grade H: all 111 instructions of 0x26ee06c-0x26ee224 read through the ret, every callee has a ReVault name (0 placeholders), members are raw offsets with their roles commented.
// @0x26ee06c — NRadEngine::CCanvasDisplay::NeedsResize
/* @confidence: H */
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ce_engine.hpp"

namespace NRadEngine {

enum EWindowMode : std::uint32_t;
enum ESplitScreenMode : std::uint32_t;
enum EMSAAMode : std::uint32_t;

// Passed and returned by value in x0 (low word x, high word y).
struct C2iVector {
    std::int32_t x;
    std::int32_t y;
};
static_assert(sizeof(C2iVector) == 0x08);

namespace NWriteLog {
enum ELogLevel : std::int32_t;
// NRadEngine::NWriteLog::WriteLog @ 0x1088c20 (thunk 0x104b960). Variadic.
void WriteLog(ELogLevel level, unsigned long long channel, const char* format, ...);
}  // namespace NWriteLog

namespace CVR {
// NRadEngine::CVR::Initialized @ 0x1b70b64 (thunk 0xfd2380).
bool Initialized();
// NRadEngine::CVR::DisplayLost @ 0x1b73460 (thunk 0x1011900); returns a word that is tested for non-zero.
std::uint32_t DisplayLost();
// NRadEngine::CVR::DeviceAvailable @ 0x1b7346c (thunk 0xfebbe0).
bool DeviceAvailable();
}  // namespace CVR

class CGRenderConfig {
public:
    // @ 0x198cf14 (thunk 0x103dca0): NRadEngine::CGRenderConfig::ViewportTargetSize(NRadEngine::C2iVector).
    static C2iVector ViewportTargetSize(C2iVector size);
    // @ 0x198cbf8 (thunk 0xff76b0).
    static ESplitScreenMode SplitScreenMode();
    // @ 0x198cc10 (thunk 0x101d4f0): NumActiveViewports(NRadEngine::ESplitScreenMode).
    static std::uint32_t NumActiveViewports(ESplitScreenMode mode);
    // @ 0x1928e74 (thunk 0x1028cd0): NumActiveViewports().
    static std::uint32_t NumActiveViewports();
    // @ 0x198c894 (thunk 0x103d880).
    static EMSAAMode MSAAMode();
};

// Sub-object at CCanvasDisplay + 0x8. Layout not established here; only the member is called.
class CGRenderer {
public:
    // NRadEngine::CGRenderer::SetCameraCut(unsigned long long, unsigned int) @ 0x19957b4 (thunk 0x1005520):
    // ORs the second argument into the word at +0x1720 of the 0x1730-byte record the first argument selects.
    void SetCameraCut(unsigned long long viewport, unsigned int flags);
};

// Sub-object at CCanvasDisplay + 0x13a60. Same declaration as cg_display_needs_resize.cpp.
class CGDisplay {
public:
    // @ 0x1934f88 (thunk 0x10493f0).
    bool NeedsResize(unsigned int width, unsigned int height, EWindowMode window_mode) const;
};

// Sub-object at CCanvasDisplay + 0x4cf8; the call site passes it in x0 (0x26ee198-0x26ee19c).
class CGShadowMapPool {
public:
    // NRadEngine::CGShadowMapPool::CheckForResize() @ 0x19d4988 (thunk 0x102eac0); the caller tests w0.
    std::int32_t CheckForResize();
};

namespace {

// Offsets inside the CCanvasDisplay object (ReVault has no layout for it).
constexpr std::size_t kRendererOffset = 0x8;            // CGRenderer sub-object (this + 8, 0x26ee118)
constexpr std::size_t kDisplayOffset = 0x13a60;         // CGDisplay sub-object (0x26ee148)
constexpr std::size_t kShadowMapPoolOffset = 0x4cf8;    // CGShadowMapPool sub-object (0x26ee194-0x26ee198)
constexpr std::size_t kTargetWidthOffset = 0x1d78;      // compared with ViewportTargetSize().x
constexpr std::size_t kTargetHeightOffset = 0x1d7c;     // compared with ViewportTargetSize().y
constexpr std::size_t kForceResizeOffset = 0x1d80;      // word; non-zero makes the result true
constexpr std::size_t kPendingOffset = 0x13a50;         // word; result when nothing else differs, cleared on exit
constexpr std::size_t kMSAAModeOffset = 0x16d48;        // stored MSAAMode (CCanvasDisplay::Resize writes it)
constexpr std::size_t kSplitScreenModeOffset = 0x16d4c; // stored SplitScreenMode (CCanvasDisplay::Resize writes it)

template <typename T>
T Load(const CCanvasDisplay* self, std::size_t offset) {
    T value;
    std::memcpy(&value, reinterpret_cast<const std::byte*>(self) + offset, sizeof(value));
    return value;
}

template <typename T>
void Store(CCanvasDisplay* self, std::size_t offset, T value) {
    std::memcpy(reinterpret_cast<std::byte*>(self) + offset, &value, sizeof(value));
}

}  // namespace

// @0x26ee06c — NRadEngine::CCanvasDisplay::NeedsResize
/* @confidence: H */
bool CCanvasDisplay::NeedsResize(std::uint32_t width, std::uint32_t height, std::uint32_t mode) {
    // 0x26ee094-0x26ee0ac: x0 = width | height << 32 (bfi x0, x21, #32, #32), then ViewportTargetSize.
    const C2iVector requested{static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
    const C2iVector target = CGRenderConfig::ViewportTargetSize(requested);

    // 0x26ee0b0-0x26ee0e0
    const std::int32_t stored_width = Load<std::int32_t>(this, kTargetWidthOffset);
    const std::int32_t stored_height = Load<std::int32_t>(this, kTargetHeightOffset);
    const ESplitScreenMode split_mode = CGRenderConfig::SplitScreenMode();
    const std::uint32_t stored_split_viewports =
        CGRenderConfig::NumActiveViewports(Load<ESplitScreenMode>(this, kSplitScreenModeOffset));
    const std::uint32_t active_viewports = CGRenderConfig::NumActiveViewports();

    // 0x26ee0e4-0x26ee138: the target size is unchanged but the split-screen mode moved with the same
    // viewport count: store the new mode and cut the camera of every active viewport.
    if (stored_width == target.x &&
        Load<ESplitScreenMode>(this, kSplitScreenModeOffset) != split_mode &&
        stored_height == target.y && stored_split_viewports == active_viewports) {
        Store<ESplitScreenMode>(this, kSplitScreenModeOffset, split_mode);
        if (CGRenderConfig::NumActiveViewports() != 0) {
            auto* const renderer = reinterpret_cast<CGRenderer*>(
                reinterpret_cast<std::byte*>(this) + kRendererOffset);
            std::uint64_t viewport = 0;
            do {
                renderer->SetCameraCut(viewport, 1);
                ++viewport;
            } while (viewport < CGRenderConfig::NumActiveViewports());
        }
    }

    // 0x26ee13c-0x26ee164: mode is the fourth argument, spilled at [sp, #0xc] across the calls above.
    const auto* const display =
        reinterpret_cast<const CGDisplay*>(reinterpret_cast<const std::byte*>(this) + kDisplayOffset);
    const bool display_needs_resize =
        display->NeedsResize(width, height, static_cast<EWindowMode>(mode));
    const std::int32_t stored_msaa = Load<std::int32_t>(this, kMSAAModeOffset);
    const EMSAAMode msaa_mode = CGRenderConfig::MSAAMode();

    // 0x26ee168-0x26ee190: any difference returns true at once (0x26ee204), leaving the pending word as it is.
    if (stored_width != target.x) {
        return true;
    }
    if (stored_height != target.y) {
        return true;
    }
    if (display_needs_resize) {
        return true;
    }
    if (stored_msaa != static_cast<std::int32_t>(msaa_mode)) {
        return true;
    }
    if (Load<ESplitScreenMode>(this, kSplitScreenModeOffset) != split_mode) {
        return true;
    }

    // 0x26ee194-0x26ee1ec: shadow-map pool or forced flag give true; otherwise the pending word decides.
    auto* const pool = reinterpret_cast<CGShadowMapPool*>(
        reinterpret_cast<std::byte*>(this) + kShadowMapPoolOffset);
    bool result;
    if (pool->CheckForResize() != 0 || Load<std::int32_t>(this, kForceResizeOffset) != 0) {
        result = true;
    } else {
        result = Load<std::int32_t>(this, kPendingOffset) != 0;
    }

    // 0x26ee1b0-0x26ee1e0: a lost VR display with an available device logs and forces true.
    if (CVR::Initialized() && CVR::DisplayLost() != 0 && CVR::DeviceAvailable()) {
        NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(2), 0,
                            "Attempting to recover from VR lost display");
        result = true;
    }

    // 0x26ee200: str wzr, [x25] with x25 = this + 0x13a50.
    Store<std::int32_t>(this, kPendingOffset, 0);
    return result;
}

}  // namespace NRadEngine
