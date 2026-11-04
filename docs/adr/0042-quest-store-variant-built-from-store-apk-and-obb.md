# ADR 0042: the Quest store variant is built from the store APK and the store OBB

Status: proposed. Tracked by #2081. Builds on ADR 0007 (the game mounts an OBB), ADR 0021 (`libr15.so` is patched on the host) and
ADR 0041 (native code is built by CMake).

## Context

The Quest store variant is the stock game plus the nEVR sentinel. Its inputs are two files the store ships:

- The store APK `r15_goldmaster_store-4987566.apk` (its `lib/arm64-v8a/libr15.so` has build ID `b243509c08ce677aeb95fa348016949b3fc45230`,
  which `justfile` pins).
- The store OBB `main.4987566.com.readyatdawn.r15.obb`, the game's stock data. Horizon OS no longer mounts it (ADR 0007), so
  the data has to reach the headset another way: `/sdcard/Android/media/com.readyatdawn.r15/files/_data`, which the first-run
  data step (ADR 0011) reads.

The OBB was measured (issue #2081, evidence at `b53828e1d4e9a059e5a407ec53228b3a62179941`):

- It ends with an Android OBB footer: `[footer version 4][package version 4][flags 4][salt 8][name length 4][name]`, then
  `[footer size 4][tag 4]` with the tag `0x01059983`. Flag bit 1 (`0x2`) says the OBB is salted. The footer of the store OBB is
  43 bytes (name `com.readyatdawn.r15`); everything before it is the encrypted image.
- The game mounts it with the key string `37c70a1635a1ad7a` (`AStorageManager_mountObb`, `libr15.so` `0xf85434`).
- Android's StorageManager turns the key string into the volume key with PBKDF2-HMAC-SHA1 over the footer salt, 1024
  rounds, a 16-byte result. vold then maps the image with dm-crypt `twofish`: Twofish-128 in CBC mode with the "plain" IV, the
  512-byte sector number as a 32-bit little-endian integer, zero-padded to the block size.
- The decrypted image is FAT16 (OEM name `fat32lib`, written by Android's `jobb`), 1,847,840 sectors, holding
  `_data/5932408047/rad15/android/` with two manifests and two packages. File names longer than 8.3 are stored as long names.

A prototype in Python over the system nettle decrypted and extracted it in about ten seconds; on a Quest 2 the pushed
manifest hashed to its pin.

Before this ADR the stock data was a hand-extracted folder and the release base was a prebuilt APK (#1994). Nothing
in the build knew where the data came from, so nothing could say that two machines had the same bytes.

## Decision

1. **Two game inputs, both per-machine.** The store variant is built from the store APK and the store OBB, and from nothing
   else of the game's. Each is an environment variable with no default: `NEVR_STORE_APK` and `NEVR_STORE_OBB`. A recipe that
   needs one and finds it unset or not a file stops with a line that names the variable. Neither path, nor any other
   per-machine path, appears in the tree.
2. **Every step is a cached step.** A step's output is stored under `build/`, in a directory or a manifest keyed by the
   SHA-256 of its inputs. A second run with the same inputs finds the output, logs `cache_hit` with the key and does nothing
   else. A different input makes a different key, and the old output is left in place.

   | Step | Recipe | Inputs (the key) | Output |
   | --- | --- | --- | --- |
   | Stock data | `just quest-stock-data` | SHA-256 of the OBB | `build/quest-stock-data/<obb sha256>/` |
   | Sentinel | `just build-android` | the tree (ninja) | `build/android-arm64/sentinel/` |
   | Repack | `just quest-store-build` | the store APK, the OBB key, the sentinel library, the libr15 patcher, the repack recipe and every `tools/` file it names, the signing key's fingerprint | `build/quest-store/` |
   | Install | `just quest-store-install` | the repack's output | the headset |

3. **The extractor is a host tool.** `nevr_quest_obb_extract` is a C++17 target of the host-tools project
   (`src/quest/host`, preset `quest-patcher`). Its cryptography is nettle, installed from `tools/quest-host-deps/vcpkg.json`
   (never a system library). It:
   - reads the footer and refuses a file that is too short, has another tag, has a footer size that does not fit, has a
     footer version other than 1, is unsalted, or whose image is not a whole number of 512-byte sectors;
   - derives the key from the game's key string, which is a named constant that cites `libr15.so` `0xf85434`;
   - reads the image through a decrypting sector source (Twofish-128 CBC, one IV per sector), so the 946 MB image is never
     written to disk decrypted;
   - reads FAT16 (boot sector, FAT, root directory, long names, directories, cluster chains) and refuses FAT12, FAT32,
     a sector size other than 512, a boot sector without the `0x55AA` mark, a chain that loops, leaves the table or ends before
     the file's size, and a bad cluster;
   - writes the tree to a staging directory and renames it into place only when every file matches its pin.
4. **Pins say what the build accepts.** `tools/quest_stock_data.pin` lists the OBB's SHA-256 and size, and for every file of
   the extracted tree its SHA-256, size and relative path. A tree with a file the pins do not list, a file missing, or a
   digest that differs is refused, and the refusal names the file, the pinned value and the measured value. The pins are the
   falsifier of #2081.
5. **Every step logs structured JSON.** The extractor and the store-build and install tools write one JSON object per line
   on standard output, tag `NEVR.QUESTBUILD`, with `event`, the identifiers of the step (paths relative to the repository
   or the build tree, digests, byte counts) and the outcome. A failure is one line with `event` `refused` or `failed`, a
   `code` that names the cause and a `detail`, and the tool exits non-zero. Keys, signing passphrases and the contents of the
   data are never logged.
6. **The store build** (`just quest-store-build`) chains the stock-data step, `build-android` and `android-repack-apk` on
   `$NEVR_STORE_APK`, and collects `build/quest-store/`: the signed APK, the `_data` tree (hard links to the cache, else
   copies) and `store-build.json`, the manifest of every input and output digest. A second run whose input key equals the
   manifest's finds both outputs in place and the manifest's output digests still true, and runs nothing.
7. **The headset step** (`just quest-store-install`) is `adb install -r` of the APK, then replacing the headset's
   `/sdcard/Android/media/com.readyatdawn.r15/files/_data` with the stock `_data` and hashing the manifest on the device. It
   logs each `adb` step and refuses without exactly one attached device. It is never part of `quest-store-build`.
8. **Other projects' build outputs are not inputs.** The combat script libraries' generated sources and the combat pack
   come from other projects and are inputs of the store variant only where a recipe marks them `KNOWN-WRONG` with their open
   issue (#1737 for the pack). The store variant as this ADR defines it uses none.

## Consequences

- The stock data is a build product with a name (`<obb sha256>`) and a check (the pins), not a folder somebody extracted.
- A recipe is one command from the two inputs to an installable APK and its data.
- The extractor's units (footer, key, cipher, FAT) are host tests with synthetic images; the real OBB is checked by an
  opt-in pinned test, as the other pinned tests are.
- nettle joins the host dependency manifest. The sentinel and the PC build do not link it.

## Not claimed

- That every store OBB the game was published with uses the key string `37c70a1635a1ad7a`. The pins accept one OBB (by its
  digest); another OBB is refused before any decryption.
- How the combat pack is made from the stock data. That is its own issue (#1737).
- That the extracted data has been run on a headset by this tool. The prototype's push was measured; the first install by
  `quest-store-install` is the headset test.
- That an unsalted OBB, a FAT variant other than FAT16, or a sector size other than 512 are readable. They are refused.
- That the image digest is checked: only the files the pins list are.

## References

- `src/quest/sentinel/obb_skip_hook.cpp` and ADR 0007 for `AStorageManager_mountObb` and the key string.
- The OBB footer layout and the salted flag: the Android `ObbFile` class (`frameworks/base/libs/androidfw/ObbFile.cpp`); the
  layout above was measured on the store OBB and does not depend on that source.
- dm-crypt IV mode `plain` (the Linux kernel's `dm-crypt` documentation): the 32-bit little-endian sector number.
- Microsoft's FAT specification: the FAT type is decided by the count of data clusters (fewer than 4085 is FAT12, fewer than
  65525 is FAT16), and long names are stored as chained entries with the 8.3 entry's checksum.
- nettle: `pbkdf2_hmac_sha1`, `twofish_set_key`, `twofish_decrypt`, `cbc_decrypt`.
