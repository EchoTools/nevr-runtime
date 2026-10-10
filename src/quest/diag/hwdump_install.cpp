// Built with -fno-exceptions (callback_thunk.h): every handler is in hwdump_handlers.cpp, which is
// personality-free; this unit only wires thunks to slots.
#include "quest/diag/hwdump_install.h"

#include "quest/diag/hwdump_handlers.h"
#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/pinned_targets.h"

#include <sys/utsname.h>

namespace nevr_quest::hwdump {
namespace {

using sentinel::CallbackThunk;
using sentinel::GotHook;
using sentinel::GotStatus;
using sentinel::GotTarget;
using sentinel::RelocKind;
namespace pinned = sentinel::pinned;

// One tag per slot: a thunk's original-call pointer is a static of its instantiation.
#define NEVR_HWDUMP_THUNK(Name, Sig) \
  struct Name##Tag {};               \
  using Name##Thunk = CallbackThunk<Name##Tag, Sig>;

NEVR_HWDUMP_THUNK(PropertyGet, int(const char*, char*))
NEVR_HWDUMP_THUNK(Sysconf, long(int))
NEVR_HWDUMP_THUNK(Uname, int(struct utsname*))
NEVR_HWDUMP_THUNK(Gethostname, int(char*, std::size_t))
NEVR_HWDUMP_THUNK(ConfigLanguage, void(void*, char*))
NEVR_HWDUMP_THUNK(ConfigCountry, void(void*, char*))
NEVR_HWDUMP_THUNK(VrapiInitialize, int(const OvrInitParms*))
NEVR_HWDUMP_THUNK(SysPropInt, int(const OvrJava*, int))
NEVR_HWDUMP_THUNK(SysPropFloat, float(const OvrJava*, int))
NEVR_HWDUMP_THUNK(SysPropFloatArray, int(const OvrJava*, int, float*, int))
NEVR_HWDUMP_THUNK(SysStatusInt, int(const OvrJava*, int))
NEVR_HWDUMP_THUNK(PropInt, int(const OvrJava*, int, int*))
NEVR_HWDUMP_THUNK(SetPropInt, int(const OvrJava*, int, int))
NEVR_HWDUMP_THUNK(SetRefreshRate, int(void*, float))
NEVR_HWDUMP_THUNK(SetClockLevels, int(void*, int, int))
NEVR_HWDUMP_THUNK(SetLatencyMode, int(void*, int))
NEVR_HWDUMP_THUNK(InstanceExts, int(char*, std::uint32_t*))
NEVR_HWDUMP_THUNK(DeviceExts, int(char*, std::uint32_t*))
#undef NEVR_HWDUMP_THUNK

NEVR_HOOK_RECORD(kPropertyGetHook, PropertyGetThunk, &HookSystemPropertyGet);
NEVR_HOOK_RECORD(kSysconfHook, SysconfThunk, &HookSysconf);
NEVR_HOOK_RECORD(kUnameHook, UnameThunk, &HookUname);
NEVR_HOOK_RECORD(kGethostnameHook, GethostnameThunk, &HookGethostname);
NEVR_HOOK_RECORD(kConfigLanguageHook, ConfigLanguageThunk, &HookConfigGetLanguage);
NEVR_HOOK_RECORD(kConfigCountryHook, ConfigCountryThunk, &HookConfigGetCountry);
NEVR_HOOK_RECORD(kVrapiInitializeHook, VrapiInitializeThunk, &HookVrapiInitialize);
NEVR_HOOK_RECORD(kSysPropIntHook, SysPropIntThunk, &HookVrapiGetSystemPropertyInt);
NEVR_HOOK_RECORD(kSysPropFloatHook, SysPropFloatThunk, &HookVrapiGetSystemPropertyFloat);
NEVR_HOOK_RECORD(kSysPropFloatArrayHook, SysPropFloatArrayThunk, &HookVrapiGetSystemPropertyFloatArray);
NEVR_HOOK_RECORD(kSysStatusIntHook, SysStatusIntThunk, &HookVrapiGetSystemStatusInt);
NEVR_HOOK_RECORD(kPropIntHook, PropIntThunk, &HookVrapiGetPropertyInt);
NEVR_HOOK_RECORD(kSetPropIntHook, SetPropIntThunk, &HookVrapiSetPropertyInt);
NEVR_HOOK_RECORD(kSetRefreshRateHook, SetRefreshRateThunk, &HookVrapiSetDisplayRefreshRate);
NEVR_HOOK_RECORD(kSetClockLevelsHook, SetClockLevelsThunk, &HookVrapiSetClockLevels);
NEVR_HOOK_RECORD(kSetLatencyModeHook, SetLatencyModeThunk, &HookVrapiSetExtraLatencyMode);
NEVR_HOOK_RECORD(kInstanceExtsHook, InstanceExtsThunk, &HookVrapiGetInstanceExtensionsVulkan);
NEVR_HOOK_RECORD(kDeviceExtsHook, DeviceExtsThunk, &HookVrapiGetDeviceExtensionsVulkan);

GotHook g_hooks[kHookCount];
bool g_installed[kHookCount];
GotStatus g_status[kHookCount];
bool g_ran[kHookCount];

// The JUMP_SLOTs in the pinned libr15 (`llvm-readelf -rW libr15.so`, build b243509c...).
struct Slot {
  const char* symbol;
  std::uint64_t vaddr;
};
constexpr Slot kSlots[kHookCount] = {
    {"__system_property_get", 0x36ee730ULL},
    {"sysconf", 0x36e4de0ULL},
    {"uname", 0x36f25a0ULL},
    {"gethostname", 0x36e3bf8ULL},
    {"AConfiguration_getLanguage", 0x36fc3b0ULL},
    {"AConfiguration_getCountry", 0x36dd9c8ULL},
    {"vrapi_Initialize", 0x36d69f8ULL},
    {"vrapi_GetSystemPropertyInt", 0x36fa728ULL},
    {"vrapi_GetSystemPropertyFloat", 0x36c06f8ULL},
    {"vrapi_GetSystemPropertyFloatArray", 0x36dffa0ULL},
    {"vrapi_GetSystemStatusInt", 0x36fad60ULL},
    {"vrapi_GetPropertyInt", 0x36c1988ULL},
    {"vrapi_SetPropertyInt", 0x36ffcf0ULL},
    {"vrapi_SetDisplayRefreshRate", 0x36ec5f0ULL},
    {"vrapi_SetClockLevels", 0x36c75f0ULL},
    {"vrapi_SetExtraLatencyMode", 0x36fdec0ULL},
    {"vrapi_GetInstanceExtensionsVulkan", 0x36c9d60ULL},
    {"vrapi_GetDeviceExtensionsVulkan", 0x36dd440ULL},
};

GotTarget TargetFor(std::size_t i) {
  return {pinned::kLibR15, kSlots[i].symbol, RelocKind::kJumpSlot, pinned::kLibR15BuildId, kSlots[i].vaddr};
}

template <typename Thunk, typename Record>
void InstallOne(std::size_t i, const Record& record) noexcept {
  Thunk::Arm(record);
  g_status[i] = sentinel::InstallThunk<Thunk>(g_hooks[i], TargetFor(i));
  g_installed[i] = g_status[i] == GotStatus::kOk;
  g_ran[i] = true;
}

}  // namespace

std::size_t InstallHooks() noexcept {
  InstallOne<PropertyGetThunk>(0, kPropertyGetHook);
  InstallOne<SysconfThunk>(1, kSysconfHook);
  InstallOne<UnameThunk>(2, kUnameHook);
  InstallOne<GethostnameThunk>(3, kGethostnameHook);
  InstallOne<ConfigLanguageThunk>(4, kConfigLanguageHook);
  InstallOne<ConfigCountryThunk>(5, kConfigCountryHook);
  InstallOne<VrapiInitializeThunk>(6, kVrapiInitializeHook);
  InstallOne<SysPropIntThunk>(7, kSysPropIntHook);
  InstallOne<SysPropFloatThunk>(8, kSysPropFloatHook);
  InstallOne<SysPropFloatArrayThunk>(9, kSysPropFloatArrayHook);
  InstallOne<SysStatusIntThunk>(10, kSysStatusIntHook);
  InstallOne<PropIntThunk>(11, kPropIntHook);
  InstallOne<SetPropIntThunk>(12, kSetPropIntHook);
  InstallOne<SetRefreshRateThunk>(13, kSetRefreshRateHook);
  InstallOne<SetClockLevelsThunk>(14, kSetClockLevelsHook);
  InstallOne<SetLatencyModeThunk>(15, kSetLatencyModeHook);
  InstallOne<InstanceExtsThunk>(16, kInstanceExtsHook);
  InstallOne<DeviceExtsThunk>(17, kDeviceExtsHook);
  std::size_t n = 0;
  for (bool ok : g_installed) n += ok ? 1U : 0U;
  return n;
}

void HookStates(HookState (&out)[kHookCount]) noexcept {
  const std::uint64_t calls[kHookCount] = {
      PropertyGetThunk::Calls(),  SysconfThunk::Calls(),        UnameThunk::Calls(),
      GethostnameThunk::Calls(),  ConfigLanguageThunk::Calls(), ConfigCountryThunk::Calls(),
      VrapiInitializeThunk::Calls(), SysPropIntThunk::Calls(),  SysPropFloatThunk::Calls(),
      SysPropFloatArrayThunk::Calls(), SysStatusIntThunk::Calls(), PropIntThunk::Calls(),
      SetPropIntThunk::Calls(),   SetRefreshRateThunk::Calls(), SetClockLevelsThunk::Calls(),
      SetLatencyModeThunk::Calls(), InstanceExtsThunk::Calls(), DeviceExtsThunk::Calls()};
  const std::uint64_t faults[kHookCount] = {
      PropertyGetThunk::Faults(),  SysconfThunk::Faults(),        UnameThunk::Faults(),
      GethostnameThunk::Faults(),  ConfigLanguageThunk::Faults(), ConfigCountryThunk::Faults(),
      VrapiInitializeThunk::Faults(), SysPropIntThunk::Faults(),  SysPropFloatThunk::Faults(),
      SysPropFloatArrayThunk::Faults(), SysStatusIntThunk::Faults(), PropIntThunk::Faults(),
      SetPropIntThunk::Faults(),   SetRefreshRateThunk::Faults(), SetClockLevelsThunk::Faults(),
      SetLatencyModeThunk::Faults(), InstanceExtsThunk::Faults(), DeviceExtsThunk::Faults()};
  for (std::size_t i = 0; i < kHookCount; ++i) {
    out[i].symbol = kSlots[i].symbol;
    out[i].slot = kSlots[i].vaddr;
    out[i].installed = g_installed[i];
    out[i].status = g_ran[i] ? sentinel::GotStatusName(g_status[i]) : "not_run";
    out[i].calls = calls[i];
    out[i].faults = faults[i];
  }
}

}  // namespace nevr_quest::hwdump
