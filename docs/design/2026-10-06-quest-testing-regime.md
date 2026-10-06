# Quest networking tests: shared code and verified game callbacks

Date: 2026-10-06

Status: Astra reviewed; callback mapping and implementation evidence pending

Companion: `docs/design/2026-10-06-quest-networking-port.md`

## Objective

Prove that the callback contracts used by the Quest build match the exact Quest game binaries, then test the shared networking behavior that PCVR already uses. Quest and PCVR speak the same EVR protocol; the service does not select a Quest variant. The source files implementing portable configuration, URL policy, login data, EVR handling, and session routing should be the same files in both builds. Only operating-system, loader, game-ABI, and hook-backend code should differ where the binary or platform requires it.

The first test target is the game callback boundary. Identify the PCVR hook site and its production callback first. Find the same logical call or callback in the exact Quest libraries. Then assert the Quest callback's function type, arguments, return behavior, ownership, lifetime, thread, and frequency from binary evidence. A passing mock or a matching method name is not evidence that the Quest binary calls that callback with the declared ABI.

Routine checks must not require a headset, APK install, full game launch, local Nakama, live service, `config.json`, or controller input. A later, separately approved qualification may be needed for any behavior that static analysis and a bounded binary harness cannot establish.

## What is known now

### Shared PCVR implementation

| Boundary | Repository evidence | Test-design consequence |
| --- | --- | --- |
| Config lookup and service URL redirect | PCVR binds `EchoVR::JsonValueAsString` at `echovr.exe` RVA `0x5FE290` (`src/abi/echovr_functions.cpp:107`, `src/abi/echovr_functions.h:49`). `JsonValueAsStringHook` calls the original, then applies shared service URL and login-override policy (`src/runtime/lifecycle/config.cpp:443-503`). | Test the actual shared redirect/override functions and the adapter's call-through contract. Map the same logical config lookup path in Quest before claiming its callback is equivalent. |
| HTTP URL connection | PCVR binds `EchoVR::HttpConnect` at RVA `0x1F60C0` (`src/abi/echovr_functions.cpp:114`, `src/abi/echovr_functions.h:83`). `HttpConnectHook` applies service-specific URL routing, then calls the original (`src/runtime/lifecycle/config.cpp:359-418`). | Test argument identity/replacement, original-call count, and return forwarding. Determine from Quest binaries whether this boundary exists and whether it covers the same requests. |
| Hook lifecycle | `src/core/hooking.h` implements MinHook/Detours lifecycle and publication ordering, but includes Windows headers and its detach API does not track individual MinHook targets. `src/runtime/tests/test_hooking.cpp` covers parts of attach rollback. | Reuse the contract and lifecycle logic where portable. If factoring is needed, define one shared hook lifecycle/interface and keep thin platform/ISA backends. Do not force the Windows backend into Bionic or write a second high-level lifecycle. |
| Pure service mapping | `src/runtime/lifecycle/service_map.cpp` and `test_service_map` already separate and test URL/config policy from game hooks. | Extend shared-source tests here rather than copying URL policy into a Quest-only implementation. |

ReVault identifies PCVR `CJson::String` at full VA `0x1405FE290` (image base `0x140000000`, hence RVA `0x5FE290`) and `HttpConnect` at `0x1401F60C0` (RVA `0x1F60C0`) for `echovr.exe` SHA-256 `b6d08277e5846900c81004b64b298df6acba834b69700a640b758bda94a52043`. These facts correspond to the ReVault binary identity; confirm the PCVR build under test before using its addresses as a comparison fixture.

### Quest binary evidence and limits

Current ReVault records:

| Library | SHA-256 | Architecture / availability | Current finding |
| --- | --- | --- | --- |
| `libr15.so` | `8dd9a961b9dca8566069a4f65b3ddee9c65682c4e9c91a6d41e3c5727b1d8b20` | AArch64; content in managed store | `0x1932838` is labeled `CNSUser::SendLogInRequest`; stored raw decompilation is available with ReVault provenance `source: imported`, but its calling convention is unknown. It is a candidate boundary, not an approved hook site. |
| `libpnsradmatchmaking.so` | `36236ab1df5783da57c064b0fbccc3a61c0e1d150c208022fbfc9cd6e5ed60ee` | AArch64; file content is not in managed store | `0x1b2274` is labeled `CNSRadMatchmaking::ConnectMatchmaker`; stored raw decompilation is available with ReVault provenance `source: imported`, but its calling convention is unknown. Its body reads `matchmaker_host` with default `wss://matchmaker.readyatdawn.com/rad/rad15_live`, then `matchingservice_host` as a fallback, and processes match-type-specific host settings. This is a candidate boundary, not an approved hook site. |

The existing repacked APK artifact `build/android-arm64/repack/r15_nevr-sentinel_signed.apk` is package `com.readyatdawn.r15`, version `4987566`, SHA-256 `4757d50c0e307281d2840243cdb69e1ead6bc7ca157d7c415d38f5195d3ab67f3`. Its `libr15.so` and `libpnsradmatchmaking.so` hashes match the ReVault records above. Build IDs are `b243509c08ce677aeb95fa348016949b3fc45230` and `8c4fddc079eae65909530132a56c48da48b2708c` respectively. This pins the inspected repack artifact, not a currently installed headset build.

PCVR's relevant hook is `EchoVR::JsonValueAsString` / ReVault `CJson::String` at `echovr.exe` RVA `0x5fe290`. The same logical Quest API is `NRadEngine::CJson::TString(char const*, char const*, unsigned int) const`, mangled `_ZNK10NRadEngine5CJson7TStringEPKcS2_j`. In `libr15.so`, its defined function is at `0xfa2e7c`, called through PLT stub `0xf28e40` and `R_AARCH64_JUMP_SLOT` slot `0x36ebe08`; `CR15NetGame::BeginLogIn` calls that stub at `0x1291b10` and `0x1291b28` for `login_host` and `loginservice_host`. In `libpnsradmatchmaking.so`, the definition is at `0x209484`, PLT stub `0x19c200`, and JUMP_SLOT `0x6b4768`; `ConnectMatchmaker` calls it for its host lookups. These names and relocations are independently present in the exact local ELF files. The Quest config-string callback can therefore use the same shared redirect policy at the corresponding CJson lookup seam, with an ELF relocation backend. This finding does not establish Quest's HTTP connect hook or every URL source.

ReVault's `source: imported` describes how decompilation entered the warehouse. It does not mean the function is an ELF imported symbol or has a GOT relocation. Only the exact ELF's symbol and relocation tables can establish that hook mechanism.

The ReVault hashes now match the two libraries in the existing repacked APK artifact described above. The APK is not established as the currently installed headset build; any later device qualification must independently pin its installed build identity. Do not turn cached names or the two candidate addresses into hook assumptions.

The earlier decompiler label `thunk_FUN_010a2e7c` at PLT stub `0xf28e40` was ambiguous. The exact `libr15.so` symbol table and JUMP_SLOT relocation resolve it to the defined `CJson::TString` function at `0xfa2e7c`; the direct query at `0x10a2e7c` was the wrong address. Retain the callsite instruction and relocation fixture so a future binary change cannot silently alter this mapping.

The old design's ReVault-timeout statement is obsolete. ReVault is responding. Its current output has partial call-graph coverage, so a caller/callee result is not exhaustive; cross-check call sites with xrefs and instruction searches, and record coverage limitations with every conclusion.

The current Quest `HookImport` implementation only finds imported-symbol relocations; it cannot attach to arbitrary internal functions (`src/quest/sentinel/got_hook.h:10-19`). Its current implementation also has no production unhook API, duplicate-install guard, or checked protection-restore result (`src/quest/sentinel/got_hook.cpp:127-136`). Whether a hook can use GOT imports is a per-boundary fact to measure, not an architectural assumption. Internal hooks require a separately justified backend and proof against exact AArch64 instructions and Android memory policy.

## Test sequence

### 0. Pin the binaries and source set

Record the exact PCVR executable identity and exact Quest APK plus `libr15.so`, `libpnsrad.so`, `libpnsradmatchmaking.so`, and loader/shim identities: SHA-256, ELF build ID, architecture, SONAME, load-bias model, relevant dependencies, imports, and relocations. Compare each ELF hash against ReVault before interpreting addresses. ReVault's current Quest records above are useful references but are not an APK identity match by themselves.

Create a reviewed, nonsecret fixture manifest tied to each binary hash. Capture the relevant ReVault function/decompilation IDs, VA/RVA/file-offset conversions, xrefs, call-site instructions and raw bytes. Commit only reviewed test fixtures under `tests/quest/fixtures/<build-id>/`; raw generated evidence and scratch output belong under `/var/tmp/work-nevr-runtime/`. A hash or build-ID mismatch fails the fixture test before any callback assertion.

Record the portable production source list used by PCVR and Quest. The same portable files must be compiled into both targets. The test should fail if either target silently substitutes a Quest copy of URL policy, login transformation, EVR framing, or session behavior. Platform adapters and narrow ABI/hook backends are the permitted differences.

### 1. Map PCVR boundary, then its Quest counterpart

For each relevant operation, make a boundary record with:

- PCVR symbol/address, production detour, source file, declaration and original-call path.
- The corresponding logical Quest function/call chain, if found; exact owning ELF hash/build ID, VA, RVA, file offset and load bias.
- ReVault function details, decompilation tier, xrefs, direct and indirect callers/callees, disassembly and raw bytes. Cross-check call-graph findings because ReVault explicitly reports incomplete graph coverage.
- Confirmed ABI: AArch64 argument registers/stack layout, return register/meaning, hidden `this` or sret arguments, relevant object/struct layout and alignment, ownership of pointers/buffers, mutability, thread/callback timing, lifetime, invocation frequency, and failure behavior.
- Candidate mechanism: imported relocation, vtable callback, or internal function. Include the exact relocation/prologue evidence and backend requirements. Keep “candidate” separate from “verified.”
- A status and evidence reference for every cell. Unknown remains unknown; do not fill it from PCVR's ABI or a demangled name.

Start with PCVR `JsonValueAsString` because it is the service URL lookup/override seam, then `HttpConnect` where the Quest request path has a corresponding HTTP dial. Map login and matchmaking separately. The supplied Quest candidates `SendLogInRequest` and `ConnectMatchmaker` are investigated only if call-chain evidence shows they are the useful boundaries; neither is preselected. Trace configuration, login and matchmaking URL sources independently, including whether the Quest game calls an imported API or an internal helper. Resolve the `0xf28e40`/`0x10a2e7c` discrepancy before assigning it a role.

No hook implementation is selected until the map identifies a suitable boundary and the callback contract. In particular, `HookImport` is valid only when that exact module imports the desired symbol through a matching relocation. If PCVR MinHook's inline detour is not portable, factor the lifecycle operations (validate, attach, publish original, enable, rollback, detach) behind one shared contract and implement only the required OS/ISA primitives in small backends. The test suite must exercise the production lifecycle and rollback APIs; it must not simulate success by restoring test memory itself.

Before choosing separate backends, record which existing or supported library operations can serve both Windows/x86_64 and Android/AArch64, and which require different instruction relocation, memory protection, cache synchronization, or loader handling. Reuse any supported portable backend implementation as well as the lifecycle. A platform difference justifies only the primitives it actually affects; backend replacement still needs its own reviewed scope and Windows regression checks.

### 2. Callback contract tests (first executable tests)

Build tests around the selected production callback and the exact Quest ABI record:

1. **Compile-time signature checks:** assert function pointer type, calling convention, return type, pointer constness and parameter width. Assert size/alignment/offsets for any game-owned data structures the callback reads or writes. Compile the shared callback/policy source for both PCVR and arm64 Android.
2. **Fixture-to-declaration checks:** parse the pinned call-site/relocation/prologue fixture and assert it still matches the selected binary identity and expected bytes. Tie each register/stack argument mapping and return-behavior assertion to the decoded caller/callee instructions and reviewed ABI record. Comparing a declaration against a second hand-entered signature does not establish agreement with the binary. A changed binary or changed bytes fails before the callback harness runs.
3. **Production-callback harness:** invoke the production callback through the declared Quest function-pointer type with bounded synthetic objects and a recorded original-call stub. Assert the exact argument values/pointer identity, allowed mutations, call-through count/order, returned value, error path, ownership rules, and no retained pointer beyond its proven lifetime. Exercise null/empty/boundary inputs only where the binary contract allows them.
4. **Lifecycle tests:** through production install/remove interfaces, cover wrong build ID, absent module/import, invalid relocation/prologue, duplicate install, protection failure, partial installation rollback, original pointer publication before enable, concurrent callback entry, unhook, and unload order. Verify code/data bytes and page protections are restored; a test-only manual restore does not count.
5. **Frequency and thread checks:** establish call-site classification and thread assumptions from binary evidence first, recording limits from indirect calls and incomplete graph coverage. A deterministic event driver then checks the callback's work and original-call count for each supplied invocation and exercises relevant interleavings. Its chosen invocation rate does not measure the game's rate or prove which thread the game uses. Any unresolved timing assumption that affects safety blocks promotion; no sleeps, guessed frame cadence, or throughput claims from a mock driver.
6. **Exact-binary caller check where needed:** when static evidence leaves a relevant ABI, ownership, or invocation question unresolved, use a bounded harness that executes the identified call path from the pinned Quest ELF under arm64 Bionic and observes the production callback plus original-call result. Calling NEVR's callback directly through the signature under test, or manually invoking a candidate function through a guessed type, is insufficient. Record any fake platform imports and the behavior they prevent the harness from proving. If this path cannot execute without a full game launch, retain the unresolved status and require later approved qualification for that hook.

These tests establish that NEVR's callback conforms to the verified Quest call contract and that the production hook lifecycle behaves correctly. Static call-site evidence plus a callback harness does not establish that an entire game process reaches the hook. Routine checks do not launch the game. If a particular invocation cannot be proven from exact static evidence plus an isolated binary harness, record that limit and gate only the affected hook's final promotion on later approved qualification; do not describe the mock as runtime invocation proof.

### 3. Shared behavior tests

Once callback contracts are fixed, test the production portable files directly in the PCVR and Android test targets:

- Config precedence and URL rewriting, including the same URL/path/query inputs producing identical decisions on both builds.
- Login profile transformation and escaping through JSON parse-round-trip assertions; use synthetic identity/profile values only.
- EVR frame production/consumption and routing through a small independent test reader. Preserve the shared EVR marker, message symbols and payload semantics. Do not add Quest-only framing or protobuf matchmaking frames.
- Connection state with distinct config/login and matchmaking game sockets: authenticated login association, one login per session, secondary matchmaking socket sharing that session, close/reconnect ordering, stale identifiers, malformed/oversized messages, and unknown-symbol behavior.
- Auth-route selection and secret redaction using synthetic tokens and credentials. Assert no secrets or serial values enter logs.

Use unit tests and an in-process scripted loopback WebSocket peer for the socket boundary. The peer returns fixed config/login/matchmaking replies; it does not imitate Nakama account validation or lobby allocation. Do not start a local Nakama service, database, or production connection. The game's shared EVR protocol is the contract under test.

### 4. Android adapter and artifact checks

On every relevant change, cross-compile the same portable production sources for PCVR and arm64 Android, then inspect the resulting artifacts. Check architecture, SONAME/dependencies, exports, Windows imports absent from Android, original-loader forwarding, linked shared objects, and exact hook-backend symbols. Test Android socket/TLS adapter behavior against the loopback peer with certificate verification enabled, including connect failure, timeout, invalid CA/hostname, short read/write, half-close, callback after teardown, and clean close.

Cross-compilation does not prove Bionic execution. If a callback/backend primitive depends on Bionic behavior, add a narrow arm64 Bionic harness test on an emulator/VM only when required for that assertion. It must not install the game APK or launch the game. Report any unexecuted Bionic-specific assertion as a blocker for that backend, not as a pass. No headset is a routine test dependency.

## Test commands and gates

The existing commands are useful only for their documented scope: `just verify` for the Windows build/unit/source checks, `just test-android` for the Android build/ELF checks, and targeted test binaries for their unit checks. They do not establish Quest callback compatibility unless the new exact-binary fixtures and callback tests are included.

Proposed commands after implementation:

| Command | Assertions |
| --- | --- |
| `just test-quest-fixtures` | Exact hash/build-ID, address conversion, call-site bytes, relocation/prologue and ABI fixture checks. |
| `just test-quest-callbacks` | Production callback signature/arguments/returns/ownership/lifetime/frequency and production hook install/rollback tests. |
| `just test-shared-networking` | Shared-source parity, URL/config/login/EVR/session tests and scripted loopback peer. |
| `just test-quest-android` | Existing Android artifact checks plus adapter/TLS tests and required Bionic harness assertions. |
| `just test-quest` | Runs the preceding gates; fail closed if required identity or a backend-specific runtime assertion is missing. |

No new test command should invoke `./launch-client.sh`, `just quest-install`, config-file fallback logic, a local Nakama service, or a headset. Do not run `just verify` merely because this document was edited; implementation-specific verification follows the authorized implementation tranche and repository rules, subject to the user's standing constraints on runtime tests.

## Pass conditions and stop points

1. **Binary identity is pinned.** Every ReVault observation is tied to the exact file hash/build ID, and address conversions are explicit.
2. **A hook site is justified.** PCVR boundary and Quest logical counterpart are mapped with evidence. Candidate names alone do not pass. If no corresponding Quest callback or safe mechanism is established, stop that hook and report the missing evidence.
3. **The ABI test passes.** The production callback's declared type and behavior agree with the call-site evidence, and production lifecycle rollback is tested. Any unknown ownership, return contract, frequency, or call-thread assumption blocks promotion.
4. **Shared behavior remains shared.** Portable source files are identical between PCVR and Quest targets; cross-target fixture tests catch drift. Differences are limited to the game ABI and thin platform/hook/network adapters justified by measurements.
5. **Network rules pass offline.** Shared EVR behavior is tested with unit fixtures and the local scripted peer. No service process is needed.
6. **Artifact/platform checks pass.** Android output has the expected AArch64 shape and adapter behavior. Any Bionic-only claim has a Bionic test result; compilation is not a substitute.

## Bounded implementation order

1. Complete and review the PCVR-to-Quest boundary map and exact-binary fixtures. No code change in this step.
2. Add signature/fixture tests for the first verified callback and hook lifecycle. Factor a shared hook interface only if inspection shows it reduces duplicated lifecycle while keeping platform-specific patch mechanics thin.
3. Move or extend only portable production code needed by the mapped login/config/matchmaking path; compile the same translation units in both targets and add direct behavior tests.
4. Add the loopback-peer and Android transport tests after the shared session behavior is identified. Keep social callbacks outside this networking tranche until their own Quest ABI is mapped.

Each code tranche requires its own reviewed plan and targeted verification before the next tranche. This document authorizes no implementation, game run, APK installation, Nakama launch, or deployment.

## Self-review pass 1 — boundary accuracy

Checked the PCVR hook declarations and install sites against `src/abi/echovr_functions.*`, `src/runtime/lifecycle/config.cpp`, and `src/runtime/lifecycle/initialize.cpp`. Replaced assumptions that Quest's named methods or GOT imports are hook sites with a map-first requirement. Added the exact ReVault Quest hashes and marked the call conventions unknown. Recorded the `0xf28e40` to `0x10a2e7c` inconsistency as unresolved rather than assigning the helper a guessed role.

## Self-review pass 2 — test validity and scope

Checked that callback tests exercise the production callback and production lifecycle using exact-build fixtures, and that mocks are not presented as proof the full game invokes a hook. Added source-parity, ownership/lifetime/frequency, rollback, and backend-specific Bionic gates. Removed routine headset/game-install/full-game-launch and Nakama-service requirements. Kept the offline peer limited to the WebSocket boundary and retained the shared EVR flow; no Quest protocol or Quest-only algorithm is introduced.

## Astra review

Approved for the documented test-design scope. Checked PCVR declarations, install sites and lifecycle against the repository, and the Quest binary claims against the supplied ReVault results. Clarified decompilation provenance versus ELF imports, portable backend reuse, fixture provenance, and the limits of mock ABI/frequency checks. Exact Quest callback mapping and any required binary invocation remain evidence gates; this approval does not assert that a candidate hook is already verified.

No implementation, builds, tests, game runs, APK installs, configuration edits, or Nakama operations were performed for this review.
