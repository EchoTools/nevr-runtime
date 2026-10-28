# ADR 0010: plugins are checked against the release-signer pipeline, and a run can require the signature

Status: accepted, implemented by #745 (merge commit 712fb5c4226335f9084ef5dcd42da986e62efc04: the signature check; signed-only and dev
builds) and #746 (merge commit 99eb052bc9debfc0ff332f35127b06437142c71f: dependencies and later loads, one refusal dialog) (#741), and
amended by #924 (the default and the argument, section 3). The text states what main does; "What changed from the plan" at the end lists
where the implementation differs from the first draft.

**The mode of a run.** A run is `unsigned-allowed` or `signed-only`. Every build starts `unsigned-allowed`: release-signer signs the
runtime and does not sign plugins yet, so a plugin a player or a plugin author can install today is unsigned. A run started with
`-require-signed-plugins` is `signed-only`, and the decision below applies in full. Once release-signer signs plugins, the default
flips to `signed-only` in a separate change, which also decides whether the argument stays.

Code is cited by file and symbol; every measurement names its probe; what is not shown is said where it matters.

## Context

`LoadPlugins` loads every DLL that `config.yaml`'s `plugins:` list names from the plugins directory and runs its `NvrPluginInit*`
inside the game process, with the runtime's authority. Nothing asks who built the file. The signing pipeline for releases exists
(`thesprockee/release-signer`, Azure Artifact Signing, profile `andrew-bates-public`); the runtime does not use its output for plugins.
The pipeline has produced a signed file: the v5.4.3 release's `BugSplat64.dll` (`tools/fetch_real_signed_dll.py` fetches
`nevr-runtime-v5.4.3-windows-signed.zip`), and `RealSigned.*` in `src/runtime/tests/test_plugin_trust.cpp` verifies it against the
runtime's pins.

## Threat model

The gate protects a player from a plugin handed around between players: a DLL from a chat, a mod pack or a download page that the
player puts in `plugins/` and lists in `config.yaml`.

It does not protect against anything that can also replace `BugSplat64.dll` or `echovr.exe`, or write the runtime's memory: an
installer, a script run as the player, or another program under the same account can disable the check by replacing the runtime. It
does not protect against a compromised signing profile beyond the denylist in section 2. It does not defend a game copy against its
own owner (the owner chooses the mode of their own run, section 3).

What checks the runtime itself: its own Authenticode signature, made by the same pipeline (`BugSplat64.dll` is in the `sign:` list of
`release-signer/config/sources.yml`), which a player or an installer can verify (`Get-AuthenticodeSignature`; `SIGNING.txt` in the
package says how). Nothing in the game process checks it: the game imports `BugSplat64.dll` statically as one of its own dependencies
(`tools/verify_pe_contract.py`). This ADR does not add that check; it states the boundary.

## Decision

In a signed-only run a plugin DLL loads only when all of these hold, checked on the file before `LoadLibrary`:

1. The file carries an embedded Authenticode signature (a catalog signature, or none, is refused) and its digest matches.
2. The signer certificate chains to the pinned Microsoft Identity Verification Root CA 2020, built by the runtime against that root
   alone (an exclusive-root chain engine), not against the machine's trust store, and the chain build reports `dwErrorStatus == 0`
   (any nonzero status refuses).
3. The signer certificate carries the pinned profile EKU (a whole-OID compare) and the code-signing EKU. The signer's subject is not
   pinned (section 2).
4. The certificate was valid at the time of an RFC 3161 timestamp that is bound to this signature (section 2); a signature with no
   timestamp is accepted only while the leaf is itself within its validity period.
5. Neither the signer's leaf serial nor the signing time falls inside the denylist (section 2).
6. Every DLL the plugin imports, delay-loads or forwards to passes the same check, transitively (section 1).

Otherwise the plugin is refused with one line (`[NEVR.PLUGIN] plugin=<name> file=<file> refused: <reason>`), recorded as a failed
load in the plugin manifest the login carries (`PluginManifestEntry`), shown to the player (section 6) and, for a plugin marked
`required`, the existing required-plugin failure path applies (N134). A refusal is final: there is no fallback load (section 1). In an
unsigned-allowed run the same checks run for each plugin and a plugin they would refuse is logged at Warning (`UNSIGNED`, or the reason)
and loads. Only the command line sets the mode (section 3): no config key and no environment variable does.

## 1. Every load path today, and where the check goes

There are two plugin loaders in the tree, not one.

**The current loader** has one load site.

- `LoadPlugins(PluginPhase)` (`src/runtime/ext/plugin_loader.cpp`) starts a `PluginLoadRun`; pass 1 calls `StageOne` for each plan
  entry in the phase.
- `StageOne` is the only caller of `LoadModule`. It refuses a disabled entry, a superseded one, a duplicate and a file outside the
  plugins directory (`PluginFileStaysInDirectory`), then verifies the file, before that call.
- `LoadModule` is the only place a plugin file is mapped: `LoadLibraryExW(held path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
  LOAD_LIBRARY_SEARCH_SYSTEM32)`, where the held path is the final path of the handle the check holds (below). With no handle held
  (an unsigned-allowed run that could not open the file) it is `LoadLibraryExA(path, ...)` as the entry spells it. In an
  unsigned-allowed run, on `ERROR_INVALID_PARAMETER`, an unrestricted `LoadLibraryW(held path)` (or `LoadLibraryA(path)`) follows.
- The early pass (N146) and the normal pass both go through `StageOne`. `module_loader.cpp` and the statically linked modules load no
  DLL files of ours (`platform_compat.cpp` loads the system's `Secur32.dll` and `ole32.dll`).

**The check is in `StageOne`**, between `PluginFileStaysInDirectory` and `LoadModule`, with the handle held open across the load:
`VerifyPluginFile(HANDLE file, const Policy& policy, int64_t nowUnix)` in `src/runtime/ext/plugin_signature.{h,cpp}` takes a file
handle and a policy value, so a test calls it without a game. A refusal goes to `FailPluginLoad`; `LoadModule` stays a plain load that
runs only after a pass.

**No fallback in a signed-only run.** The unrestricted `LoadLibraryA` branch of `LoadModule` loads with the default search order
when the OS rejects the restricted flags. In a signed-only run it does not exist: `ERROR_INVALID_PARAMETER` from the restricted
load is a refusal with its own reason (`restricted search unsupported`), because a load the check did not shape is a load the check
did not cover. An unsigned-allowed run keeps the fallback.

**The legacy loader is a second load site, and it has no check.** `src/legacy/gamepatches/plugin_loader.cpp` calls `LoadLibraryA`
on every `*.dll` in `plugins\` next to `echovr.exe` and runs `NvrPluginInit`. It is built (`add_subdirectory(src/legacy/gamepatches)`
in `CMakeLists.txt`, target `gamepatcheslegacy`) and packaged by the `dist-legacy` target.

No workflow, release package script, document or install script references it: the release build builds `--target dist` only
(`.github/workflows/build.yml`), the v5.4.3 release carries five assets and none holds a legacy DLL, and `release-signer`'s declared
file set names none. `src/legacy/` is frozen (AGENTS.md "Guardrails"), so the gate cannot be added there.

Decision: **the legacy build is not part of any package target and is never in a release package.** `dist-all` depends on `dist`
only (`CMakeLists.txt`), the `justfile` keeps `dist-legacy` as an explicit developer recipe, and `tools/tests/test_release_contract.py`
asserts that no release workflow, `package_release.py` input or `sources.yml` entry names `gamepatcheslegacy` or `gameserverlegacy`.
Why not "give it the same gate": the directory is frozen by rule, and it would mean a second verifier compiled against a different
include layout, for code nobody ships. Not shown: whether anyone outside the release flow (a player, a hosting setup) runs a locally
built `dist-legacy` package; the threat model puts anyone who installs their own runtime DLL outside the gate.

**Dependencies, delay loads, forwarders.** N75 restricts a plugin's imports to its own directory and System32 (`LoadModule`). An
unsigned DLL in the plugins directory that a signed plugin imports would load without a check. `VerifyPluginTree`
(`src/runtime/ext/plugin_tree.cpp`) therefore reads the plugin's import table, its delay-load import directory and its forwarded
exports (an export whose RVA lies inside the export directory and names `dll.symbol`), and checks each named DLL, transitively, with
`VerifyPluginFile`, up to 64 files. The rules, each pinned by a test in `test_plugin_tree.cpp`:
- A name with no extension gets `.dll`, as the loader does, so the file checked is the file mapped (`WithDefaultExtension`).
- A name that carries a path is refused: it is resolved relative to something the walk cannot know.
- An import or forwarder that is not a file in the plugins directory and is not supplied by the system (an API set, a System32
  file, or a module already loaded in the process: `SuppliedByTheSystem`) is refused: a file put there before the load would be mapped unchecked.
- An absent delay-load is not refused: it is resolved when first called, by a `LoadLibrary` the late-load filter checks then, and an
  optional one (`nvapi64.dll` on a machine without it) must not refuse a plugin that loads without it.
- A name that only System32 supplies (not an API set, not a module already loaded: `SuppliedBySystem32Only`) is loaded from System32 by its
  full path before the plugin, and kept loaded until the plugin is staged. The loader searches the plugin's directory before System32
  for a name that is not already loaded, and the check cannot hold a file that is not there, so a file put in the directory after the
  check would otherwise map with the plugin; a module already loaded under the name is taken first. A staged plugin (`PinSystemDependencies`)
  and a later load the filter accepts both pin, and a name that cannot be pinned refuses the plugin.

**Plugin-initiated loads.** A plugin that calls `LoadLibrary` after its init, or from its `DllMain`, is not on the staged path.
`DllLoadHook` (`src/runtime/hook/dll_load_hook.cpp`) hooks all four LoadLibrary variants; its filter in `plugin_loader.cpp` checks
the load against the same verifier and refuses (null, `ERROR_MOD_NOT_FOUND`) on a failure. In a signed-only run:
- a name gets `.dll` when it has no extension;
- a bare name is checked against the plugins directory;
- a full path is checked when it lies in the plugins directory, compared by the file's final path (an 8.3 name, a `\\?\` prefix or a
  trailing dot or space cannot slip past a text compare), with the ANSI path converted through the code page;
- a relative path with a directory part is refused whatever it leads to: the loader appends it to every directory it searches, each
  through links, so no list of candidates is complete. A plugin that needs such a load passes a full path.
An unsigned-allowed run logs the line `a plugin loads <file> after its init: <reason>, allowed because plugin signatures are not
required in this run (plugin trust mode <mode>)` and allows it. Known gaps, recorded and not claimed closed: a load by an absolute path outside the plugins directory (it is
not a plugin file); under Wine, a unix symbolic link to the plugins directory is not recognised by the final-path compare, so an
unsigned file reached through it loads (#788, open); the lock order of the late-load filter against a load made from a plugin's
`DllMain` is not proven (#784, open); a direct `LdrLoadDll` or a manual map by a plugin (code that is already signed by us and running in the process can do
anything); a plugin's own build flags. Plugins built from `plugins/common` get `/DEPENDENTLOADFLAG:0x800` as a template default so
their own implicit imports resolve the way N75 does; that flag lives in the plugin's repo and the runtime cannot enforce it. The
race on a dependency that the system names (a file in `plugins\` that replaces a system DLL between the check and the load) was
filed as #783 and is closed.

**Check and load are separate steps.** A file can change between them.
- `StageOne` opens the file with `FILE_SHARE_READ` only (no write or delete sharing), passes that handle to `VerifyPluginFile` and
  keeps it open across `LoadModule`, so a Windows process cannot write or replace the file in between.
- The load names the file the handle holds, not the path the entry spells. `CheckSignature` takes the handle's final path
  (`GetFinalPathNameByHandleW`: no link, no junction, no 8.3 name, no prefix) and `LoadModule` loads that path, so a link, a
  junction or a directory someone can rename in the configured path cannot make the load map another file than the one checked. A
  signed-only run that cannot name the held file refuses the plugin (`cannot name the file it checked`); an unsigned-allowed run
  loads the configured path and says so at Warning. The tests drive the file-link form under Wine (`ALinkRepointedBetweenTheCheckAndTheLoad...`); the
  directory-link form rests on Windows's own final path for a handle and is not run here.
- Known gap: under Wine and Proton a Linux process of the same user can write the underlying file without the Windows sharing rules.
  The test in section 5 measures whether Wine honours the sharing mode against another Wine process; it cannot cover a native Linux
  writer, and the threat model puts a same-user native writer out of scope. The mitigation that would cover it (copy the verified bytes
  into a private directory and load the copy) breaks N75's directory-based dependency search, so it is not adopted; it is the option to
  take if the threat model widens.

## 2. What is pinned, and how it survives 72-hour leaf certificates

Artifact Signing issues a leaf certificate valid 72 hours and renews it daily; the durable identity of the signing profile is its
EKU, and deleting and recreating the profile changes it. Source:
https://learn.microsoft.com/en-us/azure/artifact-signing/concept-certificate-management. That page and
https://learn.microsoft.com/en-us/answers/questions/5923488/how-to-trust-azure-artifact-signing-certificates-w
say a timestamped signature stays valid after the leaf expires and that Windows trusts the chain through the Microsoft Identity
Verification Root CA 2020.

What the runtime pins. The values come from `release-signer`, which is their authority:

| Pin | Value | Status |
| --- | --- | --- |
| Root | Microsoft Identity Verification Root CA 2020, compiled in as DER; its SHA-256 is compiled in and checked | from `release-signer/config/ca/ms-identity-verification-root-2020.crt`, which the pipeline's own verifier uses |
| Leaf EKU | the whole OID `1.3.6.1.4.1.311.97.918669346.25014483.651891390.88972997`, compared as a whole string (every Artifact Signing customer shares the `1.3.6.1.4.1.311.97.` prefix) | **confirmed** against the leaf of the real signed v5.4.3 `BugSplat64.dll` (sha256 `a97767e62721503d4e249b016b734c42619a5aaa209ecdcd7a25a434f6d2a629`); `kPinsConfirmed` is true in `plugin_trust.cpp` and `RealSigned.*` in `test_plugin_trust.cpp` verifies that file against the pins |
| Code-signing EKU | `1.3.6.1.5.5.7.3.3` also present | RFC 5280 |
| Subject | **not pinned**, by design: `kSubject` is empty and an empty subject skips the check | the EKU and the pinned root already bind a file to this pipeline, and a short-lived leaf's subject text changes when the signer renews or renames, which Microsoft's Artifact Signing guidance warns about; a pin would refuse every plugin signed after such a change |

The EKU was confirmed by reading it from the real signed file, as above. A corrected pin is a runtime release; nothing is released
against a value the certificate does not show.

**Survival over leaf rotation.** The pin is the EKU plus the root, neither of which changes with the daily renewal. The leaf's
validity is judged at the signing time, not at load time: the verifier reads the RFC 3161 counter-signature attribute
(`1.3.6.1.4.1.311.3.3.1`) from the signature, takes the time-stamp token's `genTime`, and builds the chain with
`CertGetCertificateChain(..., pTime = genTime, ...)`. The token's own signer chain (the Microsoft TSA) is verified the same way.

**The chain is built from the cache only.** The chain build passes `CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL`
(`plugin_signature.cpp`): an issuer is never fetched over the network (AIA) while a plugin loads, so a load takes no network round
trip and cannot hang on one. The pinned root and the certificates embedded in the signature are what the chain is built from.

**The timestamp is bound to the signature.** A token is evidence only about the signature it stamps. The verifier reads the token's
`TSTInfo.messageImprint` (hash algorithm and digest) and compares it with the hash, under that algorithm, of the `signature` OCTET
STRING of the Authenticode `SignerInfo` (the encrypted digest). A token whose imprint does not equal it is refused as `timestamp not
bound to this signature`; without this, a valid token copied from another signature would pass.

**A signature with no timestamp** is judged at the current time, so it passes only while the 72-hour leaf is live and is refused with
`no timestamp and signer certificate expired` afterwards. The pipeline timestamps everything it signs (`release-signer` verifies the
timestamp in stage 60), so a pipeline file always carries one.

**Chain status.** The chain build's `dwErrorStatus` must be exactly 0; any set bit refuses. The reason line prints the hex value and
the `CERT_TRUST_*` name of each set bit. Measured (wine-11.19, exclusive root = the test root): a signature from a root that is not in
the store gives `0x10000`, `CERT_TRUST_IS_PARTIAL_CHAIN` (mingw `wincrypt.h:5305`); a signature that embeds a forged root
certificate of its own gives `0x20`, `CERT_TRUST_IS_UNTRUSTED_ROOT` (`wincrypt.h:5294`). Both are refused by the rule "any nonzero".

**Revocation, and a compromised signing day.** Short-lived certificates make an online revocation check (disabled in the probe,
`WTD_REVOKE_NONE`) a failure mode of its own at game boot, so there is none. A timestamped signature from a compromised day would stay
valid forever, so the runtime ships a denylist, compiled in and changed only by a runtime release, with three kinds of entry:
- a leaf certificate serial number;
- a signing-time interval (`genTime` from, until) for the profile, which refuses everything stamped in a compromised window whatever
  the serial;
- a profile EKU (the pin table is a list; a retired or compromised profile's EKU is removed from it and listed here, so the reason
  line can say why).

The denylist is the policy's `deniedSerials`, `deniedWindows` and `deniedProfileEkus` fields (`plugin_signature.h`), filled in
`ProductionPolicy` (`plugin_trust.cpp`; no entry today), and a test checks that every entry parses. A runtime that predates an entry cannot learn it: the
update path is a new release, and a plugin signed in the window stays loadable on older runtimes. The release notes of the release
that adds an entry say so.

The probe in section 4 used a private CA and the DigiCert TSA; the real Artifact Signing certificate and Microsoft TSA are exercised
by `RealSigned.*` (section 5).

API pages: `WinVerifyTrust`
https://learn.microsoft.com/en-us/windows/win32/api/wintrust/nf-wintrust-winverifytrust, `CertGetCertificateChain`
https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-certgetcertificatechain.

## 3. The mode of a run, and testing an unsigned build of a plugin

The mode is set by the command line and by nothing else:

- **`-require-signed-plugins`** makes the run `signed-only`. Without it the run is `unsigned-allowed`. `PreflightRuntimeBootstrap`
  (`src/runtime/lifecycle/boot.cpp`) calls `nevr_plugin_trust::ApplyCommandLine(GetCommandLineW(), GetCommandLineA())` once, before the
  first original `PreprocessCommandLine` call, which is before the early plugin pass (N146). The early pass, the normal pass and the
  late-load filter all read `nevr_plugin_trust::ActiveMode()`, so they share one mode.
- **Any token the game would read as the argument counts.** echovr.exe tokenizes the ANSI line with its own tokenizer and looks a name
  up with every leading `-`, `+` and 0x96 skipped and A-Z lower-cased (`src/runtime/lifecycle/cmdline_strip.h`: `Tokenize`,
  `detail::NameIs`, which `plugin_trust.cpp` reuses). So `-REQUIRE-SIGNED-PLUGINS`, `+require-signed-plugins` and `---require-signed-plugins`
  are the argument, and so is one after `-config "C:\dir\"`, where the game's quote ends at the second `"` and `CommandLineToArgvW`'s does
  not. The line is read two ways, the game's over `GetCommandLineA()` and `CommandLineToArgvW`'s over `GetCommandLineW()` (each argument
  converted to the ANSI code page), and `signed-only` wins if either finds the argument. A token inside a quoted value is not an option
  to the game and not the argument; a quoted `"-require-signed-plugins"` is the argument to `CommandLineToArgvW` and so counts. If a
  line cannot be read or converted, or reading it throws a `std::exception` (out of memory), whether the argument was given is unknown and
  the run is `signed-only`; the mode line then says `source=command line unreadable` or `source=command line read failed` at Warning. The argument is registered with the game's parser (`cli.cpp`), because the game rejects a token
  with no syntax entry, and listed in the README.
- **No config key and no environment variable sets the mode.** `config.yaml` is data a plugin or an installer can write, and the
  environment is inherited from whatever started the game; the command line is the one input a player types.
- **The log states it.** The loader writes one line per run, from whichever plugin pass comes first and also when no plugin is
  configured: `[NEVR.PLUGIN] plugin trust: mode=<signed-only|unsigned-allowed> source=<default|-require-signed-plugins|command line
  unreadable> build=<release|dev> literal="NEVR-PLUGIN-TRUST <build>"`. A player's log shows whether signatures were enforced.
- **A refusal in a signed-only run is the refusal of section 6**: the log line, the manifest entry, one dialog on a client with
  graphics, `ServerFatal` for a `required` plugin, and nothing shown on a server.

Testing a plugin's signed-only behaviour needs no special build: start any build with `-require-signed-plugins`.

**The literal names the build, not the run.** Every build embeds a NUL-bounded literal, `\0NEVR-PLUGIN-TRUST <build>\0`, with `<build>`
either `release` or `dev` (`kModeArray` in `src/runtime/ext/plugin_trust.cpp`). It is a fact of the file, so it cannot say which mode a
run had; the log line above does. `-DNEVR_ALLOW_UNSIGNED_PLUGINS=ON` (the `mingw-plugin-dev` preset, which inherits `mingw-release` the
way `mingw-scenario` does for `NEVR_SCENARIO_CONTROL`; `mingw-scenario` sets this option too, because the host must load its unsigned `nevr-runtime.dll`) makes the DLL say `dev`, and `tools/package_release.py` finds exactly one such
literal in each packaged `BugSplat64.dll`, requires that it says `release`, and fails the package otherwise. `tools/tests/test_package_release.py` pins that check. The identity literal
(`\0NEVR-BUILD <version> <commit40>\0`) is a cross-repo contract (`tools/package_release.py`, the `binary:` entries of
`release-signer/config/sources.yml`, rule 2.3) and gets no new field. `release-signer` is unchanged by this ADR; a stage there that reads the
same literal is an optional follow-up. The literal is not the login's `nevr_identity` JSON (N112), which this ADR leaves unchanged.

The dev build does not change how a run checks plugins. It is the build that ADR 0012 relaxes other checks for, and the packaging
check keeps it out of a release.

There is no signing route for a developer's local build: the project issues no development certificate.

## 4. WinVerifyTrust under Wine and Proton, and how it is known

Measured with `wine-11.19`, a throwaway prefix, `DISPLAY` and `WAYLAND_DISPLAY` unset, a probe built with the repo's toolchain
(`x86_64-w64-mingw32-g++ 16.2.0`, static) calling `WinVerifyTrust`, `CryptQueryObject`, `CryptMsgGetParam` and, over an exclusive
root store, `CertCreateCertificateChainEngine` and `CertGetCertificateChain`. Files were signed with `osslsigncode 2.14` under a
private root (leaf EKUs `1.3.6.1.5.5.7.3.3` plus a test OID), one with a DigiCert RFC 3161 timestamp. An independent re-run (a fresh
prefix, different probe sources) gave the same results and added the forged-root row.

| Case | `WinVerifyTrust` | Exclusive-root chain `dwErrorStatus` | EKU test |
| --- | --- | --- | --- |
| unsigned file | `0x800b0100` (`TRUST_E_NOSIGNATURE`) | n/a | n/a |
| signed by our test root, test EKU | `0x00000000` | `0x00000000` | present |
| signed by our test root, other EKU | `0x00000000` | `0x00000000` | absent |
| signed by a root not in the store, our EKU | `0x00000000` | `0x00010000` (`CERT_TRUST_IS_PARTIAL_CHAIN`) | present |
| signed with a forged root embedded in the signature, our EKU | `0x00000000` | `0x00000020` (`CERT_TRUST_IS_UNTRUSTED_ROOT`) | present |
| our signed file, one byte of code changed | `0x80096010` (`TRUST_E_BAD_DIGEST`) | n/a | n/a |
| signed with an RFC 3161 timestamp | `0x00000000`; `csCounterSigners` is 0 | `0x00000000` | present |

What follows:
- **Under Wine, `WinVerifyTrust` returning 0 proves only that the digest matches a well-formed signature.** It returns 0 for a
  signature from a root nobody trusts. The identity decision does not rest on it: it serves as the digest check, and the root, EKU,
  status and denylist are decided by the runtime's own chain build.
- Wine does not populate counter-signers in the provider data, so the runtime reads the timestamp itself:
  `CryptQueryObject(CERT_QUERY_OBJECT_FILE, ..., CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, ...)` then
  `CryptMsgGetParam(CMSG_SIGNER_UNAUTH_ATTR_PARAM)` returned attribute `1.3.6.1.4.1.311.3.3.1` (5998 bytes) for the timestamped file
  and "not found" for the untimestamped one. The verifier parses the token (an inner CMS `SignedData` whose content is a `TSTInfo`) itself (`plugin_signature.cpp`).
- Embedded signatures only: `CryptQueryObject` with the embedded-PKCS7 flag succeeding is a precondition. A file with no embedded
  signature is refused before `WinVerifyTrust` runs, so a catalog-signed file is not accepted on native Windows, where
  `WinVerifyTrust` would otherwise accept it through the catalog.
- One code path serves native Windows and Wine, so both take the same decision. The Platform Trust List is not relied on.

Not shown: Proton (no Proton install was run; Proton ships Wine's crypt32 and wintrust, so the Wine result is the expected one, and
the section 5 test runs against it when a prefix is available) and native Windows (the `just test-winvm` rig). The probe sources are
scratch and not committed; the implementation's tests stay.

## 5. Tests

Fixtures are generated at test time by a Python helper in `tools/tests/` (openssl and osslsigncode are in the builder image,
`.github/builder/Dockerfile`): a test root, a leaf with the test EKU, a second leaf with another EKU, a second root, a forged
root embedded in a signature, and a trivial DLL from the repo's toolchain. The verifier takes the policy (root DER, EKU list,
subject, denylist) as an argument, so tests pin the test root and never the Microsoft one; one test pins the Microsoft root constant
by hash.

GTest under Wine, in `src/runtime/tests/test_plugin_signature.cpp`, `test_plugin_tree.cpp`, `test_plugin_trust.cpp` and
`test_behavioral.cpp` (`PluginSignatureGateTest`, `PluginDependencyGateTest`, `PluginRefusalNotice`):

| Test | Expected |
| --- | --- |
| signed by the pinned root and EKU, timestamped | accepted |
| unsigned file | refused, `unsigned` |
| catalog-only or no embedded signature | refused, `no embedded signature` |
| signed by the pinned root, other EKU | refused, `EKU` |
| signed by a root not in the store, same EKU | refused, status `0x10000` |
| forged root embedded in the signature | refused, status `0x20` |
| any other nonzero `dwErrorStatus` | refused (the test injects a chain with a flag set) |
| one byte changed | refused, `digest mismatch` |
| leaf expired, valid timestamp inside the leaf's validity | accepted |
| leaf expired, no timestamp | refused, `signer certificate expired and no timestamp` |
| timestamp after the leaf's `notAfter` | refused |
| valid timestamp token copied from another file's signature | refused, `timestamp not bound to this signature` |
| denied leaf serial | refused, `denylisted serial` |
| signing time inside a denied interval | refused, `denylisted signing window` |
| denied profile EKU | refused, `denylisted profile` |
| plugin imports an unsigned DLL from the plugins directory (import table, delay-load, forwarder: one case each) | refused, names the dependency |
| plugin calls `LoadLibrary` on an unsigned file in the plugins directory after init | refused by the `DllLoadHook` filter |
| restricted-search load rejected with `ERROR_INVALID_PARAMETER` | refused, no fallback load |
| file written while the verify handle is held, from another Wine process | the write fails, or the loaded bytes equal the verified bytes |
| the entry's path is a link, pointed at an unsigned file between the check and the load | the file that was checked loads; the unsigned file is not mapped |
| the held file's path cannot be had | signed-only: refused, `cannot name the file it checked`; otherwise the configured path loads, with a Warning |
| release-configured build, no argument, unsigned plugin (`PluginRunModeTest`) | loaded, with the mode line and the Warning line |
| the same build and plugin with `-require-signed-plugins` | refused, `refused: unsigned`, one dialog on a client, `ServerFatal` for a required plugin on a server |
| the early pass and a later load under each mode | both follow the run's mode; the mode line is written once |
| command line unreadable | signed-only |
| the real signed v5.4.3 `BugSplat64.dll` (`RealSigned.*`, fed by `tools/fetch_real_signed_dll.py` through `just test-auth-unit`) | accepted by the production pins |
| import with no extension, absent import or forwarder not supplied by the system, absent delay-load | the first two refused, the third left to the late-load filter |
| late load by a relative path with a directory part, in a signed-only run | refused whatever it leads to |
| a signed plugin imports a System32 library; an unsigned file of that name is put in the plugins directory after the check (a staged plugin, and a later load) | the plugin loads and the planted file does not (`PluginSystemDependencyTest`) |

Mutants (AGENTS.md "What a PR carries"): drop the root check, drop the status test, compare the EKU by prefix, drop the digest check,
drop the imprint binding, pass `pTime = now`, drop the denylist test, restore the unrestricted fallback, let the 64-file dependency cap fail open
(a 65th dependency is then accepted), compare the late-load path as text instead of its final path (an 8.3 name, a `\\?\` prefix or a trailing dot
or space then loads unchecked); each makes its row fail.

Integration: a game copy with one signed fixture plugin and one unsigned, on the nested display; the run's log must contain the load
line of the first and the refusal line of the second, and the login's `nevr_plugins` must list the second as failed.

Release contract: the packaging check for the `NEVR-PLUGIN-TRUST` literal (section 3), and the legacy targets absent from release
inputs (section 1). `just test-winvm` runs the GTest on native Windows as the second platform. The rule is N147 in the N-rules index in
`AGENTS.md`.

## 6. Plugins people use today, and what a player sees

Every plugin outside this repository (`nevr-runtime-plugins`: `broadcaster-bridge`, `anim-debugger`, `audio-intercom`,
`game-rules-override`, `session-unlocker`, `combat-mod`, `combat-2d`) is built and distributed unsigned today; the one in this
repository (`example`) is not shipped to players. With the default they load. A player or tester who starts with
`-require-signed-plugins` has an unsigned copy of any of them refused, whether it was built, downloaded or received from a friend.

- Each plugin that is to keep working once the default flips is added to `release-signer/config/sources.yml` as a source (repo,
  workflow, start commit, exact file set; POLICY.md rules 1.2 and 3.3), so its release DLLs come out of the same pipeline. That is one
  PR in `release-signer` per plugin repo, not part of #741, and each plugin repo's workflow must produce the build attestation the
  signer demands. Order: signed plugin releases exist before the default flips.
- **Community plugin releases.** A plugin published by a community mirror or index without passing through `release-signer` is
  refused in a signed-only run like any other unsigned DLL. Approved community releases must go through `release-signer` before they are
  published. How that join works is open (open item 5).
- `config.yaml` entries for refused plugins stay. A refused plugin is the same failure as one that cannot load today
  (`FailPluginLoad`): a `required` plugin stops the run, an optional one is skipped.
- #575 moves the plugins directory; the check works on whatever path `StageOne` builds.

**What the player sees beyond the log line.** A log file is not where a player looks. The runtime already has a precedent for a
player notice, the graphics-driver dialog (#553, `src/runtime/log/builtin_filter.cpp`, policy in `gpu_hung_notice.h`): a `MessageBoxA`
on its own thread, once per run, never on a server or a headless run, shown only after the record is written. A refusal notice
follows the same policy (it appears only in a signed-only run, the only run that refuses): after the log line and the manifest entry, once
per run, on a client with graphics only, one dialog that names each refused file, in these words (`plugin_refusal_notice.h`): "<file> wasn't loaded because it isn't signed by nEVR. Plugins
from the launcher are signed; ask the plugin's author for a signed release." It runs on its own thread so boot does not block. A
refusal that happens after `LoadPlugins` has finished (a late load, N147) shows the dialog itself (`RecordLateRefusal`), so none falls
in a gap. The log line, not the
dialog, carries the exact reason (`unsigned`, `other signer`, `denylisted`, or the chain status in hex). On a dedicated server there is no dialog; the refusal line and the manifest entry are the record, and a `required` plugin
ends the run through the existing path.

Release-notes line (`RELEASE-NOTES.md`, which the release pipeline builds):

> Plugins: plugins load whether or not they are signed, and `BugSplat64.dll`'s log says so (`plugin trust:` line). Start the game
> with `-require-signed-plugins` to load only plugins signed through the project's release pipeline: an unsigned plugin, or one
> signed by anyone else, is then not loaded, a message tells you which file, and the log names the file and the reason. Plugin
> developers can use the argument to check how their plugin behaves once signatures are required.

## Consequences

- One more pinned artifact in the binary (the root certificate), a second embedded literal, and a dependency of the runtime's release
  on the profile EKU, which a recreated signing profile would change.
- Plugin authors keep the drop-a-DLL workflow on a release runtime; the argument lets them check the signed-only behaviour.
- A default run does not enforce the gate, so the protection of the threat model (a DLL handed around between players) applies only
  to a run that asks for it, until the default flips.
- A stolen signing profile is the single point of trust, as it already is for `BugSplat64.dll`; the denylist is the response, and it
  moves at the speed of a runtime release.
- The legacy build is no longer part of `dist-all`.

## Open items

1. Revocation beyond the denylist is not checked. The compiled-in denylist is enough for v5.5.0. It is revisited when a signing
   profile is compromised, or when the signer signs releases for more than five repositories (third-party plugins in volume),
   whichever comes first.
2. (closed) The profile EKU is confirmed against the real signed v5.4.3 file, and the subject is deliberately not pinned (section 2).
3. The plugin repos' signing onboarding order (section 6).
4. Whether a client without a desktop (Wine without a display) skips the refusal dialog.
5. Approved community plugin releases pass through `release-signer` before publishing; how that join works is being settled outside
   this repository.
6. Whether `release-signer` also reads the `NEVR-PLUGIN-TRUST` literal (section 3) is a follow-up, not a prerequisite.
7. The change that flips the default to signed-only (once release-signer signs plugins) decides whether `-require-signed-plugins` stays.

## What changed from the plan

The first draft of this record was written before the implementation (#745, #746). Where it differs:

- The signer subject is not pinned; the EKU and the root are the pins. The draft expected the subject from a signing trial.
- The EKU was read from a real signed file (the v5.4.3 `BugSplat64.dll`) instead of waiting for a first signing run; the plugin gate
  merged once it was confirmed, so the "merge order is the control" wording no longer applies.
- The verifier's entry point is `VerifyPluginFile(HANDLE, Policy, now)`, not `VerifyPluginSignature(path, policy)`.
- The chain is built from the cache only (`CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL`).
- The dependency walk gained three rules (extension-less names, an absent import or forwarder refused, an absent delay-load left to
  the late-load filter), and the late-load filter gained the relative-path-with-a-directory-part refusal and the final-path compare.
- The refusal text spells the brand `nEVR`, and a refusal after `LoadPlugins` finished shows the dialog itself.
- The plugin trust mode is a property of the run, from the command line, and the log line states it; the `NEVR-PLUGIN-TRUST` literal
  names the kind of build (`release` or `dev`).
- Code anchors are symbols now, not line numbers of the draft's tree.
