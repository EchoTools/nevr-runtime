# Audit Artifacts — durable findings from scratch/

These files were recovered from `/var/tmp/work-nevr-runtime/` (tmpfs) during
the 2026-07-23 durable-audits sweep (C9). They represent load-bearing audit
findings paid for in real debugging time. Moving them into repo history ensures
they survive tmpfs sweeps and are discoverable by future agents.

## Artifacts

| File | Date | Description |
|------|------|-------------|
| `recon-owner-bug-batch-RESULTS.md` | 2026-07-22 | 19-item validation report from the owner bug batch |
| `fable-consistency-hunt-2026-07-23.md` | 2026-07-23 | Ranked consistency/quality ledger; its High findings became N54-N58 |
| `server-run-wave-a-verify.log` | 2026-07-23 | Trimmed registration witness from Wave A server verification run |

### Removed, and where to read them

Deleted in `e30efee` ("release cleanup"). The table above claimed both for days
after they were gone, because `tools/verify_doc_paths.py` deliberately does not
scan this directory — so nothing checked the index against the disk.

A deleted file is not a lost file. `git show <sha>:<path>` returns its exact
bytes, which is why a citation is sufficient and keeping a stale copy is not
required:

| File | Retrieve with |
|------|---------------|
| `ctrlc-shutdown-audit.md` — CTRL+C to port-zombie causal chain (N13, N37-N39) | `git show 6ffc3bb74283f26d8d418633bc2cbe85e51f2a5b:docs/audits/ctrlc-shutdown-audit.md` |
| `bridge-port-audit.md` — every site referencing the ws_bridge listen port | `git show 6ffc3bb74283f26d8d418633bc2cbe85e51f2a5b:docs/audits/bridge-port-audit.md` |

Both commands were run and verified to return the documents before this table
was written. An unverified citation is worse than none — it looks like evidence.

## These records are IMMUTABLE — including their paths

An audit states what was measured on a date. Its `file:line` citations describe
the tree **as it was then**, so updating them to today's layout does not modernise
the record, it falsifies it: the reader gets a current path paired with a line
number from months ago, and both halves look authoritative.

This is not hypothetical. On 2026-07-29 the N108/N109 reorganisation's mechanical
path rewriter processed this directory and changed 21 citations across two records
(`src/gamepatches/...` -> `src/runtime/...`, `src/common/...` -> `src/core/...`).
Reverted in the same session; `just verify` now fails if post-reorg paths reappear
in a pre-reorg record.

If you need to follow an old citation, use this mapping rather than editing the record:

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

## Generation context

All four artifacts were produced during the Waves A-C workstream (2026-07-20
through 2026-07-23), which resolved the headless DXGI rejection blocker and
established the server registration/CTRL+C teardown audit trails. They were
originally written to the scratch directory per the onboarding conventions
(RULINGS.md 2026-07-20 "Scratch dir (nevr)").

## Exclusions

The following were intentionally NOT copied — they are ephemeral queries/briefs
whose value was consumed during the work:

- `campaign-brief-*` — transient tasking briefs
- `handoff-to-spritz-*` — session handoff notes
- `verify-*` — per-run verification output, not findings
- `worktree-check.md` — temporary check artifact
- `q-*` files — one-shot query fragments
- Build/run logs (except `server-run-wave-a-verify.log`) — ephemeral output
- `*.patch` files — already applied or discarded
- Configuration extracts (`cfg*.txt`, `*.txt` extracts)
