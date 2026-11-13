---
name: verify-published-release
description: Use when a release has been published and its bytes must be proven before anyone tests it (RELEASING.md step 7). Downloads exactly the files the signed manifest names and records OK or FAIL for each.
---

# Verify a published release

The procedure for `RELEASING.md` step 7 ("prove the bytes") using `docs/reference/verifying-a-release.md`.

## Steps

1. Do steps 1 to 6 of `docs/reference/verifying-a-release.md` on the published `manifest.json` and its signature
   (fetch, signature, key, version, expiry, store). Stop on any failure.
2. Download into a new, empty directory exactly the files the manifest lists for the version: their `path` values, no others.
3. Run `go -C tools run ./cmd/release-verify -manifest <stored manifest.json> -version X.Y.Z -dir <download dir>`.
4. Keep its output (one OK or FAIL line per file, then `manifest_sha256=`) as the record for the release. Exit 1: the
   release is not tested; a failure found here goes to a patch release, and the published release is not edited.
5. Only on exit 0, install through the normal path and run the release tests of `RELEASING.md`.

## Artifact and consumer

The per-file record tied to the manifest hash. Consumer: `tools/cmd/release-verify` produces and gates it (exit 1 on any
failed file). It does not check the signature or the Windows zip's entries: steps 1 to 6 and the zip check in the
reference doc still do that.

## Done when

`release-verify` exits 0 and the record is kept with the release.

## Proof of the check

`go -C tools test ./cmd/release-verify`: a flipped byte, a padded file, a missing file, a version the manifest does not
list and a path that escapes the directory each fail; a matching set passes.
