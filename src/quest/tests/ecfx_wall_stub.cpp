// A stand-in for the queue-mode wall lib (game content, generated on the device): the four entry names the source's
// the source build's ecfx looks up, 16-aligned, exported, in an object whose soname is libac4e9374a146d3a4.so. Each entry counts its call.
#include <cstdint>

extern "C" {
void (*re33_stub_hook)(void* tp, int kind) = nullptr;
int re33_stub_calls[4] = {};
void* re33_stub_last_tp[4] = {};

#define ENTRY(name, k)                                                                      \
  __attribute__((visibility("default"), aligned(16), noinline)) void name(void* tp) {       \
    ++re33_stub_calls[k];                                                                   \
    re33_stub_last_tp[k] = tp;                                                              \
    if (re33_stub_hook != nullptr) re33_stub_hook(tp, k);                                   \
  }
ENTRY(re33_ecent_ac4e93_17900, 0)
#ifndef STUB_MISSING_BURST
ENTRY(re33_ecent_ac4e93_17700, 1)
#endif
ENTRY(re33_ecent_ac4e93_17b00, 2)
ENTRY(re33_ecent_ac4e93_17b60, 3)

// exported, 16-aligned, but not one of the four entries
__attribute__((visibility("default"), aligned(16), noinline)) void re33_not_an_entry(void*) {}
}
