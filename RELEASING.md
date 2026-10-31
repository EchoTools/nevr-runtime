# Releasing nEVR runtime

A release is a tag, a draft, two files attached to it, the signer's replacement of those files, the checks on the
result, and one publish. CI does all of it, in `.github/workflows/release-train.yml` (the train) and `build.yml` (the
build). The Quest APK is the one part CI does not do: the maintainer who holds the Quest signing key finishes the release, with a scheduled run of `tools/quest_release_timer.py` ("The Quest
half of the train"). A person does the same steps by hand only when the train is off or a run has gone red. Nothing about a
published release is edited afterwards: a wrong release is fixed by the next version.

A release seat merges its own pull requests, in this repository and in the signer repository, once the pull request's checks are
green and a fresh verifier (neither the author nor the implementer) has passed the tip with every finding fixed. It deletes the
branch and the worktree the same turn. A publish outside the train, and anything that touches a signing key, goes to the owner's seat first.

## Names

- **Version:** plain semver, `X.Y.Z` (no leading zeros, `tools/release_asset_names.sh` `release_tag_regex`). It is
  computed, not chosen: `python3 tools/next_release_version.py` prints it from the feat, fix and perf commits (a breaking
  change counts only on those types) since the newest `vX.Y.Z` tag reachable from `origin/main` (`docs/standards/planning.md`, "Release versions"). Every
  other build stamps `X.Y.(Z+1)-dev.<N>+<sha>`, which sorts after the tag it descends from; it is never released.
- **Tag:** `vX.Y.Z`, annotated (`git tag -a vX.Y.Z -m "X.Y.Z"`), on `origin/main`.
- **GitHub release title:** the tag, `vX.Y.Z`, and nothing else.
- **Pre-release or release:** the flag is the only difference. A release is created as a pre-release; promoting it
  (`gh release edit vX.Y.Z --prerelease=false`) changes no byte and fires no workflow. Nothing in a file depends on the flag.
  There is no `-rc` tag: `release_tag_regex` accepts only `vX.Y.Z`, and a published tag is never reused.

## Assets

Two files. The list is `release_asset_names` in `tools/release_asset_names.sh`; the guard, the draft attach, the
workflow and `tools/tests/test_release_assets.py` all read or check that one list, so the names below cannot drift
from the build.

| file | audience | holds |
|---|---|---|
| `nevr-runtime-vX.Y.Z-windows.zip` | players on Windows and people running a dedicated server | `BugSplat64.dll` (the host), `nevr-runtime.dll` (the runtime), `echovr_server.exe`, `install-nevr-runtime.exe` (the core installer; all stripped, debug info split off), `install.ps1`, `uninstall.ps1`, `quest-diagnose.ps1`, `README.txt` (the install steps and the dedicated-server steps), `SIGNING.txt`, `SHA256SUMS` (the files inside the zip: `install.ps1` checks `install-nevr-runtime.exe` against it, and the core checks both DLLs against the signed manifest). The zip holds one more copy of the installer under an older name ("The installer's old name"). The zip has no `plugins/` entry: a zip holds no empty directory |
| `nevr-runtime-vX.Y.Z-quest-libovrplatformloader.so` | Quest packagers | the Quest sentinel library, a loose ELF (attested; not Authenticode) |

There is no top-level `SHA256SUMS` and no `RELEASE-NOTES.md` asset. The **release body** is short: a header, one line on
what is new for players, and one line per file. `python3 tools/package_release.py notes` renders it from
`tools/package-release/RELEASE-BODY.template.md` ("Write the release body"). It holds no hash: the zip's own `SHA256SUMS`
(which `install.ps1` checks `install-nevr-runtime.exe` against), the signature and the attestation carry integrity.

Not release assets: the `.tar.zst` archives and the `.dbg` symbol files (workflow artifacts, 7 and 90 days), and any Quest APK.

**The signed Windows zip lives on the GitHub release.** Quest APKs are published outside GitHub.

**Quest APKs never go on GitHub.** They are published outside GitHub, on the release host that serves the signed
manifest, by `just publish-quest-apk vX.Y.Z` (see "The Quest APK"). Where a file lands, and which host answers for it, is in
`docs/reference/verifying-a-release.md`, "Quest APKs". Check the file locally (size, `sha256sum`, `unzip -tq`) before it
goes up. The recipe never overwrites a name, so a changed file gets a new name (the version is in it). Never request the
published address before the file is installed: the CDN caches the 200 HTML answer for that exact address; the recipe
fetches the files back only after it has put them there.

## When to release

A release is a train, not an event. The train (`release-train.yml`) runs every three hours and cuts a pre-release when `main` has a
commit that `tools/next_release_version.py` counts (feat, fix or perf, breaking or not) since the newest tag.

The train and its guard share one rule, `tools/release_window.py`: no release is cut or dispatched within 195 minutes of the newest
published release (the newest by `publishedAt` of the non-draft releases whose tag is exactly `vX.Y.Z`). A scheduled run checks it
first and holds, cutting nothing, when that release is 195.0 minutes old or less; a manual dispatch is not held. A run that held or
found nothing to cut published nothing, so it does not start a window.

The check reads the 20 newest releases. A check that finds no published `vX.Y.Z` release among them (an empty list, or only drafts
and other tags) refuses: it cannot tell an empty history from a listing that hides the release, so the scheduled run's step and the
guard both fail with a message that gives the count, and nothing is cut or dispatched.

The guard covers a dropped scheduled train run. It is `tools/release_train_guard.py`, which a scheduler on the publisher's machine runs hourly (any scheduler, such as cron; the repository ships no unit for it). Its inputs: `--repo owner/name` (default `$GITHUB_REPOSITORY`) and the GitHub CLI, which is `--gh`, else `GH_BIN`, else `gh`. A scheduler on a machine that is off delays the guard. When `main` is releasable and no release was published in the last 195 minutes (3 h 15 min), it dispatches the train. It does nothing while a train run is queued, in progress, waiting or pending, when `RELEASE_TRAIN` is not `true`, when nothing on `main` is releasable, while the newest train run failed, was cancelled or timed out 195 minutes ago or less, measured from when that run was created (a red cut can leave a tag and an unpublished draft that the version tool counts, so the guard does not re-dispatch hourly; a run that succeeded or was skipped does not hold it), or while the newest published release is inside the window. Each decision is one JSON line (`reason` is `switch_off`, `no_releasable_change`, `train_run_in_progress`, `recent_failed_run`, `recent_release` or `no_release_in_window`; `last_release`, `last_published`, `age_minutes` and `window_minutes` describe the release, `last_run_id` and `last_run_created` the newest train run: for information, except under `recent_failed_run`, where it is the failed run) on stdout and in `$XDG_STATE_HOME/nevr-runtime/release-train-guard.jsonl` (`~/.local/state/nevr-runtime/` when `XDG_STATE_HOME` is unset); a run whose log cannot be written fails before it decides anything. A run that fails exits non-zero and names the call that failed.
It is not a GitHub schedule: GitHub drops or delays scheduled runs, and a job's token cannot read repository variables (`gh variable list` answers HTTP 403 to it), so the job could not read `RELEASE_TRAIN`.
1. **Switch.** The train does nothing until the repository variable `RELEASE_TRAIN` is `true`. Merging the workflow cuts
   nothing.
2. **No build gate.** The train does not wait for a green `build.yml` run on `main`: nothing runs CI on a push to `main`,
   so `main` never has one. Each PR was built and tested before it merged, and the build of the tag (step 3 of "What CI
   does") is the proof. A red tag build leaves the draft unpublished (rule 4).
3. **Ceiling and small.** A release carries what is on `main` when it is cut. `main` is never frozen for a release.
4. **Forward.** A failure on the way out (a red step, a bad asset, a signer refusal) never holds a release open. The run
   stays red and the draft unpublished; fix forward on `main` and let the next cut be the fix. A tag is never reused.
5. **Stable.** A pre-release is promoted to stable once testers have run it with no new defect filed against it.

## Before the tag

These run by hand once per release, before the train's first step, from a clone of `origin/main`:

1. `git fetch origin` and `just release-preflight` pass: this clone is exactly `origin/main`, no local-only tag, nothing
   unpushed, no uncommitted change.
2. Every PR merged since the last release was tested by its author before it was marked ready. The full suite is not run
   here: the tag's build skips it (#1008), and the full suite and the PC login test run on main after the release cut
   (AGENTS.md, "Verification before merge").
3. `just package-dev` reads to the end. It fails on a Windows or Quest build that does not package. It needs the Quest
   signing keystore the maintainer holds, the store APK and the NDK, as its recipe says.
## Cut the draft by hand

Only when the train is off or its run is red. A draft fires no workflow, so the build is started by hand on the tag ref.

```sh
just release-preflight                                    # 1. refuses unless this clone is exactly origin/main
git tag -a vX.Y.Z -m "X.Y.Z" && git push origin vX.Y.Z   # 2. tag origin's tip with the tag `python3 tools/next_release_version.py` prints (it prints `vX.Y.Z`, with the v: `vX.Y.Z` here is that output, `X.Y.Z` the message), push the tag
just release-preflight && gh release create vX.Y.Z --title vX.Y.Z --draft --prerelease --verify-tag   # 3. the draft
gh workflow run build.yml --ref vX.Y.Z                    # 4. build, attest, attach the two files; it stays a draft
```

`tools/release_draft.sh` decides what step 4 does: a tag with exactly one draft, no published release, and neither of the
two names on the draft is built and attached; a draft that already carries either of the two names fails the run red, so a
second dispatch never overwrites files.

## What CI does

`release-train.yml`, job `cut`, in this order. Each step fails the run red on its first error.

1. **Tag** `origin/main`'s tip as `vX.Y.Z` and push the tag. The tag push starts no workflow.
2. **Draft**: `gh release create vX.Y.Z --draft --prerelease --verify-tag`. A draft fires no workflow.
3. **Build**: `gh workflow run build.yml --ref vX.Y.Z`, watched to its end. `build.yml` builds, attests the two files
   and attaches them to the draft. Its `Verify` step is skipped on a tag build (#1008).
4. **Check the draft**: `tools/release_check_draft.sh vX.Y.Z` runs "Check the draft" below.
5. **Sign**: `gh workflow run sign-release.yml -R thesprockee/release-signer --ref main -f repo=EchoTools/nevr-runtime
   -f tag=vX.Y.Z -f dry_run=false`, watched to its end. The signer replaces the draft's two files. The dispatch needs the
   repository secret `RELEASE_SIGNER_TOKEN`, a token with `actions: write` on `thesprockee/release-signer`; without it
   the step fails red before any dispatch.
6. **Check the signed draft**: `tools/release_check_draft.sh --signed vX.Y.Z` runs "Check the signed draft" below.
7. **Body**: `tools/package_release.py notes --base <previous tag> --rev vX.Y.Z` renders the body from the signed files. Its
   player line is the sentences of the `release-headlines/` files added since the base tag, else "Fixes and internal changes.".
8. **Publish**: `gh release edit vX.Y.Z --draft=false`, last. The `published` event does not rebuild: `build.yml`'s guard
   sees the two names and skips.

A draft that fails its checks is deleted and cut again: a red run leaves an unpublished draft. Delete it
(`gh release delete vX.Y.Z --yes`) and fix forward; the tag stays, and a tag with no published release may be removed by
hand (`git push origin --delete vX.Y.Z`). A second `gh workflow run build.yml` onto a draft that already carries either
of the two names fails red, so a bad draft is never rebuilt in place.

## Check the draft

`tools/release_check_draft.sh` runs these on the draft, before anything is signed or published. They are listed here so a
person can run each one by hand when the train is off.

```sh
gh release view vX.Y.Z --json assets --jq '.assets[].name'          # exactly the two names above
mkdir -p "$S" && gh release download vX.Y.Z -D "$S"                  # a new, empty directory
for f in "$S"/*; do gh attestation verify "$f" -R EchoTools/nevr-runtime; done   # each exits 0
mkdir -p "$S.unzipped" && unzip -q "$S"/nevr-runtime-vX.Y.Z-windows.zip -d "$S.unzipped"   # a new, empty directory
(cd "$S.unzipped" && sha256sum -c SHA256SUMS)                         # every file but SHA256SUMS: OK
(cd "$S.unzipped" && strings -a BugSplat64.dll nevr-runtime.dll | grep -a '^NEVR-BUILD')   # two identical lines
python3 tools/pe_version_info.py check --dll "$S.unzipped/BugSplat64.dll" --runtime-dll "$S.unzipped/nevr-runtime.dll" --exe "$S.unzipped/echovr_server.exe"   # exits 0
```

Each `NEVR-BUILD` line (one per DLL) must be exactly `NEVR-BUILD X.Y.Z <the tag's commit, 40 hex>` (one literal; `git rev-parse vX.Y.Z^{commit}`).
The VERSIONINFO check compares every field of the three files with the pinned values and with the version in the DLL's
`NEVR-BUILD` literal; `python3 tools/pe_version_info.py show <file>` prints them.

## Sign

Signing is a separate pipeline in `thesprockee/release-signer` (the nEVR Signer GitHub App and the signing service). The
build signs nothing. The signer reads the draft's files, signs the PE files and scripts in them, and puts the files back
under the same names. It replaces the assets of the **draft** only; it never writes a published release and never flips
the pre-release flag. It needs the signer's GitHub App installed on this repository, and the signing credentials; both
are the owner's. The signer also takes `dry_run=true` (it writes nothing to the draft).

By hand:

```sh
gh workflow run sign-release.yml -R thesprockee/release-signer --ref main \
  -f repo=EchoTools/nevr-runtime -f tag=vX.Y.Z -f dry_run=false    # signs, verifies, replaces the DRAFT's assets
```

Check a signed file as `SIGNING.txt` in the Windows zip says: `Get-AuthenticodeSignature` status `Valid`, or
`osslsigncode verify` with both `-CAfile` and `-TSA-CAfile` set to the Microsoft Identity Verification Root 2020
certificate. The CI attestation covers the bytes CI built; for a file the signer replaced, the Authenticode signature and
the signer's decision record are the evidence for the new bytes.

## Check the signed draft

After the signer has replaced the assets, `tools/release_check_draft.sh --signed` runs these, read this way:

- The signer replaces the zip. The `SHA256SUMS` inside it lists the signed files, so the check unzips the signed zip into
  a new directory and runs `sha256sum -c SHA256SUMS` there.
- `gh attestation verify` covers the unsigned build output CI attested, not the bytes the signer replaced. The loop under
  "Check the draft" exits non-zero for the signed zip (`HTTP 404: Not Found` from the attestations API) and still exits 0
  for the Quest library, which the signer does not replace; that failure is the signer's work, not a bad draft. For a
  replaced file the evidence is the Authenticode check above and the signer's decision record. The signed mode does not
  run that loop.
- The same two names must still be on the draft.
- `python3 tools/pe_version_info.py check` runs on the signed files too: signing must leave the VERSIONINFO as the build wrote it.

## Write the release body

The release body is a header (`# nEVR X.Y.Z`), one line for players, and one line per file in the release. CI writes it
(step 7 above). By hand, after the last signing pass:

```sh
S="$(mktemp -d)" && gh release download vX.Y.Z -D "$S"                # a new, empty directory
N="$(mktemp -d)/RELEASE-BODY.md"                                      # a new file outside $S
python3 tools/package_release.py notes --assets "$S" --out "$N" --base <previous tag> --rev vX.Y.Z
gh release edit vX.Y.Z --notes-file "$N"
gh release view vX.Y.Z --json body --jq .body
```

`notes` refuses unless `$S` holds exactly the two assets. It takes the version and the commit from the `BugSplat64.dll`
and `nevr-runtime.dll` inside the zip (they must agree), so the body cannot describe another build.

The player line is the authors' `release-headlines/` files, or the fixed "Fixes and internal changes." The commits never write
it: a line guessed from `feat` subjects read as a player-facing change twice when the change was behind a setting that is off
or internal (#1444). Read the line against the pull request bodies before publishing. The body of a published release stays
editable ("Immutable releases"); the assets do not.

**Headline files.** This is the news-fragment pattern of towncrier's `newsfragments/` and changesets' `.changeset/`. A pull
request that changes what players see adds `release-headlines/<issue-or-topic>.txt` in the same PR: one line, 1 to 200
characters, one plain sentence a player would understand about that change only, written by the PR's author, with a full stop
added if it has none (or `!` or `?`). `notes` builds the player line from the files that exist at `--rev` and not at
`--base`, joined with one space in file-name order (at most 600 characters). A path present at the base tag is old by
construction, whatever its content, so no earlier release's sentence rides along. A file with another name (a letter or
digit, then letters, digits, `.`, `_`, `-`, then `.txt`; `release-headlines/README.md` is the one exception), an empty file,
a file with more than one line or a line over 200 characters, or `release-headline.txt` at the rev fails `notes` and names
the file; a joined line over 600 fails it and names the files joined; none falls back silently. A path that already exists at
the base tag is never read, so a new change uses a new file name (a reused name is dropped, not refused). A test
(`tools/tests/test_release_headlines.py`) fails a PR whose tree holds `release-headline.txt`, so it is caught before the cut,
and `notes` refuses it at the cut as the backstop. A successful `notes` writes one JSON line to stderr
(`"event":"headlines"`, with `used`, `line`, `base` and `rev`; `used` is empty when the fixed sentence stands), which the
release-train run's log keeps. The train cannot push to `main`, so it deletes nothing: the PR that adds a fragment also
deletes the fragments present at the newest `vX.Y.Z` tag (`git ls-tree -r --name-only <newest tag> -- release-headlines/`). A
fragment left behind is harmless, since a path present at the base is never read.

## The installer's old name

The core installer is `install-nevr-runtime.exe`; that is the only name this repository's scripts, README and docs use. The
zip also carries `nevr-install.exe`, the same bytes under the name outside callers (the launcher, and any other program that
runs the installer) used before. It is a copy, not a second build: `.github/workflows/build.yml` builds the installer once
and copies it once in the packaging step, `tools/package_release.py` refuses a tree where the two are not the same bytes (the
signer signs each entry separately, so they differ after signing), `tools/verify_symbols.py` checks the copy through the new
name's symbol file, and release-signer lists `install-nevr-runtime.exe` as an optional entry, so the signer must have that
change before a build that carries the new name is signed. The old name is removed in the first release after the launcher
and every other outside caller have confirmed they use the new one. The removal is a PR that deletes the copy in the
packaging step, its gate in `package_release.py` and `verify_symbols.py`, the signer's optional entry and this paragraph.

## Publish

Only from the checked draft. The tag and the publish cannot be undone.

```sh
gh release edit vX.Y.Z --draft=false          # publish; the pre-release flag is untouched
```

Publishing fires the `published` event; `tools/release_already_built.sh` sees the two names and the workflow does
nothing. Then:

```sh
gh release verify vX.Y.Z -R EchoTools/nevr-runtime                       # needs immutable releases on; GitHub CLI 2.81.0 or newer
gh release verify-asset vX.Y.Z <file> -R EchoTools/nevr-runtime          # a local file against the release's asset
```

## The Quest APK

The APK is a repack of the store game, so no workflow builds it. From a clone whose HEAD is the release tag's commit,
after the tag is pushed (the commit need not be `origin/main`'s tip, so an older release can be built after main has moved
on; the recipe runs `tools/release_preflight.py --tag vX.Y.Z`, which wants a clean tree and the tag on origin at that commit):

```sh
just release-quest-apk vX.Y.Z   # build/quest-release/vX.Y.Z/nevr-runtime-vX.Y.Z-quest.apk and its .sha256
```

It compiles nothing: the loader in the APK is the release's own asset, so the asset and the APK carry one file. The recipe
downloads the asset (the draft is enough; `gh` is `$GH_BIN`, default `gh`), checks its size and sha256 against what GitHub
recorded for it and `gh attestation verify`, checks that it carries the tag's version and commit, repacks the store game
with it, and refuses unless the signed APK's loader has the asset's sha256. The APK is signed with the key the maintainer holds
(`tools/quest_keystore.sh`; `just quest-keystore-fingerprint` prints the signer's SHA-256). It uploads nothing. Check the file as above, then publish it from the same clone:

```sh
just publish-quest-apk vX.Y.Z   # puts the APK and its .sha256 on R2, then fetches both back from the public host
```

The recipe takes the bucket, the public host, the release-signer executable and checkout and a GitHub read token from the
environment and nothing else: `R2_ENDPOINT`, `R2_BUCKET`, `R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY`,
`RELEASE_PUBLIC_BASE`, `RELEASE_SIGNER_BIN`, `RELEASE_SIGNER_DIR`, `GH_TOKEN` and `NEVR_RCLONE_REMOTE` (the rclone remote that names the bucket). A missing one is a refusal that names
it. The APK goes up through the signer's `put-quest-apk`, run from `RELEASE_SIGNER_DIR` (the signer reads `config/` from
its working directory), which checks that the release is published, that the APK was built from the tag's commit and
that it carries the pinned signer certificate, and writes the object only if absent; the sidecar goes up with
`rclone copyto --ignore-existing`. The recipe then fetches both files from `RELEASE_PUBLIC_BASE` and exits non-zero unless
each equals the local one in size and sha256; an HTML answer or a redirect is a failure. It logs each step to
`$XDG_STATE_HOME/nevr-runtime/publish-quest-apk.jsonl` and never prints or logs a value from the environment. No workflow
runs it: the APK is built from the store game on the publisher's machine, so there is nothing for a job to upload.
Where the files land and which host answers for them is in `docs/reference/verifying-a-release.md`, "Quest APKs".

The release notes for a build signed with a new key carry one line for Quest players: uninstall the old version first; this
build is signed with the new key (Android refuses an update from a different signer). `put-quest-apk` pins the signer
certificate in the release-signer's `config/`; a new key needs that pin changed there before the publish.

The release body already has a line for the APK that names the file and says it is published on the release host, so it
needs no edit. The APK's SHA-256 is the `.sha256` file beside it on the host (`docs/reference/verifying-a-release.md`, "Quest APKs").

## The Quest half of the train

The Quest APK is built from the store game and signed with the Quest signing key, and the R2 credential is the publisher's, so no
workflow can build or upload it. The train publishes the GitHub release; a scheduler on the publisher's machine runs
`tools/quest_release_timer.py --probe` first (the repository ships no unit for it). The probe makes one GitHub call and starts the run only when there is work: a
draft with its loader asset uploaded, a published tag that is not done with fewer than three counted failures and none in the
last 30 minutes, or no run started in the last 30 minutes. A run for a draft builds the APK while the signer runs
(`build_apk`), polls the release every 10 seconds for up to 45 minutes (`await_published`), and on publication checks the
APK's loader against the published asset (`verify_loader`) and that beta has not moved (`recheck_beta`), then runs
`publish_apk` and the unchanged beta dispatch. Nothing is uploaded to R2 while the release is a draft, because the signer's
`put-quest-apk` accepts only a published release. A draft that is deleted, or not published in 45 minutes, is recorded as
`draft_abandoned` and left to the published path; its failures before its publication are not counted. A scheduled run runs
`tools/quest_release_timer.py --stage-draft`, which takes a qualifying draft or else the newest published tag.

The timer takes the **newest tag only**: the newest draft that qualifies (above), or else the newest published tag. A
release that is not the newest when the timer first sees it is never built, because a later tag supersedes it. That is
intended. A tag is built by hand (`--tag vX.Y.Z`) when it must be.

It runs from a clone of the repository that the publisher keeps for it, named by `QUEST_RELEASE_REPO` (default: the clone the script sits in). It fetches `origin/main` itself, and the `checkout` step refuses, as a `refused` record, unless the clone is clean and HEAD is the freshly fetched `origin/main`. The `tags` step then refuses, the same way, while any tag in that clone is not on origin or points at another commit there (the rule of `tools/release_preflight.py`, which `just release-quest-apk` runs later); the refusal names up to ten of them with their short ids, counts the rest, and prints the shell-quoted remove and restore commands, with the full object id, for each tag it lists.

1. Takes the newest tag (above). A tag that has a `done` record in the log is left alone. A tag that has
   failed three times is not retried: the run writes `skipped_after_failures`, with the step and the part of the last failure, and exits 0.
   `python3 tools/quest_release_timer.py --tag vX.Y.Z --reset-failures` (the tag is required) records `failures_reset` for that
   tag and does nothing else; only the failures after the newest reset count toward the three. Use it after fixing what made a tag fail three times (a stale pin, say), then run the tag again.
   A dirty or stale checkout, a local tag origin does not have, or a missing credential, is a refusal that is logged and never counts toward the three failures.
2. Builds the release-signer from a snapshot of its `main` (`QUEST_RELEASE_SIGNER_REPO`, a local clone of the signer repository), and reads `verify-manifest --live`. A tag that beta already
   lists needs no release build: the run goes on to the variant (below). Otherwise the beta release must list a `game-data` artifact (the zip the new release carries), whose
   name and size are noted; without one the run fails before it builds.
3. Makes a clean worktree at the tag and waits while two or more `ninja` processes run. Then, in `nevr-builds.slice` (8G), the
   `configure` step runs `just configure` (the vcpkg install that the new worktree lacks), and `just release-quest-apk` runs
   after it.
4. Runs `tools/publish_quest_apk.py` with `--extra-out`: the APK and its sidecar go to R2 and are fetched back from the
   public host, and the signer's claim is kept for the manifest.
5. Dispatches `sign-manifest.yml` in the release-signer repository with `action=publish`, `channel=beta`, that claim as
   `extra_artifacts` and `dry_run=false` (a live publish with no dry run, on the owner's standing word), and watches it.
   A run is accepted only when the jobs that publish (`Commit the manifest record`, `Publish to R2`, `A run that
   published nothing is red`) all concluded `success`; a green run with a skipped one is a failure.
6. Requires `verify-manifest --live` to list the tag on beta, and requires the manifest the public host serves (held to the
   sha256 the signer verified) to list, in the new release, the same `game-data` artifact by name and size. A missing or
   changed entry is a failure, never `done`.
7. Builds and publishes the variant APK, when it is configured (below).
8. Removes the worktree, the signer snapshot and the variant scratch by exact path.

Stable stays an owner step (`approve-stable`); the timer only publishes beta. To stop the timer and the train from moving beta forward behind a rollback, see "Emergency: roll beta back". **Credentials go only where they are used:**
the R2 credential to `publish_apk` and `variant_publish`, and the GitHub token to `publish_apk` and the dispatch. Every other command, the
build included, gets an environment without them. Every step appends a JSON line to
`$XDG_STATE_HOME/nevr-runtime/quest-release-timer.jsonl`, with each credential value replaced by `<redacted>`; a log that
cannot be written fails the run. A failing step exits 1 naming itself, with the redacted tail of the failing command's stderr and stdout (the stdout of `curl`, `gh` and the signer is remote content and is left out), the inputs come from the environment of the run: the R2 credential (`R2_ENDPOINT`, `R2_BUCKET`, `R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY`), `RELEASE_PUBLIC_BASE`, and `QUEST_RELEASE_SIGNER_REPO`, a local clone of the release-signer repository (`build_signer` builds from its `main`; there is no default). A missing one is a `refused` record that names it, and the run exits 1. The R2 credential is never written into the log or into a command.

**The variant APK.** Each release also gets an unlisted second APK, `nevr-runtime-vX.Y.Z-q15t-quest.apk` with its `.sha256`,
published under the same `nevr-runtime/vX.Y.Z/` prefix and never listed in the manifest. It is the same repack as the main APK
(step 3) from the release's own loader asset, signed with the same key, plus two plugin libraries (`libq15t.so` and `libq15t_early.so`) from a
private build that stays outside this repository, so the two APKs of one version can always be compared. The timer builds it
in the same run as the main APK, after the main release is complete (published, listed and verified live):

- `variant_pins` copies each library into a private scratch directory (mode 0700, files 0600), hashes the copy, holds it to
  its pin and packages the copies, so a file that changes afterwards is not what is built;
- `variant_build` runs `just release-quest-variant-apk vX.Y.Z <libq15t.so>:<libq15t_early.so>` in the same worktree, after
  the same configure, in the same slice as the main build (it writes `build/quest-release/vX.Y.Z/variant/`). Its whole output
  goes to `variant-vX.Y.Z-build.log` in the timer's work directory, a file that is 0600 from the moment it is opened, kept when
  the step fails and removed when the variant is done;
- `variant_publish` runs `tools/publish_quest_apk.py vX.Y.Z --variant q15t`: `rclone copyto --ignore-existing` for the APK and
  its `.sha256`, then the same fetch-back by size and sha256 from the public host. There is no signer call and no manifest
  claim, an object already on the host is never replaced, and only the R2 credential is in its environment.

It is optional. The run reads, from its environment, `NEVR_VARIANT_DIR` (the directory holding the two libraries) and `NEVR_VARIANT_PINS` (a `sha256sum`-format file
with exactly one `<64 hex>  <name>` line per library); neither is in this repository. Neither value is logged or given to
a command, and each is replaced by `<redacted>` in anything written; the records name the two files, their sizes and their
pinned sha256 only. With both unset the run logs `variant_not_configured` once and does nothing else. Exactly one set, a pins
file with a missing, extra, duplicate or malformed line, a missing library, or a sha256 that differs from its pin is a failure
that names the file, never a skip.

A failing variant step never blocks or undoes the main release, which is complete by then. It is recorded as `failed` with the
step (`variant_pins`, `variant_build`, `variant_publish`, or `variant_exists` and the shared `worktree`, `build_slot` and
`configure` when the tag was already listed), the run exits 1, and it counts toward the same three
tries. A failure of a variant step carries the step, the program and its exit code, never a tail of the program's output (the
plugin check prints the plugin's own symbol names): the output of `variant_build` is in the 0600 file the failure names, and
the publish tool's own log (`publish-quest-apk.jsonl`) has the reasons of `variant_publish`. Every `failed` and `refused`
record carries `part`: `main` until the main release is complete (published, listed and verified live), `variant` after it. The timer writes `main_complete` for the tag, once, at that point, and a later run whose tag has it starts as `variant`, so a failure of `build_signer` or `check_listed` on a variant-only retry is the variant's. In the already-listed path `main_complete` is written at once; a tag whose history has no `main_complete` (older records) is `main` until `check_listed` says it is listed, as before. The `skipped_after_failures` record names the step and the part of the last failure, so it says whether the variant or the main release gave up.
`done` is written only when the variant is done, so the next run finds the tag listed and checks the host for the variant pair
(`variant_exists`, below). An exception that no step expected (a broken answer from the host is one of the expected ones) is
recorded as `failed` with the step `unexpected`, naming its class and where it happened and never its text, with `part` as above;
the worktree and the scratch are removed, the run exits 1, and it counts toward the three tries like any other failure.

The private copies of the libraries are in `variant-vX.Y.Z` in the timer's work directory, which carries the marker file
`.made-by-quest-release-timer` from the moment the timer makes it. Every run that reaches the release list (or has `--tag`), the
already-listed path and a failing run included, removes a directory of that name that carries the marker (a killed run's leftovers): the files first, then the marker, then the directory, so
a sweep that fails part way leaves a directory that is still marked and the next run retries it. It records
`variant_scratch_removed`; a directory without the marker, or a link, is never removed, and the pins step then fails because it
cannot make its own.

`variant_exists` GETs the APK, following no redirect. 404 means it is absent: the variant is built and published alone, in a
worktree of its own that the run removes, with no second manifest publish. An APK that is there (200, not HTML) is listed
only when its `.sha256` is there too, is the one line `<sha256>  <variant name>`, and states the sha256 of the APK as the host
serves it (hashed as it streams in); that logs `variant_listed`, builds nothing and finishes the tag. Any other answer for
either file is a failure.

**One run at a time.** A run that does work takes a non-blocking lock on `timer.lock` in the timer's work directory and holds it
until it ends. A run that finds it held (a hand `--tag` run while the timer's is working, say) records `busy`, prints one line
to stderr and exits 0 having done nothing, so it can neither sweep a live run's scratch nor publish twice. `--reset-failures` does no work and takes no lock.

**A completed main release is never run again.** A tag whose history has `main_complete` takes the variant-only path
whatever beta lists: no signer is built, and `check_listed` and the dispatch are not run; the run records
`variant_only` and goes straight to the variant pair check. The variant does not depend on beta, so a rollback, a hand
publish or a hand `--tag` of an older release cannot make the timer publish the main release to beta again. A tag without
`main_complete` and a beta that lists another release takes the full path, as before.

**A half-published or mismatched pair.** When the APK is there and the sidecar is missing, malformed, names another file or
states another digest, `variant_exists` fails naming the two files and which check failed, and it rebuilds nothing. It
says so only when the APK's read was provably complete (its declared `Content-Length` equals the bytes received, and the host
sent them unencoded) and the sidecar's too; a digest that disagrees with a body that ends with the connection, arrives
chunked or encoded, or has no declared length is `could not confirm the answer was complete`, without the pointer to this
repair, and the next run asks again (the request asks for `Accept-Encoding: identity`). The pair is not rebuilt: the
publisher's upload never replaces an object (`rclone copyto --ignore-existing`), so a rebuild could not repair it, and a
different APK at that name is not the timer's to overwrite. A body that came in short of its `Content-Length`, a connection
that broke off or an answer that is not HTTP is not a mismatch: it fails as `the answer broke off` or `no answer`, without the
pointer to this repair, and the next run asks again. The repair is by hand with the publisher's credential (the R2 credential in the run's environment), outside the timer, and it needs a digest that comes from the build that was published, never from the
host: **never copy the digest from the error message into the sidecar**, because that would certify whatever is on the host.
Take the digest from the `local_files` record of the run that built and put up the pair (in `publish-quest-apk.jsonl`; the
run's other records are `start`, `put_variant_apk`, `put_sidecar`, `fetch` and `published`). It gives the sha256 and size of
the build that run published, is written before the upload and is local to this machine; the build's own directory is gone,
because the timer always removes its worktree when a run ends. After confirming that the object on the host has that size,
replace the sidecar with the single line
`<that sha256>  nevr-runtime-vX.Y.Z-q15t-quest.apk`, as `sha256sum` writes it. If there is no such record, or the size
differs, delete BOTH objects from the release prefix instead. Either way, run
`python3 tools/quest_release_timer.py --tag vX.Y.Z --reset-failures` and let the next run check the pair again (an absent APK
is built and published as above).

Run one tag by hand, from a clone of the repository, with the environment above set:

```sh
python3 tools/quest_release_timer.py --tag vX.Y.Z
```

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
  against Microsoft Detours built from source. It needs Windows because the plugins use MSVC structured exception handling
  (`__try`/`__except`) and Detours. It holds no secrets and publishes nothing: its output is a workflow artifact and
  build-provenance attestations. Start it by hand: `gh workflow run echocombat-plugins.yml -f echocombat_sha=<40-hex commit>`.
- `.github/workflows/defender-scan.yml` measures the release zip with Microsoft Defender. It signs nothing and is not part of
  the release.

No other workflow may use a Windows runner. `tools/tests/test_echocombat_plugins_workflow.py` checks that set.

## Immutable releases

With the repository setting on, once a release is published its tag cannot be moved or deleted and its assets cannot be
changed, added or deleted; the title, the notes and the pre-release flag can still be edited. GitHub then attaches a
release attestation (tag, commit and assets) that `gh release verify` checks. This is why a release is built and checked as
a draft, and why **a bad asset is never swapped**: the fix is the next version, and the tag name of a published release
is never reused.

**The setting is on** (the owner enabled it for this repository; it applies to every release published from now on). To
check or change it, which is the owner's to do, the steps (GitHub Docs, "Preventing changes to your releases") are:
repository **Settings**, then the **Releases** section, then **Enable release immutability**. It applies to releases
published after it is turned on; it does not touch earlier ones.

## Attestation

The `publish` job in `.github/workflows/build.yml` attests every release asset, and the symbol files, to the commit it
built (Sigstore, keyless). Its permissions are `id-token: write`, `attestations: write` and `contents: write` (the upload);
the job installs nothing and runs no repository code. To check one asset:

```sh
gh attestation verify <file> -R EchoTools/nevr-runtime
```

## When a published release is wrong

Do not edit its assets and do not delete its tag. Say so in the notes (editable), cut the next patch version through the
same steps, and promote that one.

## Emergency: roll beta back

Installers read `channels.beta` from the signed manifest. When the release beta points at is bad, `tools/rollback_beta.py`
publishes the manifest again with beta on an earlier release the manifest already lists. It uses the same `sign-manifest.yml`
publish as the Quest release timer (`action=publish`, `channel=beta`, in the release-signer repository), and `override=true`
goes into the dispatch only when the target is lower than beta (the signer refuses a channel that moves backwards without it,
and records the override). A bad published release is never edited or deleted ("When a published release is wrong"); the
rollback only changes which release new installs get. The tool cannot target a tag the manifest does not list.

**Before you run it.** The target must be the last build known good, and it must list what its players need. The plan the
tool prints names every artifact of the target with its size: read it for the Windows zip, the Quest APK (`kind`
`quest-apk`), the game data and any content pack. The publish is handed, as `extra_artifacts`, exactly the target's artifacts
that GitHub does not hold (every kind but `windows-zip` and `quest-library`, from `release-signer listed-claims`), and the
tool refuses before it changes anything unless that list equals the target's by name, no more and no fewer, and each entry
is what the manifest lists. A target that lists no Quest APK or no game data is not a rollback target. Check too that the
fault is in the build: a fault in the manifest, a service or a player's own files is not cured by moving beta.

**Where, and as whom.** Run it from a clean checkout of nevr-runtime `main`, on the machine that runs the Quest release timer, because it reads that timer's lock (`timer.lock`, in the timer's work directory) and refuses while a run holds it. The account `GH_BIN` runs as must be able to dispatch workflows on the release-signer repository and to set repository variables on `EchoTools/nevr-runtime`. When `GH_BIN` is unset the tool uses `gh`, which must be signed in as that account. Export `GH_BIN` before you start: the resume commands below use it, and the tool prints them with the gh it ran with. The manifest host's public base is `RELEASE_PUBLIC_BASE`, which the tool requires (the host is named in `docs/reference/verifying-a-release.md`, "Quest APKs"); the tool refuses loudly if it is empty. The release signer is `RELEASE_SIGNER_BIN` with `RELEASE_SIGNER_DIR`, or else `QUEST_RELEASE_SIGNER_REPO`, a local clone of the signer repository (no default); the tool refuses when none of these is set.

```sh
export RELEASE_PUBLIC_BASE=...   # the manifest host's public base, see docs/reference/verifying-a-release.md
python3 tools/rollback_beta.py vX.Y.Z --dry-run
python3 tools/rollback_beta.py vX.Y.Z --live --yes
$GH_BIN variable set RELEASE_TRAIN --body true -R EchoTools/nevr-runtime
```

The first line sets the manifest host's base. The second prints the plan, what a live run would meet and a dry-run publish. The
third pauses the train, publishes and verifies. The fourth resumes the train (see "Resume the train").

Always run the dry run first. It reads the live manifest, refuses what a live run would refuse, and then, read-only, reads
`RELEASE_TRAIN`, the timer's lock and the signer's runs and prints plan lines saying whether a live run would be
refused now. It dispatches the publish with `dry_run=true`, checks that every job that should have run did (the record job is
skipped in a dry run) and that beta did not move, then downloads the run's `manifest-signed` artifact and compares it with the
live manifest: beta must be the target, the set of releases, the other channels and every other release entry must be
unchanged, and the target's artifacts must equal the live ones by name, kind, sha256 and size. A difference in the target's
notes, or in a field of its entry other than `notes` and `published`, is printed as a note and does not fail the run. The
workflow still copies the release's files to R2 in a dry run; it never overwrites an object.

The tool reads `GH_BIN` for every GitHub call and `RELEASE_PUBLIC_BASE` for the manifest host, and needs no R2 credential. The
release signer is `RELEASE_SIGNER_BIN` with `RELEASE_SIGNER_DIR`, or, when neither is set, a snapshot of the signer's `main`
built the way `tools/quest_release_timer.py` builds it (`QUEST_RELEASE_SIGNER_REPO`, required, no default) and removed
when the run ends. Each step appends a record to `$XDG_STATE_HOME/nevr-runtime/rollback-beta.jsonl`; no token, credential or
host is printed or logged. Every command has a timeout. A run that fails, is interrupted (Ctrl-C) or crashes after it began
to pause or dispatch prints what is paused, what was dispatched (with the run id when known; a call that left no run id says a
publish MAY have been dispatched, and then you list the signer's runs before dispatching again) and the resume commands, and
exits non-zero. A rollback that was verified but whose scratch folder could not be removed says so and exits 0: the rollback is
done and verified, and a second run would only refuse with "already beta".

**What it refuses.** A tag the live manifest does not list; the tag that already is beta; a tag higher than beta unless
`--forward` is given; `--forward` for a tag that is not higher; `--live` without `--yes`; claims that are not the target's
non-GitHub artifacts. The order of a live run is fixed:

1. Read `verify-manifest --live` and the manifest the host serves (held to the sha256 the signer verified), take the
   target's claims, print the plan.
2. Pause the train, unless `--keep-train`: set the repository variable `RELEASE_TRAIN` to `false` and read it back. A pause step that fails stops the run before the
   dispatch and says what is and is not done.
3. Check that no Quest release run holds the timer's lock (`timer.lock`, which a run of `tools/quest_release_timer.py` holds while it does work); the tool refuses when one does. Then
   check that no `sign-manifest.yml` run on the signer repository is queued or in progress (any status but `completed`), and
   refuse if one is. Both checks run under `--keep-train` too, so a Quest release could start
   in the seconds between these checks and the dispatch. A rollback that is refused here changes nothing but the pauses it
   already made; wait for the run to end and run it again.
4. Check again, after the pause, that beta has not moved and the claims still match the manifest.
5. Dispatch the live publish, watch it, and require the jobs that publish (`Commit the manifest record`, `Publish to R2`,
   `A run that published nothing is red`) to have concluded `success`.
6. Require `verify-manifest --live` to list the target as beta, with every artifact the target listed still listed by name and
   sha256.
7. Print the elapsed time and what to do next. **The tool never resumes the train.** It stays paused until someone runs the
   resume commands, so decide that in the open.

A train run that is already in progress when the variable is set still finishes its cut: the variable is read when the run
starts. Look at the train's run list if one is under way.

**How long.** The manifest run takes about 5 to 6 minutes: the three most recent live publishes of `sign-manifest.yml`
took 5 min 9 s, 5 min 30 s and 5 min 15 s from the start to the end of the run. The pause steps add seconds. Expect about 6
to 7 minutes from `--live` to the verified line. The tool stops watching after 1800 s, and after 7200 s when the target lists
PC game data (`game-data` for `windows-x86_64`), because the check job then downloads about 5 GB, which the workflow allows
about 83 minutes for. A dry run dispatches the same workflow and waits for it as well.

**Why both pauses.** The release train (`.github/workflows/release-train.yml`) cuts a release only while the repository
variable `RELEASE_TRAIN` is `true` (the `if` of its `decide` job), and `tools/release_train_guard.py` reports `switch_off`
and dispatches nothing while it is not. The Quest release timer is a separate scheduled job: it takes the newest published
GitHub release and, if the manifest's beta is not that release and its log has no `done` for the tag, rebuilds and
publishes it to beta, which moves beta forward again behind the rollback. For a tag that has a `done` record it logs
`nothing_to_do`, but the next release cut would move beta forward again, and only the variable stops cuts. So the variable stops new cuts, and this tool does not stop the timer: stop whatever schedules `tools/quest_release_timer.py` yourself before a live run, or a later run publishes the newest tag to beta. Neither covers the other.

**What it does not do.** New installs and updates stop receiving the build; installs already on it are not downgraded. The
manifest consumer rule keeps an installed version that is higher than the channel's unless the user switched channel or
asked to go back (the release-signer repository's manifest design document, section 6.1 step 7; the same rule is in its table
of threats, "Rollback"). Players on the bad build get the fix by the next version, cut through the same steps as any release.

**Ship the fix.** The fix reaches beta through the train, not through this tool. After a rollback the train is paused: resume
it (the two commands above, which the tool prints). The guard then cuts the fixed release when `main` has a releasable change,
and the Quest release timer publishes it to beta as a forward move, which needs no override. Before that, the fix has to be
on `main`; the bad release's tag is never reused.

**False alarm.** If the rollback was a mistake, `python3 tools/rollback_beta.py vX.Y.Z --live --forward --yes` with the
release you rolled back from puts beta back on it. The tool prints that command, labelled for a false alarm only, because it
re-targets the release that was rolled back from: it is not how the fix ships. A forward run carries no override and prints
the command that undoes it. Run the dry run first.

**Resume the train.** When the fixed release is on `main`, or the pause is no longer wanted:

```sh
$GH_BIN variable set RELEASE_TRAIN --body true -R EchoTools/nevr-runtime
```

Resuming with beta on an older release than the newest published tag is allowed, and the timer then publishes that newest
tag to beta unless its log has a `done` for it. Resume only after you have decided which release beta should follow.

## Sources read for this file

- GitHub Docs, "Immutable releases": https://docs.github.com/en/code-security/supply-chain-security/understanding-your-software-supply-chain/immutable-releases
- GitHub Docs, "Preventing changes to your releases": https://docs.github.com/en/code-security/how-tos/secure-your-supply-chain/establish-provenance-and-integrity/prevent-release-changes
- GitHub Docs, "Verifying the integrity of a release": https://docs.github.com/en/code-security/how-tos/secure-your-supply-chain/secure-your-dependencies/verify-release-integrity
- GitHub Docs, "Using artifact attestations to establish provenance for builds": https://docs.github.com/en/actions/how-tos/secure-your-work/use-artifact-attestations/use-artifact-attestations
- GitHub CLI manual, `gh release verify-asset`: https://cli.github.com/manual/gh_release_verify-asset
