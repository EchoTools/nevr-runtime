// @0x2bc0f9c — NRadEngine::CEngine::CheckForResize()
// ReVault binary 576; reconstruction node 100600214.
// The opaque storage size documents the highest field offset observed here;
// the object is the original engine instance and is not instantiated here.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "ce_engine.hpp"

namespace NRadEngine {

namespace CVR {
bool Initialized();
void RefreshRTSize();
}  // namespace CVR

namespace CGS {
std::uint64_t DisplayCount();
}  // namespace CGS

namespace CSysWindow {
bool IsVirtual(int* window);
std::uint16_t IsActive(int* window);
void WindowExtents(int* window, CQuad* extents);
void MinimizeWindow(int* window);
}  // namespace CSysWindow

namespace {

template <typename T>
T Load(const CEngine* self, std::size_t offset) {
    T value;
    std::memcpy(&value, reinterpret_cast<const std::byte*>(self) + offset,
                sizeof(value));
    return value;
}

template <typename T>
void Store(CEngine* self, std::size_t offset, T value) {
    std::memcpy(reinterpret_cast<std::byte*>(self) + offset, &value,
                sizeof(value));
}

std::uint32_t ConvertUnsigned(float value) {
    // Match AArch64 FCVTZU: truncate, with NaN/negative mapped to zero and
    // positive overflow saturated at UINT32_MAX.
    if (std::isnan(value) || value <= 0.0F) {
        return 0;
    }
    if (value >= 4294967296.0F) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(value);
}

constexpr std::size_t kWindowOffset = 0x16d58;
constexpr std::size_t kResizeModeOffset = 0x16dd4;
constexpr std::size_t kWindowActiveOffset = 0x16dd8;
constexpr std::size_t kWindowModeOffset = 0x16e50;
constexpr std::size_t kWindowEnabledOffset = 0x16ed4;

using ResizeCallback = std::uint32_t (*)(CEngine*, std::uint32_t,
                                         std::uint32_t, std::uint32_t);

}  // namespace

std::uint32_t CEngine::CheckForResize() {
    if ((Load<std::uint8_t>(this, kWindowEnabledOffset) & 1U) == 0) {
        return 0;
    }

    auto* window = Load<int*>(this, kWindowOffset);
    if (CVR::Initialized()) {
        CVR::RefreshRTSize();
    }

    const auto prior_active = Load<std::uint16_t>(this, kWindowActiveOffset);
    if (CGS::DisplayCount() == 0) {
        Store<std::uint32_t>(this, kResizeModeOffset, 0);
    }

    const bool is_virtual = CSysWindow::IsVirtual(window);
    Store<std::uint16_t>(this, kWindowActiveOffset,
                         CSysWindow::IsActive(window));

    if ((Load<std::uint8_t>(this, kWindowModeOffset) & 0x08U) == 0 &&
        Load<std::int32_t>(this, 0x16db4) != 0 &&
        Load<std::int32_t>(this, 0x16dc4) != 0) {
        CQuad extents{};
        CSysWindow::WindowExtents(window, &extents);

        const auto previous_mode = Load<std::int32_t>(this, kResizeModeOffset);
        std::uint32_t mode = static_cast<std::uint32_t>(previous_mode);
        if (previous_mode == 1) {
            if (Load<std::uint16_t>(this, kWindowActiveOffset) == 0) {
                mode = 0;
                Store<std::uint32_t>(this, kResizeModeOffset, 0);
            } else {
                mode = 1;
            }
        }

        const auto width = ConvertUnsigned(extents.right - extents.left);
        const auto height = ConvertUnsigned(extents.bottom - extents.top);
        auto* canvas = reinterpret_cast<CCanvasDisplay*>(
            reinterpret_cast<std::byte*>(this) + sizeof(void*));
        if (!is_virtual && width != 0 && height != 0 &&
            canvas->NeedsResize(width, height, mode)) {
            auto** vtable = *reinterpret_cast<void***>(this);
            auto callback = reinterpret_cast<ResizeCallback>(
                vtable[0x30 / sizeof(void*)]);
            const auto result = callback(this, width, height, mode);
            if (result != 0) {
                return result;
            }
        }

        Store<std::int32_t>(this, kResizeModeOffset, previous_mode);
    }

    if (prior_active == 0 ||
        Load<std::int32_t>(this, kResizeModeOffset) != 1) {
        return 0;
    }

    if (Load<std::uint16_t>(this, kWindowActiveOffset) == 0) {
        CSysWindow::MinimizeWindow(window);
    }
    return 0;
}

}  // namespace NRadEngine
