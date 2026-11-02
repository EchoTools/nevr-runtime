# Releasing nEVR runtime

A release exists on GitHub only when all its binaries do. One action stages every binary for a commit, the owner's go
publishes them together, and nothing public exists before that go: no tag, no release, no asset. A failed check deletes the
staging directory, so no half-built release is ever visible. Nothing about a published release is edited afterwards: a wrong
release is fixed by the next version.

**Owner.** The release owner is accountable for a release from staging until it is live and watched, or until it is
withdrawn and cleaned up. The publish (step 4) happens on the owner's go.

**Seats.** A release seat merges its own pull requests, in this repository and in the signer repository, once the pull
request's checks are green and a fresh verifier (neither the author nor the implementer) has passed the tip with every
finding fixed. It deletes the branch and the worktree the same turn.

## The flow

| # | Step | What happens | Recipe |
|---|---|---|---|
| 0 | Preflight | This clone is exactly `origin/main`: no local-only tag, nothing unpushed, no uncommitted change. | `just release-preflight` (exists) |
| 1 | Stage | One build action makes every binary for one commit (the Windows zip, the Quest loader, the Quest APK, and the combat APK when there is one) into a staging directory. Nothing is public. The Quest APK is repacked from the locally built loader, not from a loader downloaded from a published release, so it can be staged before the publish. No recipe does this yet: `just release-quest-apk` downloads the loader from the release asset. | (no recipe yet) |
| 2 | Coming | A webhook message "vX.Y.Z coming" with the contents. The webhook URL is the input `NEVR_RELEASE_WEBHOOK`, an environment variable the recipe reads. It is never a path into a machine's files or `.env`. Prior art, by name only: the echovrce-ops nakama-release runbook, whose webhooks are a hard gate before deploy. | (no recipe yet) |
| 3 | Checks | On the staged set, only: signing, hashes, and the manifest entry. No device or gameplay test runs before the publish: release tests run after the release is cut, signed and published, on the distributed bytes. | (no recipe yet) |
| 3f | Failure branch | A check fails: delete the staging directory, and send a short "not this time" on the same webhook. | (no recipe yet) |
| 4 | Publish | On the owner's go: `gh release create vX.Y.Z --prerelease <files…>` creates the tag, the release and all its assets in one step. A release is published as a pre-release and is never left as a draft; there is no "assets later". Promoting it to stable is a separate act ("Names"). | (no recipe yet) |
| 5 | Manifest | The signed manifest lists the release. | (no recipe yet) |
| 6 | Quest APK | After the release is published: the APK goes to the release host. | `just publish-quest-apk vX.Y.Z` (exists) |
| 7 | Test the release | Starts by proving the bytes: download exactly the files the published, signed manifest names, check each file's sha256 against the manifest, and refuse to test on a mismatch. Then install from the published release through the normal install path (the path a player takes) and run the checks below on the installed bytes. Tests run only after the signed pre-release is posted. | (no recipe yet) |
| 8 | Announce and watch | The announcement goes out, reports are watched, and every transition is logged. | (no recipe yet) |
| 8w | Withdraw | If the release goes bad: withdraw it and clean up. A failure found by step 7 goes to a patch release, not a re-cut of this one. | (no recipe yet; by hand: `gh release delete vX.Y.Z --yes`) |

Steps 1 to 3 touch nothing public. Step 4 is the only point at which a tag or a release is created, and it is never run
before the staged set has passed step 3.

KNOWN-WRONG #1865: the signer ("Sign", below) replaces the assets of a GitHub draft and nothing else, so a release signed after
it exists has to be a draft first. The rule above is the end state: a draft is published as a pre-release or deleted in the same
run, and none is left standing.

**Test the release (step 7), for a combat release:**

- Windows: enter the combat area and confirm it looks right; open the combat terminal, pick a new weapon (one past the last
  four), and confirm its particle effects show.
- Quest: enter the combat area and confirm it looks right; open the terminal, pick a new weapon, fire it, and confirm it looks
  right.
- Other system tests as the owner lists them.

A run whose hashes do not match the manifest is not a release test. A failure found here goes to a patch release (the next
version through the same flow). The published release is not edited.

The recipes for steps 1, 2, 3, 3f, 4, 5, 7 and 7w do not exist yet. Until they do, the release is run by hand, one step at a
time, under the owner's go.

## Names

- **Version:** plain semver, `X.Y.Z` (no leading zeros, `tools/release_asset_names.sh` `release_tag_regex`). It is computed,
  not chosen: `python3 tools/next_release_version.py` prints it from the feat, fix and perf commits (a breaking change counts
  only on those types) since the newest `vX.Y.Z` tag reachable from `origin/main` (`docs/standards/planning.md`, "Release
  versions"). Every other build stamps `X.Y.(Z+1)-dev.<N>+<sha>`, which sorts after the tag it descends from; it is never
  released.
- **Tag:** `vX.Y.Z`, annotated (`git tag -a vX.Y.Z -m "X.Y.Z"`), on `origin/main`. Created by step 4, never before it.
- **GitHub release title:** the tag, `vX.Y.Z`, and nothing else.
- **Pre-release or release:** the flag is the only difference. Promoting a pre-release
  (`gh release edit vX.Y.Z --prerelease=false`) changes no byte and fires no workflow. Nothing in a file depends on the flag.
- **No `-rc` tag.** `release_tag_regex` accepts only `vX.Y.Z`, and a published tag is never reused.

## Assets

Two files for the GitHub release. The list is `release_asset_names` in `tools/release_asset_names.sh`; the guard, the draft
attach, the workflow and `tools/tests/test_release_assets.py` all read or check that one list, so the names below cannot
drift from the build.

| file | audience | holds |
|---|---|---|
| `nevr-runtime-vX.Y.Z-windows.zip` | players on Windows and people running a dedicated server | `BugSplat64.dll` (the host), `nevr-runtime.dll` (the runtime), `echovr_server.exe`, `install-nevr-runtime.exe` (the core installer; all stripped, debug info split off), `install.ps1`, `uninstall.ps1`, `quest-diagnose.ps1`, `README.txt` (the install steps and the dedicated-server steps), `SIGNING.txt`, `SHA256SUMS` (the files inside the zip: `install.ps1` checks `install-nevr-runtime.exe` against it, and the core checks both DLLs against the signed manifest). The zip holds one more copy of the installer under an older name ("The installer's old name"). The zip has no `plugins/` entry: a zip holds no empty directory. |
| `nevr-runtime-vX.Y.Z-quest-libovrplatformloader.so` | Quest packagers | the Quest sentinel library, a loose ELF (attested; not Authenticode) |

There is no top-level `SHA256SUMS` and no `RELEASE-NOTES.md` asset. The **release body** is short: a header, one line on what
is new for players, and one line per file. `python3 tools/package_release.py notes` renders it from
`tools/package-release/RELEASE-BODY.template.md` ("Write the release body"). It holds no hash: the zip's own `SHA256SUMS`,
the signature and the attestation carry integrity.

Not release assets: the `.tar.zst` archives and the `.dbg` symbol files (workflow artifacts, 7 and 90 days), and any Quest APK.

**The signed Windows zip lives on the GitHub release.** Quest APKs are published outside GitHub.

## Sign

Signing is a separate pipeline in `thesprockee/release-signer` (the nEVR Signer GitHub App and the signing service). The build
signs nothing. The signer reads a release's files, signs the PE files and scripts in them, and puts the files back under the
same names. The signer's workflow `sign-release.yml` is dispatched with the tag, and it replaces the assets of a **GitHub
draft** only; it never writes a published release and never flips the pre-release flag. It needs the signer's GitHub App
installed on this repository, and the signing credentials; both are the owner's. It also takes `dry_run=true`, which writes
nothing.

Step 3 (Checks) needs signing of the **staged** set before step 4 publishes it. The current signer does not sign a staged
directory: that is (no recipe yet), and it is the open item for this flow.

Check a signed file as `SIGNING.txt` in the Windows zip says: `Get-AuthenticodeSignature` status `Valid`, or
`osslsigncode verify` with both `-CAfile` and `-TSA-CAfile` set to the Microsoft Identity Verification Root 2020
certificate. The attestation covers the bytes CI built; for a file the signer replaced, the Authenticode signature and the
signer's decision record are the evidence for the new bytes.

Local signing: `just sign` signs a local `dist/` with the private CA in `certs/`, for development builds only. It is never a
release signature.

The checks that the signed zip must pass, which step 3 applies to the staged set:

- `sha256sum -c SHA256SUMS` inside the unzipped signed zip, in a new, empty directory.
- `python3 tools/pe_version_info.py check --dll <BugSplat64.dll> --runtime-dll <nevr-runtime.dll> --exe <echovr_server.exe>`
  exits 0 on the signed files: signing must leave the VERSIONINFO as the build wrote it.
- Each `NEVR-BUILD` literal (one per DLL) is exactly `NEVR-BUILD X.Y.Z <the tag's commit, 40 hex>`
  (`git rev-parse vX.Y.Z^{commit}`), read with `strings -a <dll> | grep -a '^NEVR-BUILD'`.
- The manifest entry for the release: (no recipe yet).

## Attestation

Today the `publish` job in `.github/workflows/build.yml` attests every release asset, and the symbol files, to the commit it
built (Sigstore, keyless). Its permissions are `id-token: write`, `attestations: write` and `contents: write` (the upload);
the job installs nothing and runs no repository code. To check one asset:

```sh
gh attestation verify <file> -R EchoTools/nevr-runtime
```

The attestation covers the unsigned build output CI attested, not a file the signer replaced. For a replaced file the
evidence is the Authenticode check and the signer's decision record.

## Immutable releases

With the repository setting on, once a release is published its tag cannot be moved or deleted and its assets cannot be
changed, added or deleted; the title, the notes and the pre-release flag can still be edited. GitHub then attaches a release
attestation (tag, commit and assets) that `gh release verify` checks. This is why **a bad asset is never swapped**: the fix is
the next version, and the tag name of a published release is never reused.

**The setting is on** (the owner enabled it for this repository; it applies to every release published from now on). To check
or change it, which is the owner's to do, the steps (GitHub Docs, "Preventing changes to your releases") are: repository
**Settings**, then the **Releases** section, then **Enable release immutability**. It applies to releases published after it
is turned on; it does not touch earlier ones.

## Publish

Only from the staged set that has passed step 3. The tag and the publish cannot be undone.

```sh
gh release create vX.Y.Z <staged files…>     # step 4: the tag, the release and all its assets, in one step (no recipe yet)
```

Step 4 is the only creation of a tag or a release. The release body is written from the signed files (see "Write the release
body"), and the release is checked with:

```sh
gh release verify vX.Y.Z -R EchoTools/nevr-runtime                       # needs immutable releases on; GitHub CLI 2.81.0 or newer
gh release verify-asset vX.Y.Z <file> -R EchoTools/nevr-runtime          # a local file against the release's asset
```

The Quest APK is not a release asset. Its host publish, `just publish-quest-apk vX.Y.Z`, puts the APK on the release host, and
the signer refuses it unless the release is already published, so it follows step 4 (step 6). See "The Quest APK" for the
build, which needs the tag on origin before it runs.

## Write the release body

The release body is a header (`# nEVR X.Y.Z`), one line for players, and one line per file in the release. By hand, after the
last signing pass:

```sh
S="$(mktemp -d)" && gh release download vX.Y.Z -D "$S"                # a new, empty directory
N="$(mktemp -d)/RELEASE-BODY.md"                                      # a new file outside $S
python3 tools/package_release.py notes --assets "$S" --out "$N" --base <previous tag> --rev vX.Y.Z
gh release edit vX.Y.Z --notes-file "$N"
gh release view vX.Y.Z --json body --jq .body
```

`notes` refuses unless `$S` holds exactly the two assets. It takes the version and the commit from the `BugSplat64.dll` and
`nevr-runtime.dll` inside the zip (they must agree), so the body cannot describe another build.

The player line is the authors' `release-headlines/` files, or the fixed "Fixes and internal changes." The commits never write
it: a line guessed from `feat` subjects read as a player-facing change twice when the change was behind a setting that is off
or internal (#1444). Read the line against the pull request bodies before publishing. The body of a published release stays
editable ("Immutable releases"); the assets do not.

**Headline files.** A pull request that changes what players see adds `release-headlines/<issue-or-topic>.txt`: one line, 1
to 200 characters, one plain sentence a player would understand about that change only, written by the PR's author, with a
full stop added if it has none (or `!` or `?`). `notes` builds the player line from the files that exist at `--rev` and not at
`--base`, joined with one space in file-name order (at most 600 characters). A path present at the base tag is old by
construction, whatever its content, so no earlier release's sentence rides along. A file with another name (a letter or digit,
then letters, digits, `.`, `_`, `-`, then `.txt`; `release-headlines/README.md` is the one exception), an empty file, a file
with more than one line or a line over 200 characters, or `release-headline.txt` at the rev fails `notes` and names the file;
a joined line over 600 fails it and names the files joined; none falls back silently. A path that already exists at the base
tag is never read, so a new change uses a new file name (a reused name is dropped, not refused). A test
(`tools/tests/test_release_headlines.py`) fails a PR whose tree holds `release-headline.txt`, and `notes` refuses it at the cut
as the backstop. A successful `notes` writes one JSON line to stderr (`"event":"headlines"`, with `used`, `line`, `base` and
`rev`). The PR that adds a fragment also deletes the fragments present at the newest `vX.Y.Z` tag
(`git ls-tree -r --name-only <newest tag> -- release-headlines/`). A fragment left behind is harmless, since a path present at
the base is never read.

## The installer's old name

The core installer is `install-nevr-runtime.exe`; that is the only name this repository's scripts, README and docs use. The
zip also carries `nevr-install.exe`, the same bytes under the name outside callers (the launcher, and any other program that
runs the installer) used before. It is a copy, not a second build: `.github/workflows/build.yml` builds the installer once and
copies it once in the packaging step, `tools/package_release.py` refuses a tree where the two are not the same bytes (the
signer signs each entry separately, so they differ after signing), `tools/verify_symbols.py` checks the copy through the new
name's symbol file, and release-signer lists `install-nevr-runtime.exe` as an optional entry, so the signer must have that
change before a build that carries the new name is signed. The old name is removed in the first release after the launcher and
every other outside caller have confirmed they use the new one. The removal is a PR that deletes the copy in the packaging step,
its gate in `package_release.py` and `verify_symbols.py`, the signer's optional entry and this paragraph.

## The Quest APK

The APK is a repack of the store game, so no workflow builds it. From a clone whose HEAD is the release tag's commit, after
the tag is on origin (`tools/release_preflight.py --tag vX.Y.Z` refuses otherwise):

```sh
just release-quest-apk vX.Y.Z   # build/quest-release/vX.Y.Z/nevr-runtime-vX.Y.Z-quest.apk and its .sha256
```

It compiles nothing: the loader in the APK is the release's own asset, so the asset and the APK carry one file. The recipe
downloads the asset (the draft is enough; `gh` is `$GH_BIN`, default `gh`), checks its size and sha256 against what GitHub
recorded for it and `gh attestation verify`, checks that it carries the tag's version and commit, repacks the store game with
it, and refuses unless the signed APK's loader has the asset's sha256. The APK is signed with the key the maintainer holds
(`tools/quest_keystore.sh`; `just quest-keystore-fingerprint` prints the signer's SHA-256). It uploads nothing. Check the file
(size, `sha256sum`, `unzip -tq`), then publish it from the same clone:

```sh
just publish-quest-apk vX.Y.Z   # puts the APK and its .sha256 on the release host, then fetches both back from the public host
```

The recipe takes the bucket, the public host, the release-signer executable and checkout and a GitHub read token from the
environment and nothing else: `R2_ENDPOINT`, `R2_BUCKET`, `R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY`, `RELEASE_PUBLIC_BASE`,
`RELEASE_SIGNER_BIN`, `RELEASE_SIGNER_DIR`, `GH_TOKEN` and `NEVR_RCLONE_REMOTE`. A missing one is a refusal that names it. The
APK goes up through the signer's `put-quest-apk`, which checks that the release is published, that the APK was built from the
tag's commit and that it carries the pinned signer certificate, and writes the object only if absent. The recipe then fetches
both files back and exits non-zero unless each equals the local one in size and sha256. It logs each step to
`$XDG_STATE_HOME/nevr-runtime/publish-quest-apk.jsonl` and never prints or logs a value from the environment.

Where the files land, and which host answers for them, is in `docs/reference/verifying-a-release.md`, "Quest APKs". The
recipe never overwrites a name. Never request the published address before the file is installed: the CDN caches the 200 HTML
answer for that exact address.

The release notes for a build signed with a new key carry one line for Quest players: uninstall the old version first; this
build is signed with the new key (Android refuses an update from a different signer). `put-quest-apk` pins the signer
certificate in the release-signer's `config/`; a new key needs that pin changed there before the publish.

The variant APK (`nevr-runtime-vX.Y.Z-q15t-quest.apk`) is built by `just release-quest-variant-apk vX.Y.Z <plugins>` from the
same loader asset plus private plugin libraries that stay outside this repository. It is published with
`tools/publish_quest_apk.py vX.Y.Z --variant q15t`, with no signer call and no manifest entry. The combat APK is built by
`just release-quest-combat-apk`. Neither is in the manifest through this document's flow.

## Game-derived packages

A package derived from the game's own files (a content pack, the EchoCombat combat pack included) follows these rules:

- It is hosted only on the CDN and listed in the signed manifest (`docs/reference/verifying-a-release.md`).
- It is never a GitHub release asset and never a workflow artifact of a public repository.
- It is never in the Windows zip. `tools/package_release.py` seal refuses any file outside its list.
- It is built in a private job, never in a public workflow.

Plugins that carry no game bytes may be a workflow artifact.

## Windows runners

Every build runs on `ubuntu-latest`, except two hand-started workflows that use a Windows runner:

- `.github/workflows/echocombat-plugins.yml` builds the three EchoCombat plugins (DroneFix, PowerBoost, VeilNet) with MSVC
  against Microsoft Detours built from source. It holds no secrets and publishes nothing: its output is a workflow artifact and
  build-provenance attestations. Start it by hand: `gh workflow run echocombat-plugins.yml -f echocombat_sha=<40-hex commit>`.
- `.github/workflows/defender-scan.yml` measures the release zip with Microsoft Defender. It signs nothing and is not part of
  the release.

No other workflow may use a Windows runner. `tools/tests/test_echocombat_plugins_workflow.py` checks that set.

## The manifest and the beta channel

Installers read `channels.beta` from the signed manifest. The release-signer's `sign-manifest.yml` publishes the manifest
(`action=publish`, `channel=beta`). Publishing a staged release's manifest entry is step 5 of the flow and has no recipe yet;
until it does, the publish is dispatched by hand with that workflow.

## Emergency: roll beta back

When the release beta points at is bad, `tools/rollback_beta.py` publishes the manifest again with beta on an earlier release
the manifest already lists. It uses the same `sign-manifest.yml` publish (`action=publish`, `channel=beta`, in the
release-signer repository), and `override=true` goes into the dispatch only when the target is lower than beta. A bad
published release is never edited or deleted ("When a published release is wrong"); the rollback only changes which release
new installs get.

**Before you run it.** The target must be the last build known good, and it must list what its players need. The plan the tool
prints names every artifact of the target with its size: read it for the Windows zip, the Quest APK (`kind` `quest-apk`), the
game data and any content pack. The tool refuses before it changes anything unless the claims it hands the publish equal the
target's non-GitHub artifacts by name, and each entry is what the manifest lists. Check too that the fault is in the build: a
fault in the manifest, a service or a player's own files is not cured by moving beta.

**Where, and as whom.** Run it from a clean checkout of nevr-runtime `main`, on the machine that runs the Quest release timer,
because it reads that timer's lock (`timer.lock`, in the timer's work directory) and refuses while a run holds it. The account
`GH_BIN` runs as must be able to dispatch workflows on the release-signer repository and to set repository variables on
`EchoTools/nevr-runtime`. The manifest host's public base is `RELEASE_PUBLIC_BASE`, which the tool requires (the host is named in
`docs/reference/verifying-a-release.md`, "Quest APKs"). The release signer is `RELEASE_SIGNER_BIN` with `RELEASE_SIGNER_DIR`, or
else `QUEST_RELEASE_SIGNER_REPO`, a local clone of the signer repository.

```sh
export RELEASE_PUBLIC_BASE=...   # the manifest host's public base, see docs/reference/verifying-a-release.md
python3 tools/rollback_beta.py vX.Y.Z --dry-run
python3 tools/rollback_beta.py vX.Y.Z --live --yes
$GH_BIN variable set RELEASE_TRAIN --body true -R EchoTools/nevr-runtime
```

Always run the dry run first. The live run pauses the release-cut switch (`RELEASE_TRAIN`, read by the current cut workflow
`.github/workflows/release-train.yml`), dispatches the publish, verifies it, and stops there: it never resumes the switch.
The fourth line resumes it, once the owner has decided which release beta should follow. Stop whatever schedules
`tools/quest_release_timer.py` before a live run, since the tool does not stop it. Each step appends a record to
`$XDG_STATE_HOME/nevr-runtime/rollback-beta.jsonl`; no token, credential or host is printed or logged.

**Ship the fix.** The fix reaches beta through a new release cut through the same flow as any release. The bad release's tag is
never reused.

**False alarm.** If the rollback was a mistake, `python3 tools/rollback_beta.py vX.Y.Z --live --forward --yes` with the release
you rolled back from puts beta back on it. Run the dry run first.

## When a published release is wrong

Do not edit its assets and do not delete its tag. Say so in the notes (editable), cut the next patch version through the same
flow, and promote that one.

## Sources read for this file

- GitHub Docs, "Immutable releases": https://docs.github.com/en/code-security/supply-chain-security/understanding-your-software-supply-chain/immutable-releases
- GitHub Docs, "Preventing changes to your releases": https://docs.github.com/en/code-security/how-tos/secure-your-supply-chain/establish-provenance-and-integrity/prevent-release-changes
- GitHub Docs, "Verifying the integrity of a release": https://docs.github.com/en/code-security/how-tos/secure-your-supply-chain/secure-your-dependencies/verify-release-integrity
- GitHub Docs, "Using artifact attestations to establish provenance for builds": https://docs.github.com/en/actions/how-tos/secure-your-work/use-artifact-attestations/use-artifact-attestations
- GitHub CLI manual, `gh release verify-asset`: https://cli.github.com/manual/gh_release_verify-asset
