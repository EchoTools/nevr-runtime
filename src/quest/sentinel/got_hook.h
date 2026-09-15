/* GOT/PLT import hooking for arm64 ELF — the Android/Bionic analogue of the
 * MinHook inline detours nevr-runtime uses on Windows PCVR.
 *
 * Windows nevr-runtime patches an instruction prologue in place (MinHook).
 * That doesn't translate directly to Android: ARM64 code pages are frequently
 * not writable at runtime (and patching machine code correctly needs an
 * architecture-specific trampoline). What DOES translate directly is import
 * hooking — redirect the pointer a module calls through for a symbol it
 * imports from another .so, exactly like an IAT hook on Windows. The pointer
 * lives in `.got`/`.got.plt`, in a normal writable data page, so no code
 * patching or trampoline generation is required — just overwrite a pointer.
 *
 * This only lets us intercept calls a target module makes to a symbol
 * imported from elsewhere (e.g. libr15.so calling into libc, liblog, or
 * vrapi). It does NOT let us hook an arbitrary internal (non-imported)
 * function inside libr15.so itself — that needs inline patching, which is
 * the natural "generalize" step once this basic form is proven live.
 */
#pragma once

namespace sentinel {

// Overwrites the GOT/PLT slot `moduleSoName` uses to call `symbolName` (a
// symbol it imports from some other loaded .so) so it calls `hookFn`
// instead. `*originalOut` receives the slot's previous value — the real
// resolved function, or its lazy-binding PLT stub if binding hasn't
// happened yet. Either is safely callable through a function pointer of the
// correct type: calling the stub triggers normal lazy resolution and then
// jumps to the real function, exactly as an ordinary call site would.
//
// Returns false and touches no memory if `moduleSoName` isn't a currently
// loaded module (checked via dl_iterate_phdr), its PT_DYNAMIC segment can't
// be parsed, or no PLT relocation entry names `symbolName` — this is a
// lookup miss, not a fault, and must never crash the host process.
bool HookImport(const char* moduleSoName, const char* symbolName, void* hookFn,
                 void** originalOut);

}  // namespace sentinel
