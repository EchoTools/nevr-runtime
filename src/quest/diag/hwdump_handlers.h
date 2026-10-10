#pragma once
// The GOT-hook handlers for what libr15 asks the system for (#335, part B). Each calls the original with
// exactly the arguments it was given, returns exactly what the original returned, and records the query
// and its answer in Records() (hwdump_records.h). Nothing else: no log, no allocation, no exception.
//
// Built with -fno-exceptions, both in the sentinel (where hwdump_install.cpp wraps each in a CallbackThunk)
// and in the host test, which calls them with fake originals. The prototypes are the ones libr15's call
// sites use: the argument and result registers at all 48 `bl`/`b` sites of the 18 imports, recorded in
// docs/adr/0006-quest-hardware-dump.md.

#include <cstddef>
#include <cstdint>

struct utsname;

namespace nevr_quest::hwdump {

// libr15's ovrJava: {JavaVM* Vm; JNIEnv* Env; jobject ActivityObject}. InitInternal (0x1a6eda8) fills its
// copy at 0x37a3550 from ANativeActivity+0x8 (vm) and +0x18 (clazz); FUN_01a6eefc passes the same 24 bytes
// at ovrInitParms+0x18. Only Vm and ActivityObject are kept: Env belongs to the calling thread.
struct OvrJava {
  void* vm;
  void* env;
  void* activity;
};

// ovrInitParms as FUN_01a6eefc builds it on the stack (0x1a6eff8..0x1a6f010): six 32-bit fields, then the
// ovrJava at +0x18.
struct OvrInitParms {
  std::int32_t fields[6];
  OvrJava java;
};
static_assert(sizeof(OvrInitParms) == 0x30, "ovrInitParms is 0x30 bytes at libr15's call site");

using SystemPropertyGetFn = int (*)(const char* name, char* value);
using SysconfFn = long (*)(int name);
using UnameFn = int (*)(struct utsname* buf);
using GethostnameFn = int (*)(char* name, std::size_t len);
using ConfigGetTwoCharsFn = void (*)(void* config, char* out);  // writes exactly 2 bytes, no NUL
using VrapiInitializeFn = int (*)(const OvrInitParms* parms);
using VrapiGetIntFn = int (*)(const OvrJava* java, int id);
// vrapi_GetPropertyInt(java, id, int* out): libr15 (0x1a70ca8) passes x2 = an out int and tests bit 0 of w0.
using VrapiGetPropertyIntFn = int (*)(const OvrJava* java, int id, int* out);
// vrapi_Get{Instance,Device}ExtensionsVulkan(char* names, uint32_t* size): size is in/out (0x1a73dd4, 0x1a73e00).
using VrapiGetExtensionsFn = int (*)(char* names, std::uint32_t* size);
using VrapiGetFloatFn = float (*)(const OvrJava* java, int id);
using VrapiGetFloatArrayFn = int (*)(const OvrJava* java, int id, float* values, int capacity);
using VrapiSetPropertyIntFn = int (*)(const OvrJava* java, int id, int value);
using VrapiSetRefreshRateFn = int (*)(void* ovr, float rate);
using VrapiSetClockLevelsFn = int (*)(void* ovr, int cpu, int gpu);
using VrapiSetLatencyModeFn = int (*)(void* ovr, int mode);

int HookSystemPropertyGet(SystemPropertyGetFn original, const char* name, char* value) noexcept;
long HookSysconf(SysconfFn original, int name) noexcept;
int HookUname(UnameFn original, struct utsname* buf) noexcept;
int HookGethostname(GethostnameFn original, char* name, std::size_t len) noexcept;
void HookConfigGetLanguage(ConfigGetTwoCharsFn original, void* config, char* out) noexcept;
void HookConfigGetCountry(ConfigGetTwoCharsFn original, void* config, char* out) noexcept;
int HookVrapiInitialize(VrapiInitializeFn original, const OvrInitParms* parms) noexcept;
int HookVrapiGetSystemPropertyInt(VrapiGetIntFn original, const OvrJava* java, int id) noexcept;
float HookVrapiGetSystemPropertyFloat(VrapiGetFloatFn original, const OvrJava* java, int id) noexcept;
int HookVrapiGetSystemPropertyFloatArray(VrapiGetFloatArrayFn original, const OvrJava* java, int id, float* values,
                                         int capacity) noexcept;
int HookVrapiGetSystemStatusInt(VrapiGetIntFn original, const OvrJava* java, int id) noexcept;
int HookVrapiGetPropertyInt(VrapiGetPropertyIntFn original, const OvrJava* java, int id, int* out) noexcept;
int HookVrapiGetInstanceExtensionsVulkan(VrapiGetExtensionsFn original, char* names, std::uint32_t* size) noexcept;
int HookVrapiGetDeviceExtensionsVulkan(VrapiGetExtensionsFn original, char* names, std::uint32_t* size) noexcept;
int HookVrapiSetPropertyInt(VrapiSetPropertyIntFn original, const OvrJava* java, int id, int value) noexcept;
int HookVrapiSetDisplayRefreshRate(VrapiSetRefreshRateFn original, void* ovr, float rate) noexcept;
int HookVrapiSetClockLevels(VrapiSetClockLevelsFn original, void* ovr, int cpu, int gpu) noexcept;
int HookVrapiSetExtraLatencyMode(VrapiSetLatencyModeFn original, void* ovr, int mode) noexcept;

// What the handlers captured from the game, for the dump thread.
struct GameCapture {
  void* vm = nullptr;         // JavaVM*, from the first ovrJava seen
  void* activity = nullptr;   // the NativeActivity global ref from the same ovrJava
  bool initialize_seen = false;
  int initialize_status = 0;  // vrapi_Initialize's result, valid when initialize_seen
  std::uint64_t initialize_seen_monotonic_ms = 0;
};
GameCapture Captured() noexcept;

// Test support: forget the capture (the record table is separate).
void ResetCaptureForTest() noexcept;

}  // namespace nevr_quest::hwdump
