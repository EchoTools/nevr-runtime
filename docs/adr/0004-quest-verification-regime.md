# ADR 0004: Quest networking is verified against the exact binaries, offline

Status: accepted, mostly not implemented. The shared redirect vectors and the source-parity
check exist; the fixture, callback, lifecycle and Bionic tests are tranches of #158. The design
being verified is ADR 0003.

## Objective

Show that the callback contracts the Quest build uses match the exact Quest game binaries, then
test the shared networking behavior PCVR already uses. A passing mock or a matching method name
is not evidence that the Quest binary calls a callback with the declared ABI.

Routine checks need no headset, APK install, full game launch, local Nakama, live service,
`config.json` or controller input. A behavior that static analysis and a bounded binary harness
cannot establish needs a separately approved qualification.

## Shared PCVR boundaries under test

| Boundary | Where | Test consequence |
| --- | --- | --- |
| Config lookup and URL redirect | PCVR binds `EchoVR::JsonValueAsString` (`src/abi/echovr_functions.cpp`); the hook calls the original, then applies the shared redirect and login-override policy (`src/runtime/lifecycle/config.cpp`). | Test the shared redirect and override functions and the adapter's call-through contract. |
| HTTP URL connection | PCVR binds `EchoVR::HttpConnect` (RVA `0x1F60C0`); `HttpConnectHook` applies service routing, then calls the original. | Test argument identity and replacement, original-call count and return forwarding. A Quest counterpart is not established. |
| Hook lifecycle | `src/core/hook_lifecycle.h` (no platform headers) owns publication ordering and rollback; `src/core/hooking.h` (MinHook) and `src/quest/sentinel/got_hook.cpp` (GOT) are backends of it. `src/runtime/tests/test_hooking.cpp` covers the contract and the MinHook backend; `src/quest/tests/got_hook_test.cpp` covers the GOT backend. | Add backends, not lifecycles; do not force the Windows backend onto Bionic. |
| Pure service mapping | `src/runtime/lifecycle/service_map.cpp` and `service_redirect.cpp`, tested by `test_service_map`. | Extend these shared-source tests; never copy URL policy into a Quest-only implementation. |

PCVR identity for comparison fixtures: `echovr.exe` SHA-256
`b6d08277e5846900c81004b64b298df6acba834b69700a640b758bda94a52043`, `CJson::String` at VA
`0x1405FE290`, `HttpConnect` at VA `0x1401F60C0`. Confirm the PCVR build before using these
addresses.

## Test sequence

### 0. Pin the binaries and the source set

Record the exact PCVR executable and the Quest APK plus `libr15.so`, `libpnsrad.so`,
`libpnsradmatchmaking.so` and loader or shim identities: SHA-256, ELF build ID, architecture,
SONAME, load-bias model, dependencies, imports and relocations. Compare each ELF hash with
ReVault before interpreting an address. Commit only reviewed, non-secret fixtures under
`tests/quest/fixtures/<build-id>/`; generated evidence and scratch go under
`/var/tmp/work-nevr-runtime/`. A hash or build-ID mismatch fails the fixture test before any
callback assertion.

Record the portable source list used by both targets. A test fails if either target substitutes
a Quest copy of URL policy, login transformation, EVR framing or session behavior; only
platform adapters and narrow ABI or hook-backend code may differ.

### 1. Map each PCVR boundary and its Quest counterpart

A boundary record has: the PCVR symbol, address, detour, source file, declaration and
original-call path; the corresponding Quest call chain with owning ELF, VA, RVA, file offset and
load bias; ReVault function details, decompilation tier, xrefs, direct and indirect callers and
callees, disassembly and bytes (cross-checked, because ReVault's call graph is partial); the
AArch64 argument registers or stack layout, return register, hidden `this` or sret arguments,
object layout, pointer ownership and mutability, thread and timing, lifetime, frequency and
failure behavior; and the candidate mechanism (imported relocation, vtable callback or internal
function) with its exact relocation or prologue evidence. Every cell has a status and an
evidence reference. Unknown stays unknown and is never filled from PCVR's ABI or a demangled
name.

Order: PCVR `JsonValueAsString` first, as the service-URL seam; then `HttpConnect` where Quest has
a corresponding dial; login and matchmaking are mapped separately. `SendLogInRequest` and
`ConnectMatchmaker` are investigated only if the call chain shows they are useful.

No hook implementation is selected until the map identifies a boundary and its callback
contract. The tests exercise the production lifecycle and rollback APIs; they never simulate
success by restoring test memory themselves. Before separate backends are chosen, record which
existing library operations serve both Windows/x86_64 and Android/AArch64 and which need
different instruction relocation, memory protection, cache synchronization or loader handling.

### 2. Callback contract tests (the first executable tests)

1. **Compile-time signature checks:** function pointer type, calling convention, return type,
   pointer constness and parameter width; size, alignment and offsets of any game-owned
   structure the callback touches; the shared source compiles for PCVR and arm64 Android.
2. **Fixture-to-declaration checks:** the pinned call-site, relocation and prologue fixture still
   matches the binary identity and expected bytes. Each register mapping and return assertion
   ties to the decoded caller and callee instructions; comparing a declaration with a second
   hand-entered signature does not establish agreement with the binary.
3. **Production-callback harness:** invoke the production callback through the declared Quest
   function-pointer type with bounded synthetic objects and a recorded original-call stub. Assert
   argument values and pointer identity, allowed mutations, call-through count and order, the
   returned value, the error path, ownership rules and that no pointer is retained past its
   proven lifetime.
4. **Lifecycle tests** through the production install and remove interfaces: wrong build ID,
   absent module or import, invalid relocation or prologue, duplicate install, protection
   failure, partial-install rollback, original pointer published before enable, concurrent
   callback entry, unhook and unload order, with code, data and page protections restored.
5. **Frequency and thread checks:** classify call sites and thread assumptions from binary
   evidence first, recording the limits of indirect calls and incomplete graph coverage. A
   deterministic event driver then checks the callback's work and original-call count per
   invocation and exercises interleavings. Its rate is not the game's rate, and an unresolved
   timing assumption that affects safety blocks promotion.
6. **Exact-binary caller check** when static evidence leaves ABI, ownership or invocation
   unresolved: a bounded harness executes the identified call path from the pinned Quest ELF
   under arm64 Bionic and observes the production callback and the original-call result. Calling
   the callback directly, or invoking a candidate through a guessed type, is insufficient. Fake
   platform imports and the behavior they prevent the harness from proving are recorded. If the
   path cannot run without a full game launch, the status stays unresolved and only that hook's
   promotion waits for approved qualification.

These tests show that NEVR's callback conforms to the verified call contract and that the hook
lifecycle behaves; they do not show that a whole game process reaches the hook.

### 3. Shared behavior tests

The production portable files are tested directly in the PCVR and Android targets:

- Config precedence and URL rewriting give identical decisions on both builds for the same
  URL, path and query inputs.
- Login profile transformation and escaping, by JSON parse round trip, with synthetic identity
  values only.
- EVR frame production, consumption and routing through a small independent reader, preserving
  the EVR marker, message symbols and payload semantics; no Quest-only framing and no protobuf
  matchmaking frame.
- Connection state with distinct config/login and matchmaking game sockets: authenticated login
  association, one login per session, the matchmaking socket sharing that session, close and
  reconnect ordering, stale identifiers, malformed and oversized messages, unknown symbols.
- Auth-route selection and secret redaction with synthetic tokens and credentials; no secret or
  serial value enters a log.

The socket boundary uses an in-process scripted loopback WebSocket peer that returns fixed
config, login and matchmaking replies. It does not imitate Nakama account validation or lobby
allocation, and no local Nakama, database or production connection is started.

### 4. Android adapter and artifact checks

On every relevant change, cross-compile the same portable sources for PCVR and arm64 Android and
inspect the artifacts: architecture, SONAME and dependencies, exports, no Windows imports on
Android, original-loader forwarding, linked shared objects and the hook-backend symbols. Android
socket and TLS adapter behavior is tested against the loopback peer with certificate
verification on: connect failure, timeout, invalid CA or hostname, short read or write,
half-close, callback after teardown, clean close.

Cross compilation does not prove Bionic execution. A primitive that depends on Bionic behavior
gets a narrow arm64 Bionic harness on an emulator or VM, which never installs the game APK or
launches the game. An unexecuted Bionic assertion is reported as a blocker for that backend, not
as a pass.

## Commands

`just verify` covers the Windows build, unit tests, source checks, `just test-quest-router` (the
session router through fake transports, the WebSocket wire codec, and the loopback game server
with a raw TCP client) and `just test-quest-hooks`
(the GOT backend, lifecycle and thunks against fixture shared objects and in-memory images, on
the host). `just test-quest-hooks-pinned` resolves the pinned targets in the real `libr15.so`
and `libpnsradmatchmaking.so` extracted from the pinned APK and fails if the APK is absent.
`just test-quest-tls` runs the libcurl connector against real TLS servers made on the host (trusted
chain, wrong CA, wrong host name, self-signed leaf, empty trust store, non-TLS server, `ws://`) and
asserts no plaintext fallback. `just test-android` covers the Android build and ELF shape. None of these executes a Quest
binary under Bionic, so none establishes callback compatibility by itself. The commands below
do not exist yet:

| Command | Assertions |
| --- | --- |
| `just test-quest-fixtures` | Exact hash and build ID, address conversion, call-site bytes, relocation, prologue and ABI fixtures. |
| `just test-quest-callbacks` | Production callback signature, arguments, returns, ownership, lifetime and frequency, and production hook install and rollback. |
| `just test-shared-networking` | Shared-source parity, URL, config, login, EVR and session tests with the scripted loopback peer. |
| `just test-quest-android` | Android artifact checks plus adapter, TLS and required Bionic harness assertions. |
| `just test-quest` | All of the above; fails closed if an identity or a backend-specific runtime assertion is missing. |

None of these invokes `./launch-client.sh`, `just quest-install`, config-file fallback logic, a
local Nakama or a headset.

## Pass conditions and stop points

1. Binary identity is pinned: every ReVault observation ties to the exact file hash and build
   ID, with explicit address conversions.
2. A hook site is justified: the PCVR boundary and its Quest counterpart are mapped with
   evidence. A candidate name alone does not pass; with no corresponding callback or safe
   mechanism, that hook stops and reports the missing evidence.
3. The ABI test passes: the declared type and behavior agree with the call-site evidence and the
   lifecycle rollback is tested. Unknown ownership, return contract, frequency or call thread
   blocks promotion.
4. Shared behavior stays shared: the portable files are identical across both targets and
   cross-target fixture tests catch drift.
5. Network rules pass offline, with unit fixtures and the scripted peer.
6. Artifact and platform checks pass: the Android output has the expected AArch64 shape, and any
   Bionic-only claim has a Bionic test result.
