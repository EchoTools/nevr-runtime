// Module `memsys` (rows R37 R38; docs/quest/q15t/modules/memsys.md): the render-target layout pool, 512 -> 1024 slots, through two
// JUMP_SLOT wrappers. Port of the source build's memsys M2 and its arm.
//
//   R37  CSimplePool<SGRenderTargetLayout>::Alloc   JUMP_SLOT 0x36e9b48, real 0x1901b5c (disassembly 0x1a01b5c)
//        CGRawInterface::InitCommon makes the pool with Alloc(pool, 0x200, allocator) and then fills the free stack inline
//        with 511..0 and adds 512 to the free count. The wrapper, only for that call site (return address
//        base + 0x18818cc) and only with capacity 0x200, calls Alloc with 0x400 and pre-pushes 1023..512 (free count 512);
//        the inline fill then completes 0..1023 and the count reads 1024. Any other call, or any layout it does not
//        recognise, goes to the real function untouched.
//   R38  CSysJava::Init                            JUMP_SLOT 0x36de5c0, real 0xf7e8e8 (disassembly 0x107e8e8)
//        android_main's FIRST call (0x10784e4, before it reads any heap size). In the source build this was the arm: the
//        wrapper did the swaps on the android_main thread. Here the slot swap of R37 is made at install time (data only,
//        no timing needed), and this wrapper is the TIMING WITNESS: one line when android_main reaches Init, with the
//        System-heap value the game is about to read. No line = the plugin was installed after android_main passed Init.
//
// No code word is written. Every pinned code word the wrapper relies on is read first (the source build's memsys:65-77); a mismatch
// refuses both rows with one line each.
#pragma once

#include <cstdint>

#include "import_hook.h"

namespace q15t {

struct MemsysWord {
  std::uintptr_t vaddr;  // ELF vaddr
  std::uint32_t word;    // the stock instruction word
  int row;               // 37 or 38: the row that is refused when this word differs
};

// The pins (ELF vaddrs of the pinned build), test seam: the host test places a fake library at `base` with these.
struct MemsysPins {
  std::uintptr_t got_alloc, fn_alloc, site_ret;  // R37 slot, real function, the return address of InitCommon's call
  std::uintptr_t got_init, fn_init, init_ret;    // R38 slot, real function, the return address of android_main's first call
  std::uintptr_t got_sys;                        // GLOB_DAT of kSystemMemoryAndroid (read for the witness line)
};
MemsysPins DefaultMemsysPins();
const MemsysWord* MemsysWords(int* count);

// Installs R37 and R38 (each on its own; one refusal does not stop the other). kOk when both are in; else the first
// refusal's status with `reason` naming the row. `memsys.off` in the plugin's data directory: nothing, one line.
InstallResult InstallMemsys(std::uintptr_t base);

struct MemsysStats {
  std::uint32_t alloc_calls;      // calls through the R37 wrapper
  std::uint32_t pool_raised;      // pools made with 1024 slots
  std::uint32_t pool_not_raised;  // the right call site, but the layout was not recognised (stock 512 kept)
  std::uint32_t init_calls;       // calls through the R38 wrapper
  std::uint32_t init_witnessed;   // 1 once the first android_main call was seen
};
MemsysStats GetMemsysStats();

// Test seams. A test cannot make a call return into libr15, so it reads the return addresses a call from its own function
// produced and then sets those as the "site". 0 = the pins' address (base + vaddr).
void SetMemsysPinsForTest(const MemsysPins& pins);
void SetMemsysSitesForTest(std::uintptr_t alloc_ret, std::uintptr_t init_ret);
void GetMemsysLastReturnsForTest(std::uintptr_t* alloc_ret, std::uintptr_t* init_ret);
void ResetMemsysForTest();

}  // namespace q15t
