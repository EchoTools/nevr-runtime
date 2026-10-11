#pragma once
// The pnsrad.dll / pnsovr.dll / pnsdemo.dll export tracer (#20), runtime half.
//
// OFF by default. `-traceexports <list>` (pnsrad, pnsovr, pnsdemo, all; comma separated) switches it on for
// the named DLLs. The static half is tools/pe_exports.py (the export table and the names that share a body).
//
// How it sees the exports: the game reaches a platform DLL's exports only through CSysDLL_GetSymbol (the
// game's own GetProcAddress wrapper, hooked in lifecycle/initialize.cpp). The tracer answers that lookup
// with a per-name thunk (hook/export_trace_thunk.h) that forwards to the real function and records the call,
// so no export body is patched, and the four pnsrad Mic* exports that identical-code folding put at one
// address are separate thunks, because the game asked for four different names. Observation only: every
// call reaches the real implementation with its arguments and returns its result unchanged. A symbol the
// game resolves while the tracer is off, or from another DLL, is returned as it was.
//
// What it logs (tag [NEVR.TRACE], level Info, from one drain thread, never from the game's thread):
//   enabled   modules=<list>
//   resolve   module=<m> export=<name> id=<n>            one per name the game resolves
//   call      #<k> module= export= tid= dur_ns= args=a0,a1,a2,a3 ret= xmm0=      the first 64 calls of each export
//   summary   module= export= calls= total_dur_ns= max_dur_ns= last_ret=        every 10 s, per export that was called
//   dropped   count=<n>                                                          only if the ring overflowed

namespace nevr_export_tracer {

/// Reads the selection (a `-traceexports` value), arms the thunk ring and starts the drain thread when it
/// names a module. Idempotent: only the first call has an effect.
void Configure(const char* list);

/// Configure from the process's own command line (GetCommandLineW), which is complete from process start:
/// not from a value the game's argument parser fills later. Idempotent.
void ConfigureFromCommandLine();

/// Configure from a command line text (the same as ConfigureFromCommandLine; for tests).
void ConfigureFromCommandLineText(const wchar_t* commandLine);

/// Stops the drain thread after a last drain and summary, waiting at most two seconds. Call from a normal
/// thread (not under the loader lock). Safe when the tracer is off, and safe to call twice.
void Shutdown();

/// True when `address` is in committed, executable memory (a function), false for data.
bool PointsToCode(const void* address);

/// True when Configure selected at least one module.
bool Enabled();

/// The pointer to hand the game for `symbol` of the module `dllHandle`: `resolved` when the tracer is off,
/// the module is not selected, the export is data rather than code (a thunk there would hand the game a stub
/// where it reads a value), or no thunk could be made; otherwise a thunk that forwards to `resolved`.
void* WrapSymbol(void* dllHandle, const char* symbol, void* resolved);

}  // namespace nevr_export_tracer
