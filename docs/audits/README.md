# Audit Artifacts — retired records

This directory is the historical record. Its entries are dated by design and are exempt from the
current-tree rule in `AGENTS.md` ("Documentation, plans and findings"): they cite the tree as it was
when measured, and nothing here is rewritten or deleted without the owner's confirmation.

No audit record lives in the tree any more. Each was checked against the code it
described; what was still open became a GitHub issue, and the record was removed.
A deleted file is not a lost file: `git show <sha>:<path>` returns its exact bytes,
so a citation is sufficient and keeping a stale copy is not required. Every command
below was run and returned the document before it was written here.

| File | Retrieve with | Open findings went to |
|------|---------------|-----------------------|
| `ctrlc-shutdown-audit.md` — CTRL+C to port-zombie causal chain (N13, N37-N39) | `git show 153329138694cf7a1867a97e02155d7b0e89d053:docs/audits/ctrlc-shutdown-audit.md` | none (resolved) |
| `bridge-port-audit.md` — every site referencing the ws_bridge listen port | `git show 153329138694cf7a1867a97e02155d7b0e89d053:docs/audits/bridge-port-audit.md` | none (resolved) |
| `recon-owner-bug-batch-RESULTS.md` — 19-item owner bug batch validation (2026-07-22) | `git show 94a24a16b67ca21e39cab2cd2b49f8914c1c93ab:docs/audits/recon-owner-bug-batch-RESULTS.md` | #48 (log-consistency leftovers) |
| `2026-09-26-runtime-bug-hunt.md` — 35-finding static review at `1eb93be` | `git show f94be4168caf22fb79a3fee9e8c668b588618760:docs/audits/2026-09-26-runtime-bug-hunt.md` | #37, #38, #39, #40, #41, #45, #46 |
| `fable-consistency-hunt-2026-07-23.md` — ranked consistency/quality ledger (its High findings became N54-N58) | `git show 2b99d0e21f3705561a6d43b7336ca03ac67afd5c:docs/audits/fable-consistency-hunt-2026-07-23.md` | #42, #43, #44, #47, #48, #49 |

Retired findings that were not filed, and why:

- Bug-hunt `loader-lock-bootstrap` — the deliberate N43 loader-lock exception.
- Bug-hunt `unsigned-release-accepted` — release packages require signing; local unsigned development is an owner decision.
- Bug-hunt `native-windows-gate-optional` — the native VM gate stays separate by owner decision.
- Fable B5, B7, B8 — marked STALE by the ledger itself (mitigated or guarded).

## Reading an old citation

The records cite the tree as it was when measured. To follow a pre-2026-07-29
citation, map the path rather than editing anything:

| Cited as (pre-2026-07-29) | Now |
|---|---|
| `src/gamepatches/gameserver/*` | `src/runtime/server/*` |
| `src/gamepatches/ws_bridge.*`, `winhttp_stub.*` | `src/runtime/compat/*` |
| `src/gamepatches/{dllmain,initialize,boot,cli,config,state_machine,crash_recovery}.*` | `src/runtime/lifecycle/*` |
| `src/gamepatches/{hook_guard,hook_liveness,dll_load_hook,symbol_corpus}.*` | `src/runtime/hook/*` |
| `src/gamepatches/patch_addresses.h` | `src/runtime/hook/addresses.h` |
| `src/gamepatches/gamepatches_internal.h` | `src/runtime/hook/patching.h` |
| `src/gamepatches/wave0_instrumentation.*` | `src/runtime/patch/binary_bug_fixes.*` |
| `src/gamepatches/{mode_patches,headless_graphics,xpid_patch,pnsrad_enabler,resource_override,asset_cdn,broadcaster_guard}.*` | `src/runtime/patch/*` |
| `src/gamepatches/{plugin_loader,module_loader}.*` | `src/runtime/ext/*` |
| `src/gamepatches/{boot_log_tee,builtin_log_filter}.*` | `src/runtime/log/*` |
| `src/common/{logging,globals,base64,hooking,auth_token,pch}.*` | `src/core/*` |
| `src/common/{echovr,echovr_functions,symbols,symbol_hash}.*` | `src/abi/*` |
| `src/common/nevr_{plugin,module}_interface.h` | `src/extension/{plugin,module}_interface.h` |
