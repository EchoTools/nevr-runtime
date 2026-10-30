# Releasing nEVR runtime

A release is a tag, a draft, two files attached to it, the signer's replacement of those files, the checks on the
result, and one publish. CI does all of it, in `.github/workflows/release-train.yml` (the train) and `build.yml` (the
build). The Quest APK is the one part CI does not do: a user timer on the publisher's machine finishes the release ("The Quest
half of the train"). A person does the same steps by hand only when the train is off or a run has gone red. Nothing about a
published release is edited afterwards: a wrong release is fixed by the next version.

A release seat merges its own pull requests, in this repository and in the signer repository, once the pull request's checks are
green and a fresh verifier (neither the author nor the implementer) has passed the tip with every finding fixed. It deletes the
branch and the worktree the same turn. A publish outside the train, and anything that touches a signing key, goes to the owner's seat first.

## Names

- **Version:** plain semver, `X.Y.Z` (no leading zeros, `tools/release_asset_names.sh` `release_tag_regex`). It is
  computed, not chosen: `python3 tools/next_release_version.py` prints it from the feat, fix, perf and breaking commits
  since the newest `vX.Y.Z` tag reachable from `origin/main` (`docs/standards/planning.md`, "Release versions"). Every
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
| `nevr-runtime-vX.Y.Z-windows.zip` | players on Windows and people running a dedicated server | `BugSplat64.dll` (the host), `nevr-runtime.dll` (the runtime), `echovr_server.exe`, `nevr-install.exe` (all stripped, debug info split off), `install.ps1`, `uninstall.ps1`, `quest-diagnose.ps1`, `README.txt` (the install steps and the dedicated-server steps), `SIGNING.txt`, `SHA256SUMS` (the files inside the zip: `install.ps1` checks `nevr-install.exe` against it, and the core checks both DLLs against the signed manifest). The zip has no `plugins/` entry: a zip holds no empty directory |
| `nevr-runtime-vX.Y.Z-quest-libovrplatformloader.so` | Quest packagers | the Quest sentinel library, a loose ELF (attested; not Authenticode) |

There is no top-level `SHA256SUMS` and no `RELEASE-NOTES.md` asset. The **release body** is short: a header, one line on
what is new for players, and one line per file. `python3 tools/package_release.py notes` renders it from
`tools/package-release/RELEASE-BODY.template.md` ("Write the release body"). It holds no hash: the zip's own `SHA256SUMS`
(which `install.ps1` checks `nevr-install.exe` against), the signature and the attestation carry integrity.

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
commit that `tools/next_release_version.py` counts (feat, fix, perf or a breaking change) since the newest tag.

The train and its guard share one rule, `tools/release_window.py`: no release is cut or dispatched within 195 minutes of the newest
published release (the newest by `publishedAt` of the non-draft releases whose tag is exactly `vX.Y.Z`). A scheduled run checks it
first and holds, cutting nothing, when that release is 195.0 minutes old or less; a manual dispatch is not held. A run that held or
found nothing to cut published nothing, so it does not start a window.

The check reads the 20 newest releases. A check that finds no published `vX.Y.Z` release among them (an empty list, or only drafts
and other tags) refuses: it cannot tell an empty history from a listing that hides the release, so the scheduled run's step and the
guard both fail with a message that gives the count, and nothing is cut or dispatched.

The guard covers a dropped scheduled train run. It is a systemd user timer on the publisher's machine
(`tools/systemd/nevr-train-guard.timer`, hourly at :07), which runs `tools/release_train_guard.py` from the Quest timer's dedicated
checkout with the publisher's `gh-spritz` (`--gh`, else `GH_BIN`). It runs only while that machine is on: a machine that is off delays
the guard, and the timer's `Persistent=true` catches up when the machine comes back. When `main` is releasable and no release was
published in the last 195 minutes (3 h 15 min), it dispatches the train. It does nothing while a train run is queued, in progress, waiting or pending,
when `RELEASE_TRAIN` is not `true`, when nothing on `main` is releasable, while the newest train run failed, was cancelled or timed out
195 minutes ago or less, measured from when that run was created (a red cut can leave a tag and an unpublished draft that the version tool counts, so the guard does not
re-dispatch hourly; a run that succeeded or was skipped does not hold it), or while the newest published release is inside the window. Each decision is one JSON line (`reason` is `switch_off`, `no_releasable_change`,
`train_run_in_progress`, `recent_failed_run`, `recent_release` or `no_release_in_window`; `last_release`, `last_published`, `age_minutes` and `window_minutes`
describe the release, `last_run_id` and `last_run_created` the newest train run: for information, except under `recent_failed_run`, where it is the failed run) in the journal
(`journalctl --user -u nevr-train-guard.service`) and in `~/.local/state/nevr-runtime/release-train-guard.jsonl`; a run whose log cannot
be written fails before it decides anything. A failed run couriers the release lead (`nevr-train-guard-failed.service`); it never bonks.
It is not a GitHub schedule: GitHub drops or delays scheduled runs, and a job's token cannot read repository variables (`gh variable
list` answers HTTP 403 to it), so the job could not read `RELEASE_TRAIN`. `just install-train-guard-timer` installs the timer.

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
   signing keystore in `pass`, the store APK and the NDK, as its recipe says.
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
characters, one plain sentence a player would understand about that change only, written by the PR's author, with a full
stop added if it has none (or `!` or `?`). `notes` builds the player line from the files that exist at `--rev` and not at
`--base`, joined with one space in file-name order (at most 600 characters). A path present at the base tag is old by
construction, whatever its content, so no earlier release's sentence rides along. A file with another name (letters, digits,
`.`, `_`, `-`, then `.txt`; `release-headlines/README.md` is the one exception), an empty file, a file with more than one
line or a line over 200 characters, a joined line over 600, or `release-headline.txt` at the rev fails `notes` and names the
file; it never falls back silently. A successful `notes` writes one JSON line to stderr (`"event":"headlines"`, with `used`,
`line`, `base` and `rev`; `used` is empty when the fixed sentence stands), which the release-train run's log keeps. The train
cannot push to `main`, so it deletes nothing: the PR that adds a fragment also deletes the fragments present at the newest
`vX.Y.Z` tag (`git ls-tree -r --name-only <newest tag> -- release-headlines/`). A fragment left behind is harmless, since a
path present at the base is never read.

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
with it, and refuses unless the signed APK's loader has the asset's sha256. The APK is signed with the key in `pass`
(`tools/quest_keystore.sh`; `just quest-keystore-fingerprint` prints the signer's SHA-256). It uploads nothing. Check the file as above, then publish it from the same clone:

```sh
just publish-quest-apk vX.Y.Z   # puts the APK and its .sha256 on R2, then fetches both back from the public host
```

The recipe takes the bucket, the public host, the release-signer executable and checkout and a GitHub read token from the
environment and nothing else: `R2_ENDPOINT`, `R2_BUCKET`, `R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY`,
`RELEASE_PUBLIC_BASE`, `RELEASE_SIGNER_BIN`, `RELEASE_SIGNER_DIR` and `GH_TOKEN`. A missing one is a refusal that names
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

The Quest APK is built from the store game and signed with a key in `pass`, and the R2 credential is the publisher's, so no
workflow can build or upload it and there is no self-hosted runner. The train publishes the GitHub release; a systemd user
timer on the publisher's machine (`tools/systemd/nevr-quest-release.timer`, every 30 minutes, `Persistent=true` so a
missed run is made up) runs `tools/quest_release_timer.py`, which finishes the release with no step by hand.

The timer takes the **newest published tag only**: a release that is not the newest when the timer first sees it is never
built, because a later tag supersedes it. That is intended. A tag is built by hand (`--tag vX.Y.Z`) when it must be.

It runs from its own **dedicated checkout** (`~/src/nevr-runtime-work/release-timer/checkout`, a detached worktree that
`just install-quest-release-timer` makes). The unit fetches and moves that checkout to `origin/main` before every run,
and the tool refuses, as a refused `checkout` step that couriers the owner, unless the checkout is clean and HEAD is the
freshly fetched `origin/main`. The `tags` step then refuses, the same way, while any tag in that checkout is not on origin or
points at another commit there (the rule of `tools/release_preflight.py`, which `just release-quest-apk` runs later); the
refusal names up to ten of them with their short ids, counts the rest, and prints the shell-quoted remove and restore commands,
with the full object id, for each tag it lists.

1. Lists the newest published `vX.Y.Z` release. A tag that has a `done` record in the log is left alone. A tag that has
   failed three times is not retried; the third failure couriers Spritz once, before the run exits 0. Nothing bonks the owner.
   A dirty or stale checkout, a local tag origin does not have, or a missing credential, is a refusal that is logged and never counts toward the three failures.
2. Builds the release-signer from a snapshot of its `main`, and reads `verify-manifest --live`. A tag that beta already
   lists is done. Otherwise the beta release must list a `game-data` artifact (the zip the new release carries), whose
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
7. Couriers Teth to verify the live publish and Spritz the same text (no bonk), then removes the worktree and the signer
   snapshot by exact path. A tag whose notice was never sent is noticed on the next run.

Stable stays an owner step (`approve-stable`); the timer only publishes beta. **Credentials go only where they are used:**
the R2 credential to `publish_apk`, and the GitHub token to `publish_apk` and the dispatch. Every other command, the
build included, gets an environment without them. Every step appends a JSON line to
`$XDG_STATE_HOME/nevr-runtime/quest-release-timer.jsonl`, with each credential value replaced by `<redacted>`; a log that
cannot be written fails the run. A failing step exits 1 naming itself, with the redacted tail of the failing command's stderr and stdout (the stdout of `curl`, `gh` and the signer is remote content and is left out), and the unit's `OnFailure=` runs
`quest_release_timer.py --notify-failure`, which couriers the last failure to Spritz, at most once in six hours for the same step and
error (a standing refusal repeats on every run). The credential is the file
`~/.config/metis/r2-evr-echo-taxi.env` (`R2_ENDPOINT`, `R2_BUCKET`, `R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY`), loaded by
the unit and never written into it. The unit also sets `GNUPGHOME` to the XDG gnupg home (`~/.config/gnupg`) so `pass` finds the key, and a `pass` failure prints gpg's own `gpg:` lines after the `ERROR` line. Install, from any clone of the repository:

```sh
just install-quest-release-timer   # the dedicated checkout, systemctl --user link of the three units, daemon-reload, enable --now the timer
python3 tools/quest_release_timer.py --tag vX.Y.Z   # the same run by hand, for one tag
```

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

## Sources read for this file

- GitHub Docs, "Immutable releases": https://docs.github.com/en/code-security/supply-chain-security/understanding-your-software-supply-chain/immutable-releases
- GitHub Docs, "Preventing changes to your releases": https://docs.github.com/en/code-security/how-tos/secure-your-supply-chain/establish-provenance-and-integrity/prevent-release-changes
- GitHub Docs, "Verifying the integrity of a release": https://docs.github.com/en/code-security/how-tos/secure-your-supply-chain/secure-your-dependencies/verify-release-integrity
- GitHub Docs, "Using artifact attestations to establish provenance for builds": https://docs.github.com/en/actions/how-tos/secure-your-work/use-artifact-attestations/use-artifact-attestations
- GitHub CLI manual, `gh release verify-asset`: https://cli.github.com/manual/gh_release_verify-asset
