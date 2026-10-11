# ADR 0008: Luau is the scripting VM

Status: accepted for the prototype (#440). The binding is `src/scripting/vm/luau/`, pinned to Luau `0.742`.

## Context

Mods are mostly one-file scripts that change a value or add a callback on a hook point (#440). They come from
other people and run inside the game client and the game server on Windows x64 (mingw-w64) and on Quest
(Android arm64). A script must reach only what the host API exposes (`src/extension/host_api.h`), and a script
that errs, loops or allocates without end must never crash or hang the game.

Three VMs were built against the same contract, `src/scripting/script_vm.h`, and the same tests,
`src/scripting/tests/conformance_test.cpp`. Each was built for both targets and run under `wine` (x86_64) and
`qemu-aarch64` (arm64), standalone, with the toolchains of the real builds (`cmake/toolchain-mingw64.cmake`;
NDK r26d, `arm64-v8a`, API 26, `c++_static`), through `src/scripting/CMakeLists.txt` with
`-DNEVR_SCRIPT_VM=<vm>`. Only the Luau binding is kept in the tree.

| VM | Upstream pin |
| --- | --- |
| Lua 5.4 / 5.5 (PUC-Rio) | tags `v5.4.9` and `v5.5.1` of github.com/lua/lua |
| LuaJIT 2.1 | the head of the `v2.1` branch (LuaJIT has no release tags) |
| Luau | tag `0.742` of github.com/luau-lang/luau (`extern/luau`) |

## Measurements

Conformance under wine, at the 19-test contract the three prototypes ran. The runaway tests are `t5_*`;
`--pattern-dos` runs one `string.find` that backtracks for seconds inside a single C call; `--gc-dos` registers
a `__gc` that loops forever.

| | Lua 5.4.9 | Lua 5.5.1 | LuaJIT, JIT on | LuaJIT, JIT off | LuaJIT, `LUAJIT_ENABLE_CHECKHOOK` | Luau 0.742 |
| --- | --- | --- | --- | --- | --- | --- |
| conformance (19) | 19 | 19 | 15: all four `t5_*` hang | 19 | 19 | 19 |
| `--pattern-dos` | not stopped after 20 s | not stopped after 20 s | not stopped | not stopped | not stopped | stopped, 14.1 ms |
| `--gc-dos` | `__gc` refused by the binding | `__gc` refused | `__gc` unreachable (userdata only) | same | same | no script finalizers exist |
| arm64 OOM raised in compiled code | n/a | n/a | process exits (panic) | n/a | not run | n/a |

- LuaJIT's count hook does not run in JIT-compiled code; `LUAJIT_ENABLE_CHECKHOOK` makes it run, at about 67x
  the time of a JIT-on hot loop (2739 ms against 40.6 ms for 20M iterations, mingw).
- A count hook does not run inside a C function (Lua manual §4.7, `lua_sethook`), which is why `string.find`
  is unbounded on Lua and LuaJIT. Luau's matcher calls the interrupt (`VM/src/lstrlib.cpp:437-444` at `0.742`).
- Lua runs `__gc` with hooks off (`lgc.c:920-929` at `v5.4.9`), so the Lua binding refuses `__gc` in its
  `setmetatable`.

Per-call cost, mingw Release under wine, all six built and run in one slot, three interleaved rounds, median
of the three medians (`conformance_test --bench`: 10^6 calls of hook point `test.add`, median of five runs):

| ns per call | Lua 5.4.9 | Lua 5.5.1 | LuaJIT on | LuaJIT off | LuaJIT checkhook | Luau |
| --- | --- | --- | --- | --- | --- | --- |
| no callback | 27.2 | 27.7 | 27.3 | 26.7 | 26.9 | 25.5 |
| native C callback, get + set | 50.9 | 49.4 | 51.7 | 49.3 | 52.1 | 49.3 |
| empty script callback | 251.9 | 255.3 | 180.4 | 166.9 | 310.2 | 206.6 |
| script callback, `h:set('a', h:get('a') + 1)` | 473.2 | 455.9 | 264.0 | 252.2 | 568.1 | 313.0 |
| one script's state after the sample (bytes) | 19,629 | 20,148 | 33,183 | 33,183 | 33,183 | 369,096 |

Size added to the stripped conformance executable over the same executable with no VM (`-DNEVR_SCRIPT_VM=null`):

| | Lua 5.4.9 | Lua 5.5.1 | LuaJIT | Luau |
| --- | --- | --- | --- | --- |
| Windows x64 | +243,200 | +255,488 | +549,376 | +1,099,776 |
| Android arm64 | +322,720 | +338,176 | +626,512 | +901,760 |

Not measured: any timing on a real arm64 CPU (qemu timings are functional only), any run on native Windows.

## Decision

Luau, interpreter only (no CodeGen), one `lua_State` per script, upstream's `luaL_sandbox` plus a
`luaL_sandboxthread` per script.

1. **Runaway code is stopped everywhere the game can be held.** Luau is the only one of the three that stops a
   script stuck inside one C call (`--pattern-dos`). On Lua and LuaJIT one `string.find` holds the game thread
   until it finishes; closing that would mean carrying patches to their string libraries.
2. **The sandbox is upstream's, not ours.** Luau states its sandbox cannot be escaped except through host
   functions and treats memory-safety bugs as vulnerabilities (`SECURITY.md` at `0.742`). With Lua and LuaJIT the
   sandbox is built by removal; LuaJIT's `getfenv` was a full mod-sandbox escape in Luanti
   (CVE-2026-40959, CVE-2026-41196), and LuaJIT's FAQ advises sandboxing at the process level.
3. **Per call it is faster than PUC Lua** (313 against 473 ns for get + set). LuaJIT is faster only in builds that
   either hang on a runaway loop (JIT on) or give up the JIT (off); with the check that stops runaways it is the
   slowest.
4. **Author tooling is native.** Luau has a gradual type checker and `luau-lsp` takes definition files, so the
   typed API stubs are checked, not just hinted.

## Consequences

- About +1.1 MB on Windows and +0.9 MB on Quest.
- **The shared state is the default.** One state holds every script, each in its own `luaL_sandboxthread` with
  its own memory category (`lua_setmemcat`).
  - 20 scripts take 383,408 allocator bytes, against 7,340,960 with one state per script (about 367 KB each).
  - The cost is about 20 ns more per callback in one wine run (281 against 260 ns for get + set).
  - One state per script remains a build-time option for debugging.
- The script surface has no `debug` library (introspection reached host internals; errors already carry
  chunk:line), `os` only has `clock`, `date` and `time`, and no NaN or infinity reaches a FLOAT game value.
- Errors inside the VM are `longjmp` (`LUA_USE_LONGJMP=1`), so the VM and the binding build with
  `-fno-exceptions`; the binding keeps no object with a destructor alive across a call that can raise. Luau's parser
  and compiler throw and catch internally and build with exceptions; on Quest, compiling a script on the device
  needs those two libraries built with `-fexceptions`.
- The instruction budget counts Luau interrupt safepoints (loop back-edges, calls, returns), not VM instructions.
- Mod authors write Luau, a superset of Lua 5.1.
- Luau releases about weekly; the pin moves by deliberate update, with the conformance suite as the gate.
