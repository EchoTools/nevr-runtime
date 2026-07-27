# Quest 2 Sideload Runbook — NEVR breakpad crash-reporter

**Date:** 2026-07-13
**Author:** Spritz Metis Sprock (`spritz@sprock.io`)
**Target:** Meta Quest 2, `com.readyatdawn.r15` (Echo VR), arm64-v8a
**Goal:** put the verified crash-reporter `.so` on the headset and confirm it works.

This is the step-by-step procedure to repack the store APK so our shim hijacks
`libovrplatformloader.so`, sideload it onto a Quest 2, and confirm the breakpad crash
reporter arms before the game and writes a real minidump on crash.

**Status split:**

- **Part A — Packaging (PROVEN, headless).** Every command below was executed on
  spriffy against the real store APK; the ELF-shape proof is quoted from the *signed*
  output. Reproduce with one recipe: `just android-repack-apk <apk>`.
- **Part B — On-device (ONLY Andrew-with-headset can close).** Structurally sound but
  not yet exercised on hardware. Clearly marked. These steps confirm load order,
  symbol resolution, and a real minidump.

The design rationale (why hijacking `libovrplatformloader.so` runs our constructor
before any game code, and the forward-to-renamed-original mechanism) is in
`docs/design/2026-07-13-quest-crash-reporter-injection.md`. This runbook is the how-to.

---

## The two artifacts

| File | Role |
| ---- | ---- |
| `libovrplatformloader.so` (our shim, from `build/android-arm64/sentinel/`) | Takes the hijacked name. Arms breakpad in its ELF constructor, then forwards everything to the renamed original via `DT_NEEDED`. |
| `libovrplatformloader_orig.so` (renamed real loader) | The genuine Oculus Platform loader, extracted from the store APK, soname-patched so its filename and `SONAME` match. Supplies the 170 `ovr_*` symbols `libpnsovr.so` needs. |

The shim already carries `NEEDED libovrplatformloader_orig.so` and `SONAME
libovrplatformloader.so` from its build — the repack only has to rename + soname-patch
the original and pack both in.

---

## Prerequisites — tools

Verified present on spriffy (2026-07-13):

| Tool | Path / version | Used for |
| ---- | -------------- | -------- |
| `adb` | `/opt/android-sdk/platform-tools/adb` — 1.0.41 | install, logcat, pull |
| `unzip` / `zip` | Info-ZIP 6.00 | extract / repack APK entries |
| `patchelf` | 0.19.1 | rename original + set soname |
| `zipalign` | Android build-tools r37.0.0 | page-align `.so` entries |
| `apksigner` | Android build-tools r37.0.0 — 0.9 | v2/v3 re-sign |
| `keytool` | JDK (`/usr/bin/keytool`) | generate the debug keystore |
| `readelf` / `nm` | binutils | ELF verification |

**Had to be installed** (were missing): `apksigner`, `zipalign`, `aapt`/`aapt2`.
On Arch they ship together in the AUR package **`android-sdk-build-tools`**:

```bash
yay -S android-sdk-build-tools     # installs apksigner, zipalign, aapt, aapt2 under /opt/android-sdk/build-tools/
```

**Optional (Part B symbolication):** `minidump_stackwalk` is **not installed**. It is
built from the vendored breakpad processor (`extern/breakpad`, host
build) — not required to *capture* a dump, only to stackwalk one offline. See Part B.5.

**Source APK:** the Quest store build lives on tethys at
`/srv/sprock_io/htdocs/e/echovr-prefarewell/r15_goldmaster_store.apk` (96 MB). Pull a
copy first:

```bash
scp tethys:/srv/sprock_io/htdocs/e/echovr-prefarewell/r15_goldmaster_store.apk /var/tmp/
```

---

## Part A — Packaging (proven, headless)

### A.1 One-shot: the `just` recipe

Everything below (A.2) is wrapped in a reusable recipe. Build the shim first, then repack:

```bash
just build-android                                          # produces build/android-arm64/sentinel/libovrplatformloader.so
just android-repack-apk /var/tmp/r15_goldmaster_store.apk   # extract, patchelf, inject, align, sign, verify
```

The recipe extracts the real loader from the APK, renames + soname-patches it to
`libovrplatformloader_orig.so`, drops the shim in as `libovrplatformloader.so`, strips
the store's v1 signature, injects both libs **stored** (uncompressed), `zipalign`s, and
re-signs with a generated debug keystore — then verifies the two libs' ELF shape from
the *signed* output. It never mutates the source APK or the shim; all work is under
`build/android-arm64/repack/`. Output: `build/android-arm64/repack/r15_nevr-sentinel_signed.apk`.

### A.2 The underlying steps (what the recipe runs)

```bash
WORK=build/android-arm64/repack; mkdir -p "$WORK/stage/lib/arm64-v8a"
APK=/var/tmp/r15_goldmaster_store.apk
SHIM=build/android-arm64/sentinel/libovrplatformloader.so

# 1. extract the real original loader from the store APK
unzip -o -q "$APK" lib/arm64-v8a/libovrplatformloader.so -d "$WORK/orig"

# 2. rename original + patch its soname so the NEEDED string matches the file on disk
patchelf --set-soname libovrplatformloader_orig.so \
    --output "$WORK/stage/lib/arm64-v8a/libovrplatformloader_orig.so" \
    "$WORK/orig/lib/arm64-v8a/libovrplatformloader.so"

# 3. drop our shim in under the hijacked name
cp "$SHIM" "$WORK/stage/lib/arm64-v8a/libovrplatformloader.so"

# 4. fresh APK copy; strip the store's v1 signature; inject both libs STORED (-0)
cp -f "$APK" "$WORK/r15-repacked.apk"
zip -q -d "$WORK/r15-repacked.apk" 'META-INF/*.RSA' 'META-INF/*.SF' 'META-INF/*.MF' || true
( cd "$WORK/stage" && zip -q -0 -X "$OLDPWD/$WORK/r15-repacked.apk" \
    lib/arm64-v8a/libovrplatformloader.so lib/arm64-v8a/libovrplatformloader_orig.so )

# 5. generate a throwaway DEBUG keystore (sideload only — NOT production signing)
keytool -genkeypair -v -keystore "$WORK/debug.keystore" -alias nevrdebug \
    -keyalg RSA -keysize 2048 -validity 10000 \
    -storepass android -keypass android \
    -dname "CN=NEVR Debug, OU=nevr-runtime, O=Sprock, C=US"

# 6. page-align uncompressed .so (align BEFORE signing), then re-sign v2/v3
zipalign -p -f 4 "$WORK/r15-repacked.apk" "$WORK/r15-repacked-aligned.apk"
apksigner sign --ks "$WORK/debug.keystore" --ks-key-alias nevrdebug \
    --ks-pass pass:android --key-pass pass:android \
    --out "$WORK/r15_nevr-sentinel_signed.apk" "$WORK/r15-repacked-aligned.apk"
apksigner verify --print-certs "$WORK/r15_nevr-sentinel_signed.apk"
```

Signer output (confirms v2/v3 scheme, our debug cert):

```
V3.0 Signer: certificate DN: CN=NEVR Debug, OU=nevr-runtime, O=Sprock, C=US
```

### A.3 Proof — ELF shape of both libs, read from the SIGNED APK

This is the headless proof that the packaging is sound. Extract the two libs *back out*
of the signed APK and read their dynamic sections:

```bash
unzip -o -q "$WORK/r15_nevr-sentinel_signed.apk" 'lib/arm64-v8a/libovrplatformloader*.so' -d "$WORK/verify"
readelf -d "$WORK/verify/lib/arm64-v8a/libovrplatformloader.so"      | grep -iE 'soname|needed'
nm -D     "$WORK/verify/lib/arm64-v8a/libovrplatformloader.so"       | grep nevr_sentinel_marker
readelf -d "$WORK/verify/lib/arm64-v8a/libovrplatformloader_orig.so" | grep -i soname
```

Actual output (2026-07-13, from the real store APK):

```
== shim  lib/arm64-v8a/libovrplatformloader.so ==
 (NEEDED)  Shared library: [libovrplatformloader_orig.so]     <-- forwards to renamed original
 (NEEDED)  Shared library: [liblog.so]
 (NEEDED)  Shared library: [libdl.so]
 (NEEDED)  Shared library: [libm.so]
 (NEEDED)  Shared library: [libc.so]
 (SONAME)  Library soname: [libovrplatformloader.so]          <-- takes the hijacked name
0000000000016880 T nevr_sentinel_marker                       <-- our code is in the artifact

== renamed original  lib/arm64-v8a/libovrplatformloader_orig.so ==
 (SONAME)  Library soname: [libovrplatformloader_orig.so]     <-- soname patched to match new filename
```

Supporting counts (measured on the extracted originals):

- The renamed original keeps all **1420** FUNC/OBJECT exports, including
  `ovr_GetLoggedInUserID` (one of the 170 `ovr_*` symbols `libpnsovr.so` binds).
- `libpnsovr.so` binds **170** `ovr_*` symbols; all resolve against the renamed original
  through Bionic's local-group lookup (design doc §1.3, §2.2).

**Packaging is proven.** What a headset still has to confirm is *runtime* behavior:
load order, that group-scope resolution actually satisfies `libpnsovr` on-device, and a
real minidump. That is Part B.

---

## Part B — On-device (ONLY closeable by Andrew with the Quest 2)

Everything here needs the headset in Developer Mode, connected over USB (or `adb tcpip`).
None of it has been run yet — it is the remaining verification.

### B.1 Connect + install

```bash
adb devices                 # confirm the Quest shows as 'device' (not 'unauthorized')
adb install -r build/android-arm64/repack/r15_nevr-sentinel_signed.apk
```

`-r` reinstalls over the existing app, keeping data. If install is refused with a
signature mismatch, the store build is already installed with a different signer —
`adb uninstall com.readyatdawn.r15` first (this loses app data), then `adb install`.

### B.2 Arm a logcat capture, then launch

In one shell, start filtered logcat *before* launching so the earliest lines are caught:

```bash
adb logcat -c                                   # clear the buffer
adb logcat NEVR-Sentinel:V DEBUG:V AndroidRuntime:E '*:S' > /var/tmp/nevr-boot.log &
adb shell monkey -p com.readyatdawn.r15 -c android.intent.category.LAUNCHER 1
```

### B.3 Confirm the crash reporter armed BEFORE the game (load order)

In `/var/tmp/nevr-boot.log`, expect the sentinel's own line, tagged `NEVR-Sentinel`,
**before** any libr15 game-init line:

```
I NEVR-Sentinel: armed: breakpad ExceptionHandler, dumps -> /sdcard/Android/data/com.readyatdawn.r15/files/nevr-crashes
```

Checks that must all hold:

- **(a)** the `NEVR-Sentinel: armed ...` line appears, and its timestamp **precedes**
  libr15's first log line (structurally guaranteed by the linker; the timestamp is the
  runtime proof).
- **(b)** NO `UnsatisfiedLinkError` and NO `cannot locate symbol ovr_...` /
  `cannot locate symbol ovrID_...` anywhere in the log. Such a line means group-scope
  resolution did **not** satisfy `libpnsovr`'s 170 imports on this OS build → engage the
  codegen re-export fallback (design doc §2.3) and repack.
- **(c)** the app reaches its normal main menu (didn't crash on boot).

Confirm both libs are actually mapped into the running process:

```bash
PID=$(adb shell pidof com.readyatdawn.r15)
adb shell run-as com.readyatdawn.r15 cat /proc/$PID/maps | grep -E 'libovrplatformloader(_orig)?\.so'
# expect BOTH libovrplatformloader.so (shim) AND libovrplatformloader_orig.so mapped
```

(If `run-as` is denied on a non-debuggable build, use `adb shell cat /proc/$PID/maps`
with a rooted/dev device, or `adb shell dumpsys meminfo com.readyatdawn.r15` to list
loaded natives.)

### B.4 Trigger a crash → confirm a minidump is written

The reporter handles SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE/SIGTRAP (breakpad's 6). The
cleanest deliberate trigger is a signal to the running process:

```bash
PID=$(adb shell pidof com.readyatdawn.r15)
adb shell kill -SIGSEGV $PID          # or -SIGABRT
```

Then confirm, in logcat, BOTH:

- our reporter fired — a `NEVR-Sentinel` dump line / backtrace, and
- the OS tombstone fired afterwards — a `DEBUG   : *** *** ***` /
  `Tombstone written to: /data/tombstones/...` block (we are *additive*: the callback
  returns false so the OS default handler still runs).

Confirm the minidump landed in the app-writable dir:

```bash
adb shell run-as com.readyatdawn.r15 ls -la files/nevr-crashes/
# or, if that dir isn't writable on-device, the fallbacks (design doc §3):
adb shell ls -la /sdcard/Android/data/com.readyatdawn.r15/files/nevr-crashes/
```

### B.5 Pull + inspect the minidump

```bash
adb pull /sdcard/Android/data/com.readyatdawn.r15/files/nevr-crashes/<uuid>.dmp /var/tmp/
file /var/tmp/<uuid>.dmp        # expect: "Mini DuMP crash report" / minidump magic 'MDMP'
```

Offline stackwalk (needs `minidump_stackwalk`, not currently installed — build it from
the vendored breakpad processor on the host):

```bash
# build once, on the host:
#   cd src/quest/third_party/breakpad && ./configure && make -j src/processor/minidump_stackwalk
minidump_stackwalk /var/tmp/<uuid>.dmp <symbols-dir>
```

Symbolication needs breakpad `.sym` files for `libr15.so` + our libs (generate with
breakpad's `dump_syms` against the unstripped builds). Without symbols the stackwalk
still shows module + offset, which is enough to confirm the frame is inside libr15.

---

## What proves "done"

- **Packaging (done):** A.3 — shim has SONAME `libovrplatformloader.so` + NEEDED
  `libovrplatformloader_orig.so` + the marker; the renamed original has soname
  `libovrplatformloader_orig.so` and keeps its 1420 exports; the APK is aligned + signed
  and `apksigner verify` passes.
- **On-device (remaining):** B.3 load-order + no-UnsatisfiedLinkError + both libs mapped;
  B.4 a real minidump written on a deliberate crash + the OS tombstone; B.5 the dump
  pulls and reads as a valid minidump.

If B.3 surfaces an `UnsatisfiedLinkError`/`cannot locate symbol ovr_*`, that is the one
expected failure mode — the group-scope symbol resolution assumption (design doc §2.2)
lost on this OS build; the documented fix is the §2.3 codegen re-export forwarders, then
re-run Part A.
