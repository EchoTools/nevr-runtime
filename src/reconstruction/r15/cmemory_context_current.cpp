// NRadEngine::CMemoryContext::Current
// Quest libr15.so VA 0x12c8dc4; ReVault binary 576.
// ReVault raw parent 100219500, reconstruction node 100600495; grade M: six instructions match, but the target is Ghidra's name for thunk 0x1028430 (its GOT slot holds the same unrelocated word 0xecfb40 as every other thunk slot), so the target is not byte-checked.
// @0x12c8dc4 — NRadEngine::CMemoryContext::Current
/* @confidence: M */

// Thread-local slot index read by address: Quest libr15.so VA 0x3861200 (DAT_03861200), a 32-bit int.
extern const int g_memory_context_tls_slot_0x3861200;

namespace NRadEngine {

struct CAllocator;

// CSysThread::GetTLS(int), 0x108a22c (thunk 0x104b720). It tail-jumps to pthread_getspecific (import thunk
// 0x1044960), so the void* that call returns is what Current returns.
struct CSysThread {
  static void* GetTLS(int slot);
};

struct CMemoryContext {
  // NRadEngine::CMemoryContext::Current(), Quest libr15.so VA 0x12c8dc4, reached through thunk 0x1028430.
  static CAllocator* Current();
};

CAllocator* CMemoryContext::Current() {
  return static_cast<CAllocator*>(CSysThread::GetTLS(g_memory_context_tls_slot_0x3861200));
}

}  // namespace NRadEngine
