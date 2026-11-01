// NRadEngine::CMemoryContext::SetCurrent
// Quest libr15.so VA 0x12cafac; ReVault binary 576.
// ReVault raw parent 100219611, reconstruction node 100600494; grade H: five instructions match; the allocator arrives in x0, is moved to x1 and forwarded to SetTLS as its value, and the stored size 20 ends at the b.
// @0x12cafac — NRadEngine::CMemoryContext::SetCurrent
/* @confidence: H */

// Thread-local slot index read by address: Quest libr15.so VA 0x3861200 (DAT_03861200), a 32-bit int.
extern const int g_memory_context_tls_slot_0x3861200;

namespace NRadEngine {

struct CAllocator;

// CSysThread::SetTLS(int, void*), 0x108a228 (thunk 0x101c580). It tail-jumps to pthread_setspecific (import thunk
// 0x100bfc0).
struct CSysThread {
  static void SetTLS(int slot, void* value);
};

struct CMemoryContext {
  // NRadEngine::CMemoryContext::SetCurrent(CAllocator*), Quest libr15.so VA 0x12cafac (thunk 0xfd3590).
  static void SetCurrent(CAllocator* allocator);
};

void CMemoryContext::SetCurrent(CAllocator* allocator) {
  CSysThread::SetTLS(g_memory_context_tls_slot_0x3861200, allocator);
}

}  // namespace NRadEngine
