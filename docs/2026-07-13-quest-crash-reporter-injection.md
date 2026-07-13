# Quest (arm64-v8a) Crash-Reporter `.so` — Injection Recon & Design

**Date:** 2026-07-13
**Author:** Spritz Metis Sprock (`spritz@sprock.io`)
**Target:** Meta Quest, `com.readyatdawn.r15` (Echo VR), arm64-v8a
**Goal:** a breakpad-grade crash reporter that loads *very early* by hijacking the
APK-bundled `libovrplatformloader.so`, forwarding the real library so the game still
boots.

All facts below are ground-truthed against the real APK-extracted binaries and the
decoded manifest — every claim carries the command that produced it. This mirrors the
Windows `BugSplat64.dll` / `LibOVRPlatform64_1.dll` static-import hijack, adapted to
ELF/Bionic where the crash reporter is statically-linked breakpad (no BugSplat `.so`
exists to replace).

---

## 1. Ground truth (measured, not assumed)

### 1.1 Binaries

APK: `r15_goldmaster_store.apk` (tethys `/srv/sprock_io/htdocs/e/echovr-prefarewell/`).
Libs extracted to
`/mnt/games/evr/-src-evr-reconstruction/cache/quest_triage/apk_contents/lib/arm64-v8a/`.

```
$ readelf -h libr15.so | grep -E 'Class|Machine|Type'
  Class:   ELF64
  Type:    DYN (Shared object file)
  Machine: AArch64
$ readelf -h libovrplatformloader.so | grep -E 'Class|Machine|Type'
  Class:   ELF64
  Type:    DYN (Shared object file)
  Machine: AArch64
$ readelf -d libovrplatformloader.so | grep -i soname
  0x0e (SONAME)  Library soname: [libovrplatformloader.so]
```

`libr15.so` (ReVault binary id 576) is AArch64. `libovrplatformloader.so` is AArch64,
soname `libovrplatformloader.so`, 831 KB.

### 1.2 The load chain — why a `libovrplatformloader.so` constructor runs "very early"

Decoded `AndroidManifest.xml` (via `androguard axml`):

```
package="com.readyatdawn.r15"
<uses-sdk android:minSdkVersion="29" android:targetSdkVersion="29"/>
<activity android:name="com.oculus.gles3jni.MainActivity" android:launchMode="2" ...>
  <meta-data android:name="android.app.lib_name" android:value="r15"/>
```

The activity is a `NativeActivity`-style entry with `android.app.lib_name = "r15"`.
Android's `NativeActivity`/`android_native_app_glue` bootstrap calls
`System.loadLibrary("r15")` → `dlopen("libr15.so")` as the **first** native code the
process touches.

`dlopen("libr15.so")` forces the dynamic linker to load `libr15.so`'s entire
`DT_NEEDED` closure **first**, and Bionic runs each dependency's ELF constructors
(`DT_INIT` / `.init_array`, and `JNI_OnLoad` when the lib is `System.loadLibrary`'d)
**before** the depending library's own code. `libovrplatformloader.so` is in that
closure:

```
$ readelf -d libr15.so | grep -i needed
 (NEEDED) [libandroid.so]
 (NEEDED) [libEGL.so]
 (NEEDED) [libGLESv3.so]
 (NEEDED) [libOpenSLES.so]
 (NEEDED) [liblog.so]
 (NEEDED) [libvrapi.so]
 (NEEDED) [libovrplatformloader.so]   <-- our injection point
 (NEEDED) [libdeviceconfigclient-jni.so]
 (NEEDED) [libc++_shared.so]
 (NEEDED) [libm.so]  [libdl.so]  [libc.so]
```

**Therefore:** a `__attribute__((constructor))` inside a library that carries the name
`libovrplatformloader.so` is executed as part of resolving `libr15.so`'s dependencies —
i.e. **before `libr15.so`'s `JNI_OnLoad` / `ANativeActivity_onCreate`, before any game
code runs.** That is the earliest hook point reachable without repacking `libr15.so`
itself. This is the direct ELF/Bionic analogue of the Windows trick where the game
statically imports `BugSplat64.dll`, so it loads before `WinMain`.

> On-device confirmation item: emit a `__android_log_print` line with a monotonic
> timestamp from our constructor and confirm via `logcat` that it precedes libr15's
> first log line. Structurally the ordering is guaranteed by the linker; the timestamp
> is the runtime proof.

### 1.3 Who actually *uses* ovrplatformloader's exports (the forwarding requirement)

This is the crucial, non-obvious fact. `libr15.so` `DT_NEEDED`s
`libovrplatformloader.so` but binds **zero** symbols from it at link time — the NEEDED
is purely a side-effect load (it pulls the platform SDK + its `JNI_OnLoad` into the
process):

```
$ nm -D -u libr15.so | grep -c '^ovr'      # undefined ovr* in libr15
0
$ comm -12 <exports of ovrplatformloader> <undefined syms of libr15>
0
```

The real consumer is **`libpnsovr.so`** (the Oculus Platform social layer), which binds
**all 175** of its undefined `ovr_*` symbols directly from `libovrplatformloader.so`:

```
$ nm -D -u libpnsovr.so | grep -c '^ovr'                          # 175
$ comm -12 ovrpl_exports.txt pnsovr_undef_ovr.txt | wc -l         # 175  (all resolve)
$ readelf -d libpnsrad.so           | grep ovrplatform            # NEEDED (0 ovr syms)
$ readelf -d libpnsradmatchmaking.so| grep ovrplatform            # NEEDED (0 ovr syms)
```

`libovrplatformloader.so` exports **1423** dynamic symbols total (1154 `ovr_*` C
functions + a `JNI_OnLoad`, `setupUnityShim`, a `NativeJava`/`OVRPlatformException`
C++ helper surface, etc.). The 175 that `libpnsovr.so` resolves at link time are the
**hard requirement**: if they don't resolve, `libpnsovr.so` fails to load and the game
never boots.

Example resolved symbols (from `libpnsovr.so`):
`ovr_GetLoggedInUserID`, `ovr_FreeMessage`, `ovr_Error_GetMessage`,
`ovr_ApplicationLifecycle_GetLaunchDetails`, `ovr_GroupPresence_LaunchInvitePanel`,
`ovr_DataStore_GetValue`, `ovr_Entitlement_GetIsViewerEntitled`, …

---

## 2. Injection design — forward-to-renamed-original

### 2.1 Layout

```
Original (renamed):  libovrplatformloader_orig.so   (real Oculus Platform loader, 831 KB)
Our shim (the hook): libovrplatformloader.so        (small; arms breakpad, forwards)
```

Consumers (`libpnsovr.so`, `libpnsrad.so`, `libpnsradmatchmaking.so`, `libr15.so`)
still `DT_NEEDED "libovrplatformloader.so"` — **unchanged**. They now load *our* shim.

### 2.2 How forwarding works on Bionic — WITHOUT 1423 hand-written trampolines

Two mechanisms combine, and we rely on both:

1. **Our shim `DT_NEEDED`s `libovrplatformloader_orig.so`.** That guarantees the real
   loader is pulled into the process (nothing else references the renamed name).

2. **Bionic resolves undefined symbols across the whole *local group*.** When
   `libr15.so` is `dlopen`'d, its entire dependency closure — `libpnsovr.so`, our shim,
   `libovrplatformloader_orig.so`, everything — is loaded as **one local group**, and
   Bionic's symbol lookup for any library in that group searches every member of the
   group. So when `libpnsovr.so` needs `ovr_GetLoggedInUserID`, the linker finds the
   real definition in `libovrplatformloader_orig.so` (same group). **No re-export or
   trampoline is required for the 175 (or all 1423) symbols.**

This is the elegant path: the shim is a *thin* library whose only jobs are (a) arm the
crash handler in its constructor, and (b) `DT_NEEDED` the renamed original so it comes
along. It re-exports nothing and does not need to enumerate the 1423 symbols.

### 2.3 Fallback if group-scope resolution surprises us on-device

Bionic's group-lookup semantics have shifted across Android versions (namespaces,
`RTLD_LOCAL` for `System.loadLibrary`). If, on-device, `libpnsovr.so` fails to resolve
`ovr_*` through the group, the fallback is **explicit re-export forwarders**:

- In the shim constructor, `dlopen("libovrplatformloader_orig.so", RTLD_NOW|RTLD_GLOBAL)`.
- For every one of the 1423 exported symbols, ship a same-named exported trampoline
  that tail-jumps to the `dlsym`'d original. These are code-generated from
  `nm -D --defined-only libovrplatformloader_orig.so` (a Go/CMake codegen step, not
  hand-written), producing naked `B` (branch) thunks for the C `ovr_*` surface. C++
  symbols (`_ZN10NativeJava…`) are only needed if a consumer binds them — none of the
  three consumers do (all 175 are `ovr_*`), so the C surface is sufficient.

The primary design (2.2) is preferred and tried first; the codegen forwarder is the
documented safety valve. Which one is needed is an **on-device determination**.

### 2.4 Repack / deploy (documented, NOT executed — no prod deploy)

The on-device install (out of scope for this foundation; requires a headset) is:

```
# rename the real loader + fix its soname so the NEEDED string matches the file
patchelf --set-soname libovrplatformloader_orig.so \
         --output libovrplatformloader_orig.so  libovrplatformloader.so
# drop our shim in as libovrplatformloader.so, repack + re-sign the APK, sideload
```

A `just` recipe (`android-repack-libovr`, added with the build wiring) scripts the
`patchelf` rename against a copy so we never mutate the source-of-truth extract.

---

## 3. The crash reporter itself

- **Vendored google_breakpad** (the *same* reporter statically linked into `libr15.so` —
  `NRadEngine::CDebugCrashReport` + google_breakpad), built for arm64-v8a via the NDK.
  Real minidumps, not a bespoke writer.
- **Armed in a load-time constructor** (`__attribute__((constructor))`) *and* `JNI_OnLoad`,
  so `ExceptionHandler` is installed before `libr15` runs (§1.2).
- Handles **SIGSEGV / SIGABRT / SIGBUS / SIGILL / SIGFPE**. On crash it writes:
  1. a real breakpad **minidump** (`.dmp`),
  2. `/proc/self/maps` (module layout for offline symbolication),
  3. a best-effort **backtrace** to the platform log (`__android_log_print`, tag
     `NEVR-Sentinel`).
- Output path: app-writable
  `/sdcard/Android/data/com.readyatdawn.r15/files/nevr-crashes/` with a fallback to
  `/data/data/com.readyatdawn.r15/files/` and finally the module dir. Path comes from
  config/compile-def, **never an environment variable** (repo rule).
- Structure mirrors the Windows `nevr-sentinel-dll` (`sentinel.h`/`sentinel.cpp` +
  entry glue), adapted from SEH/`SetUnhandledExceptionFilter` to POSIX signals +
  breakpad `ExceptionHandler`.

---

## 4. Toolchain choice — NDK r26d (26.3.11579264)

Already present at `/home/andrew/src/android-ndk-r26d` (clang 17.0.2), so no download.

**Why r26d over the r21b named as the compat floor:**

- The **arm64-v8a AAPCS64 ELF ABI is stable across all NDKs** — the machine code a
  modern clang emits is interchangeable with an r21b-built lib at the linkage boundary.
  ABI compatibility is not an argument for an old NDK here.
- The only real cross-NDK compat risks are (a) minSdk floor baked into the ELF, (b) the
  libc++ ABI, (c) newer relocation formats (RELR/packed) an old *loader* might reject.
  We neutralize all three:
  - **Target `android-26`** (`minSdkVersion=26`) — comfortably below the app's actual
    runtime floor (`minSdkVersion=29`, Quest OS = Android 10 / API 29), so the OS loader
    accepts us, and we don't demand anything newer than the app already requires.
  - **Static libc++** (`-static-libstdc++`, libc++_static) — breakpad is C++; static
    linking means we do **not** depend on the app's bundled `libc++_shared.so` version,
    eliminating the one genuine ABI-drift hazard that would otherwise force us to match
    the game's exact (r21-era) NDK.
  - Quest's API-29 loader supports RELR + Android packed relocations (added API 28), so
    r26d's defaults load fine.
- r21b is EOL (clang 9); r26d is the current maintained line with security/codegen
  fixes. Choosing it costs nothing on compatibility and buys a supported toolchain.

Net: **NDK r26d, `ANDROID_ABI=arm64-v8a`, `ANDROID_PLATFORM=android-26`, static
libc++.**

---

## 5. Build integration

The repo root `CMakeLists.txt` is hard-wired to the MinGW/MSVC + vcpkg toolchain from
line 1 (it `unset`s `VCPKG_ROOT`, rewrites `message()`, and every `add_subdirectory`
targets Windows DLLs). Forcing an Android target through it would fight all of that.

Following the precedent of `nevr-sentinel-dll` (its own standalone CMake project), the
Android target lives in a **self-contained sub-project** at `src/quest/` with its own
`CMakePresets.json` (`android-arm64` preset → NDK `android.toolchain.cmake`). Root
`justfile` recipes drive it (`configure-android`, `build-android`, `test-android`),
mirroring the `mingw-*` recipe shape so the UX is consistent.

---

## 6. Test strategy (BAC / TDD)

On-device runtime needs a headset, so the automatable ground truth is the **ELF shape
of the built `.so`**. BACs:

- **BAC-1** the build produces `libovrplatformloader.so`, an `ELF64 … AArch64` shared
  object.
- **BAC-2** it exports the sentinel marker `nevr_sentinel_marker` (proof our code is in
  the artifact).
- **BAC-3** it `DT_NEEDED`s `libovrplatformloader_orig.so` (proof the forwarding-load of
  the renamed original is wired).
- **BAC-4** its soname is `libovrplatformloader.so` (proof it takes the hijacked name).
- **BAC-5** it installs `JNI_OnLoad` and a constructor (`.init_array` non-empty).

Test runs **RED first** (asserts against a not-yet-built artifact → fails), then
**GREEN** after `build-android`. Implemented as a Go ground-truth test under
`tests/quest/` in the repo's existing Go-test idiom, shelling to `readelf`/`nm` on the
real output.

**Remains for on-device (named, not hidden):** load-order timestamp vs libr15;
group-scope symbol resolution for `libpnsovr`'s 175 imports (else engage §2.3 codegen
forwarders); a real signal → real minidump written to the app-writable path;
minidump symbolication against `libr15.so`.
