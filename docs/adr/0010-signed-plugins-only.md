# ADR 0010: the runtime loads only plugins signed through the release-signer pipeline

Status: proposed (#741). Nothing in this document is implemented. Sections 1 to 6 answer the questions the design has to settle; every
code claim is a `file:line` on the tree this ADR was written against, every measurement names its probe.

## Context

`LoadPlugins` loads every DLL that `config.yaml`'s `plugins:` list names from the plugins directory and runs its `NvrPluginInit*` inside
the game process, with the runtime's authority. Nothing asks who built the file. The signing pipeline for releases already exists
(`thesprockee/release-signer`, Azure Artifact Signing, profile `andrew-bates-public`); the runtime does not use its output for plugins.

## Decision

A plugin DLL loads only when all of these hold, checked on the file before `LoadLibrary` and on the same open handle:

1. The Authenticode signature is intact (the file digest matches the signed digest).
2. The signer certificate chains to the pinned Microsoft Identity Verification Root CA 2020, built by the runtime against that
   root alone (an exclusive-root chain engine), not against the machine's trust store.
3. The signer certificate carries the whole EKU OID of our profile (section 2) and, once the signer fills it in, the pinned subject.
4. The certificate was valid at the time the RFC 3161 timestamp counter-signature records; a signature with no valid timestamp is
   accepted only while the leaf is itself within its validity period.

Otherwise the plugin is refused with one line (`[NEVR.PLUGIN] plugin=<name> file=<file> refused: <reason>`), recorded in the plugin
manifest the login carries as a failed load (`PluginManifestEntry`), and, for a plugin marked `required`, the existing required-plugin
failure path applies (N134). There is no config key and no environment variable that relaxes this in a release build. The one
relaxation is a build-time option (section 3).

## 1. Every LoadLibrary path for plugins today, and where the check goes

All plugin loading goes through one function.

- `LoadPlugins(PluginPhase)` at `src/runtime/ext/plugin_loader.cpp:634` starts a `PluginLoadRun`; its pass 1 calls `StageOne` for each plan
  entry in the phase (`plugin_loader.cpp:205-213`).
- `StageOne` (`plugin_loader.cpp:274`) is the only caller of `LoadModule` (`plugin_loader.cpp:292`). It already refuses a disabled entry, a
  superseded one, a duplicate and a file outside the plugins directory (`PluginFileStaysInDirectory`) before that call.
- `LoadModule` (`plugin_loader.cpp:368`) is the only place a plugin file is mapped: `LoadLibraryExA(path, nullptr,
  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)` at `:384`, and, when the OS rejects those flags with
  `ERROR_INVALID_PARAMETER`, an unrestricted `LoadLibraryA(path)` at `:391` (logged as "hardening inactive").
- The early pass (N146) and the normal pass both run through `StageOne`; there is no second loader. `module_loader.cpp` and the
  statically linked modules (`platform_compat`, `token_auth`) load no DLL files of ours (`git grep -n LoadLibrary -- src/runtime/ext
  src/modules` finds only `platform_compat.cpp:211,269`, which load the system's `Secur32.dll` and `ole32.dll`).
- `DllLoadHook` (`src/runtime/hook/dll_load_hook.cpp:308-322`) hooks all four LoadLibrary variants. It filters the game's loads and is not
  part of the plugin path; it is not the place for this check because it sees a load after the file is chosen and cannot name the
  plugin entry that asked.

The check goes in `StageOne`, between `PluginFileStaysInDirectory` and `LoadModule`, as one call:
`PluginSignatureVerdict v = VerifyPluginSignature(path, policy)`. Verdict "refused" goes to `FailPluginLoad(item, reason, itemReport)`
(the same function `LoadModule` failure uses at `plugin_loader.cpp:294`). The verifier is a separate file
(`src/runtime/ext/plugin_signature.{h,cpp}`), a function over a file handle and a policy value, so a test calls it without a game.

Two properties the placement has to keep:

- **Verify and load the same bytes.** `VerifyPluginSignature` opens the file with `FILE_SHARE_READ` only (no write, no delete sharing)
  and `StageOne` keeps that handle open across `LoadModule`; a writer cannot swap the file between the check and the map. The handle goes
  to `WinVerifyTrust` in `WINTRUST_FILE_INFO.hFile`. Not shown: that Wine honours the sharing mode on `LoadLibraryExA` of an already open
  file; the test in section 5 (swap attempt) measures it.
- **Dependencies in the plugins directory.** N75 restricts a plugin's import resolution to its own directory and System32. An unsigned
  DLL in the plugins directory that a signed plugin imports would load without a check. The verifier therefore also reads the verified
  plugin's import table and requires every import that resolves to a file in the plugins directory to pass the same check. System32
  imports are the OS's. This is an addition to the stated design (the order says "a plugin DLL"); it is in the plan because without it the
  check can be bypassed through a dependency. Decision for the reviewer: keep it (recommended) or drop it and state the gap.

## 2. What is pinned, and how it survives 72-hour leaf certificates

Artifact Signing issues a leaf certificate that is valid 72 hours and renews it daily; the durable identity of the signing profile is the
profile's EKU, and deleting and recreating the profile changes it. Source: Microsoft Learn, Artifact Signing "Certificate management",
https://learn.microsoft.com/en-us/azure/artifact-signing/concept-certificate-management. The same page and the Q&A thread
https://learn.microsoft.com/en-us/answers/questions/5923488/how-to-trust-azure-artifact-signing-certificates-w
say that a timestamped signature stays valid after the leaf expires, and that Windows trusts the chain through the Microsoft Identity
Verification Root CA 2020 distributed in its trust list.

What the runtime pins (values from `release-signer`, which is the authority for them; the runtime copies, it does not invent):

| Pin | Value | Source |
| --- | --- | --- |
| Root | Microsoft Identity Verification Root CA 2020 certificate, compiled in as DER (SHA-256 of the DER is also compiled in and checked at startup of the verifier) | `release-signer/config/ca/ms-identity-verification-root-2020.crt` |
| Leaf EKU | the whole OID `1.3.6.1.4.1.311.97.918669346.25014483.651891390.88972997`, compared as a whole string, never by the `1.3.6.1.4.1.311.97.` prefix (every Artifact Signing customer shares the prefix) | `release-signer/config/signing.yml:15` |
| Code signing EKU | `1.3.6.1.5.5.7.3.3` must also be present | RFC 5280 |
| Subject | the exact subject of the signing identity, RFC 2253 form | `release-signer/config/signing.yml:17-21`: **empty today**. `release-signer`'s verifier already compares it when set (`internal/authenticode/verify.go`). Filling it in is a prerequisite PR in that repo (not this issue); until it lands the runtime pins EKU plus root only, and the ADR's status stays "proposed". |
| Account and profile names | not in the certificate; not pinned by the runtime (`sprockeesigning`, `andrew-bates-public` are the signer's configuration) | |

Survival over leaf rotation: the pin is the EKU and the root, neither of which changes with the daily renewal. The leaf's validity is
judged **at the signing time**, not at load time: the verifier reads the RFC 3161 counter-signature attribute
(`1.3.6.1.4.1.311.3.3.1`) from the signature, takes the `genTime` of the time-stamp token, and builds the chain with
`CertGetCertificateChain(..., pTime = genTime, ...)`. The timestamp token's own signer chain (the Microsoft TSA,
`http://timestamp.acs.microsoft.com`, per `release-signer`) is verified the same way. A signature with no timestamp is judged at the
current time, so it passes only while the 72-hour leaf is live; that is an accepted case only for a dev build (section 3), and a release
plugin without a timestamp is refused with the reason `no timestamp and signer certificate expired`.

Not shown: the tail of that chain with a real Artifact Signing certificate and the real Microsoft TSA. The probe in section 4 used a
private CA and the DigiCert TSA. A real run needs a file the pipeline signed; the first signed plugin from `release-signer` is the
witness, and the test plan (section 5) includes a fixture signed by the pipeline once one exists.

API pages: `WinVerifyTrust`
https://learn.microsoft.com/en-us/windows/win32/api/wintrust/nf-wintrust-winverifytrust, `CertGetCertificateChain`
https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-certgetcertificatechain.

## 3. Testing an unsigned local build of a plugin

Recommendation: **a dev build of the runtime**, a build-time option and not a runtime switch.

- `-DNEVR_ALLOW_UNSIGNED_PLUGINS=ON` compiles the verifier in "log and allow" mode: an unsigned or foreign-signed plugin loads, with a
  Warning line per plugin (`[NEVR.PLUGIN] plugin=<name> file=<file> UNSIGNED, loaded because this is a dev build`) and the build
  identity (`nevr_identity`, N112) carries `plugins=unsigned-allowed`, so the server side can see it. Default OFF. Only the dev preset and
  an explicit configure flag turn it on.
- A release cannot carry it: `tools/package_release.py` (which already reads the identity literal from the shipped
  `BugSplat64.dll`) gets a check that the literal says `plugins=signed-only`, and `tools/tests/test_release_contract.py` pins that
  check. The release-signer's source declaration reads the same literal (`binary: BugSplat64.dll`, `sources.yml`).

Why not the alternatives: a config key or environment variable is something a malicious package can set next to the DLL, which defeats
the gate ("no config flag turns it off in a release build"). A "test certificate" the runtime also trusts means a second pin in the
shipping binary, and anyone with that key loads plugins into every player's game. A build-time option leaves the shipping binary with
exactly one trust root.

There is no signing route for a developer's local build, on purpose: the project issues no development certificate. A plugin
developer builds the runtime with the option on (`just preset=mingw-debug build`; local builds already stamp `-dev`) and uses that
`BugSplat64.dll` in their own game copy.

## 4. WinVerifyTrust under Wine and Proton, and how it is known

Measured with `wine-11.19`, a throwaway prefix, `DISPLAY` and `WAYLAND_DISPLAY` unset, a probe built with the repo's toolchain
(`x86_64-w64-mingw32-g++ 16.2.0`, static) calling `WinVerifyTrust`, `CryptQueryObject`, `CryptMsgGetParam` and, with an exclusive root
store, `CertCreateCertificateChainEngine` + `CertGetCertificateChain`. Test files were signed with `osslsigncode 2.14` under a private
root CA (leaf EKUs `1.3.6.1.5.5.7.3.3` plus a test OID), plus one DigiCert RFC 3161 timestamp.

| Case | `WinVerifyTrust` | Exclusive-root chain (`dwErrorStatus`) | EKU test |
| --- | --- | --- | --- |
| unsigned file | `0x800b0100` (`TRUST_E_NOSIGNATURE`) | n/a | n/a |
| signed by our test root, test EKU | `0x00000000` | `0x00000000` | present |
| signed by our test root, other EKU | `0x00000000` | `0x00000000` | absent |
| signed by a different root, our EKU | `0x00000000` | `0x00010000` (`CERT_TRUST_IS_UNTRUSTED_ROOT`) | present |
| our signed file, one byte of code changed | `0x80096010` (`TRUST_E_BAD_DIGEST`) | n/a | n/a |
| signed with RFC 3161 timestamp | `0x00000000`; `CRYPT_PROVIDER_SGNR.csCounterSigners` is 0 | `0x00000000` | present |

What follows from it:

- **Under Wine, `WinVerifyTrust` returning 0 proves only that the digest matches a well-formed signature.** It returns 0 for a signature
  from a root nobody trusts. The identity decision cannot rest on its verdict. It is used for the digest check; the root and EKU are
  decided by the runtime's own exclusive-root chain build, which gave the right answer for every row.
- Wine does not populate counter-signers in the provider data, so the timestamp is read by the runtime itself: `CryptQueryObject(
  CERT_QUERY_OBJECT_FILE, ..., CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, ...)` then `CryptMsgGetParam(CMSG_SIGNER_UNAUTH_ATTR_PARAM)`
  returned attribute `1.3.6.1.4.1.311.3.3.1` (5998 bytes) for the timestamped file and "not found" for the untimestamped one.
  Parsing the token (an inner CMS `SignedData` whose content is a `TSTInfo`) is the work item; it is not done in the probe.
- The same code path serves native Windows, where `WinVerifyTrust` additionally checks the Platform Trust List. We do not rely on that
  half of it, so the two platforms take the same decision.

Not shown: Proton (no Proton install was run; Proton ships Wine's crypt32 and wintrust, so the Wine result is the expected one, and the
test in section 5 runs against it when a Proton prefix is available), native Windows (the `just test-winvm` rig), and revocation (the
probe disabled it, `WTD_REVOKE_NONE`; the plan does not check revocation, because the signer's certificates are short-lived and renewed
daily, and an online check inside the game's boot is a failure mode of its own). The probe sources are scratch and are not committed;
the implementation's tests (section 5) are what stay.

## 5. Tests

Fixtures are generated at test time by a Python helper in `tools/tests/` (openssl + osslsigncode, both present in the builder image,
`.github/builder/Dockerfile:37-44`): a test root CA, a leaf with the test EKU, a second leaf with another EKU, a second root, and a
trivial DLL built from the repo's toolchain. The verifier takes the policy (root DER, EKU, subject) as an argument, so the tests pin the
test root and never the Microsoft one; one more test pins the Microsoft root constant by hash.

GTest under Wine (`src/runtime/tests/test_plugin_signature.cpp`, added to the GTest floor):

| Test | Expected |
| --- | --- |
| signed by the pinned root and EKU, timestamped | accepted |
| unsigned file | refused, reason `unsigned` |
| signed by the pinned root, other EKU | refused, reason `EKU` |
| signed by another root, same EKU | refused, reason `untrusted root` |
| signed file with one byte changed | refused, reason `digest mismatch` |
| leaf expired, valid timestamp inside the leaf's validity | accepted |
| leaf expired, no timestamp | refused, reason `signer certificate expired and no timestamp` |
| timestamp after the leaf's `notAfter` | refused |
| file replaced between verify and load (swap attempt on the held handle) | the swap is refused by the OS, or the loaded bytes equal the verified bytes |
| a dependency in the plugins directory that is unsigned | refused, reason names the dependency |
| `NEVR_ALLOW_UNSIGNED_PLUGINS` build: unsigned plugin | loaded with the Warning line |

Each test is also run as a mutant (the AGENTS.md rule): drop the root check, drop the EKU compare, compare the EKU by prefix, drop the
digest check, pass `pTime = now`; each must make the row it guards fail.

Integration (`tests/system` or a `launch-client.sh` run on a game copy, nested display): a game copy with one signed fixture plugin and
one unsigned in `config.yaml`; the run's log must contain the load line of the first and the refusal line of the second, and the manifest
carried by the login (`nevr_plugins`) must list the second as failed. Per AGENTS.md "Logging", the refusal record is read from a real
run.

`just test-winvm` runs the same GTest on native Windows as the second platform.

Also: `tools/tests` gets the release contract tests of section 3, and the N-rules index in `AGENTS.md` gets a row for the new rule once
it is implemented.

## 6. Plugins people use today, and the release-notes line

Today every plugin outside this repository (`nevr-runtime-plugins`: `broadcaster-bridge`, `anim-debugger`, `audio-intercom`,
`game-rules-override`, `session-unlocker`, `combat-mod`, `combat-2d`) is built and distributed unsigned, and the one plugin in this
repository (`example`) is not shipped to players. From the first release with this change an unsigned copy of any of them is refused
with a clear line, whether the player built it, downloaded it, or got it from a friend.

- Each plugin that is to keep working is added to `release-signer/config/sources.yml` as a source (repo, workflow, start commit, exact
  file set; POLICY.md rules 1.2 and 3.3), so its release DLLs come out of the same pipeline with the same signature. That is a separate
  PR in `release-signer` per plugin repo, not part of #741; the plugin repos' release workflows must produce the build attestation the
  signer demands. The order of merging: the signed plugin releases exist before the runtime release that enforces.
- Two upgrades of the runtime exist, so a player is never left with a plugin that no longer loads without being told: the release notes
  say it, and each refusal line names the file and the cause.
- `config.yaml` entries for refused plugins stay; a refused plugin is the same failure as one that cannot load today
  (`FailPluginLoad`), so a `required` plugin stops the run and an optional one is skipped.
- `#575` moves the plugins directory to `%LOCALAPPDATA%\nevr\echovr`; the check works on whatever path `LoadModule` receives, so the two
  changes do not touch each other.

Release-notes line (for `RELEASE-NOTES.md`, which the release pipeline builds):

> Plugins: the runtime now loads only plugins signed through the project's release pipeline. An unsigned plugin, or one signed by anyone else,
> is not loaded; `BugSplat64.dll`'s log names the file and the reason on one line. Use the signed releases of each plugin.
> Plugin developers: build the runtime with `-DNEVR_ALLOW_UNSIGNED_PLUGINS=ON` to test an unsigned build; those builds say so in their
> login and are not released.

## Consequences

- One more pinned artifact in the binary (the root certificate) and a dependency of the runtime's release on `release-signer`'s
  `signer_subject` being set.
- Plugin authors lose the drop-a-DLL workflow on a release runtime; the dev build is the replacement.
- A stolen signing profile is the single point of trust, as it already is for `BugSplat64.dll`.

## Open items for the reviewer

1. Keep the dependency check (section 1) or drop it and record the gap.
2. Revocation is not checked (section 4); say if that is acceptable.
3. `signer_subject` must be filled in `release-signer` before this ships.
4. The plugin repos' signing onboarding order (section 6).
