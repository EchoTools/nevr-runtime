// NRadEngine::NRadGame::CR15NetCustomizationRenderer::DestroyContext
// Quest libr15.so VA 0x13492fc; ReVault binary 576.
// ReVault raw parent 100223207, reconstruction node 100600568; grade M: all 89 instructions traced; brk #1 modelled as the tree's SoftwareBreakpoint call, CAllocator slot +0x38 unnamed with a void result, and the context and renderer have byte-offset access only (no recovered sizes).
// @0x13492fc — NRadEngine::NRadGame::CR15NetCustomizationRenderer::DestroyContext
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

struct CAllocator;

// The brk #1 at 0x1349340 is shown by the decompiler as a call to this handler; the tree models the trap the same way.
using BreakpointHandler = void (*)();
BreakpointHandler SoftwareBreakpoint(int kind, std::uintptr_t address);

namespace NWriteLog {
// Enumerators are not established. This function uses 8 (error level).
enum ELogLevel : std::int32_t {};
// NRadEngine::NWriteLog::WriteLog(ELogLevel, unsigned long long, char const*, ...) @ 0x1088c20 (thunk 0x104b960). Variadic.
void WriteLog(ELogLevel level, unsigned long long channel, const char* format, ...);
}  // namespace NWriteLog

// NRadEngine::OutputError(int, NRadEngine::NWriteLog::ELogLevel) @ 0x12d1b38 (thunk 0x104e4c0). Returns the status.
std::int32_t OutputError(std::int32_t status, NWriteLog::ELogLevel level);

struct CMemoryContext {
  // Read at 0x1349324 through a pointer to the cell; the first word of that cell is the current CAllocator*.
  static void* context;
  // NRadEngine::CMemoryContext::Current() @ 0x12c8dc4 (thunk 0x1028430). Static.
  static CAllocator* Current();
  // NRadEngine::CMemoryContext::SetCurrent(CAllocator*) @ 0x12cafac (thunk 0xfd3590). Static.
  static void SetCurrent(CAllocator* allocator);
};

// NRadEngine::CResourceInstance::UnloadAndShutdown() @ 0x2642230 (thunk 0xfe3770). Returns a 64-bit status.
class CResourceInstance {
 public:
  std::uint64_t UnloadAndShutdown();
};

// NRadEngine::CGRenderTexture::Destroy() @ 0x19a8070 (thunk 0xff5aa0). Returns a 64-bit status.
class CGRenderTexture {
 public:
  std::uint64_t Destroy();
};

// NRadEngine::CGRenderTargetState::Shutdown() @ 0x1953e40 (thunk 0xfd6140). Returns a 64-bit status. No class record in ReVault.
class CGRenderTargetState {
 public:
  std::uint64_t Shutdown();
};

// NRadEngine::CGGraphicsState::Shutdown() @ 0x1954fbc (thunk 0x1022830). Returns nothing. No class record in ReVault.
class CGGraphicsState {
 public:
  void Shutdown();
};

namespace NRadGame {

// NRadEngine::NRadGame::SR15NetCustomizationContext::~SR15NetCustomizationContext() @ 0x13d4930 (thunk 0xfe8510).
// No size is recovered, so the fields are reached by byte offset only and no sizeof is asserted.
struct SR15NetCustomizationContext {
  ~SR15NetCustomizationContext();
};

// NRadEngine::NRadGame::CR15NetCustomizationRenderer (base NRadEngine::CGRenderPass at +0x0). No size is recovered here.
class CR15NetCustomizationRenderer {
 public:
  void DestroyContext(SR15NetCustomizationContext* context);
};

namespace {

// Byte offsets read from the code. ReVault records no field names or sizes for these objects.
constexpr std::size_t kContextSlotsOffset = 0x78;       // SR15NetCustomizationContext +0x78: pointer to the slot array
constexpr std::size_t kContextCountOffset = 0xa8;       // SR15NetCustomizationContext +0xa8: slot count (64-bit)
constexpr std::uint64_t kSlotStride = 0x9718;           // bytes per slot element, applied once per count
constexpr std::size_t kSlotResourceA = 0x1b8;           // CResourceInstance inside a slot
constexpr std::size_t kSlotResourceB = 0x1d0;           // CResourceInstance inside a slot
constexpr std::size_t kSlotResourceC = 0x1e8;           // CResourceInstance inside a slot
constexpr std::size_t kSlotResourceD = 0x210;           // CResourceInstance inside a slot
constexpr std::size_t kSlotTexture = 0x228;             // CGRenderTexture inside a slot
constexpr std::size_t kSlotTargetState = 0x8db0;        // CGRenderTargetState inside a slot
constexpr std::size_t kRendererContextsOffset = 0xbf8;  // CR15NetCustomizationRenderer +0xbf8: array of context pointers
constexpr std::size_t kRendererCountOffset = 0xdf8;     // CR15NetCustomizationRenderer +0xdf8: entry count (32-bit)
// CAllocator vtable slot +0x38: PURE_VIRTUAL in NRadEngine::CAllocator and unnamed; called as (allocator, block).
constexpr std::size_t kAllocatorReleaseSlot = 0x38;
using ReleaseFn = void (*)(CAllocator*, void*);
constexpr auto kErrorLevel = static_cast<NWriteLog::ELogLevel>(8);

void* LoadPtr(const std::byte* base, std::size_t offset) {
  void* value = nullptr;
  std::memcpy(&value, base + offset, sizeof value);
  return value;
}

std::uint64_t LoadU64(const std::byte* base, std::size_t offset) {
  std::uint64_t value = 0;
  std::memcpy(&value, base + offset, sizeof value);
  return value;
}

std::uint32_t LoadU32(const std::byte* base, std::size_t offset) {
  std::uint32_t value = 0;
  std::memcpy(&value, base + offset, sizeof value);
  return value;
}

void StorePtr(std::byte* base, std::size_t offset, const void* value) {
  std::memcpy(base + offset, &value, sizeof value);
}

// Reads the function pointer at byte offset `slot` of the vtable of `object` (its first word is the vptr).
template <typename Fn>
Fn VirtualSlot(const void* object, std::size_t slot) {
  std::uintptr_t vtable = 0;
  std::memcpy(&vtable, object, sizeof vtable);
  Fn fn = nullptr;
  std::memcpy(&fn, reinterpret_cast<const std::byte*>(vtable) + slot, sizeof fn);
  return fn;
}

}  // namespace

void CR15NetCustomizationRenderer::DestroyContext(SR15NetCustomizationContext* context) {
  // 0x13492fc-0x1349304: a null context returns at once.
  if (context == nullptr) {
    return;
  }
  // 0x1349324-0x134933c: with no memory context, the message is logged and the brk #1 at 0x1349340 is taken.
  // Execution continues at 0x1349344, the tree's SoftwareBreakpoint handler convention.
  if (CMemoryContext::context == nullptr) {
    NWriteLog::WriteLog(kErrorLevel, 0ull, "Trying to use memory context before it's initialized!");
    SoftwareBreakpoint(1, 0x1349344)();
  }
  // 0x1349344-0x134934c: the current allocator is the first word of the object CMemoryContext::context points to.
  CAllocator* const allocator =
      static_cast<CAllocator*>(LoadPtr(static_cast<const std::byte*>(CMemoryContext::context), 0));

  // 0x1349348-0x1349360: the slot array is shut down when the count and count * 0x9718 (64-bit) are both nonzero.
  const auto* const ctx = reinterpret_cast<const std::byte*>(context);
  const std::uint64_t count = LoadU64(ctx, kContextCountOffset);
  const std::uint64_t span = count * kSlotStride;
  if (count != 0 && span != 0) {
    std::byte* slot = static_cast<std::byte*>(LoadPtr(ctx, kContextSlotsOffset));
    std::uint64_t remaining = span;
    // 0x1349374-0x13493e4: one iteration per slot; each status is reported at level 8.
    do {
      OutputError(static_cast<std::int32_t>(reinterpret_cast<CResourceInstance*>(slot + kSlotResourceA)->UnloadAndShutdown()), kErrorLevel);
      OutputError(static_cast<std::int32_t>(reinterpret_cast<CResourceInstance*>(slot + kSlotResourceB)->UnloadAndShutdown()), kErrorLevel);
      OutputError(static_cast<std::int32_t>(reinterpret_cast<CResourceInstance*>(slot + kSlotResourceC)->UnloadAndShutdown()), kErrorLevel);
      OutputError(static_cast<std::int32_t>(reinterpret_cast<CResourceInstance*>(slot + kSlotResourceD)->UnloadAndShutdown()), kErrorLevel);
      OutputError(static_cast<std::int32_t>(reinterpret_cast<CGRenderTexture*>(slot + kSlotTexture)->Destroy()), kErrorLevel);
      OutputError(static_cast<std::int32_t>(reinterpret_cast<CGRenderTargetState*>(slot + kSlotTargetState)->Shutdown()), kErrorLevel);
      reinterpret_cast<CGGraphicsState*>(slot)->Shutdown();
      slot += kSlotStride;
      remaining -= kSlotStride;
    } while (remaining != 0);
  }

  // 0x13493e8-0x134940c: the renderer drops its first entry that equals this context.
  auto* const self = reinterpret_cast<std::byte*>(this);
  std::uint64_t entries = LoadU32(self, kRendererCountOffset);
  if (entries != 0) {
    std::size_t entry = kRendererContextsOffset;
    do {
      if (LoadPtr(self, entry) == context) {
        StorePtr(self, entry, nullptr);
        break;
      }
      entries -= 1;
      entry += sizeof(void*);
    } while (entries != 0);
  }

  // 0x1349418-0x1349424: the current allocator is saved, and this context's allocator becomes current.
  CAllocator* const saved = CMemoryContext::Current();
  CMemoryContext::SetCurrent(allocator);
  // 0x1349428-0x134942c: the context's destructor runs on the context.
  context->~SR15NetCustomizationContext();
  // 0x1349430-0x1349440: vtable slot +0x38 of the allocator is called with (allocator, context).
  VirtualSlot<ReleaseFn>(allocator, kAllocatorReleaseSlot)(allocator, context);
  // 0x1349444-0x134945c: the saved allocator is restored by the tail call.
  CMemoryContext::SetCurrent(saved);
}

}  // namespace NRadGame

}  // namespace NRadEngine
