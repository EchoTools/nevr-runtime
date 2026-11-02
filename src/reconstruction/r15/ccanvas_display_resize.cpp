// NRadEngine::CCanvasDisplay::Resize
// Quest libr15.so VA 0x26edfb4; ReVault binary 576.
// ReVault raw parent 100323515, reconstruction node 100600781; grade H: all 46 instructions 0x26edfb4-0x26ee068 (through the ret) are mapped in the body comments; 0 placeholders (every callee is a ReVault name and no DAT_ global is read); CCanvasDisplay members are raw offsets 0x8, 0x13a60, 0x16d48, 0x16d4c because ReVault has no layout for the class.
// @0x26edfb4 — NRadEngine::CCanvasDisplay::Resize
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ce_engine.hpp"

namespace NRadEngine {

enum EWindowMode : std::uint32_t;

// CCanvasDisplay (vtable 0x37ac500) derives from CCanvas at +0x0 (ReVault class_hierarchy). The merged
// ce_engine.hpp declares CCanvasDisplay without that base, so the two direct base-class calls below go
// through a CCanvas pointer to the same address.
class CCanvas {
public:
    // CCanvas vslot 6 @0x26edc0c; CCanvasDisplay does not override it. Called by a direct bl (not through
    // the vtable): x0 = this, result tested in w0.
    int BeforeResize();

    // CCanvas vslot 7 @0x26edcb4. The call at 0x26edff8-0x26ee004 loads only x0, x1, x2 (w3 is not
    // reloaded after the earlier calls) and the 76 instructions of 0x26edcb4-0x26edde0 never read w3, so
    // the callee takes the two extents; the demangled name in ReVault also lists an EWindowMode.
    int Resize(std::uint32_t&, std::uint32_t&);
};

// CGDisplay is the sub-object at CCanvasDisplay + 0x13a60; Resize is CGDisplay vslot 4 @0x1934748.
class CGDisplay {
public:
    int Resize(std::uint32_t&, std::uint32_t&, EWindowMode);
};

// CGRenderer is the sub-object at CCanvasDisplay + 0x8; SetCameraCut @0x19957b4 is
// `ldr x8,[x0,#0x1268]; madd x8,x1,#0x1730,x8; ldr/orr w2/str [x8,#0x1720]`.
class CGRenderer {
public:
    void SetCameraCut(unsigned long long, unsigned int);
};

// Static accessors of the render configuration: MSAAMode @0x198c894 returns the word at 0x389c080,
// SplitScreenMode @0x198cbf8 the word at 0x389c0fc, NumActiveViewports @0x1928e74 loads the 32-bit
// entry of the table at 0x2e6e4b0 indexed by that same word; each returns an unsigned 32-bit value in w0.
class CGRenderConfig {
public:
    static std::uint32_t MSAAMode();
    static std::uint32_t SplitScreenMode();
    static std::uint32_t NumActiveViewports();
};

namespace {

// ReVault has no layout for CCanvasDisplay, so its own members are written at raw byte offsets
// (the convention of ce_engine_resize.cpp).
template <typename T>
void Store(void* base, std::size_t offset, T value) {
    std::memcpy(static_cast<std::byte*>(base) + offset, &value, sizeof(value));
}

}  // namespace

// @0x26edfb4 — NRadEngine::CCanvasDisplay::Resize
/* @confidence: H */
int CCanvasDisplay::Resize(std::uint32_t* width, std::uint32_t* height,
                           std::uint32_t mode) {
    // 0x26edfb4-0x26edfcc: save x22/x21/x20/x19/x30; w22 = mode, x20 = height, x21 = width, x19 = this.
    auto* const canvas = reinterpret_cast<CCanvas*>(this);  // CCanvas base at +0x0
    auto* const display = reinterpret_cast<CGDisplay*>(
        reinterpret_cast<std::byte*>(this) + 0x13a60);  // w8 = 0x13a60 (mov/movk); add x0, x19, x8
    auto* const renderer = reinterpret_cast<CGRenderer*>(
        reinterpret_cast<std::byte*>(this) + 0x8);  // add x19, x19, #0x8

    // 0x26edfd0-0x26edfd4: bl CCanvas::BeforeResize (this); cbnz w0 -> epilogue returning w0.
    int result = canvas->CCanvas::BeforeResize();
    if (result != 0) {
        return result;
    }

    // 0x26edfd8-0x26edff4: x0 = this + 0x13a60, x1 = width, x2 = height, w3 = mode;
    // bl CGDisplay::Resize; cbnz w0 -> epilogue returning w0.
    result = display->Resize(*width, *height, static_cast<EWindowMode>(mode));
    if (result != 0) {
        return result;
    }

    // 0x26edff8-0x26ee008: x0 = this, x1 = width, x2 = height; bl CCanvas::Resize; cbnz w0 -> epilogue.
    result = canvas->CCanvas::Resize(*width, *height);
    if (result != 0) {
        return result;
    }

    // 0x26ee00c-0x26ee024: x20 = this + 0x16d48; [+0x16d48] = CGRenderConfig::MSAAMode();
    // [+0x16d4c] = CGRenderConfig::SplitScreenMode(); both 32-bit stores (str w0,[x20]; str w0,[x20,#4]).
    Store<std::uint32_t>(this, 0x16d48, CGRenderConfig::MSAAMode());         // MSAA mode in effect
    Store<std::uint32_t>(this, 0x16d4c, CGRenderConfig::SplitScreenMode());  // split-screen mode in effect

    // 0x26ee028-0x26ee054: bl CGRenderConfig::NumActiveViewports; cbz w0 -> epilogue returning w0 (0).
    // Otherwise x20 = 0 (64-bit counter) and the loop body is
    //   w2 = 1; x0 = this + 8; x1 = x20; bl CGRenderer::SetCameraCut; x20 += 1;
    //   bl CGRenderConfig::NumActiveViewports; cmp x20, w0, uxtw; b.cc (unsigned less-than) -> body.
    for (unsigned long long viewport = 0; viewport < CGRenderConfig::NumActiveViewports(); ++viewport) {
        renderer->SetCameraCut(viewport, 1);
    }

    // 0x26ee058: mov w0, wzr; 0x26ee05c-0x26ee068: restore x19-x22/x30, ret.
    return 0;
}

}  // namespace NRadEngine
