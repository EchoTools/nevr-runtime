# Releasing nEVR runtime

Every release follows this file (`AGENTS.md`). A release is a tag, a draft that CI fills, a signing pass, and one
publish. Nothing about a published release is edited afterwards: a wrong release is fixed by the next version.

## Names

- **Version:** plain semver, `X.Y.Z` (no leading zeros, `tools/release_asset_names.sh` `release_tag_regex`). Every
  other build stamps `X.Y.(Z+1)-dev.<N>+<sha>`, which sorts after the tag it descends from; it is never released.
- **Tag:** `vX.Y.Z`, annotated (`git tag -a vX.Y.Z -m "X.Y.Z"`), cut from `origin/main`.
- **GitHub release title:** the tag, `vX.Y.Z`, and nothing else (`gh release create vX.Y.Z --title vX.Y.Z`).
- **Pre-release or release:** the flag is the only difference. A release is created as a pre-release; promoting it
  (`gh release edit vX.Y.Z --prerelease=false`) changes no byte and fires no workflow. Nothing in a file depends on the flag.

## Assets

One file per audience, with the audience in its name. The list is `release_asset_names` in
`tools/release_asset_names.sh`; the guard, the draft attach, the workflow and `tools/tests/test_release_assets.py`
all read or check that one list, so the names below cannot drift from the build.

| file | audience | holds |
|---|---|---|
| `nevr-runtime-vX.Y.Z-windows-player.zip` | players on Windows | `BugSplat64.dll`, `install.ps1`, `uninstall.ps1`, `quest-diagnose.ps1`, `README.txt`, `SIGNING.txt`, `SHA256SUMS` |
| `nevr-runtime-vX.Y.Z-windows-server.zip` | people running a dedicated server | `BugSplat64.dll`, `echovr_server.exe` (both stripped, debug info split off), `README.md`, `INSTALL.md`, an empty `plugins/` |
| `nevr-runtime-vX.Y.Z-quest-libovrplatformloader.so` | Quest packagers | the Quest sentinel library, a loose ELF (attested; not Authenticode) |
| `SHA256SUMS` | everyone | SHA-256 of the files above |
| `RELEASE-NOTES.md` | everyone | the notes `tools/package_release.py seal` writes from `tools/package-release/RELEASE-NOTES.template.md` |

Not release assets: the `.tar.zst` archives and the `.dbg` symbol files (workflow artifacts, 7 and 90 days), and any Quest APK.

**Quest APKs never go on GitHub.** They are hosted on echo.taxi, `https://echo.taxi/nevr-runtime/quest/<file>` with a
`<file>.sha256` beside it. Check the file locally (size, `sha256sum`, `unzip -tq`), install it on the host without
overwriting a name, and verify it from outside by hash. Never request the URL before the file is installed: the CDN
caches the 200 HTML answer for that exact URL. A changed file gets a new name (the version is in it).

## Before the tag

1. `git fetch origin` and `just release-preflight` pass: this clone is exactly `origin/main`, no local-only tag, nothing
   unpushed, no uncommitted change.
2. `just verify` has run on this tip of `main`: its log starts `verify: HEAD <sha>` and `verify: tree clean`, and ends
   `verify: OK`. It runs once per tip (`AGENTS.md`, "Builds"); a missing HEAD line is checked, not re-run.

## Cut the draft

A release is built from a **draft**. A draft fires no workflow, so the build is started by hand on the tag ref, attaches
its files to the draft, and the draft stays a draft until the checks below pass.

```sh
just release-preflight                                    # 1. refuses unless this clone is exactly origin/main
git tag -a vX.Y.Z -m "X.Y.Z" && git push origin vX.Y.Z   # 2. tag origin's tip, push the tag
just release-preflight && gh release create vX.Y.Z --title vX.Y.Z --draft --prerelease --verify-tag   # 3. the draft
gh workflow run build.yml --ref vX.Y.Z                    # 4. build, attest, attach the five files; it stays a draft
```

`tools/release_draft.sh` decides what step 4 does: a tag with exactly one draft, no published release, and none of the
five names on the draft is built and attached; a draft that already carries any of the five names fails the run red, so a
second dispatch never overwrites files. The `quest` job runs only for a draft dispatch or a release event, never for a
plain dispatch: a green plain dispatch says nothing about the Quest tests.

Do not create the release as published. A published release is immutable once the setting below is on, and the build
attaches its files after the release exists.

## Check the draft

All of these on the draft, before anything is signed or published. Read the output; do not trust a green run.

```sh
gh release view vX.Y.Z --json assets --jq '.assets[].name'          # exactly the five names above
mkdir -p "$S" && gh release download vX.Y.Z -D "$S"                  # a new, empty directory
(cd "$S" && sha256sum -c SHA256SUMS)                                  # every listed file OK
for f in "$S"/*; do gh attestation verify "$f" -R EchoTools/nevr-runtime; done   # each exits 0
(cd "$S" && unzip -p nevr-runtime-vX.Y.Z-windows-player.zip BugSplat64.dll | strings -a | grep -a '^NEVR-BUILD')
```

The last line must be exactly `NEVR-BUILD X.Y.Z <the tag's commit, 40 hex>` (one literal; `git rev-parse vX.Y.Z^{commit}`).

## Sign

Signing is a separate pipeline in `thesprockee/release-signer` (the nEVR Signer GitHub App and the signing service). The
build signs nothing. The signer reads the draft's files, signs the PE files and scripts in them, and puts the files back
under the same names.

```sh
gh workflow run sign-release.yml -R thesprockee/release-signer --ref main \
  -f repo=EchoTools/nevr-runtime -f tag=vX.Y.Z -f dry_run=true     # signs, verifies, writes nothing to the draft
```

A dry run keeps the signed set as a workflow artifact and a decision record binding the tag, the source commit and each
asset's unsigned and signed SHA-256. `dry_run=false` also replaces the assets of the **draft** (the signer never writes a
published release and never flips the pre-release flag); it needs the signer's GitHub App to be installed on this repository.
That installation, and the signing credentials, are the owner's.

Check a signed file as `SIGNING.txt` in the player zip says: `Get-AuthenticodeSignature` status `Valid`, or
`osslsigncode verify` with both `-CAfile` and `-TSA-CAfile` set to the Microsoft Identity Verification Root 2020
certificate. The CI attestation covers the bytes CI built; for a file the signer replaced, the Authenticode signature and
the signer's decision record are the evidence for the new bytes.

## Publish

Only from the checked draft:

```sh
gh release edit vX.Y.Z --draft=false          # publish; the pre-release flag is untouched
```

Publishing fires the `published` event; `tools/release_already_built.sh` sees the five names and the workflow does
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

**The setting is the repository owner's to flip.** Steps, from GitHub's page "Preventing changes to your releases":
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
