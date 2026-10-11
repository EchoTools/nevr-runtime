# The platform-DLL export tracer (`-traceexports`)

What the game calls in `pnsrad.dll`, `pnsovr.dll` and `pnsdemo.dll`, with the arguments and results, from a
run. The export tables themselves are in `docs/reference/pns-export-surface.md` (`tools/pe_exports.py`);
this is the runtime half: which of those exports a client or server run actually uses.

Off by default. It records; it never changes an argument, a result or a call.

## Switching it on

```
echovr.exe ... -traceexports pnsrad            # or pnsovr, pnsdemo, all; comma separated
echovr.exe ... -traceexports pnsrad,pnsovr
```

The list names the DLLs whose exports are traced. The flag is read when the game first resolves a symbol, so
it works on any build that carries it. Log lines carry the tag `[NEVR.TRACE]` and go through the runtime's
normal log (the same file and JSON lines as every other `[NEVR.*]` tag).

## How it sees the exports

The game looks platform exports up through `CSysDLL_GetSymbol` (`echovr.exe` `0x1400eaef0`, hooked in
`src/runtime/lifecycle/initialize.cpp`). With the tracer on, that hook answers a lookup in a selected DLL with a
thunk for that name (`src/runtime/hook/export_trace_thunk.cpp`) that forwards to the real function and records
the call. Nothing in a DLL's code is patched, and exports that identical-code folding put at one address
(`MicAvailable`, `MicCreate`, `MicDetected`, `MicRead` at `pnsrad.dll+0x88d10`) stay separate: the game asked
for four names, so each has its own thunk and its own counters. The mic provider's overrides (WASAPI) are
traced as the thing the game was handed.

## What a traced call costs

On the game's thread: two `rdtsc`, a copy of the first twelve stack arguments, and one lock-free push of an
88-byte record into a 4096-entry ring. No log call, no allocation, no lock, no system call. A drain thread
(woken every 250 ms) formats the records. A full ring drops the new record and counts it (`dropped` line); a
call is never delayed for a slow logger. Arguments in `rcx`, `rdx`, `r8`, `r9`, the stack and the XMM argument
registers reach the original unchanged, and `rax` and `xmm0` come back unchanged.

## Log lines

| Line | When | Fields |
| --- | --- | --- |
| `[NEVR.TRACE] enabled modules=...` | once | the DLLs selected |
| `[NEVR.TRACE] resolve module= export= id= original=` | once per name the game resolves | the real address |
| `[NEVR.TRACE] call #k module= export= tid= dur_ns= args=a0,a1,a2,a3 ret= xmm0=` | the first 64 calls of each export | raw 64-bit values: `rcx,rdx,r8,r9`, `rax`, low half of `xmm0` |
| `[NEVR.TRACE] summary module= export= calls= total_dur_ns= max_dur_ns= last_ret=` | every 10 s, each export called since start | cumulative |
| `[NEVR.TRACE] dropped count=` | when the ring overflowed | cumulative |

The tracer does not know the signatures, so it records four integer arguments raw and never dereferences
one (a pointer argument shows as an address; a `float` shows as a raw bit pattern in `xmm0` when it is the
result). Read an argument against `echovr-reconstruction/src/pnsrad/Plugin/RadPluginAPI.h`.

## Comparing a pnsrad run with a pnsovr run

Run each with the same flag set (`-traceexports pnsrad`, then `-traceexports pnsovr` on a machine with the
Oculus runtime, `-micprovider OVR`) and compare the `resolve` and `summary` lines by export name:

```
grep '\[NEVR.TRACE\] resolve' run-pnsrad.log | sed 's/.*export=\([^ ]*\).*/\1/' | sort > rad.txt
grep '\[NEVR.TRACE\] resolve' run-pnsovr.log | sed 's/.*export=\([^ ]*\).*/\1/' | sort > ovr.txt
comm -13 rad.txt ovr.txt     # names the game resolved from pnsovr and not from pnsrad
```

`summary` lines give the call counts per export to diff what pnsrad is silently not providing.

## Limits

- It sees what the game resolves by name through `CSysDLL_GetSymbol`; a direct `GetProcAddress` by another
  caller is not traced.
- Up to 256 distinct names, 12 stack arguments, 4 recorded register arguments.
- The thunk is generated at run time in a read-write-execute page (other threads may be running earlier
  thunks while a new one is written).
- Not proven here: a run on the game. The thunk's transparency is tested (`test_export_trace`, under Wine); the
  hook in `CSysDLL_GetSymbolHook` and the log lines need a client run: see the hand-off in issue #20.
