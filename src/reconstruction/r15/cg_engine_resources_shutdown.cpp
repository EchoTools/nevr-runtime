// NRadEngine::CGEngineResources::Shutdown
// Quest libr15.so VA 0x19406b0; ReVault binary 576.
// ReVault raw parent 100257299, reconstruction node 100600738; grade M: all 984 instructions of the listing (0x19406b0-0x194160c) were executed by an AArch64 mini-simulator over 21 runs (0 unexecuted) and the file, syntax-checked clean and run against stub callees, gave the identical 109-call trace, returned status and final 0x2900-byte object in 23 runs (15 forced callee failures, 2 statuses with only the upper word set, 5 flag and count variants) while 4 planted bugs turned it red, with 119 bl, 0 blr, 0 brk, 27 cbz/cbnz and 10 tbz/tbnz accounted for and the inlined CGSkin::Shutdown (0x1942468) matched to its standalone listing; M because 1 callee (0x1988a98) stays the FUN_01988a98 placeholder, no global is touched, the object layouts are raw offsets and the callee bodies were not compared.
// @0x19406b0 — NRadEngine::CGEngineResources::Shutdown
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "cg_raw_interface_vk.hpp"

// Overview. Shutdown releases the default GPU resources that Initialize (0x193e52c) builds, in the order of
// the listing (offsets are into the CGEngineResources object; the roles come from the Initialize
// reconstruction, cg_engine_resources_initialize.cpp):
//   - four vertex/index buffer sets: solid sphere 0x1b0/0xb50, solid cone 0x418/0xc18, quad 0x680/0xce0 and
//     solid box 0x8e8/0xda8 (each set: the CGVertexFormat 0xb0 bytes after the vertex buffer is reset, the
//     vertex view is cleared, then each buffer that holds a resource is cleaned up and its fields reset);
//   - the five material blocks at +0x0, +0x90, +0x120, +0xe80 and +0xf10 (CResourceInstance::Shutdown when
//     bit 1 of the flag word at +0x10 is set, CGMaterial::Unload(1) when bit 0 is) and the three
//     CResourceInstance::UnloadAndShutdown calls at +0xfa0, +0x1030 and +0x10c0;
//   - the default textures (2D 0x13a8 and 0x1400, 3D 0x1458, 3D shadow 0x14b0, ambient cube 0x1508), the
//     ambient_spec_box_default buffer 0x1560 and the instancing constants 0x1dc8, then the lightmap images
//     (0x1610, 0x1668, 0x16c0, 0x1718, 0x1770) and two more UnloadAndShutdown calls (+0x1cf0, +0x1d08);
//   - the record tables are rewritten with default images: 0xa8 bytes at +0x1d20, five records at +0x1e78
//     and seven at +0x17c8 (built in the listing's own stack frame);
//   - the irradiance, light-map UV and two particle buffers (0x1a30, 0x1ae0, 0x1b90, 0x1c40), the
//     fullscreen triangle and quad sets (0x2090/0x22f8 and 0x23c0/0x2628), the two "Multi-Res Mask IB"
//     buffers (0x26f0, 0x27a0), the jitter textures (0x2850, 0x28a8), CGSkin::Shutdown (+0x12f0, inlined)
//     and, last, CGFog::Shutdown (+0x1150), whose result is the result of the whole function.
// Every failing status returns at once (cbnz w0 -> the epilogue at 0x19415f4); the call to
// CGFog::Shutdown at 0x19415f0 falls into the same epilogue.

// ---------------------------------------------------------------------------------------------------
// FUN_01988a98 has no name in ReVault (it stores FUN_01888a98, the base-0 label of the same function) or in
// the decompiler. Every call here passes kind 0xe and a handle word read from an image record, right
// after CGRawInterfaceVK::DeferredDestroy of that image. It stays a placeholder at global scope.
/* UNRESOLVED: FUN_01988a98 */
extern void FUN_01988a98(std::uint32_t kind, std::uint64_t handle);  // 0x1988a98

namespace NRadEngine {

namespace CMemory {
// NRadEngine::CMemory::Fill @ 0x12cafcc (thunk 0x1047300): a tail jump to memset.
void Fill(void* destination, unsigned char value, std::uint64_t count);
}  // namespace CMemory

// NRadEngine::CGVertexFormat::Reset(), 0x2909268 (thunk 0xfe9330). The object is 0x128 bytes (Initialize).
struct alignas(8) CGVertexFormat {
    unsigned char storage[0x128];
    void Reset();
};
static_assert(sizeof(CGVertexFormat) == 0x128, "CGVertexFormat is 0x128 bytes");

// NRadEngine::CGBuffer::CleanupResources(), 0x192a694 (thunk 0x1033800). The buffer layout is not
// recovered; the fields this function resets are written by offset below.
struct CGBuffer {
    void CleanupResources();
};

// NRadEngine::CGMaterial::Unload(unsigned int), 0x1942350 (thunk 0x1019b10). It is vtable slot 3 of
// CGMaterial (vtable 0x375c868); this function reaches it by a direct bl, not through the vtable. Ghidra
// types it void; the caller tests w0, so the status is an int here.
struct CGMaterial {
    int Unload(std::uint32_t flag);
};

// NRadEngine::CResourceInstance::Shutdown(), 0x26423a0 (thunk 0x103ad80): clears bit 1 of the flag word at
// +0x10 and returns 0 (Ghidra: undefined8). NRadEngine::CResourceInstance::UnloadAndShutdown(), 0x2642230
// (thunk 0xfe3770): a 64-bit status, as declared in cr15_net_customization_renderer_destroy_context.cpp.
struct CResourceInstance {
    std::uint64_t Shutdown();
    std::uint64_t UnloadAndShutdown();
};

// NRadEngine::CGFog::Shutdown(), 0x1942554 (thunk 0x1016ae0); ReVault types it int.
struct CGFog {
    int Shutdown();
};

// NRadEngine::CGSkin::Shutdown(), 0x1942468. ReVault lists no caller of it: the compiler inlined it here.
// The inlined body (59 instructions, 0x1942468-0x1942550) is the stretch 0x1941500-0x19415ec of this
// function, its first instructions interleaved with the end of the 0x28a8 copy (0x19414ec-0x194150c):
// image at skin+0x58, image at skin+0, then the word at skin+0xb0.
struct CGSkin {
    void Shutdown();
};

// CGEngineResources: only the extent this function touches is known (the last record it releases, the image
// at this+0x28a8, ends at 0x2900); every member is reached by raw offset, so the object is modelled as
// aligned byte storage of that extent. The true object size is larger or equal. Shutdown is public here:
// CCanvas::Destroy (0x26ea0e4) calls it through thunk 0xfdce80 and tests the status.
class CGEngineResources {
public:
    // NRadEngine::CGEngineResources::Initialize(), 0x193e52c (cg_engine_resources_initialize.cpp).
    int Initialize();
    // NRadEngine::CGEngineResources::Shutdown(), 0x19406b0 (thunk 0xfdce80). Ghidra types it void; the
    // callers test w0 (Initialize, CCanvas::Destroy), so the status is an int here.
    int Shutdown();

private:
    alignas(8) unsigned char bytes_[0x2900];
};
static_assert(sizeof(CGEngineResources) == 0x2900, "touched extent of CGEngineResources");
static_assert(alignof(CGEngineResources) == 8, "CGEngineResources storage is 8-byte aligned");

namespace {

constexpr std::size_t kFrameBytes = 0x270;  // sub sp, sp, #0x270 at 0x19406c4
constexpr std::size_t kImageBytes = 0x58;   // an image record: 0x50 bytes CMemory::Fill clears, then a tail word
constexpr std::size_t kImageFilled = 0x50;
constexpr std::uint32_t kHandleKind = 0xe;  // w0 of every FUN_01988a98 call

template <typename T>
T Ld(const unsigned char* base, std::size_t offset) {
    T value;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}
template <typename T>
void St(unsigned char* base, std::size_t offset, T value) {
    std::memcpy(base + offset, &value, sizeof value);
}
std::uint64_t Ld64(const unsigned char* base, std::size_t offset) { return Ld<std::uint64_t>(base, offset); }
std::uint32_t Ld32(const unsigned char* base, std::size_t offset) { return Ld<std::uint32_t>(base, offset); }
void St64(unsigned char* base, std::size_t offset, std::uint64_t value) { St<std::uint64_t>(base, offset, value); }
void St32(unsigned char* base, std::size_t offset, std::uint32_t value) { St<std::uint32_t>(base, offset, value); }
void St16(unsigned char* base, std::size_t offset, std::uint16_t value) { St<std::uint16_t>(base, offset, value); }
void St8(unsigned char* base, std::size_t offset, std::uint8_t value) { St<std::uint8_t>(base, offset, value); }

// cbnz w0: only the low 32 bits of a callee's status are tested, and they are what the function returns.
int LowWord(std::uint64_t status) { return static_cast<int>(static_cast<std::uint32_t>(status)); }

// ---- CGBuffer objects (0xb0 bytes each) ----

// The fields the listing resets after CleanupResources: the count at +0x98 (the word the conditional
// resets test), the halfword at +0x9c, the 64-bit word at +0x90, and the bytes +0x9f = 0, +0xa0 = 0x69,
// +0xa2 = 6 (the conditional resets store +0x9f/+0xa0 as one halfword 0x6900, the others as bytes).
void ClearBufferState(unsigned char* self, std::size_t buffer) {
    St32(self, buffer + 0x98, 0);
    St16(self, buffer + 0x9c, 0);
    St64(self, buffer + 0x90, 0);
    St8(self, buffer + 0x9f, 0);
    St8(self, buffer + 0xa0, 0x69);
    St8(self, buffer + 0xa2, 6);
}

// bl CGBuffer::CleanupResources (thunk 0x1033800) with x0 = the buffer, then the field reset.
void ReleaseBuffer(unsigned char* self, std::size_t buffer) {
    reinterpret_cast<CGBuffer*>(self + buffer)->CleanupResources();
    ClearBufferState(self, buffer);
}

// The same, only when the count at +0x98 is nonzero (ldr w8 / cbz w8).
void ReleaseBufferIfHeld(unsigned char* self, std::size_t buffer) {
    if (Ld32(self, buffer + 0x98) != 0) {
        ReleaseBuffer(self, buffer);
    }
}

// The view that follows a vertex buffer's CGVertexFormat (buffer + 0x1d8, 0x90 bytes): the listing clears
// +0x0..+0x60 and +0x68..+0x88 (the words at +0x60 and +0x88 are not touched).
void ClearVertexView(unsigned char* self, std::size_t view) {
    std::memset(self + view, 0, 0x60);
    std::memset(self + view + 0x68, 0, 0x20);
}

// The view at an index buffer + 0xb0: two 64-bit words and one 32-bit word.
void ClearIndexView(unsigned char* self, std::size_t view) {
    St64(self, view, 0);
    St64(self, view + 8, 0);
    St32(self, view + 0x10, 0);
}

// One vertex/index buffer set, in the order the listing runs it: CGVertexFormat::Reset on the format
// 0xb0 bytes after the vertex buffer, clear the vertex view, release the vertex buffer when it holds a
// resource, clear the index view, release the index buffer when it holds a resource.
void ReleaseMeshBuffers(unsigned char* self, std::size_t vertex_buffer, std::size_t index_buffer) {
    reinterpret_cast<CGVertexFormat*>(self + vertex_buffer + 0xb0)->Reset();  // bl 0xfe9330
    ClearVertexView(self, vertex_buffer + 0x1d8);
    ReleaseBufferIfHeld(self, vertex_buffer);
    ClearIndexView(self, index_buffer + 0xb0);
    ReleaseBufferIfHeld(self, index_buffer);
}

// ---- material blocks and resource instances ----

// ldr w8,[block+0x10]; tbz w8,#1: CResourceInstance::Shutdown (thunk 0x103ad80), a nonzero status returns;
// the flag word is read again; tbz w8,#0: CGMaterial::Unload(block, 1) (thunk 0x1019b10), a nonzero status
// returns. A zero status for both falls to the next block.
int ReleaseMaterial(unsigned char* self, std::size_t block) {
    std::uint32_t flags = Ld32(self, block + 0x10);
    if ((flags & 2U) != 0) {
        const int status = LowWord(reinterpret_cast<CResourceInstance*>(self + block)->Shutdown());
        if (status != 0) {
            return status;
        }
        flags = Ld32(self, block + 0x10);
    }
    if ((flags & 1U) != 0) {
        const int status = reinterpret_cast<CGMaterial*>(self + block)->Unload(1);
        if (status != 0) {
            return status;
        }
    }
    return 0;
}

// bl CResourceInstance::UnloadAndShutdown (thunk 0xfe3770), then cbnz w0.
int UnloadAndShutdownAt(unsigned char* self, std::size_t instance) {
    return LowWord(reinterpret_cast<CResourceInstance*>(self + instance)->UnloadAndShutdown());
}

// ---- image records (0x58 bytes) ----

// The default record the listing builds on the stack (movi v0 / stp q0,q0 / str xzr to zero it, bl
// CMemory::Fill(record, 0, 0x50), str xzr over the tail) and copies over the image.
void StoreDefaultImage(unsigned char* self, std::size_t image) {
    unsigned char record[kImageBytes] = {};
    CMemory::Fill(record, 0, kImageFilled);
    St64(record, kImageFilled, 0);
    std::memcpy(self + image, record, kImageBytes);
}

// bl CGRawInterfaceVK::DeferredDestroy (thunk 0x103c3a0) on the image, FUN_01988a98(0xe, [image+0x28]),
// FUN_01988a98(0xe, [image+0x30]), then `resets` times a default record copied over the image (the first
// four textures are rewritten twice, the other images once).
void ReleaseImage(unsigned char* self, std::size_t image, int resets) {
    CGRawInterfaceVK::DeferredDestroy(*reinterpret_cast<SGImage*>(self + image));
    FUN_01988a98(kHandleKind, Ld64(self, image + 0x28));
    FUN_01988a98(kHandleKind, Ld64(self, image + 0x30));
    for (int i = 0; i < resets; ++i) {
        StoreDefaultImage(self, image);
    }
}

// A default record built in place inside the listing's frame: the tail word is zeroed and the first 0x50
// bytes are cleared by bl CMemory::Fill(frame + offset, 0, 0x50).
void ResetFrameImage(unsigned char* frame, std::size_t offset) {
    St64(frame, offset + kImageFilled, 0);
    CMemory::Fill(frame + offset, 0, kImageFilled);
}

}  // namespace

// @0x19406b0 — NRadEngine::CGEngineResources::Shutdown
int CGEngineResources::Shutdown() {
    unsigned char* const self = bytes_;
    alignas(16) unsigned char frame[kFrameBytes];  // the listing's frame: sp .. sp+0x270
    int status = 0;                                // w0 of the status-returning callees

    // ---- 0x19406cc-0x1940928: the first four vertex/index buffer sets ----
    ReleaseMeshBuffers(self, 0x1b0, 0xb50);  // solid sphere: Reset @0x19406d0, cleanups @0x19406f8 @0x1940734
    ReleaseMeshBuffers(self, 0x418, 0xc18);  // solid cone: Reset @0x194075c, cleanups @0x1940798 @0x19407d4
    ReleaseMeshBuffers(self, 0x680, 0xce0);  // quad: Reset @0x19407fc, cleanups @0x1940828 @0x1940864
    ReleaseMeshBuffers(self, 0x8e8, 0xda8);  // solid box: Reset @0x194088c, cleanups @0x19408c8 @0x1940904

    // ---- 0x1940928-0x1940a14: the five material blocks (Shutdown/Unload call sites in the comments) ----
    status = ReleaseMaterial(self, 0x0);  // @0x1940934, @0x194094c
    if (status != 0) {                    // cbnz w0 @0x1940938, @0x1940950
        return status;
    }
    status = ReleaseMaterial(self, 0x90);  // @0x1940964, @0x194097c
    if (status != 0) {                     // cbnz w0 @0x1940968, @0x1940980
        return status;
    }
    status = ReleaseMaterial(self, 0x120);  // @0x1940994, @0x19409ac
    if (status != 0) {                      // cbnz w0 @0x1940998, @0x19409b0
        return status;
    }
    status = ReleaseMaterial(self, 0xe80);  // @0x19409c4, @0x19409dc
    if (status != 0) {                      // cbnz w0 @0x19409c8, @0x19409e0
        return status;
    }
    status = ReleaseMaterial(self, 0xf10);  // @0x19409f4, @0x1940a0c
    if (status != 0) {                      // cbnz w0 @0x19409f8, @0x1940a10
        return status;
    }

    // ---- 0x1940a14-0x1940a3c: three resource instances ----
    status = UnloadAndShutdownAt(self, 0xfa0);  // bl @0x1940a18
    if (status != 0) {                          // cbnz w0 @0x1940a1c
        return status;
    }
    status = UnloadAndShutdownAt(self, 0x1030);  // bl @0x1940a28
    if (status != 0) {                           // cbnz w0 @0x1940a2c
        return status;
    }
    status = UnloadAndShutdownAt(self, 0x10c0);  // bl @0x1940a38
    if (status != 0) {                           // cbnz w0 @0x1940a3c
        return status;
    }

    // ---- 0x1940a40-0x1940d98: the default textures ----
    ReleaseImage(self, 0x13a8, 2);  // 2D texture, black texel: Destroy @0x1940a54, Fill @0x1940a90 @0x1940ad4
    ReleaseImage(self, 0x1400, 2);  // 2D texture, white texel: Destroy @0x1940b04, Fill @0x1940b40 @0x1940b8c
    ReleaseImage(self, 0x1458, 2);  // "Default 3D Texture": Destroy @0x1940bc4, Fill @0x1940c00 @0x1940c44
    ReleaseImage(self, 0x14b0, 2);  // "Default 3D Shadow Texture": Destroy @0x1940c74, Fill @0x1940cb0 @0x1940cfc
    ReleaseImage(self, 0x1508, 1);  // ambient cube texture: Destroy @0x1940d34, Fill @0x1940d70

    // ---- 0x1940d9c-0x1940dc8: the ambient_spec_box_default buffer and the instancing constants ----
    ReleaseBuffer(self, 0x1560);  // cleanup @0x1940d9c, fields reset @0x1940db0-0x1940dc4
    ReleaseBuffer(self, 0x1dc8);  // cleanup @0x1940dc8, fields reset @0x1940dd0-0x1940dec

    // ---- 0x1940df0-0x1941020: the lightmap images ----
    ReleaseImage(self, 0x1610, 1);  // gi_lightmap_default: Destroy @0x1940df0, Fill @0x1940e2c
    ReleaseImage(self, 0x1668, 1);  // ao0_lightmap_default: Destroy @0x1940e64, Fill @0x1940ea0
    ReleaseImage(self, 0x16c0, 1);  // ao0_lightmap_default again: Destroy @0x1940ed0, Fill @0x1940f0c
    ReleaseImage(self, 0x1718, 1);  // baked_shadow_map_default: Destroy @0x1940f44, Fill @0x1940f80
    ReleaseImage(self, 0x1770, 1);  // baked_shadow_map_default again: Destroy @0x1940fb0, Fill @0x1940fec

    // ---- 0x1941020-0x1941034: the two instances Initialize loads at +0x1cf0 and +0x1d08 ----
    status = UnloadAndShutdownAt(self, 0x1cf0);  // bl @0x1941020
    if (status != 0) {                           // cbnz w0 @0x1941024
        return status;
    }
    status = UnloadAndShutdownAt(self, 0x1d08);  // bl @0x1941030
    if (status != 0) {                           // cbnz w0 @0x1941034
        return status;
    }

    // ---- 0x1941038-0x194120c: default records rewritten from the listing's frame ----
    // The first two records: the image copied at +0x1d20 by Initialize and the 0x50 bytes after it.
    std::memset(frame, 0, 0xa8);                   // movi v0 / stp q0,q0 x5 / str xzr [sp,#0xa0]
    CMemory::Fill(frame, 0, kImageFilled);         // bl @0x194106c
    St64(frame, 0x50, 0);                          // str xzr [sp,#0x50] @0x1941080
    CMemory::Fill(frame + 0x58, 0, kImageFilled);  // bl @0x1941084
    std::memcpy(self + 0x1d20, frame, 0xa8);       // ldp/stp copy @0x1941088-0x19410d8 to +0x1d20..+0x1dc8
    // Five records, copied to +0x1e78 (the lightmap table: gi, ao0, ao0, baked shadow, baked shadow).
    ResetFrameImage(frame, 0x00);              // Fill @0x19410dc
    ResetFrameImage(frame, 0x58);              // Fill @0x19410f0
    ResetFrameImage(frame, 0xb0);              // Fill @0x1941108
    ResetFrameImage(frame, 0x108);             // Fill @0x1941120
    ResetFrameImage(frame, 0x160);             // Fill @0x1941138
    std::memcpy(self + 0x1e78, frame, 0x1b8);  // bl @0x1941150 (memcpy thunk 0x10476e0)
    // Seven records, copied to +0x17c8 (the table of image records Initialize fills from the 3D textures).
    std::memset(frame, 0, 0x268);  // bl @0x1941160 (memset thunk 0xfe71f0)
    ResetFrameImage(frame, 0x00);  // Fill @0x1941170
    ResetFrameImage(frame, 0x58);  // Fill @0x1941184
    ResetFrameImage(frame, 0xb0);  // Fill @0x1941198
    ResetFrameImage(frame, 0x108);  // Fill @0x19411ac
    ResetFrameImage(frame, 0x160);  // Fill @0x19411c0
    ResetFrameImage(frame, 0x1b8);  // Fill @0x19411d4
    ResetFrameImage(frame, 0x210);  // Fill @0x19411e8
    std::memcpy(self + 0x17c8, frame, 0x268);  // bl @0x1941200

    // ---- 0x194120c-0x1941284: irradiance constants, light-map UVs and the two particle buffers ----
    ReleaseBuffer(self, 0x1a30);  // "Default Irradiance Constants": cleanup @0x194120c
    ReleaseBuffer(self, 0x1ae0);  // "Default Light Map UVs": cleanup @0x1941238
    ReleaseBuffer(self, 0x1b90);  // "Default Particle Mesh Emitter Tris": cleanup @0x194125c
    ReleaseBuffer(self, 0x1c40);  // the second particle buffer: cleanup @0x1941280

    // ---- 0x1941284-0x19413d4: the fullscreen triangle and quad sets ----
    ReleaseMeshBuffers(self, 0x2090, 0x22f8);  // triangle: Reset @0x19412a4, cleanups @0x19412d8 @0x1941310
    ReleaseMeshBuffers(self, 0x23c0, 0x2628);  // quad: Reset @0x194133c, cleanups @0x1941370 @0x19413b0

    // ---- 0x19413d4-0x194140c: the two "Multi-Res Mask IB" index buffers ----
    ReleaseBuffer(self, 0x26f0);  // cleanup @0x19413dc
    ReleaseBuffer(self, 0x27a0);  // cleanup @0x1941408

    // ---- 0x194140c-0x194150c: the jitter textures ----
    ReleaseImage(self, 0x2850, 1);  // jitter_texture_1d_4: Destroy @0x1941430, Fill @0x194146c
    ReleaseImage(self, 0x28a8, 1);  // jitter_texture_1d_8: Destroy @0x19414a4, Fill @0x19414e0

    // ---- 0x1941500-0x19415ec: CGSkin::Shutdown (0x1942468) inlined, skin at +0x12f0 ----
    // Destroy @0x1941510 and Fill @0x194154c (skin+0x58 = 0x1348), Destroy @0x194157c and Fill @0x19415b8
    // (skin+0 = 0x12f0), and the word at skin+0xb0 = 0x13a0 cleared by str xzr [x19,#0x13a0] @0x19415e0.
    reinterpret_cast<CGSkin*>(self + 0x12f0)->Shutdown();

    // ---- 0x19415c4-0x19415f0: the fog, whose status is returned ----
    return reinterpret_cast<CGFog*>(self + 0x1150)->Shutdown();  // bl @0x19415f0, then the epilogue and ret
}

}  // namespace NRadEngine
