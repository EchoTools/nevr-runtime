# Releasing nEVR runtime

Every release follows this file (`AGENTS.md`). A release is a tag, a draft that CI fills, a signing pass, and one
publish. The checks before the tag run locally; the workflow runs once, on the real tag (GitHub Actions minutes are
billed). Nothing about a published release is edited afterwards: a wrong release is fixed by the next version.

## Names

- **Version:** plain semver, `X.Y.Z` (no leading zeros, `tools/release_asset_names.sh` `release_tag_regex`). It is
  computed, not chosen: `python3 tools/next_release_version.py` prints it from the commits since the last release tag
  (`docs/standards/planning.md`, "Release versions"). Every other build stamps `X.Y.(Z+1)-dev.<N>+<sha>`, which sorts
  after the tag it descends from; it is never released.
- **Tag:** `vX.Y.Z`, annotated (`git tag -a vX.Y.Z -m "X.Y.Z"`), cut from `origin/main`.
- **GitHub release title:** the tag, `vX.Y.Z`, and nothing else (`gh release create vX.Y.Z --title vX.Y.Z`).
- **Pre-release or release:** the flag is the only difference. A release is created as a pre-release; promoting it
  (`gh release edit vX.Y.Z --prerelease=false`) changes no byte and fires no workflow. Nothing in a file depends on the flag.
  There is no `-rc` tag: `release_tag_regex` accepts only `vX.Y.Z`, so a pre-release carries the version it will have if
  promoted, and the next pre-release is the next version the commits compute (a published tag is never reused).

## Assets

Two files. The list is `release_asset_names` in `tools/release_asset_names.sh`; the guard, the draft attach, the
workflow and `tools/tests/test_release_assets.py` all read or check that one list, so the names below cannot drift
from the build.

| file | audience | holds |
|---|---|---|
| `nevr-runtime-vX.Y.Z-windows.zip` | players on Windows and people running a dedicated server | `BugSplat64.dll` (the host), `nevr-runtime.dll` (the runtime), `echovr_server.exe` (all stripped, debug info split off), `install.ps1`, `uninstall.ps1`, `quest-diagnose.ps1`, `README.txt` (the install steps and the dedicated-server steps), `SIGNING.txt`, `SHA256SUMS` (the files inside the zip: `install.ps1` and `uninstall.ps1` check both DLLs against it). The zip has no `plugins/` entry: a zip holds no empty directory |
| `nevr-runtime-vX.Y.Z-quest-libovrplatformloader.so` | Quest packagers | the Quest sentinel library, a loose ELF (attested; not Authenticode) |

There is no top-level `SHA256SUMS` and no `RELEASE-NOTES.md` asset. The **release body** carries the notes and a table
of the SHA-256 of each of the two assets, rendered by `python3 tools/package_release.py notes` from
`tools/package-release/RELEASE-NOTES.template.md` and the asset files as they are after signing ("Write the release
body"). The attestation stays the machine-checkable proof of the CI bytes.

Not release assets: the `.tar.zst` archives and the `.dbg` symbol files (workflow artifacts, 7 and 90 days), and any Quest APK.

**The signed Windows zip lives on the GitHub release.** Quest APKs are published outside GitHub.

**Quest APKs never go on GitHub.** They are published outside GitHub, under `nevr-runtime/quest/<file>` with a
`<file>.sha256` beside it. Check the file locally (size, `sha256sum`, `unzip -tq`), install it on the publishing host
without overwriting a name, and verify it from outside by hash. Never request the published address before the file is
installed: the CDN caches the 200 HTML answer for that exact address. A changed file gets a new name (the version is in it).

## Before the tag

1. `git fetch origin` and `just release-preflight` pass: this clone is exactly `origin/main`, no local-only tag, nothing
   unpushed, no uncommitted change.
2. `just verify` has run on this tip of `main`: its log starts `verify: HEAD <sha>` and `verify: tree clean`, and ends
   `verify: OK`. It runs once per tip (`AGENTS.md`, "Builds"); a missing HEAD line is checked, not re-run.
3. The build the workflow does passes locally on this exact commit. GitHub Actions minutes are billed, so the pipeline
   runs once, on the real tag; nothing is rehearsed there. After step 2 (which builds `mingw-release` and checks it),
   run the packaging the workflow runs, from the same clone:
   ```sh
   just package-dev    # tools/package_release.py over the stripped DLL and the Quest library; stamps -dev, not a release
   ```
   Read its output to the end: it fails on a Windows or Quest build that does not package. It needs the Quest signing
   keystore in `pass`, the store APK and the NDK, as its recipe says. What it cannot show is the signer, the attestation
   and the draft upload; those are checked on the draft ("Check the draft").
4. The PC login test passes with this tip's `BugSplat64.dll` and `nevr-runtime.dll` on a throwaway copy of the install, against production
   (`AGENTS.md`, "Client login test mechanics"): the run's log shows the four keys on the `built-in defaults embedded in
   this build:` line, then `LOGIN SUCCESS` and `to logged in`.

## Cut the draft

A release is built from a **draft**. A draft fires no workflow, so the build is started by hand on the tag ref, attaches
its files to the draft, and the draft stays a draft until the checks below pass.

```sh
just release-preflight                                    # 1. refuses unless this clone is exactly origin/main
git tag -a vX.Y.Z -m "X.Y.Z" && git push origin vX.Y.Z   # 2. tag origin's tip with the version `python3 tools/next_release_version.py` prints, push the tag
just release-preflight && gh release create vX.Y.Z --title vX.Y.Z --draft --prerelease --verify-tag   # 3. the draft
gh workflow run build.yml --ref vX.Y.Z                    # 4. build, attest, attach the two files; it stays a draft
```

`tools/release_draft.sh` decides what step 4 does: a tag with exactly one draft, no published release, and neither of the
two names on the draft is built and attached; a draft that already carries either of the two names fails the run red, so a
second dispatch never overwrites files. The `quest` job runs only for a draft dispatch or a release event, never for a
plain dispatch: a green plain dispatch says nothing about the Quest tests.

**The draft path is the only path.** Release immutability is on for this repository. A release created as published is
locked the moment it exists, before the build can attach its files, and can never be filled or fixed. Never run
`gh release create` without `--draft`.

**A draft that fails its checks is deleted and cut again.** A second `gh workflow run build.yml` onto a draft that already
carries either of the two names fails red, so a bad draft is never rebuilt in place. Delete it (`gh release delete vX.Y.Z
--yes`; this leaves the tag) and, when the fault is in the files only, create the draft again from step 3. When the fault
is in the tree, also delete the tag (`git push origin --delete vX.Y.Z`, `git tag -d vX.Y.Z`), fix `main`, and start again at
"Before the tag". A tag with no published release may be removed; the rule that a tag name is never reused applies once a
release is published ("When a published release is wrong"). Each cut is another workflow run, so the local checks before
the tag are where a fault is found.

## Check the draft

All of these on the draft, before anything is signed or published. Read the output; do not trust a green run.

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
The last line reads the VERSIONINFO of the three files (the Details tab: description, product name, copyright, company, original
file name and the file and product version `X.Y.Z`, numeric `X.Y.Z.0`) and compares every field with the pinned values and
with the version in the DLL's `NEVR-BUILD` literal; `python3 tools/pe_version_info.py show <file>` prints them.

## Sign

Signing is a separate pipeline in `thesprockee/release-signer` (the nEVR Signer GitHub App and the signing service). The
build signs nothing. The signer reads the draft's files, signs the PE files and scripts in them, and puts the files back
under the same names.

```sh
gh workflow run sign-release.yml -R thesprockee/release-signer --ref main \
  -f repo=EchoTools/nevr-runtime -f tag=vX.Y.Z -f dry_run=false    # signs, verifies, replaces the DRAFT's assets
```

The run keeps the signed set as a workflow artifact and a decision record binding the tag, the source commit and each
asset's unsigned and signed SHA-256. It replaces the assets of the **draft** only (the signer never writes a published
release and never flips the pre-release flag); it needs the signer's GitHub App to be installed on this repository. That
installation, and the signing credentials, are the owner's. The signer also takes `dry_run=true` (it writes nothing to the
draft); it is another Actions run, so it is not a step here.

Check a signed file as `SIGNING.txt` in the Windows zip says: `Get-AuthenticodeSignature` status `Valid`, or
`osslsigncode verify` with both `-CAfile` and `-TSA-CAfile` set to the Microsoft Identity Verification Root 2020
certificate. The CI attestation covers the bytes CI built; for a file the signer replaced, the Authenticode signature and
the signer's decision record are the evidence for the new bytes.

After the signer has replaced assets, run the checks above again on the draft before publishing,
read this way:

- The signer replaces the zip. The `SHA256SUMS` inside it lists the signed files in it, so unzip the signed zip into a
  new directory and run `sha256sum -c SHA256SUMS` there.
- `gh attestation verify` covers the unsigned build output CI attested, not the bytes the signer replaced. The loop under
  "Check the draft" exits non-zero for the signed zip (`HTTP 404: Not Found` from the attestations API) and still exits 0
  for the Quest library, which the signer does not replace; that failure is the signer's work, not a bad draft. For a
  replaced file the evidence is the Authenticode check above and the signer's decision record.
- The same two names must still be on the draft.
- Run `python3 tools/pe_version_info.py check` on the signed files too (the command under "Check the draft"): signing
  must leave the VERSIONINFO as the build wrote it.
- Write the release body next (below) from these final bytes.

## Write the release body

The release body is the notes and a SHA-256 table of the two assets as they are on the draft after signing. Render it
from the draft's own files, put it on the draft, and read it back against the files:

```sh
mkdir -p "$S" && gh release download vX.Y.Z -D "$S"                  # a new, empty directory
python3 tools/package_release.py notes --assets "$S" --out "$N"      # $N: a new file outside $S; refuses unless $S holds exactly the two assets
gh release edit vX.Y.Z --notes-file "$N"
gh release view vX.Y.Z --json body --jq .body                         # the table's two SHA-256 values
(cd "$S" && sha256sum nevr-runtime-vX.Y.Z-*)                          # the same two values
```

`notes` takes the version and the commit from the `BugSplat64.dll` and `nevr-runtime.dll` inside the zip (they must agree), so the body cannot describe another
build. The table must be written after the last signing pass: each pass signs with a fresh timestamp and changes the
zip's bytes. `notes` renders the table and the install notes only. Before the body goes on the draft, append a section
that says, for each audience the release is for (players, server hosts, plugin developers), what changed for them in plain
words, and says what is not tested or not in the build. The body of a published release stays editable ("Immutable
releases"); the assets do not.

## Publish

Only from the checked draft. The tag and the publish cannot be undone: the owner is told right before the publish.

```sh
gh release edit vX.Y.Z --draft=false          # publish; the pre-release flag is untouched
```

Publishing fires the `published` event; `tools/release_already_built.sh` sees the two names and the workflow does
nothing. Then:

```sh
gh release verify vX.Y.Z -R EchoTools/nevr-runtime                       # needs immutable releases on; GitHub CLI 2.81.0 or newer
gh release verify-asset vX.Y.Z <file> -R EchoTools/nevr-runtime          # a local file against the release's asset
```

Announce a green pre-release the same hour, saying which asset is for whom.

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
