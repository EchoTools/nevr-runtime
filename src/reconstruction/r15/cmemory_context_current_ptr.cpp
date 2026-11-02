// NRadEngine::CMemoryContext::CurrentPtr
// Quest libr15.so VA 0x12cae90; ReVault binary 576.
// ReVault raw parent 100219606, reconstruction node 100600492; grade H: three instructions (adrp, ldr, tail b) match; the tail jump passes GetTLS's void* through, and the stored size 12 ends at the b.
// @0x12cae90 — NRadEngine::CMemoryContext::CurrentPtr
/* @confidence: H */

// Thread-local slot index read by address: Quest libr15.so VA 0x3861200 (DAT_03861200), a 32-bit int.
extern const int g_memory_context_tls_slot_0x3861200;

namespace NRadEngine {

struct CAllocator;

// CSysThread::GetTLS(int), 0x108a22c (thunk 0x104b720). It tail-jumps to pthread_getspecific (import thunk
// 0x1044960), so the void* that call returns is what CurrentPtr returns.
struct CSysThread {
  static void* GetTLS(int slot);
};

struct CMemoryContext {
  // NRadEngine::CMemoryContext::CurrentPtr(), Quest libr15.so VA 0x12cae90 (thunk 0xffbc50).
  static CAllocator* CurrentPtr();
};

CAllocator* CMemoryContext::CurrentPtr() {
  return static_cast<CAllocator*>(CSysThread::GetTLS(g_memory_context_tls_slot_0x3861200));
}

}  // namespace NRadEngine
