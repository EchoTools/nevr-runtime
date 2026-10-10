# ADR 0006: Quest hardware and environment dump

Status: accepted, implemented (`src/quest/diag/`, #335). Off by default.

## Context

To diagnose Quest-specific behaviour we need to know two things.

1. Everything the game process can read about the headset and its environment.
2. What libr15 itself asks the system for.

The sentinel already runs inside that process (`src/quest/sentinel/entry.cpp`) and can install GOT hooks on
libr15's imports (`sentinel/callback_thunk.h`).

libr15 renders through Vulkan via VrApi, not GLES:
- `vrapi_Initialize` is passed `GraphicsAPI` `0x40100`, built at `0x1a6efd4`/`0x1a6efe0` in `FUN_01a6eefc`.
- It imports `vrapi_CreateSystemVulkan` and the `vrapi_*Vulkan` extension calls.
- It has no `gl*`, `egl*` or `vk*` imports, even though `libEGL.so` and `libGLESv3.so` are NEEDED.
- It does not use OpenXR: the APK ships no loader, and libr15 has no `xr*` import.

## Decision

**The feature flag.** `features.hwdump` in `nevr-quest.json`, row `hwdump` of `kFeatures` in `sentinel/quest_config.cpp`. It defaults off and needs no other feature.

**The ctor step.** The constructor step `install_hwdump` (`integration/ctor_sequence.cpp`) runs last. When the flag is off it is skipped with `hwdump_off`. It calls `hwdump::StartHwDump`, which:
1. installs 18 GOT hooks on libr15's own system queries (part B);
2. starts one detached thread, `nevr-hwdump`, that writes the dump (part A).

It registers no reporter counters. Each thunk's own `Calls()`/`Faults()` go into the file instead.

### Part B: the hooks

The handlers are in `diag/hwdump_handlers.cpp`, built `-fno-exceptions` and personality-free. Each one:
1. calls the original with the arguments it received;
2. records `(function, id or name)` with a call count and the first and last answers in a fixed lock-free table (`diag/hwdump_records.cpp`, 128 entries; the overflow is counted);
3. returns the original's result unchanged.

All slots are JUMP_SLOTs in the pinned libr15 build `b243509c…`. libr15 is `BIND_NOW`.

The prototypes are the register use at every one of the 48 `bl`/`b` call sites of these imports in libr15:

| Import | Slot | Prototype as libr15 calls it | Evidence |
|---|---|---|---|
| `__system_property_get` | `0x36ee730` | `int (const char*, char* /*92-byte buffer*/)` | `0xf874ac`, `0xf87500`, `0xf87600` |
| `sysconf` | `0x36e4de0` | `long (int)` | `0xfba950`, `0xfe3288`, `0xfe34f0`, `0x10fcdd4` |
| `uname` | `0x36f25a0` | `int (struct utsname*)` | `0xfba9e0`, `0xfcd61c` |
| `gethostname` | `0x36e3bf8` | `int (char*, size_t)` | `0xf98aec` and `0xf98bd4` (len 0x40); `0x10351e0` (caller's len) |
| `AConfiguration_getLanguage` | `0x36fc3b0` | `void (AConfiguration*, char* /*2 bytes, no NUL*/)` | `0xf78bd4`, `0xf799c0` |
| `AConfiguration_getCountry` | `0x36dd9c8` | `void (AConfiguration*, char* /*2 bytes, no NUL*/)` | `0xf78be0`, `0xf799cc` |
| `vrapi_Initialize` | `0x36d69f8` | `int (const ovrInitParms* /*0x30 bytes*/)` | `0x1a6f010`. The struct is built at `0x1a6eff8`–`0x1a6f00c` |
| `vrapi_GetSystemPropertyInt` | `0x36fa728` | `int (const ovrJava*, int id)`. IDs 0, 5, 6, 7, 8, 0xf, 0x40 | 8 sites, `0x1a6f024`–`0x1a71cec` |
| `vrapi_GetSystemPropertyFloat` | `0x36c06f8` | `float (const ovrJava*, int id)`; result in `s0`. ID 4 only, **per frame** (hand-velocity divisor) | 11 sites, `0x1a6ff84`–`0x1a72034` |
| `vrapi_GetSystemPropertyFloatArray` | `0x36dffa0` | `int (const ovrJava*, int id, float*, int count)`. ID 0x41; count from ID 0x40, bounded to 1..31 | `0x1a71d28` |
| `vrapi_GetSystemStatusInt` | `0x36fad60` | `int (const ovrJava*, int id)`. ID 1, per frame | `0x1a71ed4` |
| `vrapi_GetPropertyInt` | `0x36c1988` | `int (const ovrJava*, int id, int* out)`; bit 0 of `w0` says `*out` is set. ID 0x20 | `0x1a70ca8` |
| `vrapi_SetPropertyInt` | `0x36ffcf0` | `int (const ovrJava*, int id, int value)`; the result is unused at the `bl` sites | 5 sites |
| `vrapi_SetDisplayRefreshRate` | `0x36ec5f0` | `int (ovrMobile*, float rate)`; rate in `s0` | `0x1a71de8` |
| `vrapi_SetClockLevels` | `0x36c75f0` | `int (ovrMobile*, int cpu, int gpu)`; called with (5,5) or (2,2) | `0x1a71cc8` |
| `vrapi_SetExtraLatencyMode` | `0x36fdec0` | `int (ovrMobile*, int mode)`; called with 1 | `0x1a71e50` |
| `vrapi_GetInstanceExtensionsVulkan` | `0x36c9d60` | `int (char* names, uint32_t* size /*in/out*/)` | `0x1a73e00` |
| `vrapi_GetDeviceExtensionsVulkan` | `0x36dd440` | `int (char* names, uint32_t* size /*in/out*/)` | `0x1a73dd4` |

**`ovrJava` and `ovrInitParms`.**
- `ovrJava` is `{JavaVM* Vm; JNIEnv* Env; jobject ActivityObject}`. `InitInternal` (`0x1a6eda8`) fills libr15's copy at `0x37a3550` from `ANativeActivity+0x8` (vm) and `+0x18` (clazz, a global reference).
- `ovrInitParms` carries that same `ovrJava` at `+0x18`.
- The hooks keep only `Vm` and `ActivityObject`. `Env` belongs to the calling thread.

**Not hooked:**
- `fopen` of `/proc/cpuinfo` in `CSysInfo::GetNumberOfProcessors`: `fopen` is libr15's file I/O path. Part A reads the same file.
- The JNI `Build.SERIAL` read in `CSysInfo::SerialNumber`: a JNI field read, not an import. Part A reads every `Build` field.
- `getifaddrs` in `CSysNet`: the answer is a linked list the game frees.

### Part A: the dump thread

**When it runs.** 30 s after `vrapi_Initialize` is seen, or 180 s after the sentinel starts if it never is (`diag/hwdump_report.cpp`, `Decide`). The trigger reason is logged and written to the file.

**Its JNI use** (`JniSession`, `diag/hwdump_android.cpp`):
- It attaches itself and detaches on every path.
- Its context is the `Application` of the game's activity, held as its own global reference.
- Every read runs in a local frame. Any pending exception is cleared and becomes that field's error.

**It writes the file three times:**
1. Stage `os`: jni, os, ndk, Vulkan, VrApi and the libr15 queries.
2. Stage `gl`: an own GLES 3 context on a 1x1 pbuffer, current only on this thread. Teardown is `eglMakeCurrent(NO_CONTEXT)`, destroy, `eglReleaseThread`, never `eglTerminate`.
3. Stage `openxr`.

The GLES and OpenXR stages each run after a complete file is already on disk, because they load runtimes the game itself does not use. Each stage logs a `begin` line first.

**VrApi reads** go through libvrapi with this thread's own `ovrJava` (the game's `Vm` and activity, this thread's `Env`). They use only the IDs libr15 uses, through the getter libr15 uses for each, and only after `vrapi_Initialize` returned 0.

**Libraries.** Vulkan, EGL, GLES, libandroid, libvrapi and the OpenXR loader are reached with `dlopen`/`dlsym`. `TestSentinelNeededList` (`tests/quest`) pins the sentinel's DT_NEEDED list.

### The file

`/sdcard/Android/data/com.readyatdawn.r15/files/nevr-hwdump.json`:
- Written atomically with `quest/auth/atomic_write`.
- Pulled with `adb pull`.
- Never sent anywhere.

It contains device identifiers, and says so in its `notice` field: `ro.serialno` when readable, `android_id`, the Wi-Fi MAC, IP addresses, and Bluetooth audio device addresses. The app targets API 29, so Android 11's MAC hiding for API 30+ apps does not apply to it.

Every leaf is a field:
- `{"ok":true,"source":"<api>","value":…}` for a read that worked;
- `{"ok":false,"source":"<api>","error":"…"}` for one that failed.

A failed read is never an omitted field. `FixedOsFieldPaths` lists the paths the `os` section always writes. Top-level keys:
- `schema`, `stage`, `trigger`, `written_unix_ms`, `libr15_build_id`, `notice`
- `jni`, `os`, `ndk`, `gpu.vulkan`, `gpu.gl`, `vrapi`, `openxr`, `libr15_queries`

### The log

One line per write, carrying counts and the path, never a value:

`hwdump written stage=<s> path=<p> fields=<n> failed=<m> hooks_installed=<k>/18 hook_faults=<f> records=<r> overflow=<o>`

Every failure path has its own line:
- the hook install count (Warn below 18);
- `hwdump thread not started: <strerror>`;
- `hwdump write_failed … error=…`;
- `hwdump trigger reason=fallback_no_vrapi_initialize` (Warn);
- `hwdump jni unavailable`;
- `hwdump failed: c++ exception …`.

## Fields and their sources

| Path | Source |
|---|---|
| `os.properties` | `__system_property_foreach` + `__system_property_read_callback`: every property the app can read |
| `os.properties_wanted.<name>` | `__system_property_find` + `read_callback` for 30 named properties (`WantedProperties`) |
| `os.uname`, `os.hostname`, `os.kernel_version` | `uname`, `gethostname`, `/proc/version` |
| `os.cpu.cpuinfo_raw`, `os.cpu.sysconf`, `os.cpu.hwcap`, `os.cpu.sys.*` | `/proc/cpuinfo`, `sysconf(_SC_NPROCESSORS_*)`, `getauxval(AT_HWCAP/2)`, `/sys/devices/system/cpu/{present,possible,online,kernel_max}` |
| `os.cpu.cpufreq` | `/sys/devices/system/cpu/cpuN/{cpufreq/*,online,topology/*}` |
| `os.memory.*` | `/proc/meminfo`, `/proc/self/status`, `sysconf(_SC_PHYS_PAGES/_SC_AVPHYS_PAGES/_SC_PAGESIZE)` |
| `os.storage_statvfs.<path>` | `statvfs` of `/`, `/data`, `/sdcard`, the files dir and every app dir from `jni.storage_dirs` |
| `os.net.ifaddrs` | `getifaddrs`: name, flags, family, address (MAC for `AF_PACKET`), netmask |
| `os.net.sys_class_net` | `/sys/class/net/<if>/{address,mtu,operstate,type,speed}` |
| `os.thermal.zones` | `/sys/class/thermal/thermal_zoneN/{type,temp,mode}` |
| `os.battery.power_supply` | `/sys/class/power_supply/<n>/{type,status,capacity,temp,voltage_now,current_now,health,…}` |
| `jni.vm`, `jni.context` | the captured `ovrJava`, or `JNI_GetCreatedJavaVMs` + `ActivityThread.currentApplication` |
| `jni.build`, `jni.build_version` | reflection over every public static field of `android.os.Build` and `Build$VERSION` |
| `jni.build_getSerial` | `Build.getSerial()` |
| `jni.runtime` | `Runtime.availableProcessors/maxMemory/totalMemory`, `System.getProperty` |
| `jni.package`, `jni.permissions` | `PackageManager.getPackageInfo` (versions; `GET_PERMISSIONS` with the granted flag) |
| `jni.android_id` | `Settings.Secure.getString(ANDROID_ID)` |
| `jni.activity_manager` | `ActivityManager.getMemoryInfo/getMemoryClass/getLargeMemoryClass/isLowRamDevice` |
| `jni.displays` | `DisplayManager.getDisplays`: id, name, refresh rate, state, supported modes, real metrics |
| `jni.audio_devices`, `jni.audio_properties` | `AudioManager.getDevices(GET_DEVICES_ALL)`, `getProperty(OUTPUT_SAMPLE_RATE/FRAMES_PER_BUFFER)` |
| `jni.storage_dirs` | `Context.get{Files,Cache,NoBackupFiles,Data,Obb,ExternalFiles,ExternalCache}Dir` |
| `jni.battery_intent`, `jni.battery_manager` | the sticky `ACTION_BATTERY_CHANGED` intent; `BatteryManager.getIntProperty/getLongProperty` |
| `jni.power` | `PowerManager.getCurrentThermalStatus/isPowerSaveMode/isInteractive` |
| `jni.network_interfaces` | `java.net.NetworkInterface`: name, index, MTU, up, loopback, hardware address |
| `jni.locale` | `Locale.getDefault().toLanguageTag()`, `TimeZone.getDefault().getID()` |
| `ndk.sensors` | `ASensorManager_getInstanceForPackage` + `getSensorList` + `ASensor_*` |
| `ndk.thermal_status`, `ndk.thermal_headroom` | `AThermal_getCurrentThermalStatus`, `AThermal_getThermalHeadroom(0)` |
| `gpu.vulkan.*` | own `VkInstance`: instance version, extensions and layers; per device the properties, every `VkPhysicalDeviceLimits` and `VkPhysicalDeviceFeatures` member (`tools/gen_hwdump_vk_fields.py`), memory heaps and types, queue families, device extensions |
| `gpu.gl.*` | `eglInitialize/eglQueryString`, `glGetString`, `glGetStringi(GL_EXTENSIONS)`, `glGetIntegerv` limits |
| `vrapi.*` | `vrapi_GetVersionString`, `GetSystemPropertyInt` (0, 5, 6, 7, 8, 0xf, 0x40), `GetSystemPropertyFloat(4)`, `GetSystemPropertyFloatArray(0x41)`, `GetSystemStatusInt(1)` |
| `openxr.*` | `dlopen libopenxr_loader.so`. The OpenXR headers are not vendored, so with a loader present the queries are fields with that error |
| `libr15_queries.*` | the part B hooks: per-hook install status, slot, calls and faults; the records; the table's overflow; what is not hooked |

## Consequences

- With the flag off, nothing is installed and no thread starts. `kInstallHwDump` is skipped with `hwdump_off`.
- With the flag on:
  - The game's per-frame `GetSystemPropertyFloat(4)` and `GetSystemStatusInt(1)` calls each pay one table lookup: a linear scan of at most 128 entries with atomic loads and no lock.
  - One extra Vulkan instance, and one GLES context in stage `gl`, exist briefly in the game process.
  - The file holds device identifiers, so it is not committed and not attached to public issues.
- What the dump finds on a headset is known only from a device run.

## Tests

| Test | What it covers |
|---|---|
| `quest_config_test` | the flag defaults off; only a JSON `true` turns it on |
| `integration_sequence_test` | skipped by default with `hwdump_off`; runs alone; a failing or throwing dump step leaves the other steps unchanged |
| `hwdump_core_test`, also under ThreadSanitizer | the field schema; readers against a fake root; the os section's fixed fields against an empty and a populated root; record dedupe, overflow and concurrency; trigger timing; a log line without values; the queries section |
| `hwdump_handlers_test`, `-fno-exceptions` | every handler passes its arguments bit-exactly (including `s0` floats), returns the original's result, and stays inside the game's buffers |
| `TestSentinelNeededList` | the sentinel's DT_NEEDED list |
| `check-android-static-init` | covers both hwdump targets |
| `TestHookFramesCarryNoPersonality` | the 18 hook records |
