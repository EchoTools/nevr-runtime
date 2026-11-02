// NRadEngine::NRadGame::CR15NetCustomizationRenderer::LoadMeshData
// Quest libr15.so VA 0x1347d90; ReVault binary 576.
// ReVault raw parent 100223186, reconstruction node 100600616; grade M: all 747 instructions traced to the ret at 0x1348938, every callee named by the decompiler or an import, 1 placeholder stays (global DAT_0386dec8, VA 0x386dec8), the 0x400 size limit behind GOT slot 0x38173d0 is Ghidra's resolved value (that slot is zero on disk, so it is not byte-verified), brk #1 is the tree's SoftwareBreakpoint call, the renderer, context, draw-data and init structs are byte offsets only, the key at 0x2ca4e38 is loaded directly with no lookup, and the thunks at 0x102a260 (GetPropertyValue) and 0x1041f10 (CGGraphicsState::Initialize) are not byte-checked, since their GOT slots 0x37ec818 and 0x37f8670 hold 0x00ecfb40 on disk, not the callee's address.
// @0x1347d90 — NRadEngine::NRadGame::CR15NetCustomizationRenderer::LoadMeshData
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

// Placeholder name (ReVault and the decompiler give it none): the pointer-sized global the listing loads through
// GOT slot 0x386dec8 at 0x1347e08 (adrp x9, 0x386d000; ldr x9, [x9, #0xec8]); the file has no bytes there. The listing
// adds 0x13a8 to it once and stores the sum at +0x200 and +0x208 of every new draw-data slot.
extern std::uint64_t DAT_0386dec8;

namespace NRadEngine {

class CAllocator;
class CGTextureResource;

// The brk #1 at 0x1348678 is shown by the decompiler as a call to this handler, resumed at 0x134867c; the tree models the
// trap the same way.
using BreakpointHandler = void (*)();
BreakpointHandler SoftwareBreakpoint(int kind, std::uintptr_t address);

namespace NWriteLog {
// Enumerators are not established. This function uses 8 (error level).
enum ELogLevel : std::int32_t {};
// NRadEngine::NWriteLog::WriteLog(ELogLevel, unsigned long long, char const*, ...) @ 0x1088c20 (thunk 0x104b960). Variadic.
void WriteLog(ELogLevel level, unsigned long long channel, const char* format, ...);
}  // namespace NWriteLog

namespace CMemory {
// NRadEngine::CMemory::Fill(void*, unsigned char, unsigned long long) @ 0x12cafcc (thunk 0x1047300).
void Fill(void* destination, unsigned char value, std::uint64_t count);
}  // namespace CMemory

struct CMemoryContext {
  // The cell read at 0x1348640-0x1348650 through GOT slot 0x3821d00; its value is the memory context pointer.
  static void* context;
  // NRadEngine::CMemoryContext::CurrentPtr() @ 0x12cae90 (thunk 0xffbc50). Static.
  static CAllocator* CurrentPtr();
  // NRadEngine::CMemoryContext::SetCurrent(CAllocator*) @ 0x12cafac (thunk 0xfd3590). Static.
  static void SetCurrent(CAllocator* allocator);
};

struct CMemBlock {
  // NRadEngine::CMemBlock::Resize(unsigned long long, unsigned int) @ 0x12dc30c (thunk 0x103e400): this, new size, keep flag.
  void Resize(std::uint64_t new_size, std::uint32_t keep_contents);
};

struct CSymbol64 {
  std::uint64_t key;
  // NRadEngine::CSymbol64::String() const @ 0x12ccb48 (thunk 0x1038ec0).
  const char* String() const;
  // NRadEngine::CSymbol64::CalculateSymbolValue @ 0x12cc580 (thunk 0x1046210), declared as in cjson_navigate_path.cpp.
  static std::uint64_t CalculateSymbolValue(const char* text, std::int64_t length, std::uint32_t arg3, std::uint64_t arg4,
                                            std::uint32_t arg5);
};

// NRadEngine::InvalidValue<NRadEngine::CSymbol64>::value, the name the decompiler gives the GOT target at 0x38127f8.
template <typename T>
struct InvalidValue {
  static const T value;
};

// STempString<N>: the temporary that MakeTStrPrintF returns in the caller's frame.
template <std::size_t N>
struct STempString {
  unsigned char storage[N];
};

// MakeTStrPrintF<512>(char const*, ...) @ 0x1086b9c (thunk 0x102b370).
template <std::size_t N>
STempString<N> MakeTStrPrintF(const char* format, ...);

// SCompactPoolHandle: a 32-bit pool index, passed in w1.
struct SCompactPoolHandle {
  std::uint32_t index;
};

class CModelCS {
 public:
  // NRadEngine::CModelCS::Model(SCompactPoolHandle) const @ 0x16b39f0 (thunk 0x102d780). Returns a pointer to the model data.
  void* Model(SCompactPoolHandle handle) const;
};

template <typename T>
class CComponentHandle {
 public:
  // CComponentHandle<T>::RefreshCache() const @ 0x1313e00 (thunk 0x1009460).
  void RefreshCache() const;
};

class CGMaterial {
 public:
  // NRadEngine::CGMaterial::MaterialData() const @ 0x1962364 (thunk 0xffc5b0): returns *(*(this + 8) + 0x70).
  void* MaterialData() const;
  // NRadEngine::CGMaterial::GetPropertyValue(CSymbol64, void*, unsigned int) const @ 0x19627b0. Called through the thunk at
  // 0x102a260, which ReVault names thunk_FUN_019627c0 (0x19627c0 is 0x10 bytes into GetPropertyValue); its decompile is a call
  // to GetPropertyValue. The thunk's GOT slot 0x37ec818 holds 0x00ecfb40 on disk, so the binding to 0x19627b0 is not byte-checked.
  std::uint64_t GetPropertyValue(CSymbol64 name, void* out, std::uint32_t size) const;
};

// The structure GetMeshRuntimeData fills. Its highest write is the u32 at +0x74, so 0x78 bytes; the caller clears the first
// 0x58 bytes. Fields read here: +0x00 mesh header, +0x08 vertex format source, +0x38 submesh array, +0x40 and +0x48 pointers.
struct SGMeshRuntimeData {
  alignas(8) std::byte bytes[0x78];
};

class CGMeshListResource {
 public:
  // NRadEngine::CGMeshListResource::GetMeshRuntimeData(unsigned long long, SGMeshRuntimeData&) const @ 0x19632e0
  // (thunk 0xff9e70).
  void GetMeshRuntimeData(std::uint64_t index, SGMeshRuntimeData& out) const;
};

class CGTexture {
 public:
  // NRadEngine::CGTexture::Resource() @ 0x19e19d0 (thunk 0x1011be0): returns *(this + 8). Not const.
  CGTextureResource* Resource();
  // NRadEngine::CGTexture::MaxWidth() const @ 0x19e19d8 (thunk 0x101c5f0).
  std::uint32_t MaxWidth() const;
  // NRadEngine::CGTexture::MaxHeight() const @ 0x19e19f4 (thunk 0x10082c0).
  std::uint32_t MaxHeight() const;
};

class CResourceInstance {
 public:
  // NRadEngine::CResourceInstance::LoadAndInit(CSymbol64, unsigned int) @ 0x26423b4 (thunk 0xfd6c90). Only w0 is tested.
  std::uint64_t LoadAndInit(CSymbol64 name, std::uint32_t flags);
};

template <typename T>
class CTableXT {
 public:
  // NRadEngine::CTableXT<NRadEngine::NRadGame::SR15NetCustomDrawData>::Grow() @ 0x134893c (thunk 0x102c210). Returns the new element.
  T* Grow();
};

template <typename T, template <typename> class Storage>
class CSet {
 public:
  // NRadEngine::CSet<NRadEngine::CGTextureResource*, NRadEngine::CTableXT>::Add(CGTextureResource* const&) @ 0x1348ad4
  // (thunk 0x103dfc0).
  std::uint64_t Add(T const& value);
};

// NRadEngine::SGRTInit, the argument of CGRenderTexture::Initialize. No layout is recovered; 0x50 bytes is the extent this
// function writes (0x1348608-0x1348688).
struct SGRTInit {
  alignas(8) std::byte bytes[0x50];
};

class CGRenderTexture {
 public:
  // NRadEngine::CGRenderTexture::Initialize(SGRTInit const&) @ 0x19b1934 (thunk 0xffb710).
  std::uint64_t Initialize(const SGRTInit& init);
};

class CGRenderTargetState {
 public:
  // NRadEngine::CGRenderTargetState::SGRTState, the argument of Initialize; 0x148 bytes is the extent this function writes
  // (0x13486a4-0x1348760).
  struct SGRTState {
    alignas(8) std::byte bytes[0x148];
  };
  // NRadEngine::CGRenderTargetState::Initialize(SGRTState const&) @ 0x1953a24 (thunk 0xfe77a0).
  std::uint64_t Initialize(const SGRTState& state);
};

// NRadEngine::CGGraphicsStateInit, the argument of CGGraphicsState::Initialize. Its first 0x128 bytes are a CGVertexFormat
// (Initialize copies them), and it reads +0x278 and +0x2c8; 0x340 bytes is the extent this function writes.
struct CGGraphicsStateInit {
  alignas(8) std::byte bytes[0x340];
};

class CGGraphicsState {
 public:
  // NRadEngine::CGGraphicsState::Initialize(CGGraphicsStateInit const&) @ 0x1954cc0. Called through the thunk at 0x1041f10,
  // which ReVault names thunk_FUN_01954cd8 (0x1954cd8 is 0x18 bytes into Initialize); its decompile is a call to
  // CGGraphicsState::Initialize. The thunk's GOT slot 0x37f8670 holds 0x00ecfb40 on disk, so the binding to 0x1954cc0 is not byte-checked.
  void Initialize(const CGGraphicsStateInit& init);
};

class CGVertexFormat {
 public:
  // Enumerators are not established.
  enum EUsage : std::uint32_t {};
  enum EType : std::uint32_t {};
  // NRadEngine::CGVertexFormat::CGVertexFormat() @ 0x2909098 (thunk 0x102ad40).
  CGVertexFormat();
  // NRadEngine::CGVertexFormat::AppendElement(EUsage, EType, unsigned long long, unsigned long long, unsigned long long,
  // unsigned int) @ 0x290a4c8 (thunk 0x10348e0). The parameter names are not recovered.
  void AppendElement(EUsage usage, EType type, std::uint64_t arg3, std::uint64_t arg4, std::uint64_t arg5, std::uint32_t arg6);
  // 296 bytes, from the decompiler's array size and the 0x128 byte copies.
  alignas(8) std::byte bytes[0x128];
};

// The 0x58 bytes MakeSubpassLayoutDefault writes through x8; the demangled name does not name the type.
struct MakeSubpassLayoutDefaultResult {
  alignas(8) std::byte bytes[0x58];
};

// NRadEngine::MakeSubpassLayoutDefault(CSymbol64, CSymbol64 const*) @ 0x199e478 (thunk 0x1008560).
MakeSubpassLayoutDefaultResult MakeSubpassLayoutDefault(CSymbol64 target, const CSymbol64* override_symbol);

static_assert(sizeof(SGMeshRuntimeData) == 0x78);
static_assert(sizeof(SGRTInit) == 0x50);
static_assert(sizeof(CGRenderTargetState::SGRTState) == 0x148);
static_assert(sizeof(CGGraphicsStateInit) == 0x340);
static_assert(sizeof(CGVertexFormat) == 0x128);
static_assert(sizeof(MakeSubpassLayoutDefaultResult) == 0x58);

namespace {

template <typename T>
T ReadAt(const void* base, std::size_t offset) {
  T value;
  std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof value);
  return value;
}

template <typename T>
void WriteAt(void* base, std::size_t offset, T value) {
  std::memcpy(static_cast<std::byte*>(base) + offset, &value, sizeof value);
}

// CComponentHandle<CModelCS>.
constexpr std::size_t kHandleComponentOffset = 0x40;  // +0x40: the cached CModelCS*, read between the two RefreshCache calls
constexpr std::size_t kHandlePoolOffset = 0x48;       // +0x48: the SCompactPoolHandle (u32) passed to CModelCS::Model
// The model data CModelCS::Model returns.
constexpr std::size_t kModelMeshListOffset = 0xc0;    // +0xc0: the CGMeshListResource*
constexpr std::size_t kModelMaterialsOffset = 0x378;  // +0x378: array of CGMaterial, 0x90 bytes each
constexpr std::uint64_t kMaterialStride = 0x90;
constexpr std::size_t kMaterialResourceOffset = 0x08;  // CGMaterial +0x08: pointer whose +0x38 is the material key
constexpr std::size_t kMaterialKeyOffset = 0x38;
// CGMeshListResource and the mesh header.
constexpr std::size_t kMeshListCountOffset = 0xa0;     // CGMeshListResource +0xa0: mesh count
constexpr std::size_t kMeshFlagsOffset = 0x64;         // mesh header +0x64: flags byte; bit 1 skips the mesh
constexpr unsigned kMeshSkipFlag = 0x2;
constexpr std::size_t kMeshSubmeshCountOffset = 0x38;  // mesh header +0x38: submesh count (u32)
constexpr std::size_t kMeshDataMeshOffset = 0x00;      // SGMeshRuntimeData +0x00: mesh header pointer
constexpr std::size_t kMeshDataVertexFormatOffset = 0x08;  // SGMeshRuntimeData +0x08: vertex format source pointer
constexpr std::size_t kMeshDataSubmeshesOffset = 0x38;     // SGMeshRuntimeData +0x38: submesh array
constexpr std::size_t kMeshDataPointerAOffset = 0x40;      // SGMeshRuntimeData +0x40: copied into the record at +0x10
constexpr std::size_t kMeshDataPointerBOffset = 0x48;      // SGMeshRuntimeData +0x48: copied into the record at +0x18
constexpr std::size_t kMeshDataClearBytes = 0x58;          // bytes the caller clears before GetMeshRuntimeData
constexpr std::uint64_t kSubmeshStride = 0x70;
constexpr std::size_t kSubmeshMaterialIndexOffset = 0x28;  // submesh +0x28: material index (u32)
constexpr std::size_t kSubmeshRecordFieldOffset = 0x4c;    // submesh +0x4c: unaligned u64 copied into the record at +0x08
// CGMaterial data (CGMaterial::MaterialData()).
constexpr std::size_t kMaterialDataTypeOffset = 0x190;     // +0x190: compared with kPlayerMaterialType
constexpr std::size_t kMaterialDataEntriesOffset = 0x150;  // +0x150: sorted (key, value) pairs, 16 bytes each
constexpr std::size_t kMaterialDataCountOffset = 0x180;    // +0x180: number of pairs
// SR15NetCustomizationContext.
constexpr std::size_t kContextSlotsOffset = 0x78;         // +0x78: slot array pointer, and the CTableXT that Grow extends
constexpr std::size_t kContextSlotCountOffset = 0xa8;     // +0xa8: slot count (64-bit)
constexpr std::size_t kContextTextureSetOffset = 0x370;   // +0x370: CSet<CGTextureResource*, CTableXT>
constexpr std::uint64_t kSlotStride = 0x9718;             // bytes per SR15NetCustomDrawData slot
// SR15NetCustomDrawData (one slot).
constexpr std::size_t kSlotRecordTableOffset = 0x148;      // +0x148: table of 0x28 byte records; data pointer first
constexpr std::size_t kSlotRecordAllocatorOffset = 0x158;  // +0x158: allocator of that table
constexpr std::size_t kSlotGrowthOffset = 0x168;           // +0x168: word the growth step is at least
constexpr std::size_t kSlotCapacityOffset = 0x170;         // +0x170: capacity
constexpr std::size_t kSlotCountOffset = 0x178;            // +0x178: record count
constexpr std::size_t kSlotKeyOffset = 0x180;              // +0x180: the key (material +0x38 chain)
constexpr std::size_t kSlotCombineFlagOffset = 0x1a0;      // +0x1a0: property read last, tested before filling a record
constexpr std::size_t kSlotTextureAOffset = 0x1b8;         // +0x1b8: CResourceInstance / CGTexture, sizes the target
constexpr std::size_t kSlotTextureBOffset = 0x1d0;         // +0x1d0: CResourceInstance / CGTexture
constexpr std::size_t kSlotTextureCOffset = 0x1e8;         // +0x1e8: CResourceInstance / CGTexture
constexpr std::size_t kSlotSharedAOffset = 0x200;          // +0x200: DAT_0386dec8 + 0x13a8
constexpr std::size_t kSlotSharedBOffset = 0x208;          // +0x208: DAT_0386dec8 + 0x13a8
constexpr std::size_t kSlotResourceDOffset = 0x210;        // +0x210: CResourceInstance, loaded from kUiNumberArrayKey
constexpr std::size_t kSlotRenderTextureOffset = 0x228;    // +0x228: CGRenderTexture
constexpr std::size_t kSlotRenderTargetStateOffset = 0x8db0;  // +0x8db0: CGRenderTargetState
constexpr std::size_t kSlotFormatValueOffset = 0x9704;     // +0x9704: u32 stored in the graphics state init at +0x2dc
constexpr std::uint64_t kSharedObjectOffset = 0x13a8;
// The 0x28 byte record appended to a slot's table.
constexpr std::uint64_t kRecordStride = 0x28;
// CGTextureResource.
constexpr std::size_t kTextureResourceInnerOffset = 0x70;  // +0x70: pointer to the underlying texture
constexpr std::size_t kTextureResourceStateOffset = 0xc0;  // +0xc0 of that: u32, zero means not yet in the set
// Renderer (this).
constexpr std::size_t kRendererPointerAOffset = 0xe00;  // address stored in the graphics state init at +0x278
constexpr std::size_t kRendererPointerBOffset = 0xe20;  // address stored in the graphics state init at +0x2c8
constexpr std::size_t kRendererBlockAOffset = 0xe58;    // 0x28 bytes copied to the graphics state init at +0x250
constexpr std::size_t kRendererBlockBOffset = 0xe98;    // 0x28 bytes copied to the graphics state init at +0x2a0
constexpr std::size_t kRendererBlockBytes = 0x28;
// Graphics state init (CGGraphicsStateInit) offsets.
constexpr std::size_t kGfxInitVertexFormatBOffset = 0x000;  // CGVertexFormat constructed at 0x134881c, then overwritten at 0x13488f0
constexpr std::size_t kGfxInitVertexFormatCOffset = 0x128;  // CGVertexFormat constructed at 0x134882c, then overwritten at 0x134888c
constexpr std::size_t kGfxInitBlockAOffset = 0x250;
constexpr std::size_t kGfxInitFillAOffset = 0x258;
constexpr std::size_t kGfxInitPointerAOffset = 0x278;
constexpr std::size_t kGfxInitZeroBytes = 0x30;
constexpr std::size_t kGfxInitBlockBOffset = 0x2a0;
constexpr std::size_t kGfxInitFillBOffset = 0x2a8;
constexpr std::size_t kGfxInitPointerBOffset = 0x2c8;
constexpr std::size_t kGfxInitTwoOffset = 0x2d0;
constexpr std::size_t kGfxInitZeroWordOffset = 0x2d8;
constexpr std::size_t kGfxInitFormatValueOffset = 0x2dc;
constexpr std::size_t kGfxInitFourOffset = 0x2e0;
constexpr std::size_t kGfxInitZeroWord2Offset = 0x2e4;
constexpr std::size_t kGfxInitAllOnesOffset = 0x2e8;
constexpr std::size_t kGfxInitAllOnesBytes = 0x48;
constexpr std::size_t kGfxInitTailOffset = 0x330;
constexpr std::size_t kGfxInitTailBytes = 0x10;
constexpr std::size_t kVertexFormatBytes = 0x128;
// Render target state (SGRTState) offsets.
constexpr std::size_t kStateHeaderOffset = 0x00;
constexpr std::size_t kStateTextureOffset = 0x08;
constexpr std::size_t kStateZeroOffset = 0x10;
constexpr std::size_t kStateZeroBytes = 0xc0;
constexpr std::size_t kStateOneOffset = 0xd0;
constexpr std::size_t kStateWordAOffset = 0xd4;
constexpr std::size_t kStateWordBOffset = 0xdc;
constexpr std::size_t kStateWordCOffset = 0xe0;
constexpr std::size_t kStateWordDOffset = 0xe4;
constexpr std::size_t kStateLayoutOffset = 0xe8;
constexpr std::size_t kStateAllOnesBytes = 0x48;
constexpr std::size_t kStateTailOffset = 0x130;
constexpr std::size_t kStateTailBytes = 0x18;
// Render texture init (SGRTInit) offsets.
constexpr std::size_t kRtInitFormatOffset = 0x00;
constexpr std::size_t kRtInitWidthOffset = 0x04;
constexpr std::size_t kRtInitHeightOffset = 0x08;
constexpr std::size_t kRtInitOnesOffset = 0x0c;
constexpr std::size_t kRtInitZeroAOffset = 0x14;
constexpr std::size_t kRtInitZeroABytes = 0x14;
constexpr std::size_t kRtInitOneOffset = 0x18;
constexpr std::size_t kRtInitMinusOneAOffset = 0x28;
constexpr std::size_t kRtInitZeroBOffset = 0x30;
constexpr std::size_t kRtInitZeroBBytes = 0x18;
constexpr std::size_t kRtInitContextWordOffset = 0x38;
constexpr std::size_t kRtInitSymbolOffset = 0x48;

// The largest width or height a customization target takes (0x13485bc, 0x13485cc). The listing loads a u32 through GOT slot
// 0x38173d0 (0x1347df4-0x1347e04, spilled to sp+0xb4) and the decompiler resolves it to this value; the slot has no file bytes.
constexpr std::uint32_t kMaxTargetDimension = 0x400;
constexpr std::uint32_t kPropertyValueSize = 4;
constexpr auto kErrorLevel = static_cast<NWriteLog::ELogLevel>(8);

// Symbol keys, as the 8 bytes loaded from .rodata. The string stored right after each key in .rodata is named beside it.
// The slot's material type: compared with CGMaterial data +0x190. The string after 0x2ca4bb8 is "r15player".
constexpr std::uint64_t kPlayerMaterialType = 0x7a8154038e31439aULL;
// Texture symbols looked up in the material data: the keys at 0x2ca4db0, 0x2ca4dd8 and 0x2ca4e08, with the names
// "shading_basecolormap", "customization_primaryblendmask" and "customization_secondaryblendmask" stored after them. The key
// at 0x2ca4e38 is not looked up (see kUiNumberArrayKey).
constexpr std::uint64_t kTextureAKey = 0x01bc8bf07fbfc184ULL;  // "shading_basecolormap"
constexpr std::uint64_t kTextureBKey = 0x94ffd3d8117518b9ULL;  // "customization_primaryblendmask"
constexpr std::uint64_t kTextureCKey = 0x7abbb0b3b865219bULL;  // "customization_secondaryblendmask"
constexpr std::uint64_t kUiNumberArrayKey = 0x6a03c3fb262735e1ULL;  // "ui_number_array" (after 0x2ca4e38); loaded directly, not looked up
// The u64 at 0x2ca3b88, stored at the head of the render target state.
constexpr std::uint64_t kStateHeader = 0x0000000100000043ULL;

struct MaterialProperty {
  std::uint64_t key;
  std::size_t slotOffset;
};

// The eleven GetPropertyValue calls of 0x13481b8-0x13482a8, in call order. Each key is the u64 at the .rodata address
// beside it; the string stored right after that key is named.
constexpr MaterialProperty kMaterialProperties[] = {
    {0x7ca79a9d8ee4141cULL, 0x188},  // 0x2ca4bd8, "customization.enablepattern"
    {0x56ceaf59f50c3ea9ULL, 0x18c},  // 0x2ca4c00, "customization.patternuvscale"
    {0xb5659811d9811e6eULL, 0x190},  // 0x2ca4c30, "customization.applydecal"
    {0x7cb98e8498e41401ULL, 0x194},  // 0x2ca4c58, "customization.enablenumbers"
    {0x27310cd90da00a8bULL, 0x198},  // 0x2ca4c80, "customization.numbersuvscale"
    {0x7760b7b20bf979dbULL, 0x19c},  // 0x2ca4cb0, "customization.tiledcomposite"
    {0x9f55242f1042753cULL, 0x1a8},  // 0x2ca4ce0, "baseuvset"
    {0xfebbf6019b385f52ULL, 0x1ac},  // 0x2ca4cf8, "customization.patternuvset"
    {0xff3db5562847ee68ULL, 0x1b0},  // 0x2ca4d20, "customization.decaluvset"
    {0xb4d4410ceb043a12ULL, 0x1b4},  // 0x2ca4d48, "customization.numbersuvset"
    {0x2dd22b885b2f4e41ULL, 0x1a0},  // 0x2ca4d78, "customization.combinemesheswhencompositing"
};

// A call returns a status in x0; the listing tests only w0 (cbnz w0).
bool Failed(std::uint64_t status) {
  return static_cast<std::int32_t>(status) != 0;
}

// Inlined three times (0x13482b0-0x13483f8, 0x1348410-0x1348474, 0x1348490-0x13484f0): a binary search of the material
// data's sorted (key, value) pairs. Keys compare as signed 64-bit; an empty table or a miss gives the InvalidValue symbol.
CSymbol64 FindSymbolValue(const std::byte* materialData, std::uint64_t key) {
  const std::uint64_t count = ReadAt<std::uint64_t>(materialData, kMaterialDataCountOffset);
  if (count == 0) {
    return InvalidValue<CSymbol64>::value;
  }
  const auto* const entries = ReadAt<const std::byte*>(materialData, kMaterialDataEntriesOffset);
  std::uint64_t low = 0;
  std::uint64_t high = count;
  while (low < high) {
    const std::uint64_t mid = low + ((high - low) >> 1);
    const auto entryKey = ReadAt<std::int64_t>(entries, mid * 16);
    if (entryKey > static_cast<std::int64_t>(key)) {
      high = mid;
    } else if (entryKey < static_cast<std::int64_t>(key)) {
      low = mid + 1;
    } else {
      // 0x13483f0: the listing tests the found index against -1 before taking the value address.
      if (mid == ~std::uint64_t{0}) {
        return InvalidValue<CSymbol64>::value;
      }
      return CSymbol64{ReadAt<std::uint64_t>(entries, mid * 16 + 8)};
    }
  }
  return InvalidValue<CSymbol64>::value;
}

// Inlined twice (0x13480d8-0x1348178 and 0x1348304-0x1348398): append one default-constructed 0x28 byte record to a slot's
// table, growing it first when the count has reached the capacity. The record is built with the table's allocator current.
std::byte* AppendRecord(std::byte* slot) {
  const std::uint64_t capacity = ReadAt<std::uint64_t>(slot, kSlotCapacityOffset);
  std::uint64_t count = ReadAt<std::uint64_t>(slot, kSlotCountOffset);
  if (count >= capacity) {
    const std::uint64_t minimum = ReadAt<std::uint64_t>(slot, kSlotGrowthOffset);
    const std::uint64_t grow = minimum < capacity ? capacity : minimum;
    reinterpret_cast<CMemBlock*>(slot + kSlotRecordTableOffset)->Resize((grow + capacity) * kRecordStride, 1);
    // The listing reloads the capacity and the count after the call.
    WriteAt<std::uint64_t>(slot, kSlotCapacityOffset, ReadAt<std::uint64_t>(slot, kSlotCapacityOffset) + grow);
    count = ReadAt<std::uint64_t>(slot, kSlotCountOffset);
  }
  auto* const data = ReadAt<std::byte*>(slot, kSlotRecordTableOffset);
  auto* const allocator = ReadAt<CAllocator*>(slot, kSlotRecordAllocatorOffset);
  WriteAt<std::uint64_t>(slot, kSlotCountOffset, count + 1);
  std::byte* const record = data + count * kRecordStride;
  CAllocator* const previous = CMemoryContext::CurrentPtr();
  CMemoryContext::SetCurrent(allocator);
  WriteAt<std::uint64_t>(record, 0x00, ~std::uint64_t{0});
  WriteAt<std::uint32_t>(record, 0x08, 0xffffffffU);
  WriteAt<std::uint64_t>(record, 0x0c, 0);
  WriteAt<std::uint64_t>(record, 0x1c, 0);
  WriteAt<std::uint64_t>(record, 0x14, 0);
  WriteAt<std::uint32_t>(record, 0x24, 0);
  CMemoryContext::SetCurrent(previous);
  return record;
}

// 0x1348190-0x13481ac and 0x13483a8-0x13483c0: the record fields taken from the mesh data and the submesh.
void StoreMeshFields(std::byte* record, const SGMeshRuntimeData& meshData, const std::byte* submesh) {
  WriteAt<std::uint64_t>(record, 0x10, ReadAt<std::uint64_t>(meshData.bytes, kMeshDataPointerAOffset));
  WriteAt<std::uint64_t>(record, 0x18, ReadAt<std::uint64_t>(meshData.bytes, kMeshDataPointerBOffset));
  WriteAt<std::uint64_t>(record, 0x20, ReadAt<std::uint64_t>(meshData.bytes, kMeshDataVertexFormatOffset));
  WriteAt<std::uint64_t>(record, 0x08, ReadAt<std::uint64_t>(submesh, kSubmeshRecordFieldOffset));
}

// 0x1348508-0x1348588, three times: a texture whose resource state is zero is added to the context's texture set.
void RegisterTexture(CSet<CGTextureResource*, CTableXT>* set, CGTexture* texture) {
  const auto* const resource = reinterpret_cast<const std::byte*>(texture->Resource());
  if (ReadAt<std::uint32_t>(ReadAt<const std::byte*>(resource, kTextureResourceInnerOffset), kTextureResourceStateOffset) == 0) {
    CGTextureResource* const added = texture->Resource();
    set->Add(added);
  }
}

}  // namespace

namespace NRadGame {

struct SR15NetCustomDrawData;
struct SR15NetCustomizationContext;

// NRadEngine::NRadGame::CR15NetCustomizationRenderer. No layout or size is recovered; this function only reads it, through a
// byte view, at +0xe00..+0xec0.
class CR15NetCustomizationRenderer {
 public:
  // NRadEngine::NRadGame::CR15NetCustomizationRenderer::LoadMeshData(SR15NetCustomizationContext&,
  // CComponentHandle<CModelCS>&) @ 0x1347d90. Returns 0, or the first nonzero status of a load or initialize call.
  std::uint64_t LoadMeshData(SR15NetCustomizationContext& context, CComponentHandle<CModelCS>& modelHandle);
};

// @0x1347d90 — NRadEngine::NRadGame::CR15NetCustomizationRenderer::LoadMeshData
/* @confidence: M */
std::uint64_t CR15NetCustomizationRenderer::LoadMeshData(SR15NetCustomizationContext& context,
                                                         CComponentHandle<CModelCS>& modelHandle) {
  // The renderer, the context and the component handle have no recovered layout; each is read where the listing reads it.
  const auto* const renderer = reinterpret_cast<const std::byte*>(this);
  auto* const contextBytes = reinterpret_cast<std::byte*>(&context);
  const auto* const handleBytes = reinterpret_cast<const std::byte*>(&modelHandle);

  // 0x1347dc4-0x1347de0: the cached component is read between the two RefreshCache calls, the pool handle after the second.
  modelHandle.RefreshCache();
  const auto* const modelComponent = ReadAt<const CModelCS*>(handleBytes, kHandleComponentOffset);
  modelHandle.RefreshCache();
  const SCompactPoolHandle modelPool{ReadAt<std::uint32_t>(handleBytes, kHandlePoolOffset)};
  auto* const model = static_cast<std::byte*>(modelComponent->Model(modelPool));

  // 0x1347de4-0x1347dec: no meshes, nothing to load.
  const std::uint64_t meshCount =
      ReadAt<std::uint64_t>(ReadAt<const std::byte*>(model, kModelMeshListOffset), kMeshListCountOffset);
  if (meshCount == 0) {
    return 0;
  }

  // 0x1347e08-0x1347e4c: read once, after the mesh count test.
  const std::uint64_t sharedObject = DAT_0386dec8 + kSharedObjectOffset;

  for (std::uint64_t meshIndex = 0; meshIndex != meshCount; ++meshIndex) {
    // 0x1347fd0-0x1347ff4: clear 0x58 bytes, then fetch this mesh's runtime data from the model's mesh list.
    SGMeshRuntimeData meshData;
    std::memset(meshData.bytes, 0, kMeshDataClearBytes);
    ReadAt<const CGMeshListResource*>(model, kModelMeshListOffset)->GetMeshRuntimeData(meshIndex, meshData);

    // 0x1347ff8-0x134800c: a mesh with flag bit 1 set, or with no submeshes, is skipped.
    const auto* const mesh = ReadAt<const std::byte*>(meshData.bytes, kMeshDataMeshOffset);
    if ((ReadAt<std::uint8_t>(mesh, kMeshFlagsOffset) & kMeshSkipFlag) != 0) {
      continue;
    }
    const std::uint32_t submeshCount = ReadAt<std::uint32_t>(mesh, kMeshSubmeshCountOffset);
    if (submeshCount == 0) {
      continue;
    }

    // 0x134800c-0x1348068: the first submesh whose material's type is kPlayerMaterialType. The submesh array and the model's
    // material array are reloaded on every pass.
    const std::uint64_t submeshBytes = std::uint64_t{submeshCount} * kSubmeshStride;
    std::uint64_t submeshOffset = 0;
    std::uint32_t submeshIndex = 0;
    const std::byte* submeshes = nullptr;
    const CGMaterial* material = nullptr;
    std::byte* materialData = nullptr;
    std::uint64_t materialKey = 0;
    bool matched = false;
    do {
      submeshes = ReadAt<const std::byte*>(meshData.bytes, kMeshDataSubmeshesOffset);
      const auto* const materials = ReadAt<const std::byte*>(model, kModelMaterialsOffset);
      const std::uint32_t materialIndex = ReadAt<std::uint32_t>(submeshes + submeshOffset, kSubmeshMaterialIndexOffset);
      material = reinterpret_cast<const CGMaterial*>(materials + std::uint64_t{materialIndex} * kMaterialStride);
      materialData = static_cast<std::byte*>(material->MaterialData());
      materialKey = ReadAt<std::uint64_t>(ReadAt<const std::byte*>(material, kMaterialResourceOffset), kMaterialKeyOffset);
      if (ReadAt<std::uint64_t>(materialData, kMaterialDataTypeOffset) == kPlayerMaterialType) {
        matched = true;
        break;
      }
      submeshOffset += kSubmeshStride;
      ++submeshIndex;
    } while (submeshOffset != submeshBytes);
    if (!matched) {
      continue;
    }

    // 0x1348084-0x13480cc: find the context slot whose key (+0x180) equals the material key. An empty slot array is
    // treated as the empty range.
    const std::uint64_t slotCount = ReadAt<std::uint64_t>(contextBytes, kContextSlotCountOffset);
    auto* const slotTable = ReadAt<std::byte*>(contextBytes, kContextSlotsOffset);
    std::byte* const slotsBegin = slotCount == 0 ? nullptr : slotTable;
    std::byte* const slotsEnd = slotCount == 0 ? nullptr : slotTable + slotCount * kSlotStride;
    std::byte* existingSlot = nullptr;
    if (slotsBegin != slotsEnd) {
      const auto span = static_cast<std::uint64_t>(slotsEnd - slotsBegin);
      std::uint64_t slotOffset = 0;
      do {
        std::byte* const candidate = slotsBegin + slotOffset;
        if (ReadAt<std::uint64_t>(candidate, kSlotKeyOffset) == materialKey) {
          existingSlot = candidate;
          break;
        }
        slotOffset += kSlotStride;
      } while (slotOffset != span);
    }

    if (existingSlot != nullptr) {
      // 0x1348304-0x13483d4: the slot exists; append a record, filled only when the slot's +0x1a0 property is nonzero.
      std::byte* const record = AppendRecord(existingSlot);
      WriteAt<std::uint32_t>(record, 0x00, static_cast<std::uint32_t>(meshIndex));
      WriteAt<std::uint32_t>(record, 0x04, submeshIndex);
      if (ReadAt<std::uint32_t>(existingSlot, kSlotCombineFlagOffset) != 0) {
        StoreMeshFields(record, meshData, submeshes + submeshOffset);
      }
      continue;
    }

    // 0x13480d0-0x13480d8: a new slot from the context's table. Its key is stored before the record is appended.
    auto* const slot = reinterpret_cast<std::byte*>(
        reinterpret_cast<CTableXT<SR15NetCustomDrawData>*>(contextBytes + kContextSlotsOffset)->Grow());
    WriteAt<std::uint64_t>(slot, kSlotKeyOffset, materialKey);
    std::byte* const record = AppendRecord(slot);
    WriteAt<std::uint32_t>(record, 0x00, static_cast<std::uint32_t>(meshIndex));
    WriteAt<std::uint32_t>(record, 0x04, submeshIndex);
    StoreMeshFields(record, meshData, submeshes + submeshOffset);

    // 0x13481b8-0x13482a8: eleven material properties, each 4 bytes into the slot.
    for (const MaterialProperty& property : kMaterialProperties) {
      material->GetPropertyValue(CSymbol64{property.key}, slot + property.slotOffset, kPropertyValueSize);
    }

    // 0x13482ac-0x13484f0 and 0x1348508-0x13485ac: three textures from the material data's symbol table, each loaded in
    // turn; a nonzero status is returned at once.
    std::byte* const textureA = slot + kSlotTextureAOffset;
    std::byte* const textureB = slot + kSlotTextureBOffset;
    std::byte* const textureC = slot + kSlotTextureCOffset;
    std::uint64_t status =
        reinterpret_cast<CResourceInstance*>(textureA)->LoadAndInit(FindSymbolValue(materialData, kTextureAKey), 0);
    if (Failed(status)) {
      return status;
    }
    status = reinterpret_cast<CResourceInstance*>(textureB)->LoadAndInit(FindSymbolValue(materialData, kTextureBKey), 0);
    if (Failed(status)) {
      return status;
    }
    status = reinterpret_cast<CResourceInstance*>(textureC)->LoadAndInit(FindSymbolValue(materialData, kTextureCKey), 0);
    if (Failed(status)) {
      return status;
    }

    // 0x1348508-0x1348588: register the three textures with the context's texture set.
    auto* const textureSet =
        reinterpret_cast<CSet<CGTextureResource*, CTableXT>*>(contextBytes + kContextTextureSetOffset);
    RegisterTexture(textureSet, reinterpret_cast<CGTexture*>(textureA));
    RegisterTexture(textureSet, reinterpret_cast<CGTexture*>(textureB));
    RegisterTexture(textureSet, reinterpret_cast<CGTexture*>(textureC));

    // 0x134858c-0x13485ac: the shared object address twice, then the fourth resource (a fixed symbol, no lookup).
    WriteAt<std::uint64_t>(slot, kSlotSharedAOffset, sharedObject);
    WriteAt<std::uint64_t>(slot, kSlotSharedBOffset, sharedObject);
    status = reinterpret_cast<CResourceInstance*>(slot + kSlotResourceDOffset)->LoadAndInit(CSymbol64{kUiNumberArrayKey}, 0);
    if (Failed(status)) {
      return status;
    }

    // 0x13485b0-0x13485d0: the target is the first texture's size, each side limited to kMaxTargetDimension.
    const auto* const sizeTexture = reinterpret_cast<const CGTexture*>(textureA);
    const std::uint32_t maxWidth = sizeTexture->MaxWidth();
    const std::uint32_t width = kMaxTargetDimension < maxWidth ? kMaxTargetDimension : maxWidth;
    const std::uint32_t maxHeight = sizeTexture->MaxHeight();
    const std::uint32_t height = kMaxTargetDimension < maxHeight ? kMaxTargetDimension : maxHeight;

    // 0x13485d4-0x1348604: "Customization Target - <key>" and its symbol value.
    const CSymbol64 targetKey{materialKey};
    STempString<512> label = MakeTStrPrintF<512>("Customization Target - %s", targetKey.String());
    const std::uint64_t targetSymbol = CSymbol64::CalculateSymbolValue(
        reinterpret_cast<const char*>(&label), -1, 1, ~std::uint64_t{0}, 1);

    // 0x1348608-0x1348658: the render texture init.
    SGRTInit renderTextureInit;
    WriteAt<std::uint64_t>(renderTextureInit.bytes, kRtInitOnesOffset, 0x0000000100000001ULL);
    std::memset(renderTextureInit.bytes + kRtInitZeroAOffset, 0, kRtInitZeroABytes);
    WriteAt<std::uint64_t>(renderTextureInit.bytes, kRtInitMinusOneAOffset, ~std::uint64_t{0});
    std::memset(renderTextureInit.bytes + kRtInitZeroBOffset, 0, kRtInitZeroBBytes);
    WriteAt<std::uint32_t>(renderTextureInit.bytes, kRtInitFormatOffset, 0xd);
    WriteAt<std::uint32_t>(renderTextureInit.bytes, kRtInitWidthOffset, width);
    WriteAt<std::uint32_t>(renderTextureInit.bytes, kRtInitOneOffset, 1);
    WriteAt<std::uint32_t>(renderTextureInit.bytes, kRtInitHeightOffset, height);

    // 0x1348650-0x1348688: without a memory context the message is logged and the brk #1 at 0x1348678 is taken; the
    // handler resumes at 0x134867c, which reloads the cell.
    void* memoryContext = CMemoryContext::context;
    if (memoryContext == nullptr) {
      NWriteLog::WriteLog(kErrorLevel, 0ULL, "Trying to use memory context before it's initialized!");
      SoftwareBreakpoint(1, 0x134867c)();
      memoryContext = CMemoryContext::context;
    }
    WriteAt<std::uint64_t>(renderTextureInit.bytes, kRtInitContextWordOffset, ReadAt<std::uint64_t>(memoryContext, 8));
    WriteAt<std::uint64_t>(renderTextureInit.bytes, kRtInitSymbolOffset, targetSymbol);
    status = reinterpret_cast<CGRenderTexture*>(slot + kSlotRenderTextureOffset)->Initialize(renderTextureInit);
    if (Failed(status)) {
      return status;
    }

    // 0x13486a4-0x1348760: the render target state, with the default subpass layout copied in.
    CGRenderTargetState::SGRTState state;
    std::memset(state.bytes + kStateZeroOffset, 0, kStateZeroBytes);
    WriteAt<std::uint32_t>(state.bytes, kStateOneOffset, 0x3f800000U);
    WriteAt<std::uint64_t>(state.bytes, kStateWordAOffset, 0xffffffff00000000ULL);
    WriteAt<std::uint32_t>(state.bytes, kStateWordBOffset, 0xffffffffU);
    WriteAt<std::uint32_t>(state.bytes, kStateWordDOffset, 0);
    std::memset(state.bytes + kStateLayoutOffset, 0xff, kStateAllOnesBytes);
    std::memset(state.bytes + kStateTailOffset, 0, kStateTailBytes);
    WriteAt<std::uint32_t>(state.bytes, kStateWordCOffset, 0xffffU);
    WriteAt<const void*>(state.bytes, kStateTextureOffset, slot + kSlotRenderTextureOffset);
    WriteAt<std::uint64_t>(state.bytes, kStateHeaderOffset, kStateHeader);
    const MakeSubpassLayoutDefaultResult layout = MakeSubpassLayoutDefault(CSymbol64{targetSymbol}, nullptr);
    std::memcpy(state.bytes + kStateLayoutOffset, layout.bytes, sizeof layout.bytes);
    status = reinterpret_cast<CGRenderTargetState*>(slot + kSlotRenderTargetStateOffset)->Initialize(state);
    if (Failed(status)) {
      return status;
    }

    // 0x134876c-0x1348818: a vertex format with five elements.
    const std::uint32_t formatValue = ReadAt<std::uint32_t>(slot, kSlotFormatValueOffset);
    CGVertexFormat vertexFormat;
    vertexFormat.AppendElement(static_cast<CGVertexFormat::EUsage>(1), static_cast<CGVertexFormat::EType>(1), 3, 0, 0, 0);
    vertexFormat.AppendElement(static_cast<CGVertexFormat::EUsage>(4), static_cast<CGVertexFormat::EType>(8), 2, 0, 0, 0);
    vertexFormat.AppendElement(static_cast<CGVertexFormat::EUsage>(4), static_cast<CGVertexFormat::EType>(8), 2, 1, 0, 0);
    vertexFormat.AppendElement(static_cast<CGVertexFormat::EUsage>(4), static_cast<CGVertexFormat::EType>(8), 2, 2, 0, 0);
    vertexFormat.AppendElement(static_cast<CGVertexFormat::EUsage>(4), static_cast<CGVertexFormat::EType>(8), 2, 3, 0, 0);

    // 0x134881c-0x13488f0: the graphics state init. Two vertex formats are constructed in it and overwritten below.
    CGGraphicsStateInit init;
    new (static_cast<void*>(init.bytes + kGfxInitVertexFormatBOffset)) CGVertexFormat();
    new (static_cast<void*>(init.bytes + kGfxInitVertexFormatCOffset)) CGVertexFormat();
    WriteAt<std::uint64_t>(init.bytes, kGfxInitBlockAOffset, 0);
    CMemory::Fill(init.bytes + kGfxInitFillAOffset, 0, 0x20);
    std::memset(init.bytes + kGfxInitPointerAOffset, 0, kGfxInitZeroBytes);
    CMemory::Fill(init.bytes + kGfxInitFillBOffset, 0, 0x20);
    WriteAt<std::uint32_t>(init.bytes, kGfxInitZeroWord2Offset, 0);
    std::memset(init.bytes + kGfxInitAllOnesOffset, 0xff, kGfxInitAllOnesBytes);
    std::memset(init.bytes + kGfxInitTailOffset, 0, kGfxInitTailBytes);
    std::memcpy(init.bytes + kGfxInitVertexFormatCOffset, vertexFormat.bytes, kVertexFormatBytes);
    WriteAt<std::uint32_t>(init.bytes, kGfxInitFourOffset, 4);
    std::memcpy(init.bytes + kGfxInitBlockAOffset, renderer + kRendererBlockAOffset, kRendererBlockBytes);
    WriteAt<std::uint32_t>(init.bytes, kGfxInitZeroWordOffset, 0);
    WriteAt<std::uint32_t>(init.bytes, kGfxInitFormatValueOffset, formatValue);
    WriteAt<std::uint64_t>(init.bytes, kGfxInitTwoOffset, 2);
    std::memcpy(init.bytes + kGfxInitBlockBOffset, renderer + kRendererBlockBOffset, kRendererBlockBytes);
    WriteAt<const void*>(init.bytes, kGfxInitPointerAOffset, renderer + kRendererPointerAOffset);
    WriteAt<const void*>(init.bytes, kGfxInitPointerBOffset, renderer + kRendererPointerBOffset);

    // 0x13488ec-0x13488fc: the record's vertex format source is copied over the first one, then the slot is initialized.
    std::memcpy(init.bytes + kGfxInitVertexFormatBOffset, ReadAt<const std::byte*>(record, 0x20), kVertexFormatBytes);
    reinterpret_cast<CGGraphicsState*>(slot)->Initialize(init);
  }

  // 0x1348910: every mesh handled.
  return 0;
}

}  // namespace NRadGame

}  // namespace NRadEngine
