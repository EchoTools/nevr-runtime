# ADR 0029: a Quest APK can carry a game-data pack, and the first-run data step places it

Status: proposed. Tracking issue: #1608; a newer pack over an older one is #1614. Builds on ADR 0011 and ADR 0013 (the first-run
data step, its roots and its leftovers), ADR 0019 (what the repack stages and what `verify-signed` checks) and ADR 0021 (the repack
patches `libr15.so` on the host). Nothing here has run on a headset when it is written; "Not established" lists what only a headset
run decides.

## Requirement

A variant build of the Quest game (the unlisted APK `release-quest-variant-apk` writes) needs two extra files under the stock data
tree: a replacement manifest and one new package. They are game-derived bytes, so they travel in the variant APK and nowhere in
this repository, and:

1. **Nothing changes for a build with no pack.** The release APK and every `package-dev` APK carry none; the data step and the
   repack recipe do what they did before.
2. **The game never sees a manifest without the package it names**, whatever kills the process and whenever.
3. **Nothing is written that the pack does not name, and nothing is replaced that is not the stock file the pack was built over.**
4. **A failure is visible and ends the process**, like a failed data step, never a game that starts on half a pack.

## Decision

### 1. The carry format

An APK that carries a pack has, as STORED (uncompressed) entries:

- `assets/nevr-pack/pack.json`: schema 1, one JSON object with sorted keys:
  `schema` (1), `id` (`[a-z0-9][a-z0-9._-]{0,63}`), `version` (`X.Y.Z`), `stock_data_version` (the folder under `_data`, which must
  be the one in `kManifestPath`, `src/quest/data/plan.h`), `requires_stock_manifest_sha256` (a sorted list of one or two lowercase
  sha-256 digests of the stock manifests the pack may replace), `files` (1 to 8 objects `{path, sha256, size}`), `source_zip_sha256`
  and `source_zip_size` (the zip the repack was given).
- `assets/nevr-pack/files/<path>` for each file: bytes exactly as `pack.json` hashes them.

A file's `path` is relative to `_data/<stock_data_version>/rad15/android/` and is `manifests/<id>` for the game's one manifest
(the id of `kManifestPath`, exactly one) or `packages/<name>` for any other file: one plain file name (`[A-Za-z0-9._-]`, at most 64,
not starting with a dot, not ending in `.nevr-pack.tmp`, `.stock` or `.nevr-owned`), sizes 1 byte to 2 GiB less one, no two paths equal
in any letter case. The pack describes itself: the runtime holds no hash of any pack. The rules are `ParsePackJson`
(`src/quest/data/pack.cpp`) and, for the repack, `pack_problems` (`tools/quest_repack_libs.py`); the two are the same rules.

The APK stays under the 4 GiB a zip without zip64 addresses (the sentinel's zip reader refuses zip64), less 8 MiB for the central
directory, the signing block and alignment.

### 2. The repack

`just android-repack-apk ... pack=<zip> pack_meta=<file>` (both empty for every release and `package-dev` APK; empty, the recipe runs
no line of this). `pack_meta` is a JSON file with `id`, `version`, `requires_stock_manifest_sha256` and `zip_prefix`
(`_data/<version>/rad15/android/`, where the zip's entries sit). `tools/quest_repack_libs.py stage-pack` reads the pack zip and
checks every entry before it writes one byte: names safe by the sentinel's own reader rule, no link or other special file, not
encrypted, no two names equal in any case, every file under the prefix and in `manifests/` or `packages/`, the sizes the zip records
equal to the bytes read, the sha-256 taken from those bytes, the version folder and manifest id equal to the data step's pins in
`plan.h`, at most 8 files, none empty, the store APK without an `assets/nevr-pack/` entry of its own, and the APK size under the
limit above. It stages `assets/nevr-pack/` under the recipe's stage directory; the recipe adds those entries with `zip -0` and
`pack-problems` checks the staged tree as the runtime reads it. `verify-signed` then reads the SIGNED APK: every staged pack file is
there with the staged bytes and STORED, no `assets/nevr-pack/` entry is there that was not staged (a build with no pack carries
none), and the file is under 4 GiB. `release-quest-variant-apk` takes `pack` and `pack_meta` and passes them through; the release
APK recipe takes none.

### 3. The runtime step

After `RunDataStep` ends `kUsed` or `kFetched` (`AfterStorage`, `src/quest/sentinel/data_wiring.cpp`), on the same thread, `RunPackStep`
(`src/quest/data/pack.cpp`) runs in the root the data step used (`engine_root`). It runs in a release build too: the data step only
looks there (ADR 0013), but the variant APK that carries a pack is a release build, and an APK without a pack makes the step one log
line. A data step that failed or found no data (`kFailed`, `kMissing`) or ended on the Storage message never reaches it.

1. **The APK.** The path is the one the linker mapped the sentinel from: `dladdr` on the sentinel gives `<apk>!/lib/...`, or a library
   extracted beside `base.apk` (`ApkPathForLibrary`). The listing uses the existing zip reader with raised limits (`PackZipLimits`: an
   APK has hundreds of entries), and one entry is read through `ReadZipEntry`, which makes every check `ExtractZip` makes of an
   entry. `ExtractZip` and its refusals are unchanged. No path, or an APK that does not read: one Info `pack_check outcome=none` with
   its reason, and the game starts.
2. **No `pack.json`:** `pack_check outcome=none`, one Info event, nothing else, behaviour unchanged.
3. **An unusable `pack.json`** (a rule above broken, a named file missing from the APK, a size that differs): Error `pack_check
   outcome=invalid` with the reason, nothing touched, the game starts on the data that is there. The repack refuses such a pack, so this
   is a build defect, and ending the process on it would strand every launch of that APK.
4. **The gate.** The stock manifest in the root is hashed (never through a link). It must equal one of
   `requires_stock_manifest_sha256`, or already equal the pack's manifest digest. Any other manifest, or none: nothing is touched,
   Warning `pack_check outcome=stock_mismatch` naming the digest found, the digests required and the pack's, and the game starts.
5. **Idempotence.** A file already in place with its recorded size and sha-256 is `current` and not written (the manifest is current
   when its digest is the pack's). All files current: `pack_check outcome=current`, nothing written, no `data_version` event. A pack
   manifest in place with its package missing or wrong has the package written again and the manifest left alone.
6. **Space.** `statvfs` before the first write: the bytes still to write plus 64 MiB. Short: nothing is written, the floor message names
   the free and needed MiB.
7. **Writing.** Each file in turn, the packages first and the manifest last, so the game never sees a manifest without its package:
   a marker `<tmp>.nevr-owned` first (`MarkOwned`, the data step's convention), then `<file>.nevr-pack.tmp` written from the APK entry
   with its size and sha-256 taken from the bytes, fsync'd, checked against `pack.json`; for the manifest, the stock manifest is kept as
   `<manifest>.stock` (a verified copy, its marker beside it) unless that name exists, and a backup is never overwritten; then one
   `rename` and an fsync of the directory, and the temporary's marker removed. A temporary whose name is taken by a file with no marker is not
   ours, and the step fails rather than remove it.
8. **A failure** (no space, a digest that does not match, an entry that does not read, a write error) removes the temporary it made,
   shows the floor message through the data step's presenter, holds it as a data-step failure is held, and ends the process nonzero
   with reason `pack_failed` (`EndFailed`). The data already in place is the stock data plus whatever whole files were renamed; a
   verified package without its manifest is harmless.
9. **Leftovers.** A kill leaves a marked `*.nevr-pack.tmp`. `CleanLeftovers` (the routine the data step runs on its roots) also calls
   `CleanPackLeftovers`, which removes, in `manifests/` and `packages/` of each real `_data/<version>/rad15/android`, a marked
   temporary and the marker of an item that is gone, and nothing else: not an unmarked name, not a `.stock` backup. The pack step calls
   it first, so a release build, which recovers nothing else, still cleans what its own pack left.
10. **Events** (sentinel log, JSON, no secrets, `docs/standards/logging.md`): `pack_check` (`outcome` none, invalid, stock_mismatch,
    current or place; `id`, `version`, `root`, `apk`, the manifest digest found, the digests required, the pack's manifest digest),
    `pack_place` (per file: `path`, `bytes`, `sha256`, `result` placed, current or failed, `status`, `detail`, `stock_backup` for the
    manifest), `pack_step` (`outcome` placed, current, stock_mismatch, invalid or failed; `id`, `version`, `root`, `duration_ms`,
    `status`) and, after a placement, `data_version` with the manifest digest read back from the root (so a report says which data is in
    place) and the pack's id and version.
11. **The data step looks at the pack's packages too.** Before the pack step, the data step checks the package files of a pinned data
    set (`src/quest/data/package_pins.h`, which holds the pack's manifest digest and the length and sha-256 of its packages; ADR 0011,
    "The packages"). A missing or wrong package is a `data_packages` Warning and does not change the decision; the repair in item 5
    follows when the APK carries the pack.

### 4. Platform parity

PC reads content packs from the nevr root through a plugin that serves them (ADR 0028, `docs/reference/content-packs.md`): a different
path, host-config driven, with its own manifest kind. This record is the Quest data-store overlay and has no PC counterpart. The
release manifest's `platform` for a pack that is a Quest APK asset is a release-signer matter (an `android-arm64` value) and is not
this repository's.

## Not handled

- A newer pack over an installed older pack of the same id is `stock_mismatch` (#1614).
- A pack the stock manifests of which are unknown: `requires_stock_manifest_sha256` holds one or two digests; a third stock build needs
  a new pack.

## Not established

- That a headset gives the sentinel a readable APK path (`dladdr` form: `<apk>!/lib/...` or an extracted library directory). The
  `pack_check` line carries `apk`, and `reason=apk_path_unknown` is the answer when neither form applied.
- That the game starts on the overlaid data, and that it ignores an extra `manifests/<id>.stock` file beside its manifest.
- That `rename` over an existing file and `fsync` of the directory behave on the headset's app media storage as they do on the host.
- How long the sha-256 of a package that is hundreds of MB takes on a headset: a launch over a placed pack hashes each file once to
  say `current` (`duration_ms` on `pack_step`).

## Tests

- Host, `just test-quest-data`: `src/quest/tests/pack_test.cpp` (the step: none, current, placed, repaired, stock mismatch, no space and
  its boundary, digest mismatch, a kill between the writes, a backup never overwritten, every unusable `pack.json`, the APK path),
  `src/quest/tests/data_test.cpp` (`ReadZipEntry`, `FindZipEntry`, the extractor's refusals unchanged, `CleanPackLeftovers`).
- Host, `just test-quest-data-net`: `src/quest/tests/data_wiring_test.cpp`, the real `AfterStorage` over a scratch root with a synthetic
  APK: placed after `kUsed` and after `kFetched`, in a release build, none, stock mismatch, a failure that ends the process with
  `pack_failed`, a kill between the writes finished by the next launch, and no pack step after a failed or missing data step.
- Python, `just test-tools`: `tools/tests/test_quest_repack_pack.py` (`stage-pack`, `pack-problems`, `verify-signed` over the pack, and the
  recipe end to end with and without a pack).
