#pragma once

#include <cstddef>
#include <cstdint>

namespace NRadEngine {

struct CQuad {
    float left;
    float top;
    float right;
    float bottom;
};

struct CCanvasDisplay {
    bool NeedsResize(std::uint32_t width, std::uint32_t height,
                     std::uint32_t mode);
    int Resize(std::uint32_t* width, std::uint32_t* height,
               std::uint32_t mode);
};

class CEngine {
public:
    std::uint32_t CheckForResize();
    int Resize(std::uint32_t width, std::uint32_t height, std::uint32_t mode);

private:
    // Opaque engine storage through the highest field used by these methods.
    std::byte opaque_[0x16ed8];
};

}  // namespace NRadEngine
