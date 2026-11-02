// NRadEngine::CGEngineResources::Initialize
// Quest libr15.so VA 0x193e52c; ReVault binary 576.
// ReVault raw parent 100257298, reconstruction node 100600629; grade M: every instruction checked against the listing in a full four-shard pass, 0 mismatches fixed (not checkable: the stored values of the runtime-initialised key words 0x389b4c8 0x389b4d0 0x389b4d8 0x389b4e0 lie past the binary content, only which word each buffer uses was checked); the 126 bl, 41 blr, 34 branches and 3 traps of the listing are matched by address, the four views quad/sphere/cone/box read [sp+0x5c8], [sp+0x5d0] and [sp+0x5d8] before any store (the frame is zero-initialised here, the original reads stale stack); M because 4 callees (0x19dad8c 0x19daf90 0x19db678 0x19421f8), 5 callback functions (0x19dc8c0 0x19dc91c 0x19dc978 0x19dc9cc 0x19dc9d0) and 13 globals (0x389b184 0x389b188 0x389b228 0x389b4c8 0x389b4d0 0x389b4d8 0x389b4e0 0x3853780 0x38537a0 0x3875688 0x3875690 0x38755d8 0x3875fd8) stay FUN_/DAT_ placeholders and the object layouts are raw offsets.
// @0x193e52c — NRadEngine::CGEngineResources::Initialize
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "cmemory_context.hpp"

// Overview. Initialize builds the engine's default GPU resources inside the CGEngineResources object:
//   - Shutdown, CGFog, CGSkin, then three CResourceInstance loads (this+0xe80, +0xfa0, +0x10c0);
//   - a quad, a solid sphere, a solid cone and a solid box as vertex/index buffer pairs;
//   - the default 2D/3D textures, the table of 0x58-byte image records copied from them, the default
//     irradiance / light-map / particle / instancing buffers and the ambient cube texture;
//   - the gi, ao0 and baked-shadow default lightmaps (each probed through the graphics function table);
//   - a fullscreen triangle and quad, the two "Multi-Res Mask IB" index buffers and the two jitter textures.
// Every failing step returns the callee's status (w0); the normal end returns 0 (b @0x19406ac -> ret).
// The listing's own stack frame is modelled as `frame` (offsets are the listing's [sp, #off]) because
// the compiler reuses the same stack words for the vertex format, the image descriptors and the
// buffer-init records; only the words this function writes and reads are touched.

namespace NRadEngine {

struct C3Vector;  // 12-byte vertex position (3 floats); only passed by pointer

// A 64-bit symbol key. String() is CSymbol64::String() const, 0x12ccb48 (thunk 0x1038ec0); the
// recorded tree declaration (set_material_property_node_update.cpp) is the same.
struct CSymbol64 {
    std::uint64_t key;
    const char* String() const;
};
static_assert(sizeof(CSymbol64) == 8, "CSymbol64 is one 64-bit key");

// Buffer init records: the objects at this+0x680 etc. are passed by address; their layouts are not
// recovered, so the five kinds stay opaque.
struct SGVertexBufferInit;
struct SGIndexBufferInit;
struct SGConstantBufferInit;
struct SGTypedBufferInit;
struct SGStructuredBufferInit;

// NRadEngine::CGFog::Initialize(), 0x1941610 (thunk 0xff33f0). Ghidra types it undefined4; the caller
// tests w0 (cbnz).
struct CGFog {
    int Initialize();
};

// NRadEngine::CGSkin::Initialize(), 0x1941a8c (thunk 0x103bec0). The caller ignores any result.
struct CGSkin {
    void Initialize();
};

// NRadEngine::CResourceInstance::LoadAndInit(NRadEngine::CSymbol64, unsigned int), 0x26423b4
// (thunk 0xfd6c90). It calls vtable slot +0x10 and, when that returns 0, tail-calls slot +0x20; the
// caller tests w0.
struct CResourceInstance {
    int LoadAndInit(CSymbol64 key, unsigned int flag);
};

// NRadEngine::CGVertexFormat, 0x128 bytes. The constructor (0x2909098, thunk 0x102ad40) ends by
// storing 0 at +0x120 (the element count); this function later clears the format by storing 0 at
// +0x120 itself (str xzr,[sp,#0x580]) and appends elements again.
struct alignas(8) CGVertexFormat {
    unsigned char storage[0x128];
    CGVertexFormat();
    // AppendElement(EUsage, EType, unsigned long long, unsigned long long, unsigned long long,
    // unsigned int), 0x290a4c8 (thunk 0x10348e0). EUsage/EType enumerators are not recovered.
    void AppendElement(std::uint32_t usage, std::uint32_t type, std::uint64_t arg3,
                       std::uint64_t arg4, std::uint64_t arg5, std::uint32_t arg6);
    // AppendElementWithAlias(EUsage, EUsage, EType, ull, ull, ull, unsigned int), 0x290a694
    // (thunk 0x1008350).
    void AppendElementWithAlias(std::uint32_t usage, std::uint32_t alias_usage, std::uint32_t type,
                                std::uint64_t arg4, std::uint64_t arg5, std::uint64_t arg6,
                                std::uint32_t arg7);
    // VertexSize(unsigned long long) const, 0x2909538 (thunk 0x1036240); the argument is 0 here.
    std::uint64_t VertexSize(std::uint64_t stream) const;
};
static_assert(sizeof(CGVertexFormat) == 0x128, "CGVertexFormat is 0x128 bytes");

struct CGShape {
    // 0x2906a8c (thunk 0xfe8340) and 0x2906a44 (thunk 0x1049450); both are called with 8.
    static int NumVerticesForSolidSphere(unsigned long long segments);
    static int NumIndicesForSolidSphere(unsigned long long segments);
    // 0x2905078 (thunk 0xffc0d0)
    static void SetupIndexedSolidSphere(std::uint16_t& first, std::uint16_t& second,
                                        C3Vector* vertices, std::uint16_t* indices, float* extent,
                                        unsigned long long segments);
    // 0x2905c70 (thunk 0x101ecf0)
    static void SetupIndexedSolidCone(std::uint16_t& first, std::uint16_t& second,
                                      C3Vector* vertices, std::uint16_t* indices, float* extent);
    // 0x2903088 (thunk 0x1011ce0)
    static void SetupIndexedSolidBox(std::uint16_t& first, std::uint16_t& second,
                                     C3Vector* vertices, std::uint16_t* indices);
};

struct CGSurfaceManager {
    // CreateCubeTexture(unsigned long long, ETextureFormat, unsigned long long, void const*,
    // unsigned long long&, unsigned long long, unsigned int, CAllocator&, char const*), 0x1941fdc
    // (thunk 0x102c9b0). ReVault types it void; the 0x58-byte image record comes back through the sret
    // pointer (x8), passed here as the first argument; the name is the stack argument.
    static void CreateCubeTexture(unsigned char* image_record, std::uint64_t arg1, int format,
                                  std::uint64_t arg3, const void* data, std::uint64_t& size,
                                  std::uint64_t arg6, std::uint32_t arg7, CAllocator& allocator,
                                  const char* name);
};

// NRadEngine::CGBuffer::Initialize overloads. The recorded signatures read
// Initialize(SG*BufferInit const&, CAllocator&) and Ghidra types the two parameters in that order,
// but the listing passes x0 = the buffer object inside CGEngineResources and x1 = the init record
// built on the stack (size/count at +0, flags at +8..+0x1b, data pointer at +0x20, name at +0x28).
struct CGBuffer {
    static void Initialize(SGVertexBufferInit* buffer, CAllocator* init);      // 0x192c2b4, thunk 0x1003d60
    static void Initialize(SGIndexBufferInit* buffer, CAllocator* init);       // 0x192c428, thunk 0x1006b00
    static void Initialize(SGConstantBufferInit* buffer, CAllocator* init);    // 0x192c178, thunk 0x102a980
    static void Initialize(SGTypedBufferInit* buffer, CAllocator* init);       // 0x192aab4, thunk 0x101da70
    static void Initialize(SGStructuredBufferInit* buffer, CAllocator* init);  // 0x192a948, thunk 0xff67a0
};

}  // namespace NRadEngine

// ---------------------------------------------------------------------------------------------------
// Names ReVault and the decompiler do not have. Each stays a placeholder at global scope.

// libgcc emulated TLS; ReVault stores the 16-byte stub at 0x1021c10 as FUN_00f21c10, the decompiler
// names it __emutls_get_address. Argument: the control block.
extern "C" void* __emutls_get_address(void* control);

// UNRESOLVED callees (direct bl; the decompiler names them FUN_<va>, ReVault stores the same
// functions as FUN_018dad8c, FUN_018daf90, FUN_018db678 and FUN_018421f8 - the ReVault names carry
// the base-0 address).
/* UNRESOLVED: FUN_019dad8c */
void FUN_019dad8c(void* create_info, const void* image_desc, int flag);  // 0x19dad8c; called as (scratch, desc, 0)
/* UNRESOLVED: FUN_019daf90 */
void FUN_019daf90(void* image, const void* create_info, std::uint32_t format, std::uint32_t arg3,
                  std::uint32_t arg4, const void* pixels, const char* name);  // 0x19daf90; builds the image into image
/* UNRESOLVED: FUN_019db678 */
void FUN_019db678(void* image_record, const void* image_desc);  // 0x19db678; 3D image record from its desc
/* UNRESOLVED: FUN_019421f8 */
void FUN_019421f8(std::uint64_t count, void* arena);  // 0x19421f8; (4 or 8, {allocator, result, 0, 0}) fills result

// UNRESOLVED allocation-callback functions: only their addresses are stored (into the thread-local
// callback block at __emutls DAT_038537a0 + 8 .. + 0x28).
/* UNRESOLVED: FUN_019dc8c0 */
void FUN_019dc8c0();  // 0x19dc8c0 (ReVault FUN_018dc8c0)
/* UNRESOLVED: FUN_019dc91c */
void FUN_019dc91c();  // 0x19dc91c (ReVault FUN_018dc91c)
/* UNRESOLVED: FUN_019dc978 */
void FUN_019dc978();  // 0x19dc978 (ReVault FUN_018dc978)
/* UNRESOLVED: FUN_019dc9cc */
void FUN_019dc9cc();  // 0x19dc9cc (ReVault FUN_018dc9cc), a bare ret
/* UNRESOLVED: FUN_019dc9d0 */
void FUN_019dc9d0();  // 0x19dc9d0 (ReVault FUN_018dc9d0), a bare ret

// UNRESOLVED globals.
/* UNRESOLVED: DAT_0389b184 */
extern std::uint8_t DAT_0389b184;  // 0x389b184, one-shot "NotImplemented" flag, line 0x1ab
/* UNRESOLVED: DAT_0389b188 */
extern std::uint8_t DAT_0389b188;  // 0x389b188, one-shot "NotImplemented" flag, line 0x1bb
/* UNRESOLVED: DAT_0389b228 */
extern NRadEngine::CSymbol64 DAT_0389b228;  // 0x389b228, key of the LoadAndInit at this+0x1d08
/* UNRESOLVED: DAT_0389b4c8 */
extern NRadEngine::CSymbol64 DAT_0389b4c8;  // 0x389b4c8, name key of the sphere index buffer and vertex buffer
/* UNRESOLVED: DAT_0389b4d0 */
extern NRadEngine::CSymbol64 DAT_0389b4d0;  // 0x389b4d0, name key of the cone buffers
/* UNRESOLVED: DAT_0389b4d8 */
extern NRadEngine::CSymbol64 DAT_0389b4d8;  // 0x389b4d8, name key of the quad buffers
/* UNRESOLVED: DAT_0389b4e0 */
extern NRadEngine::CSymbol64 DAT_0389b4e0;  // 0x389b4e0, name key of the box buffers
/* UNRESOLVED: DAT_03853780 */
extern unsigned char DAT_03853780[];  // 0x3853780, emulated-TLS control block of the {file, line} record
/* UNRESOLVED: DAT_038537a0 */
extern unsigned char DAT_038537a0[];  // 0x38537a0, emulated-TLS control block of the callback block
// The graphics function table the listing reaches through the GOT slot 0x380f360 (table base
// 0x3875488): +0x150, +0x200, +0x208 and +0xb50.
/* UNRESOLVED: DAT_03875688 */
extern void (*DAT_03875688)(void* device, const void* create_info, const void* callbacks,
                            void* out_handle);  // 0x3875688 = table +0x200: create an image from its create info
/* UNRESOLVED: DAT_038755d8 */
extern void (*DAT_038755d8)(void* device, std::uint64_t handle,
                            void* out_requirements);  // 0x38755d8 = table +0x150: query a 24-byte requirements block
/* UNRESOLVED: DAT_03875690 */
extern void (*DAT_03875690)(void* device, std::uint64_t handle,
                            const void* callbacks);  // 0x3875690 = table +0x208: destroy the probe image
/* UNRESOLVED: DAT_03875fd8 */
extern void* DAT_03875fd8;  // 0x3875fd8 = table +0xb50: the device handle

namespace NRadEngine {

// CGEngineResources: only the extent this function touches is known (the last record it hands out,
// the image at this+0x28a8, ends at 0x2900); every member is reached by raw offset, so the object is
// modelled as aligned byte storage of that extent. The true object size is larger or equal.
class CGEngineResources {
public:
    // NRadEngine::CGEngineResources::Initialize(), 0x193e52c (called from Parse @0x1048640).
    int Initialize();

private:
    // NRadEngine::CGEngineResources::Shutdown(), 0x19406b0 (thunk 0xfdce80). Ghidra types it void;
    // Initialize tests w0, so the status is an int here.
    int Shutdown();

    alignas(8) unsigned char bytes_[0x2900];
};
static_assert(sizeof(CGEngineResources) == 0x2900, "touched extent of CGEngineResources");

namespace {

constexpr std::size_t kFrameBytes = 0x610;    // listing's frame: [sp, #0x0 .. #0x610)
constexpr std::size_t kVertexFormat = 0x460;  // the CGVertexFormat object, later the image descriptor

using AllocateFn = void* (*)(CAllocator* allocator, std::size_t size);  // CAllocator vtable slot +0x10
using FreeFn = void (*)(CAllocator* allocator, void* pointer);          // CAllocator vtable slot +0x38

// A 16-byte value stored as four 32-bit words.
struct Quad {
    std::uint32_t w[4];
};

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
std::uint8_t Ld8(const unsigned char* base, std::size_t offset) { return Ld<std::uint8_t>(base, offset); }
void St64(unsigned char* base, std::size_t offset, std::uint64_t value) { St<std::uint64_t>(base, offset, value); }
void St32(unsigned char* base, std::size_t offset, std::uint32_t value) { St<std::uint32_t>(base, offset, value); }
void St16(unsigned char* base, std::size_t offset, std::uint16_t value) { St<std::uint16_t>(base, offset, value); }
void St4(unsigned char* base, std::size_t offset, std::uint32_t a, std::uint32_t b, std::uint32_t c,
         std::uint32_t d) {
    const Quad quad = {{a, b, c, d}};
    St<Quad>(base, offset, quad);
}
// Copies n bytes inside the frame or the object (str/ldr q pairs in the listing).
void Move(unsigned char* dst, std::size_t dst_offset, const unsigned char* src, std::size_t src_offset,
          std::size_t n) {
    std::memmove(dst + dst_offset, src + src_offset, n);
}
// One float (str s) and a pair of floats (str d) of the listing.
void StF(unsigned char* base, std::size_t offset, float value) { St<float>(base, offset, value); }
void StF2(unsigned char* base, std::size_t offset, float first, float second) {
    const float pair[2] = {first, second};
    std::memcpy(base + offset, pair, sizeof pair);
}
std::uint64_t Addr(const void* pointer) { return reinterpret_cast<std::uintptr_t>(pointer); }
const void* AsPointer(std::uint64_t address) { return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)); }
const char* AsString(std::uint64_t address) { return reinterpret_cast<const char*>(static_cast<std::uintptr_t>(address)); }
const CSymbol64* SymbolAt(const unsigned char* base, std::size_t offset) {
    return reinterpret_cast<const CSymbol64*>(base + offset);
}
// The function pointer in an allocator's vtable at a byte offset (the blr target).
template <typename Fn>
Fn VirtualSlot(CAllocator* allocator, std::size_t byte_offset) {
    void* const* const vtable = *reinterpret_cast<void* const* const*>(allocator);
    return reinterpret_cast<Fn>(vtable[byte_offset / sizeof(void*)]);
}

// ---- strings read from libr15.so .rodata (addresses in the comments) ----
const char kNotImplementedFormat[] = "NotImplemented_(%s) : %s:%d (%s)";  // 0x2c2e8f3
const char kVulkan[] = "Vulkan";                                           // 0x2d58511
const char kEngineResourcesPath[] =
    "d:\\projects\\rad\\dev\\src\\engine\\libs\\gs\\cgengineresources.cpp";  // 0x2d58518
const char kInitializeName[] = "Initialize";                                  // 0x329be09
const char kNoMemoryContext[] = "Trying to use memory context before it's initialized!";  // 0x2c2e866
const char kSurfaceManagerPath[] =
    "d:\\projects\\rad\\dev\\src\\engine\\libs\\gs\\cgsurfacemanager_vk.cpp";  // 0x2d5fb01
const char kName2D[] = "default 2D texture";                    // 0x2d5859f
const char kName3D[] = "Default 3D Texture";                    // 0x2d585b2
const char kName3DShadow[] = "Default 3D Shadow Texture";      // 0x2d585c5
const char kNameIrradiance[] = "Default Irradiance Constants";  // 0x2d585df
const char kNameLightMapUvs[] = "Default Light Map UVs";        // 0x2d585fc
const char kNameParticleTris[] = "Default Particle Mesh Emitter Tris";  // 0x2d58612
const char kNameAmbientSpec[] = "ambient_spec_default";         // 0x2d58635
const char kNameAmbientSpecBox[] = "ambient_spec_box_default";  // 0x2d5864a
const char kNameInstancing[] = "Default Instancing Constants";  // 0x2d58663
const char kNameGiLightmap[] = "gi_lightmap_default";           // 0x2d58680
const char kNameAo0Lightmap[] = "ao0_lightmap_default";         // 0x2d58694
const char kNameBakedShadow[] = "baked_shadow_map_default";     // 0x2d586a9
const char kNameMultiResMask[] = "Multi-Res Mask IB";           // 0x2d60fc4
const char kNameJitter4[] = "jitter_texture_1d_4";              // 0x2d5875d
const char kNameJitter8[] = "jitter_texture_1d_8";              // 0x2d58783

// ---- constant tables read from libr15.so .rodata ----
// 0x2d55eb0: the quad's four (x, y, z) positions.
const float kQuadVertices[12] = {-1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f};
// 0x3270544 (48 values) followed by 0x2d55f40 (48 values): the first "Multi-Res Mask IB" indices.
const std::uint16_t kMultiResMaskIndices[96] = {
    0,    1,    5,    5,    4,    0,    1,    2,    6,    6,    5,    1,    2,    3,    7,    7,
    6,    2,    4,    5,    9,    9,    8,    4,    6,    7,    11,   11,   10,   6,    8,    9,
    13,   13,   12,   8,    9,    10,   14,   14,   13,   9,    10,   11,   15,   15,   14,   10,
    0x10, 0x11, 0x15, 0x15, 0x14, 0x10, 0x11, 0x12, 0x16, 0x16, 0x15, 0x11, 0x12, 0x13, 0x17, 0x17,
    0x16, 0x12, 0x14, 0x15, 0x19, 0x19, 0x18, 0x14, 0x16, 0x17, 0x1b, 0x1b, 0x1a, 0x16, 0x18, 0x19,
    0x1d, 0x1d, 0x1c, 0x18, 0x19, 0x1a, 0x1e, 0x1e, 0x1d, 0x19, 0x1a, 0x1b, 0x1f, 0x1f, 0x1e, 0x1a};
// 0x32705a4: the second "Multi-Res Mask IB" indices (12 values).
const std::uint16_t kMultiResMaskIndices2[12] = {0, 1, 3, 3, 2, 0, 4, 5, 7, 7, 6, 4};
// 0x2d586e8: the 8-byte key the two fullscreen vertex buffers and the two small index buffers are
// named with (the listing computes 0x2d586e9 & ~7 and loads it; the text "Procedural_FS" follows
// it in .rodata).
const std::uint64_t kProceduralKey = 0x8de6b4e6d6359c93ULL;

// ---- memory-only helpers (no calls) ----

// Vertex-buffer init record at sp+0x3b0: {u64 size; zeros; data pointer at +0x20}; the name pointer
// at +0x28 is stored by the caller after CSymbol64::String().
void StoreVertexRecord(unsigned char* sp, std::uint64_t bytes, std::uint64_t data) {
    St64(sp, 0x3c0, 0);
    St64(sp, 0x3b8, 0);
    St32(sp, 0x3c8, 0);
    St64(sp, 0x3b0, bytes);
    St64(sp, 0x3d0, data);
}

// Index-buffer init record at sp+record: {u64 count; u32 2 (index size); zeros; data at +0x20}; the
// name pointer at +0x28 is stored by the caller after CSymbol64::String().
void StoreIndexRecordAt(unsigned char* sp, std::size_t record, std::uint64_t count, std::uint64_t data) {
    St32(sp, record + 0x1c, 0);
    St32(sp, record + 0x8, 2);
    St64(sp, record + 0x14, 0);
    St64(sp, record + 0xc, 0);
    St64(sp, record, count);
    St64(sp, record + 0x20, data);
}

// The same record at the listing's usual place, sp+0x3b0.
void StoreIndexRecord(unsigned char* sp, std::uint64_t count, std::uint64_t data) {
    StoreIndexRecordAt(sp, 0x3b0, count, data);
}

// What every vertex-buffer section does after CGBuffer::Initialize and the 0x128-byte format copy
// to buffer+0xb0: clear the temporary words, then build the view that follows the buffer object
// (buffer+0x1d8) from the object's own fields at +0x28, +0x34 and +0x9e and from the stack words
// the listing leaves in sp+0x5a8..0x5dc. [sp+0x5d8] is read but never written by this function.
void StoreVertexView(unsigned char* self, unsigned char* sp, std::size_t buffer, std::uint32_t vertex_count) {
    const std::size_t view = buffer + 0x1d8;
    St64(sp, 0x5b4, 0);
    St64(sp, 0x5ac, 0);
    St32(sp, 0x5bc, 0);
    St64(sp, 0x5cc, 0);
    St64(sp, 0x5c4, 0);
    St32(sp, 0x5d4, 0);
    unsigned char head[16];
    std::memcpy(head, self + buffer + 0x28, sizeof head);
    const std::uint32_t product = Ld32(self, buffer + 0x34) * Ld8(self, buffer + 0x9e);
    St32(self, view + 0x88, 0);
    St64(self, view + 0x10, product);
    std::memcpy(self + view, head, sizeof head);
    St64(self, view + 0x68, 0);
    St32(self, view + 0x78, vertex_count);
    St64(self, view + 0x18, 0);
    Move(self, view + 0x20, sp, 0x5a8, 16);
    St32(self, view + 0x7c, 0);
    Move(self, view + 0x40, sp, 0x5c8, 8);
    Move(self, view + 0x30, sp, 0x5b8, 16);
    St64(self, view + 0x70, 0);
    St64(self, view + 0x80, 0);
    Move(self, view + 0x48, sp, 0x5d0, 8);
    Move(self, view + 0x50, sp, 0x5d8, 4);
    St32(self, view + 0x54, 0);
    St64(self, view + 0x58, 0);
}

// Image descriptor at sp+0x460: three 16-byte heads, then {-1, 0, 0} and the data/owner/name words.
void StoreImageDesc(unsigned char* sp, Quad head, Quad second, Quad third, std::uint64_t data,
                    std::uint64_t owner, const char* name) {
    St4(sp, 0x460, head.w[0], head.w[1], head.w[2], head.w[3]);
    St4(sp, 0x470, second.w[0], second.w[1], second.w[2], second.w[3]);
    St4(sp, 0x480, third.w[0], third.w[1], third.w[2], third.w[3]);
    St64(sp, 0x490, ~static_cast<std::uint64_t>(0));
    St64(sp, 0x498, 0);
    St32(sp, 0x4a0, 0);
    St64(sp, 0x4a8, data);
    St64(sp, 0x4b0, owner);
    St64(sp, 0x4b8, Addr(name));
}

// The {file, line} record and the 6-word callback block the listing fills before each graphics-table
// call: block[0] = record, [1..5] = the five callback addresses; record = {file, line}.
void StoreAllocationCallbacks(std::uint64_t* record, std::uint64_t* block, std::uint64_t line) {
    record[0] = Addr(kSurfaceManagerPath);
    record[1] = line;
    block[0] = Addr(record);
    block[1] = reinterpret_cast<std::uintptr_t>(&FUN_019dc8c0);
    block[2] = reinterpret_cast<std::uintptr_t>(&FUN_019dc91c);
    block[3] = reinterpret_cast<std::uintptr_t>(&FUN_019dc978);
    block[4] = reinterpret_cast<std::uintptr_t>(&FUN_019dc9cc);
    block[5] = reinterpret_cast<std::uintptr_t>(&FUN_019dc9d0);
}

}  // namespace

// @0x193e52c — NRadEngine::CGEngineResources::Initialize
int CGEngineResources::Initialize() {
    unsigned char* const self = bytes_;
    alignas(16) unsigned char frame[kFrameBytes] = {};
    unsigned char* const sp = frame;
    CGVertexFormat* const vertex_format = reinterpret_cast<CGVertexFormat*>(sp + kVertexFormat);
    std::uint64_t context_allocator = 0;  // x20: the word at CMemoryContext::context + 0x50
    int status = 0;                       // w0 of the status-returning callees

    // ---- 0x193e52c-0x193e570: the memory context check, then Shutdown ----
    if (CMemoryContext::context == nullptr) {  // cbz x8 @0x193e560
        NWriteLog::WriteLog(8, nullptr, kNoMemoryContext);  // bl @0x193e8b4
        SoftwareBreakpoint(1, 0x193e8bc)();                 // brk #1 @0x193e8b8
        // 0x193e8bc: the listing repeats the load and the Shutdown call after the trap.
        context_allocator = Ld64(static_cast<const unsigned char*>(CMemoryContext::context), 0x50);
        status = Shutdown();  // bl @0x193e8c8
        if (status != 0) {    // cbz w0 @0x193e8cc falls to the epilogue
            return status;
        }
    } else {
        context_allocator = Ld64(static_cast<const unsigned char*>(CMemoryContext::context), 0x50);
        status = Shutdown();  // bl @0x193e56c
        if (status != 0) {    // cbnz w0 @0x193e570
            return status;
        }
    }

    if ((DAT_0389b184 & 1U) == 0) {  // tbnz w9,#0 @0x193e57c
        DAT_0389b184 = 1;
        NWriteLog::WriteLog(4, nullptr, kNotImplementedFormat, kVulkan, kEngineResourcesPath, 0x1ab,
                            kInitializeName);  // bl @0x193e5b4
    }

    status = reinterpret_cast<CGFog*>(self + 0x1150)->Initialize();  // bl @0x193e5c0
    if (status != 0) {                                               // cbnz w0 @0x193e5c4
        return status;
    }
    reinterpret_cast<CGSkin*>(self + 0x12f0)->Initialize();  // bl @0x193e5d0

    // 64-bit keys at 0x2d58558, 0x2d58570, 0x2d58588 (each is followed in .rodata by its text:
    // "depthonly", "prepass", "outline").
    status = reinterpret_cast<CResourceInstance*>(self + 0xe80)->LoadAndInit(
        CSymbol64{0x6080e0d9da2e174dULL}, 1);  // bl @0x193e5ec
    if (status != 0) {                         // cbnz w0 @0x193e5f0
        return status;
    }
    St32(self, 0xe98, 0x10);
    status = reinterpret_cast<CResourceInstance*>(self + 0xfa0)->LoadAndInit(
        CSymbol64{0x2fc79d9a4e754499ULL}, 0);  // bl @0x193e614
    if (status != 0) {                         // cbnz w0 @0x193e618
        return status;
    }

    if ((DAT_0389b188 & 1U) == 0) {  // tbnz w9,#0 @0x193e624
        DAT_0389b188 = 1;
        NWriteLog::WriteLog(4, nullptr, kNotImplementedFormat, kVulkan, kEngineResourcesPath, 0x1bb,
                            kInitializeName);  // bl @0x193e65c
    }
    status = reinterpret_cast<CResourceInstance*>(self + 0x10c0)->LoadAndInit(
        CSymbol64{0x2fd89a8b527d598fULL}, 0);  // bl @0x193e67c
    if (status != 0) {                         // cbnz w0 @0x193e680
        return status;
    }

    // ---- 0x193e684-0x193e824: quad vertex buffer (this+0x680) and its index buffer (this+0xce0) ----
    new (vertex_format) CGVertexFormat();                         // bl @0x193e69c
    vertex_format->AppendElement(0, 8, 3, 0, 0, 0);               // bl @0x193e6bc
    std::memcpy(sp + 0xc0, kQuadVertices, sizeof kQuadVertices);  // q0,q2 -> [sp+0xc0], q1 -> [sp+0xe0]
    {
        const std::uint64_t quad_bytes = vertex_format->VertexSize(0) << 2;  // bl @0x193e6f0
        St64(sp, 0x5a8, DAT_0389b4d8.key);
        StoreVertexRecord(sp, quad_bytes, Addr(sp + 0xc0));
        St64(sp, 0x3d8, Addr(SymbolAt(sp, 0x5a8)->String()));  // bl @0x193e71c
        CGBuffer::Initialize(reinterpret_cast<SGVertexBufferInit*>(self + 0x680),
                             reinterpret_cast<CAllocator*>(sp + 0x3b0));  // bl @0x193e72c
    }
    // 0x193e730-0x193e7e0: the view that follows the vertex buffer object (this+0x858), 4 vertices.
    std::memcpy(self + 0x730, sp + 0x460, 0x128);  // bl @0x193e76c
    StoreVertexView(self, sp, 0x680, 4);
    St64(sp, 0x5a8, 0x0002000100020000ULL);  // 0x2d57110: indices 0, 2, 1, 2
    St32(sp, 0x5b0, 0x30000);                // indices 0, 3
    St64(sp, 0x2d0, DAT_0389b4d8.key);
    StoreIndexRecord(sp, 6, Addr(sp + 0x5a8));
    St64(sp, 0x3d8, Addr(SymbolAt(sp, 0x2d0)->String()));  // bl @0x193e80c
    CGBuffer::Initialize(reinterpret_cast<SGIndexBufferInit*>(self + 0xce0),
                         reinterpret_cast<CAllocator*>(sp + 0x3b0));  // bl @0x193e81c
    St64(self, 0xd90, Addr(self + 0xce0));
    St64(self, 0xd98, 0x600000000ULL);  // d8 = 0x2d57118 = {0, 6}: first index 0, count 6

    // ---- 0x193e838-0x193eaf0: solid sphere (8 segments): vertex buffer this+0x1b0, index buffer this+0xb50 ----
    new (vertex_format) CGVertexFormat();                    // bl @0x193e838
    vertex_format->AppendElement(0, 8, 3, 0, 0, 0);          // bl @0x193e858
    const std::uint64_t sphere_vertices =
        static_cast<std::uint32_t>(CGShape::NumVerticesForSolidSphere(8));  // bl @0x193e860
    const std::uint64_t sphere_indices =
        static_cast<std::uint32_t>(CGShape::NumIndicesForSolidSphere(8));  // bl @0x193e86c
    CMemoryContext::CurrentPtr();                                          // bl @0x193e874 (result unused)
    CAllocator* const sphere_vertex_allocator = CMemoryContext::CurrentPtr();  // bl @0x193e878
    void* sphere_vertex_data;
    if (sphere_vertices == 0) {  // cbz x26 @0x193e880 -> 0x193e8f8
        sphere_vertex_data = nullptr;
    } else {
        sphere_vertex_data = VirtualSlot<AllocateFn>(sphere_vertex_allocator, 0x10)(
            sphere_vertex_allocator, sphere_vertices * 12);  // blr @0x193e898, then b @0x193e8a0
    }
    CMemoryContext::CurrentPtr();                                          // bl @0x193e91c (result unused)
    CAllocator* const sphere_index_allocator = CMemoryContext::CurrentPtr();  // bl @0x193e920
    void* sphere_index_data;
    if (sphere_indices == 0) {  // cbz x24 @0x193e928 -> 0x193e948
        sphere_index_data = nullptr;
    } else {
        sphere_index_data = VirtualSlot<AllocateFn>(sphere_index_allocator, 0x10)(
            sphere_index_allocator, sphere_indices * 2);  // blr @0x193e93c, then b @0x193e944
    }
    St16(sp, 0xc0, 0);
    St16(sp, 0x2d0, 0);
    CGShape::SetupIndexedSolidSphere(
        *reinterpret_cast<std::uint16_t*>(sp + 0xc0), *reinterpret_cast<std::uint16_t*>(sp + 0x2d0),
        static_cast<C3Vector*>(sphere_vertex_data), static_cast<std::uint16_t*>(sphere_index_data),
        reinterpret_cast<float*>(self + 0xe70), 8);  // bl @0x193e96c
    {
        const std::uint64_t sphere_bytes = vertex_format->VertexSize(0) * sphere_vertices;  // bl @0x193e980
        St64(sp, 0x5a8, DAT_0389b4c8.key);
        StoreVertexRecord(sp, sphere_bytes, Addr(sphere_vertex_data));
        St64(sp, 0x3d8, Addr(SymbolAt(sp, 0x5a8)->String()));  // bl @0x193e9a4
        CGBuffer::Initialize(reinterpret_cast<SGVertexBufferInit*>(self + 0x1b0),
                             reinterpret_cast<CAllocator*>(sp + 0x3b0));  // bl @0x193e9b4
    }
    std::memcpy(self + 0x260, sp + 0x460, 0x128);  // bl @0x193e9f4
    StoreVertexView(self, sp, 0x1b0, static_cast<std::uint32_t>(sphere_vertices));
    StoreIndexRecord(sp, sphere_indices, Addr(sphere_index_data));
    St64(sp, 0x5a8, DAT_0389b4c8.key);
    St64(sp, 0x3d8, Addr(SymbolAt(sp, 0x5a8)->String()));  // bl @0x193ea88
    CGBuffer::Initialize(reinterpret_cast<SGIndexBufferInit*>(self + 0xb50),
                         reinterpret_cast<CAllocator*>(sp + 0x3b0));  // bl @0x193ea9c
    St64(self, 0xc00, Addr(self + 0xb50));
    St32(self, 0xc08, 0);
    St32(self, 0xc0c, static_cast<std::uint32_t>(sphere_indices));
    // The scratch allocations are given back through the allocators they came from (an allocator
    // value of 0 or -1 means "none").
    if (reinterpret_cast<std::uintptr_t>(sphere_index_allocator) + 1 >= 2) {  // b.cc @0x193eab4
        if (sphere_index_data != nullptr) {                                    // cbz x27 @0x193eab8
            VirtualSlot<FreeFn>(sphere_index_allocator, 0x38)(sphere_index_allocator,
                                                              sphere_index_data);  // blr @0x193eacc
        }
    }
    if (reinterpret_cast<std::uintptr_t>(sphere_vertex_allocator) + 1 >= 2) {  // b.cc @0x193ead8
        if (sphere_vertex_data != nullptr) {                                    // cbz x23 @0x193eadc
            VirtualSlot<FreeFn>(sphere_vertex_allocator, 0x38)(sphere_vertex_allocator,
                                                               sphere_vertex_data);  // blr @0x193eaf0
        }
    }

    // ---- 0x193eaf4-0x193ed68: solid cone: vertex buffer this+0x418, index buffer this+0xc18 ----
    new (vertex_format) CGVertexFormat();            // bl @0x193eb10
    vertex_format->AppendElement(0, 8, 3, 0, 0, 0);  // bl @0x193eb30
    // The vertex scratch (allocator at this+0x2030, data at +0x2038, 0x16 vertices) is replaced.
    CAllocator* const cone_vertex_allocator = CMemoryContext::CurrentPtr();  // bl @0x193eb34
    {
        CAllocator* const previous = reinterpret_cast<CAllocator*>(Ld64(self, 0x2030));
        if (reinterpret_cast<std::uintptr_t>(previous) + 1 >= 2) {  // b.cc @0x193eb48
            void* const previous_data = reinterpret_cast<void*>(Ld64(self, 0x2038));
            if (previous_data != nullptr) {  // cbz x1 @0x193eb50
                VirtualSlot<FreeFn>(previous, 0x38)(previous, previous_data);  // blr @0x193eb60
            }
        }
    }
    St64(self, 0x2030, 0);
    St64(self, 0x2038, 0);
    St64(self, 0x2040, 0);
    St64(self, 0x2048, 0);
    St64(self, 0x2030, Addr(cone_vertex_allocator));
    St64(self, 0x2048, 0x16);
    St64(self, 0x2038, Addr(VirtualSlot<AllocateFn>(cone_vertex_allocator, 0x10)(cone_vertex_allocator,
                                                                                  0x108)));  // blr @0x193eb88
    St64(self, 0x2040, 0x16);
    // The index scratch (allocator at this+0x2050, data at +0x2058, 0x78 indices) is replaced.
    CAllocator* const cone_index_allocator = CMemoryContext::CurrentPtr();  // bl @0x193eb94
    {
        CAllocator* const previous = reinterpret_cast<CAllocator*>(Ld64(self, 0x2050));
        if (reinterpret_cast<std::uintptr_t>(previous) + 1 >= 2) {  // b.cc @0x193eba8
            void* const previous_data = reinterpret_cast<void*>(Ld64(self, 0x2058));
            if (previous_data != nullptr) {  // cbz x1 @0x193ebb0
                VirtualSlot<FreeFn>(previous, 0x38)(previous, previous_data);  // blr @0x193ebc0
            }
        }
    }
    St64(self, 0x2050, 0);
    St64(self, 0x2058, 0);
    St64(self, 0x2060, 0);
    St64(self, 0x2068, 0);
    St64(self, 0x2050, Addr(cone_index_allocator));
    St64(self, 0x2068, 0x78);
    void* const cone_index_data =
        VirtualSlot<AllocateFn>(cone_index_allocator, 0x10)(cone_index_allocator, 0xf0);  // blr @0x193ebe8
    St64(self, 0x2058, Addr(cone_index_data));
    St64(self, 0x2060, 0x78);
    St16(sp, 0xc0, 0);
    St16(sp, 0x2d0, 0);
    CGShape::SetupIndexedSolidCone(*reinterpret_cast<std::uint16_t*>(sp + 0xc0),
                                   *reinterpret_cast<std::uint16_t*>(sp + 0x2d0),
                                   reinterpret_cast<C3Vector*>(Ld64(self, 0x2038)),
                                   static_cast<std::uint16_t*>(cone_index_data),
                                   reinterpret_cast<float*>(self + 0xe74));  // bl @0x193ec10
    {
        const std::uint64_t cone_bytes = vertex_format->VertexSize(0) * 0x16;  // bl @0x193ec28
        St64(sp, 0x5a8, DAT_0389b4d0.key);
        StoreVertexRecord(sp, cone_bytes, Ld64(self, 0x2038));
        St64(sp, 0x3d8, Addr(SymbolAt(sp, 0x5a8)->String()));  // bl @0x193ec4c
        CGBuffer::Initialize(reinterpret_cast<SGVertexBufferInit*>(self + 0x418),
                             reinterpret_cast<CAllocator*>(sp + 0x3b0));  // bl @0x193ec5c
    }
    std::memcpy(self + 0x4c8, sp + 0x460, 0x128);  // bl @0x193ec98
    StoreVertexView(self, sp, 0x418, 0x16);
    StoreIndexRecord(sp, 0x78, Ld64(self, 0x2058));
    St64(sp, 0x5a8, DAT_0389b4d0.key);
    St64(sp, 0x3d8, Addr(SymbolAt(sp, 0x5a8)->String()));  // bl @0x193ed34
    CGBuffer::Initialize(reinterpret_cast<SGIndexBufferInit*>(self + 0xc18),
                         reinterpret_cast<CAllocator*>(sp + 0x3b0));  // bl @0x193ed44
    St64(self, 0xcc8, Addr(self + 0xc18));
    St64(self, 0xcd0, 0x7800000000ULL);  // 0x2d57120 = {0, 0x78}

    // ---- 0x193ed68-0x193ef8c: solid box: vertex buffer this+0x8e8, index buffer this+0xda8 ----
    new (vertex_format) CGVertexFormat();            // bl @0x193ed68
    vertex_format->AppendElement(0, 8, 3, 0, 0, 0);  // bl @0x193ed8c
    CMemoryContext::CurrentPtr();                    // bl @0x193ed90 (result unused)
    CAllocator* const box_vertex_allocator = CMemoryContext::CurrentPtr();  // bl @0x193ed94
    void* const box_vertex_data =
        VirtualSlot<AllocateFn>(box_vertex_allocator, 0x10)(box_vertex_allocator, 0x60);  // blr @0x193eda8
    CAllocator* const box_index_allocator = CMemoryContext::CurrentPtr();  // bl @0x193edb0
    {
        CAllocator* const previous = reinterpret_cast<CAllocator*>(Ld64(self, 0x2070));
        if (reinterpret_cast<std::uintptr_t>(previous) + 1 >= 2) {  // b.cc @0x193edc4
            void* const previous_data = reinterpret_cast<void*>(Ld64(self, 0x2078));
            if (previous_data != nullptr) {  // cbz x1 @0x193edcc
                VirtualSlot<FreeFn>(previous, 0x38)(previous, previous_data);  // blr @0x193eddc
            }
        }
    }
    St64(self, 0x2070, 0);
    St64(self, 0x2078, 0);
    St64(self, 0x2080, 0);
    St64(self, 0x2088, 0);
    St64(self, 0x2070, Addr(box_index_allocator));
    St64(self, 0x2088, 0x24);
    void* const box_index_data =
        VirtualSlot<AllocateFn>(box_index_allocator, 0x10)(box_index_allocator, 0x48);  // blr @0x193ee04
    St64(self, 0x2078, Addr(box_index_data));
    St64(self, 0x2080, 0x24);
    St16(sp, 0xc0, 0);
    St16(sp, 0x2d0, 0);
    CGShape::SetupIndexedSolidBox(*reinterpret_cast<std::uint16_t*>(sp + 0xc0),
                                  *reinterpret_cast<std::uint16_t*>(sp + 0x2d0),
                                  static_cast<C3Vector*>(box_vertex_data),
                                  static_cast<std::uint16_t*>(box_index_data));  // bl @0x193ee28
    {
        const std::uint64_t box_bytes = vertex_format->VertexSize(0) << 3;  // bl @0x193ee3c
        St64(sp, 0x5a8, DAT_0389b4e0.key);
        StoreVertexRecord(sp, box_bytes, Addr(box_vertex_data));
        St64(sp, 0x3d8, Addr(SymbolAt(sp, 0x5a8)->String()));  // bl @0x193ee60
        CGBuffer::Initialize(reinterpret_cast<SGVertexBufferInit*>(self + 0x8e8),
                             reinterpret_cast<CAllocator*>(sp + 0x3b0));  // bl @0x193ee70
    }
    std::memcpy(self + 0x998, sp + 0x460, 0x128);  // bl @0x193eeac
    StoreVertexView(self, sp, 0x8e8, 8);
    StoreIndexRecord(sp, 0x24, Ld64(self, 0x2078));
    St64(sp, 0x5a8, DAT_0389b4e0.key);
    St64(sp, 0x3d8, Addr(SymbolAt(sp, 0x5a8)->String()));  // bl @0x193ef44
    CGBuffer::Initialize(reinterpret_cast<SGIndexBufferInit*>(self + 0xda8),
                         reinterpret_cast<CAllocator*>(sp + 0x3b0));  // bl @0x193ef54
    St64(self, 0xe58, Addr(self + 0xda8));
    St64(self, 0xe60, 0x2400000000ULL);  // 0x2d57128 = {0, 0x24}
    if (reinterpret_cast<std::uintptr_t>(box_vertex_allocator) + 1 >= 2) {  // b.cc @0x193ef70
        if (box_vertex_data != nullptr) {                                    // cbz x23 @0x193ef74
            VirtualSlot<FreeFn>(box_vertex_allocator, 0x38)(box_vertex_allocator,
                                                            box_vertex_data);  // blr @0x193ef88
        }
    }

    // ---- 0x193ef8c-0x193f12c: default 2D texture, one black texel (this+0x13a8) ----
    St64(self, 0xe78, 0x3f8000003f800000ULL);  // d9 = {1.0f, 1.0f}
    St32(sp, 0x45c, 0);                        // the texel
    // (the listing spills the constants {9,1,1,1}, later {2,0,0,0}, {0,1,0,0}, {0x12,1,1,1} and
    // {0x20,1,1,1} to [sp+0x70], [sp+0x60], [sp+0x30] and [sp+0x10] and reloads them; they are
    // written directly here)
    StoreImageDesc(sp, Quad{{9, 1, 1, 1}}, Quad{{1, 0, 0, 0}}, Quad{{0, 0, 0, 0}}, Addr(sp + 0x45c),
                   context_allocator, kName2D);
    FUN_019dad8c(sp + 0x3b0, sp + 0x460, 0);  // bl @0x193f004
    FUN_019daf90(self + 0x13a8, sp + 0x3b0, Ld32(sp, 0x460), Ld32(sp, 0x484), 0,
                 AsPointer(Ld64(sp, 0x4a8)), AsString(Ld64(sp, 0x4b8)));  // bl @0x193f024
    FUN_019dad8c(sp + 0x3b0, sp + 0x460, 0);                              // bl @0x193f034
    // Probe image: the {file, line} record and callback block live in emulated TLS; the three
    // graphics-table calls create the probe image, query its requirements and destroy it.
    std::uint64_t* const callback_record =
        static_cast<std::uint64_t*>(__emutls_get_address(DAT_03853780));  // bl @0x193f054
    std::uint64_t* const callback_block =
        static_cast<std::uint64_t*>(__emutls_get_address(DAT_038537a0));  // bl @0x193f074
    St64(sp, 0xc0, 0);
    StoreAllocationCallbacks(callback_record, callback_block, 0x1c7);
    DAT_03875688(DAT_03875fd8, sp + 0x3b0, callback_block, sp + 0xc0);  // blr @0x193f0cc
    St64(sp, 0x5b8, 0);
    St64(sp, 0x5b0, 0);
    St64(sp, 0x5a8, 0);
    DAT_038755d8(DAT_03875fd8, Ld64(sp, 0xc0), sp + 0x5a8);  // blr @0x193f0ec
    StoreAllocationCallbacks(callback_record, callback_block, 0x1cd);
    DAT_03875690(DAT_03875fd8, Ld64(sp, 0xc0), callback_block);  // blr @0x193f128

    // ---- 0x193f12c-0x193f264: the same texture with one white texel (this+0x1400) ----
    St32(sp, 0x44c, 0xffffffffU);
    StoreImageDesc(sp, Quad{{9, 1, 1, 1}}, Quad{{1, 0, 0, 0}}, Quad{{0, 0, 0, 0}}, Addr(sp + 0x44c),
                   context_allocator, kName2D);
    FUN_019dad8c(sp + 0x3b0, sp + 0x460, 0);  // bl @0x193f198
    FUN_019daf90(self + 0x1400, sp + 0x3b0, Ld32(sp, 0x460), Ld32(sp, 0x484), 0,
                 AsPointer(Ld64(sp, 0x4a8)), AsString(Ld64(sp, 0x4b8)));  // bl @0x193f1b8
    FUN_019dad8c(sp + 0x3b0, sp + 0x460, 0);                              // bl @0x193f1c8
    St64(sp, 0xc0, 0);
    StoreAllocationCallbacks(callback_record, callback_block, 0x1c7);
    DAT_03875688(DAT_03875fd8, sp + 0x3b0, callback_block, sp + 0xc0);  // blr @0x193f210
    St64(sp, 0x5b8, 0);
    St64(sp, 0x5b0, 0);
    St64(sp, 0x5a8, 0);
    DAT_038755d8(DAT_03875fd8, Ld64(sp, 0xc0), sp + 0x5a8);  // blr @0x193f234
    StoreAllocationCallbacks(callback_record, callback_block, 0x1cd);
    DAT_03875690(DAT_03875fd8, Ld64(sp, 0xc0), callback_block);  // blr @0x193f260

    // ---- 0x193f264-0x193f2d4: "Default 3D Texture" (this+0x1458), one black texel at sp+0x45c ----
    St64(sp, 0x450, Ld64(sp, 0x5a8));  // requirements size of the last probe
    St4(sp, 0x3b0, 9, 1, 1, 1);
    St32(sp, 0x3c0, 1);
    St64(sp, 0x3c4, 0);
    St32(sp, 0x3cc, 0);
    St64(sp, 0x3d0, Addr(sp + 0x45c));
    St64(sp, 0x3d8, context_allocator);
    St64(sp, 0x3e0, Addr(kName3D));
    FUN_019db678(sp + 0x460, sp + 0x3b0);                                  // bl @0x193f2b0
    FUN_019daf90(self + 0x1458, sp + 0x460, 9, 0, 0, sp + 0x45c, kName3D);  // bl @0x193f2d0

    // ---- 0x193f2d4-0x193f344: "Default 3D Shadow Texture" (this+0x14b0), one 0xffff texel ----
    St4(sp, 0x3b0, 0x12, 1, 1, 1);
    St32(sp, 0x3c0, 1);
    St64(sp, 0x3c4, 0);
    St32(sp, 0x3cc, 0);
    St16(sp, 0x5a8, 0xffff);
    St64(sp, 0x3d0, Addr(sp + 0x5a8));
    St64(sp, 0x3d8, context_allocator);
    St64(sp, 0x3e0, Addr(kName3DShadow));
    FUN_019db678(sp + 0x460, sp + 0x3b0);                                       // bl @0x193f320
    FUN_019daf90(self + 0x14b0, sp + 0x460, 0x12, 0, 0, sp + 0x5a8, kName3DShadow);  // bl @0x193f340

    // ---- 0x193f344-0x193f424: a table of seven 0x58-byte image records at this+0x17c8, each a copy
    // of the 3D texture (this+0x1458) or the shadow texture (this+0x14b0) ----
    Move(self, 0x17c8, self, 0x1458, 0x58);
    Move(self, 0x1820, self, 0x1458, 0x58);
    Move(self, 0x1878, self, 0x1458, 0x58);
    Move(self, 0x18d0, self, 0x1458, 0x58);
    Move(self, 0x1928, self, 0x14b0, 0x58);
    Move(self, 0x1980, self, 0x14b0, 0x58);
    Move(self, 0x19d8, self, 0x1458, 0x58);

    // ---- 0x193f424-0x193f4a8: "Default Irradiance Constants" (this+0x1a30), 0x9c bytes at sp+0x3b0 ----
    St4(sp, 0x418, 0, 0, 0, 0);
    St4(sp, 0x40c, 0, 0, 0, 0);
    St4(sp, 0x3fc, 0, 0, 0, 0);
    St4(sp, 0x42c, 0, 0, 0, 0);
    St4(sp, 0x43c, 0, 0, 0, 0);
    St32(sp, 0x3b0, 0x3f800000U);
    St64(sp, 0x3bc, 0);
    St64(sp, 0x3b4, 0);
    St32(sp, 0x3c4, 0x3f800000U);
    St32(sp, 0x3d8, 0x3f800000U);
    St64(sp, 0x394, 0x400000000ULL);
    St64(sp, 0x38c, 0);
    St64(sp, 0x380, 0x9c);
    St64(sp, 0x3d0, 0);
    St64(sp, 0x3c8, 0);
    St64(sp, 0x3e4, 0);
    St64(sp, 0x3dc, 0);
    St4(sp, 0x3ec, 0x3f800000U, 0, 0xffffffffU, 0xffffffffU);  // 0x2d55f00
    St32(sp, 0x39c, 0);
    St32(sp, 0x388, 0);
    St64(sp, 0x3a8, Addr(kNameIrradiance));
    St64(sp, 0x3a0, Addr(sp + 0x3b0));
    CGBuffer::Initialize(reinterpret_cast<SGConstantBufferInit*>(self + 0x1a30),
                         reinterpret_cast<CAllocator*>(sp + 0x380));  // bl @0x193f4a4

    // ---- 0x193f4a8-0x193f4f0: "Default Light Map UVs" typed buffer (this+0x1ae0) ----
    St32(sp, 0x378, 0);
    St32(sp, 0x364, 0);
    St64(sp, 0x348, 0x0000000400000004ULL);  // d10 = {4, 4}
    St64(sp, 0x35c, 0);
    St64(sp, 0x354, 0);
    St32(sp, 0x350, 0x17);
    St64(sp, 0x340, 1);
    St64(sp, 0x370, Addr(kNameLightMapUvs));
    St64(sp, 0x368, Addr(sp + 0x378));
    CGBuffer::Initialize(reinterpret_cast<SGTypedBufferInit*>(self + 0x1ae0),
                         reinterpret_cast<CAllocator*>(sp + 0x340));  // bl @0x193f4ec

    // ---- 0x193f4f0-0x193f58c: two "Default Particle Mesh Emitter Tris" structured buffers ----
    St64(sp, 0x330, 0);
    St64(sp, 0x328, 0);
    St64(sp, 0x320, 0);
    St32(sp, 0x338, 0);
    St64(sp, 0x2e0, 0x0000000400000004ULL);
    St64(sp, 0x2e8, 0);
    St64(sp, 0x2f8, 0);
    St64(sp, 0x2f0, 0);
    St32(sp, 0x300, 0);
    St64(sp, 0x310, Addr(kNameParticleTris));
    St64(sp, 0x308, Addr(sp + 0x320));
    St4(sp, 0x2d0, 0x1c, 0, 1, 0);  // 0x2d55f10
    CGBuffer::Initialize(reinterpret_cast<SGStructuredBufferInit*>(self + 0x1c40),
                         reinterpret_cast<CAllocator*>(sp + 0x2d0));  // bl @0x193f540
    St64(sp, 0x2c0, 0);
    St64(sp, 0x2b8, 0);
    St32(sp, 0x2c8, 0);
    St64(sp, 0x280, 0x0000000400000004ULL);
    St64(sp, 0x288, 0);
    St64(sp, 0x298, 0);
    St64(sp, 0x290, 0);
    St32(sp, 0x2a0, 0);
    St64(sp, 0x2b0, Addr(kNameParticleTris));
    St64(sp, 0x2a8, Addr(sp + 0x2b8));
    St4(sp, 0x270, 0x14, 0, 1, 0);  // 0x2d55f20
    CGBuffer::Initialize(reinterpret_cast<SGStructuredBufferInit*>(self + 0x1b90),
                         reinterpret_cast<CAllocator*>(sp + 0x270));  // bl @0x193f588

    // ---- 0x193f58c-0x193f6f8: ambient cube texture, its copies and two more buffers ----
    St64(sp, 0x268, 0);
    St64(sp, 0x260, 0);
    St64(sp, 0x258, 0);
    {
        std::uint64_t cube_size = Ld64(sp, 0x450);
        alignas(8) unsigned char cube_record[0x58];  // the image record, written through the sret pointer
        CGSurfaceManager::CreateCubeTexture(cube_record, 1, 9, 1, sp + 0x258, cube_size, 1, 1,
                                            *reinterpret_cast<CAllocator*>(context_allocator),
                                            kNameAmbientSpec);  // bl @0x193f5c8
        std::memcpy(sp + 0x460, cube_record, sizeof cube_record);
        St64(sp, 0x450, cube_size);
    }
    Move(self, 0x1508, sp, 0x460, 0x58);
    St64(sp, 0x1f0, 0x0000000400000004ULL);
    St64(sp, 0x1f8, 0);
    St64(sp, 0x200, 0);
    St64(sp, 0x208, 0);
    St32(sp, 0x210, 0);
    St64(sp, 0x230, 0);  // 32 zero bytes of data at sp+0x230
    St64(sp, 0x238, 0);
    St64(sp, 0x240, 0);
    St64(sp, 0x248, 0);
    St64(sp, 0x218, Addr(sp + 0x230));
    St64(sp, 0x220, Addr(kNameAmbientSpecBox));
    St4(sp, 0x1e0, 0x20, 0, 1, 0);  // 0x2d4d930
    CGBuffer::Initialize(reinterpret_cast<SGStructuredBufferInit*>(self + 0x1560),
                         reinterpret_cast<CAllocator*>(sp + 0x1e0));  // bl @0x193f640
    // copies of the cube image (this+0x1508) and of 0x50 bytes of the buffer object at this+0x1560
    Move(self, 0x1d20, self, 0x1508, 0x58);
    Move(self, 0x1d78, self, 0x15a0, 0x50);
    // "Default Instancing Constants": 0x28 bytes at sp+0x1b8, record at sp+0x188
    St32(sp, 0x190, 1);
    St64(sp, 0x19c, 0x600000000ULL);
    St32(sp, 0x1a4, 0);
    St64(sp, 0x1b8, 0);
    St64(sp, 0x1c0, 0);
    St4(sp, 0x1c8, 0xffffffffU, 0xffffffffU, 0, 0xffffffffU);  // 0x2c9fea0
    St64(sp, 0x1d8, 0);
    St64(sp, 0x194, 0);
    St64(sp, 0x188, 0x28);
    St64(sp, 0x1a8, Addr(sp + 0x1b8));
    St64(sp, 0x1b0, Addr(kNameInstancing));
    CGBuffer::Initialize(reinterpret_cast<SGConstantBufferInit*>(self + 0x1dc8),
                         reinterpret_cast<CAllocator*>(sp + 0x188));  // bl @0x193f6f4

    // ---- 0x193f6f8-0x193f830: "gi_lightmap_default" (this+0x1610), data word at sp+0x180 ----
    St64(sp, 0x498, 0);
    St64(sp, 0x180, 0);
    StoreImageDesc(sp, Quad{{9, 1, 1, 1}}, Quad{{2, 0, 0, 0}}, Quad{{0, 1, 0, 0}}, Addr(sp + 0x180),
                   context_allocator, kNameGiLightmap);
    FUN_019dad8c(sp + 0x5a8, sp + 0x460, 0);  // bl @0x193f760
    FUN_019daf90(self + 0x1610, sp + 0x5a8, Ld32(sp, 0x460), Ld32(sp, 0x484), 0,
                 AsPointer(Ld64(sp, 0x4a8)), AsString(Ld64(sp, 0x4b8)));  // bl @0x193f780
    FUN_019dad8c(sp + 0x5a8, sp + 0x460, 0);                              // bl @0x193f790
    St64(sp, 0x118, 0);
    StoreAllocationCallbacks(callback_record, callback_block, 0x1c7);
    DAT_03875688(DAT_03875fd8, sp + 0x5a8, callback_block, sp + 0x118);  // blr @0x193f7e4
    St64(sp, 0xc8, 0);
    St64(sp, 0xd0, 0);
    St64(sp, 0xc0, 0);
    DAT_038755d8(DAT_03875fd8, Ld64(sp, 0x118), sp + 0xc0);  // blr @0x193f800
    StoreAllocationCallbacks(callback_record, callback_block, 0x1cd);
    DAT_03875690(DAT_03875fd8, Ld64(sp, 0x118), callback_block);  // blr @0x193f82c

    // ---- 0x193f830-0x193f964: "ao0_lightmap_default" (this+0x1668), 16 data bytes 0x4103 at sp+0x170 ----
    St64(sp, 0x170, 0x4103);
    St64(sp, 0x178, 0x4103);
    St64(sp, 0x450, Ld64(sp, 0xc0));
    StoreImageDesc(sp, Quad{{0x20, 1, 1, 1}}, Quad{{2, 0, 0, 0}}, Quad{{0, 1, 0, 0}}, Addr(sp + 0x170),
                   context_allocator, kNameAo0Lightmap);
    FUN_019dad8c(sp + 0x5a8, sp + 0x460, 0);  // bl @0x193f8a4
    FUN_019daf90(self + 0x1668, sp + 0x5a8, Ld32(sp, 0x460), Ld32(sp, 0x484), 0,
                 AsPointer(Ld64(sp, 0x4a8)), AsString(Ld64(sp, 0x4b8)));  // bl @0x193f8c4
    FUN_019dad8c(sp + 0x5a8, sp + 0x460, 0);                              // bl @0x193f8d4
    St64(sp, 0x118, 0);
    StoreAllocationCallbacks(callback_record, callback_block, 0x1c7);
    DAT_03875688(DAT_03875fd8, sp + 0x5a8, callback_block, sp + 0x118);  // blr @0x193f914
    St64(sp, 0xc8, 0);
    St64(sp, 0xd0, 0);
    St64(sp, 0xc0, 0);
    DAT_038755d8(DAT_03875fd8, Ld64(sp, 0x118), sp + 0xc0);  // blr @0x193f930
    StoreAllocationCallbacks(callback_record, callback_block, 0x1cd);
    DAT_03875690(DAT_03875fd8, Ld64(sp, 0x118), callback_block);  // blr @0x193f960

    // ---- 0x193f964-0x193fa90: the same name again, zero data (this+0x16c0), data at sp+0x160 ----
    St64(sp, 0x160, 0);
    St64(sp, 0x168, 0);
    StoreImageDesc(sp, Quad{{0x20, 1, 1, 1}}, Quad{{2, 0, 0, 0}}, Quad{{0, 1, 0, 0}}, Addr(sp + 0x160),
                   context_allocator, kNameAo0Lightmap);
    FUN_019dad8c(sp + 0x5a8, sp + 0x460, 0);  // bl @0x193f9c0
    FUN_019daf90(self + 0x16c0, sp + 0x5a8, Ld32(sp, 0x460), Ld32(sp, 0x484), 0,
                 AsPointer(Ld64(sp, 0x4a8)), AsString(Ld64(sp, 0x4b8)));  // bl @0x193f9e0
    FUN_019dad8c(sp + 0x5a8, sp + 0x460, 0);                              // bl @0x193f9f0
    St64(sp, 0x118, 0);
    StoreAllocationCallbacks(callback_record, callback_block, 0x1c7);
    DAT_03875688(DAT_03875fd8, sp + 0x5a8, callback_block, sp + 0x118);  // blr @0x193fa40
    St64(sp, 0xc8, 0);
    St64(sp, 0xd0, 0);
    St64(sp, 0xc0, 0);
    DAT_038755d8(DAT_03875fd8, Ld64(sp, 0x118), sp + 0xc0);  // blr @0x193fa5c
    StoreAllocationCallbacks(callback_record, callback_block, 0x1cd);
    DAT_03875690(DAT_03875fd8, Ld64(sp, 0x118), callback_block);  // blr @0x193fa8c

    // ---- 0x193fa90-0x193fbb4: "baked_shadow_map_default" (this+0x1718), texel 0xffffffff at sp+0x15c ----
    St32(sp, 0x15c, 0xffffffffU);
    StoreImageDesc(sp, Quad{{0x12, 1, 1, 1}}, Quad{{2, 0, 0, 0}}, Quad{{0, 1, 0, 0}}, Addr(sp + 0x15c),
                   context_allocator, kNameBakedShadow);
    FUN_019dad8c(sp + 0x5a8, sp + 0x460, 0);  // bl @0x193faf0
    FUN_019daf90(self + 0x1718, sp + 0x5a8, Ld32(sp, 0x460), Ld32(sp, 0x484), 0,
                 AsPointer(Ld64(sp, 0x4a8)), AsString(Ld64(sp, 0x4b8)));  // bl @0x193fb10
    FUN_019dad8c(sp + 0x5a8, sp + 0x460, 0);                              // bl @0x193fb20
    St64(sp, 0x118, 0);
    StoreAllocationCallbacks(callback_record, callback_block, 0x1c7);
    DAT_03875688(DAT_03875fd8, sp + 0x5a8, callback_block, sp + 0x118);  // blr @0x193fb64
    St64(sp, 0xc8, 0);
    St64(sp, 0xd0, 0);
    St64(sp, 0xc0, 0);
    DAT_038755d8(DAT_03875fd8, Ld64(sp, 0x118), sp + 0xc0);  // blr @0x193fb80
    StoreAllocationCallbacks(callback_record, callback_block, 0x1cd);
    DAT_03875690(DAT_03875fd8, Ld64(sp, 0x118), callback_block);  // blr @0x193fbb0

    // ---- 0x193fbb4-0x193fcd4: the same name again (this+0x1770), texel at sp+0x158 ----
    St32(sp, 0x158, 0xffffffffU);
    StoreImageDesc(sp, Quad{{0x12, 1, 1, 1}}, Quad{{2, 0, 0, 0}}, Quad{{0, 1, 0, 0}}, Addr(sp + 0x158),
                   context_allocator, kNameBakedShadow);
    FUN_019dad8c(sp + 0x5a8, sp + 0x460, 0);  // bl @0x193fc14
    FUN_019daf90(self + 0x1770, sp + 0x5a8, Ld32(sp, 0x460), Ld32(sp, 0x484), 0,
                 AsPointer(Ld64(sp, 0x4a8)), AsString(Ld64(sp, 0x4b8)));  // bl @0x193fc34
    FUN_019dad8c(sp + 0x5a8, sp + 0x460, 0);                              // bl @0x193fc44
    St64(sp, 0x118, 0);
    StoreAllocationCallbacks(callback_record, callback_block, 0x1c7);
    DAT_03875688(DAT_03875fd8, sp + 0x5a8, callback_block, sp + 0x118);  // blr @0x193fc88
    St64(sp, 0xc8, 0);
    St64(sp, 0xd0, 0);
    St64(sp, 0xc0, 0);
    DAT_038755d8(DAT_03875fd8, Ld64(sp, 0x118), sp + 0xc0);  // blr @0x193fca4
    StoreAllocationCallbacks(callback_record, callback_block, 0x1cd);
    DAT_03875690(DAT_03875fd8, Ld64(sp, 0x118), callback_block);  // blr @0x193fcd0

    // ---- 0x193fcd4-0x193fdec: the lightmap records are copied into the second table (0x58 bytes each) ----
    St64(sp, 0x450, Ld64(sp, 0xc0));
    Move(self, 0x1e78, self, 0x1610, 0x58);  // gi_lightmap
    Move(self, 0x1ed0, self, 0x1668, 0x58);  // ao0, first
    Move(self, 0x1f28, self, 0x16c0, 0x58);  // ao0, second
    Move(self, 0x1f80, self, 0x1718, 0x58);  // baked shadow, first
    Move(self, 0x1fd8, self, 0x1770, 0x58);  // baked shadow, second

    // ---- 0x193fdec-0x193fe10: two more resource loads ----
    status = reinterpret_cast<CResourceInstance*>(self + 0x1cf0)->LoadAndInit(
        CSymbol64{0xbc33e5bcbe614747ULL}, 1);  // bl @0x193fdec (key text "iris_irradiance")
    if (status != 0) {                         // cbnz w0 @0x193fdf0
        return status;
    }
    status = reinterpret_cast<CResourceInstance*>(self + 0x1d08)->LoadAndInit(DAT_0389b228, 0);  // bl @0x193fe08
    if (status != 0) {                                                                         // cbnz w0 @0x193fe0c
        return status;
    }

    // ---- 0x193fe10-0x1940100: fullscreen triangle (this+0x2090) and fullscreen quad (this+0x23c0) ----
    // Five floats per vertex (x, y, z, u, v), stride 0x14: the quad's four vertices from sp+0xc0, the
    // triangle's three from sp+0x118. The constants come from 0x2d57130, 0x2d57138, 0x2d57140,
    // 0x2ca3c18, 0x2cb31f8, 0x2cb3068, 0x2d4af78 and 0x2c9fda8.
    StF2(sp, 0xc0, -1.0f, 1.0f);
    StF(sp, 0xc8, 1.0f);
    St64(sp, 0xcc, 0);
    StF2(sp, 0xd4, 1.0f, 1.0f);  // d9
    StF(sp, 0xdc, 1.0f);
    StF2(sp, 0xe0, 1.0f, 0.0f);
    StF2(sp, 0xe8, 1.0f, -1.0f);
    StF(sp, 0xf0, 1.0f);
    StF2(sp, 0xf4, 1.0f, 1.0f);  // d9
    StF(sp, 0xfc, -1.0f);
    StF2(sp, 0x100, -1.0f, 1.0f);
    StF2(sp, 0x108, 0.0f, 1.0f);
    StF2(sp, 0x118, -1.0f, 1.0f);
    StF(sp, 0x120, 1.0f);
    St64(sp, 0x124, 0);
    StF2(sp, 0x12c, 3.0f, 1.0f);
    StF(sp, 0x134, 1.0f);
    StF2(sp, 0x138, 2.0f, 0.0f);
    StF2(sp, 0x140, -1.0f, -3.0f);
    StF(sp, 0x148, 1.0f);
    StF2(sp, 0x14c, 0.0f, 2.0f);
    // Format: the element count is cleared (str xzr,[sp,#0x580]) and two elements are appended.
    St64(sp, 0x580, 0);
    vertex_format->AppendElementWithAlias(0, 0xb, 8, 3, 0, 0, 0);  // bl @0x193fed8
    vertex_format->AppendElement(4, 8, 2, 0, 0, 0);                // bl @0x193fef8
    {
        const std::uint64_t triangle_bytes = vertex_format->VertexSize(0) * 3;  // bl @0x193ff14
        St64(sp, 0x90, kProceduralKey);
        St64(sp, 0x5b8, 0);
        St64(sp, 0x5b0, 0);
        St32(sp, 0x5c0, 0);
        St64(sp, 0x5a8, triangle_bytes);
        St64(sp, 0x5c8, Addr(sp + 0x118));
        St64(sp, 0x5d0, Addr(SymbolAt(sp, 0x90)->String()));  // bl @0x193ff3c
        CGBuffer::Initialize(reinterpret_cast<SGVertexBufferInit*>(self + 0x2090),
                             reinterpret_cast<CAllocator*>(sp + 0x5a8));  // bl @0x193ff50
    }
    std::memcpy(self + 0x2140, sp + 0x460, 0x128);  // bl @0x193ff90
    StoreVertexView(self, sp, 0x2090, 3);
    St64(sp, 0x580, 0);
    vertex_format->AppendElementWithAlias(0, 0xb, 8, 3, 0, 0, 0);  // bl @0x1940024
    vertex_format->AppendElement(4, 8, 2, 0, 0, 0);                // bl @0x1940044
    {
        const std::uint64_t fullscreen_quad_bytes = vertex_format->VertexSize(0) << 2;  // bl @0x1940054
        St64(sp, 0x90, kProceduralKey);
        St64(sp, 0x5b8, 0);
        St64(sp, 0x5b0, 0);
        St32(sp, 0x5c0, 0);
        St64(sp, 0x5a8, fullscreen_quad_bytes);
        St64(sp, 0x5c8, Addr(sp + 0xc0));
        St64(sp, 0x5d0, Addr(SymbolAt(sp, 0x90)->String()));  // bl @0x1940080
        CGBuffer::Initialize(reinterpret_cast<SGVertexBufferInit*>(self + 0x23c0),
                             reinterpret_cast<CAllocator*>(sp + 0x5a8));  // bl @0x1940094
    }
    std::memcpy(self + 0x2470, sp + 0x460, 0x128);  // bl @0x19400d0
    StoreVertexView(self, sp, 0x23c0, 4);

    // ---- 0x19400fc-0x19401f8: two index buffers over the same six indices (this+0x22f8, this+0x2628) ----
    St64(sp, 0xb0, 0x0002000200010000ULL);  // 0x2d57148: indices 0, 1, 2, 2
    St32(sp, 0xb8, 3);                      // indices 3, 0
    StoreIndexRecordAt(sp, 0x460, 3, Addr(sp + 0xb0));
    St64(sp, 0x5a8, kProceduralKey);
    St64(sp, 0x488, Addr(SymbolAt(sp, 0x5a8)->String()));  // bl @0x1940180
    CGBuffer::Initialize(reinterpret_cast<SGIndexBufferInit*>(self + 0x22f8),
                         reinterpret_cast<CAllocator*>(sp + 0x460));  // bl @0x1940190
    St64(self, 0x23a8, Addr(self + 0x22f8));
    St64(self, 0x23b0, 0x0000000300000000ULL);  // 0x2cb30e0 = {0, 3}: first index 0, count 3
    StoreIndexRecordAt(sp, 0x460, 6, Addr(sp + 0xb0));
    St64(sp, 0x5a8, kProceduralKey);
    St64(sp, 0x488, Addr(SymbolAt(sp, 0x5a8)->String()));  // bl @0x19401d4
    CGBuffer::Initialize(reinterpret_cast<SGIndexBufferInit*>(self + 0x2628),
                         reinterpret_cast<CAllocator*>(sp + 0x460));  // bl @0x19401e8
    St64(self, 0x26d8, Addr(self + 0x2628));
    St64(self, 0x26e0, 0x600000000ULL);  // d8 still holds 0x2d57118 = {0, 6}

    // ---- 0x19401ec-0x194032c: the two "Multi-Res Mask IB" index buffers (this+0x26f0, this+0x27a0) ----
    // 192 bytes of indices at sp+0x460: 96 bytes from 0x3270544, then 96 bytes from 0x2d55f40.
    std::memcpy(sp + 0x460, kMultiResMaskIndices, sizeof kMultiResMaskIndices);
    St64(sp, 0x5a8, 0x60);
    St64(sp, 0x5c8, Addr(sp + 0x460));
    St64(sp, 0x5d0, Addr(kNameMultiResMask));
    St64(sp, 0x5c0, 0);
    St64(sp, 0x5b8, 0);
    St64(sp, 0x5b0, 2);  // d8 = 0x2c43d60: index size 2
    if (CMemoryContext::context == nullptr) {               // cbnz x10 @0x19402a8
        NWriteLog::WriteLog(8, nullptr, kNoMemoryContext);  // bl @0x19402bc
        SoftwareBreakpoint(1, 0x19402c4)();                 // brk #1 @0x19402c0
    }
    CGBuffer::Initialize(reinterpret_cast<SGIndexBufferInit*>(self + 0x26f0),
                         reinterpret_cast<CAllocator*>(sp + 0x5a8));  // bl @0x19402cc
    // 24 bytes of indices (0x32705a4) at sp+0x90.
    std::memcpy(sp + 0x90, kMultiResMaskIndices2, sizeof kMultiResMaskIndices2);
    St64(sp, 0x5c0, 0);
    St64(sp, 0x5b8, 0);
    St64(sp, 0x5b0, 2);
    St64(sp, 0x5a8, 0xc);
    St64(sp, 0x5d0, Addr(kNameMultiResMask));
    St64(sp, 0x5c8, Addr(sp + 0x90));
    if (CMemoryContext::context == nullptr) {               // cbnz x10 @0x1940308
        NWriteLog::WriteLog(8, nullptr, kNoMemoryContext);  // bl @0x194031c
        SoftwareBreakpoint(1, 0x1940324)();                 // brk #1 @0x1940320
    }
    CGBuffer::Initialize(reinterpret_cast<SGIndexBufferInit*>(self + 0x27a0),
                         reinterpret_cast<CAllocator*>(sp + 0x5a8));  // bl @0x194032c

    // ---- 0x1940330-0x19404f8: "jitter_texture_1d_4" (this+0x2850), 4 samples from FUN_019421f8 ----
    CAllocator* const jitter4_allocator = CMemoryContext::CurrentPtr();  // bl @0x1940330
    St64(sp, 0x90, Addr(jitter4_allocator));
    St64(sp, 0x98, 0);
    St64(sp, 0xa0, 0);
    St64(sp, 0xa8, 0);
    FUN_019421f8(4, sp + 0x90);  // bl @0x1940344: the sample data pointer comes back at sp+0x98
    const std::uint64_t jitter4_data = Ld64(sp, 0x98);
    St4(sp, 0x460, 0x12, 0x40, 0x40, 1);  // 0x2d55fa0
    St4(sp, 0x470, 4, 0, 0, 0);           // 0x2c9ebc0
    St4(sp, 0x480, 0, 1, 0, 0);           // the third head kept in [sp+0x70] since the lightmaps
    St64(sp, 0x490, ~static_cast<std::uint64_t>(0));
    St64(sp, 0x498, 0);
    St32(sp, 0x4a0, 0);
    St64(sp, 0x4b8, Addr(kNameJitter4));
    St64(sp, 0x4b0, context_allocator);
    St64(sp, 0x4a8, jitter4_data);
    St64(sp, 0x608, jitter4_data);  // kept for the release below
    FUN_019dad8c(sp + 0x5a8, sp + 0x460, 0);  // bl @0x19403ac
    FUN_019daf90(self + 0x2850, sp + 0x5a8, Ld32(sp, 0x460), Ld32(sp, 0x484), 0,
                 AsPointer(Ld64(sp, 0x4a8)), AsString(Ld64(sp, 0x4b8)));  // bl @0x19403cc
    FUN_019dad8c(sp + 0x5a8, sp + 0x460, 0);                              // bl @0x19403dc
    St64(sp, 0x5a0, 0);
    std::uint64_t* const jitter4_record =
        static_cast<std::uint64_t*>(__emutls_get_address(DAT_03853780));  // bl @0x1940400
    std::uint64_t* const jitter4_block =
        static_cast<std::uint64_t*>(__emutls_get_address(DAT_038537a0));  // bl @0x1940420
    StoreAllocationCallbacks(jitter4_record, jitter4_block, 0x1c7);
    DAT_03875688(DAT_03875fd8, sp + 0x5a8, jitter4_block, sp + 0x5a0);  // blr @0x1940478
    St64(sp, 0x598, 0);
    St64(sp, 0x590, 0);
    St64(sp, 0x588, 0);
    DAT_038755d8(DAT_03875fd8, Ld64(sp, 0x5a0), sp + 0x588);  // blr @0x1940498
    StoreAllocationCallbacks(jitter4_record, jitter4_block, 0x1cd);
    DAT_03875690(DAT_03875fd8, Ld64(sp, 0x5a0), jitter4_block);  // blr @0x19404c4
    St64(sp, 0x450, Ld64(sp, 0x588));
    // The sample data goes back to the allocator it came from (both reloaded from the frame).
    CAllocator* const jitter4_owner = reinterpret_cast<CAllocator*>(Ld64(sp, 0x90));
    if (reinterpret_cast<std::uintptr_t>(jitter4_owner) + 1 >= 2) {  // b.cc @0x19404dc
        void* const release = reinterpret_cast<void*>(Ld64(sp, 0x608));
        if (release != nullptr) {  // cbz x8 @0x19404e4
            VirtualSlot<FreeFn>(jitter4_owner, 0x38)(jitter4_owner, release);  // blr @0x19404f4
        }
    }

    // ---- 0x19404f8-0x19406ac: "jitter_texture_1d_8" (this+0x28a8), 8 samples ----
    CAllocator* const jitter8_allocator = CMemoryContext::CurrentPtr();  // bl @0x19404f8
    St64(sp, 0x90, Addr(jitter8_allocator));
    St64(sp, 0x98, 0);
    St64(sp, 0xa0, 0);
    St64(sp, 0xa8, 0);
    FUN_019421f8(8, sp + 0x90);  // bl @0x194050c
    const std::uint64_t jitter8_data = Ld64(sp, 0x98);
    St4(sp, 0x460, 0x12, 0x40, 0x40, 1);
    St4(sp, 0x470, 8, 0, 0, 0);  // 0x2d55fb0
    St4(sp, 0x480, 0, 1, 0, 0);
    St64(sp, 0x490, ~static_cast<std::uint64_t>(0));
    St32(sp, 0x4a0, 0);
    St64(sp, 0x498, 0);
    St64(sp, 0x4b8, Addr(kNameJitter8));
    St64(sp, 0x4b0, context_allocator);
    St64(sp, 0x4a8, jitter8_data);
    FUN_019dad8c(sp + 0x5a8, sp + 0x460, 0);  // bl @0x1940564
    FUN_019daf90(self + 0x28a8, sp + 0x5a8, Ld32(sp, 0x460), Ld32(sp, 0x484), 0,
                 AsPointer(Ld64(sp, 0x4a8)), AsString(Ld64(sp, 0x4b8)));  // bl @0x1940584
    FUN_019dad8c(sp + 0x5a8, sp + 0x460, 0);                              // bl @0x1940594
    St64(sp, 0x5a0, 0);
    std::uint64_t* const jitter8_record =
        static_cast<std::uint64_t*>(__emutls_get_address(DAT_03853780));  // bl @0x19405b8
    std::uint64_t* const jitter8_block =
        static_cast<std::uint64_t*>(__emutls_get_address(DAT_038537a0));  // bl @0x19405d0
    StoreAllocationCallbacks(jitter8_record, jitter8_block, 0x1c7);
    DAT_03875688(DAT_03875fd8, sp + 0x5a8, jitter8_block, sp + 0x5a0);  // blr @0x194062c
    St64(sp, 0x598, 0);
    St64(sp, 0x590, 0);
    St64(sp, 0x588, 0);
    DAT_038755d8(DAT_03875fd8, Ld64(sp, 0x5a0), sp + 0x588);  // blr @0x194064c
    StoreAllocationCallbacks(jitter8_record, jitter8_block, 0x1cd);
    DAT_03875690(DAT_03875fd8, Ld64(sp, 0x5a0), jitter8_block);  // blr @0x1940678
    St64(sp, 0x450, Ld64(sp, 0x588));
    CAllocator* const jitter8_owner = reinterpret_cast<CAllocator*>(Ld64(sp, 0x90));
    if (reinterpret_cast<std::uintptr_t>(jitter8_owner) + 1 >= 2) {  // b.cc @0x1940690
        if (jitter8_data != 0) {                                      // cbz x21 @0x1940694
            VirtualSlot<FreeFn>(jitter8_owner, 0x38)(jitter8_owner,
                                                     reinterpret_cast<void*>(jitter8_data));  // blr @0x19406a4
        }
    }
    return 0;  // mov w0, wzr; b @0x19406ac to the shared epilogue at 0x193e8d0, which ends in ret @0x193e8f4
}

}  // namespace NRadEngine
