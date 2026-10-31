// shaderstep (issue #1553): the CTableXT growth step of the CGShaderSetResource table is 1, not 32.
//
// The factory's ConstructInstance (disassembly libr15 0x1a06f4c, ELF 0x1906f4c, 304 bytes) builds a shader-set resource in
// place and stores the table's growth step at object+0x98 (disassembly 0x1a07000: `mov w8,#0x20`, `str x8,[x19,#0x98]`).
// The table is the CTableXT at object+0x78: its CMemBlock is first, the step at +0x20, the capacity at +0x28 (the same
// layout custctx's DrawTable reads). This module wraps the factory's virtual ConstructInstance: it calls the game's
// own constructor unchanged and then writes 1 to the step of the object it built, after reading that the game wrote 0x20.
//
// The wrapped slot is vtable slot 1 of the factory's vtable, which the factory's destructor (disassembly 0x1a06ef0) points
// at: the address point is _ZTV+0x10 (disassembly 0x375ddc0), so the slot is disassembly 0x375ddc8 (ELF 0x365ddc8). A data-page
// swap through got_swap.h, the same mechanism as the JUMP_SLOT wrappers; no code word is written.
//
// Off switch: `shaderstep.off` in the plugin data directory (module_switch.h). Details: docs/quest/q15t/modules/shaderstep.md.
#pragma once

#include <cstdint>

#include "import_hook.h"

namespace q15t {

// `base`: the game library's load base. Logs its own lines; a refusal leaves the slot alone.
InstallResult InstallShaderStep(std::uintptr_t base);

// What the wrapper did to one freshly constructed object. kSet: the game wrote 0x20 and this wrote 1 (read back).
// kNotStock: the word at +0x98 was not 0x20, so nothing was written. kUnreadable: the object could not be read.
enum class StepFix { kSet, kNotStock, kUnreadable };

// The step correction, on its own (the host test drives it on a local buffer; the wrapper calls it on the built object).
StepFix ApplyShaderSetStep(void* obj);

struct ShaderStepStats {
  std::uint32_t constructed;  // objects the wrapper saw built
  std::uint32_t stepped;      // of those, the step written to 1
  std::uint32_t not_stock;    // of those, the word was not the game's 0x20: left alone
};
ShaderStepStats GetShaderStepStats();
void ResetShaderStepForTest();

}  // namespace q15t
