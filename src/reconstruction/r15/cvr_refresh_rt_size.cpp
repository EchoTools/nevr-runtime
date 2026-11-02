// @0x1b709a0 — NRadEngine::CVR::RefreshRTSize()
// ReVault binary 576; reconstruction node 100600141.
#include <cstdint>

namespace NRadEngine {
struct OvrJava;
}
extern "C" std::int32_t vrapi_GetSystemPropertyInt(
    NRadEngine::OvrJava* java, std::int32_t property);

namespace NRadEngine {

struct C2Vector {
    float x;
    float y;
};

struct C2iVector {
    std::int32_t x;
    std::int32_t y;
    explicit C2iVector(C2Vector value);
};

extern C2iVector g_rtSize;          // DAT_038a33e0: packed width and height
extern C2iVector g_rtSizeOverride;  // DAT_038a33e8: {0, 0} means no override
extern OvrJava g_ovrJava;           // DAT_038a3550

namespace CGRenderConfig {
float CustomViewportScale(std::uint32_t viewport);
}  // namespace CGRenderConfig

namespace CVR {
void RefreshRTSize() {
    const float scale = CGRenderConfig::CustomViewportScale(0);
    C2Vector size;
    if (g_rtSizeOverride.x == 0 && g_rtSizeOverride.y == 0) {
        const auto width = ::vrapi_GetSystemPropertyInt(&g_ovrJava, 5);
        const auto height = ::vrapi_GetSystemPropertyInt(&g_ovrJava, 6);
        size = {scale * static_cast<float>(width),
                scale * static_cast<float>(height)};
    } else {
        size = {static_cast<float>(g_rtSizeOverride.x) * scale,
                static_cast<float>(g_rtSizeOverride.y) * scale};
    }
    g_rtSize = C2iVector(size);
}

}  // namespace CVR
}  // namespace NRadEngine
