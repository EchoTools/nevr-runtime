// CGEngineResources::Initialize (partial: default 3D texture upload evidence)
// Quest libr15.so VA 0x193e52c (RVA 0x192e52c); PC echovr.exe: no matching function located.
// ReVault binary 576, raw parent 100257298, reconstruction node 100600195; tier: reconstructed; grade: L, assessed by revault-codex.
// @0x193e52c — NRadEngine::CGEngineResources::Initialize
// tier: reconstructed; grade: L, assessed by revault-codex
// Partial evidence reconstruction only: default 3D texture upload source selection.
// The rest of Initialize remains represented by raw parent 100257298.
/* @confidence: L — partial evidence note, not a function body; not in the nevr_r15_reconstruction target. */
#include <cstddef>
#include <cstdint>

namespace NRadEngine {
struct CGEngineResourcesInitializeUploadEvidence {
    static constexpr std::ptrdiff_t default_3d_texture = 0x1458;
    static constexpr std::ptrdiff_t default_2d_texture = 0x13a8;
    static constexpr std::uint32_t rgba8_format = 9;
    static constexpr std::ptrdiff_t stack_texel = 0x45c;
    static constexpr std::uintptr_t upload_site = 0x193f2d0;
};
} // namespace NRadEngine

// At the upload site, this+0x1458 receives RGBA8 data from [sp+0x45c].
// The default 2D texture at this+0x13a8 is separate. The shipping R27 patch
// redirects only the 3D upload's source pointer; R28 is a white alternative and
// R30 changes shared/diagnostic texture data. No return value or remaining
// initialization behavior is inferred here.
