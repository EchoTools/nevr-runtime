#include <elf.h>
#include <pthread.h>
#include <time.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include "hook_install.h"
#include "clock_levels_hook.h"
#include "hook_log.h"
#include "pinned_targets.h"
#include "render_config.h"
#include "render_config_share.h"
#include "texture_quality_hook.h"

namespace nevr_quest::render_config {

namespace {

using sentinel::pinned::OvrJavaOpaque;
using sentinel::pinned::OvrMobileOpaque;
using sentinel::pinned::OvrSubmitFrameDescriptionOpaque;

using RefreshRateThunk = sentinel::pinned::LibR15SetDisplayRefreshRateThunk;
using ClockLevelsThunk = sentinel::pinned::LibR15SetClockLevelsThunk;
using SetPropertyThunk = sentinel::pinned::LibR15SetPropertyIntThunk;
using SystemFloatThunk = sentinel::pinned::LibR15GetSystemPropertyFloatThunk;
using SubmitFrameThunk = sentinel::pinned::LibR15SubmitFrame2Thunk;

// VrApi.h property ids, as libr15 passes them (docs/adr/0022-render-config-log.md).
constexpr int kFoveationLevelProperty = 0xf;
constexpr int kDynamicFoveationProperty = 0x1e;
constexpr int kRefreshRateSystemProperty = 4;

// Pinned link-time addresses of the libr15 globals (disassembly shows each 0x100000 higher). Read after the build id is proved.
constexpr std::uint64_t kEyeSizeVaddr = 0x37a33e0;      // CVR::RefreshRTSize's result: {int32 w, int32 h}
constexpr std::uint64_t kOverrideSizeVaddr = 0x37a33e8;  // CVR::SetRTSizeOverride's pair; non-zero: the query is skipped
constexpr std::uint64_t kMsaaModeVaddr = 0x379c080;     // CGRenderConfig::MSAAMode()
constexpr Value kMaxEyeDimension = 16384;                // a pair outside 1..16384 is not a size

// ---- what the hooks store (atomics only) ----------------------------------------------------------------------------------

std::atomic<Value> g_refreshRequestedMilli{kUnset};
std::atomic<Value> g_refreshRequestResult{kUnset};
std::atomic<Value> g_refreshReportedMilli{kUnset};
std::atomic<Value> g_cpuLevel{kUnset};
std::atomic<Value> g_gpuLevel{kUnset};
std::atomic<Value> g_clockResult{kUnset};
std::atomic<Value> g_foveationLevel{kUnset};
std::atomic<Value> g_foveationDynamic{kUnset};
std::atomic<std::uint64_t> g_frames{0};

Value ToMilli(float hertz) noexcept {
  if (!(hertz >= 0.0f && hertz < 100000.0f)) return 0;  // also false for NaN
  return static_cast<Value>(hertz * 1000.0f + 0.5f);
}

// Every handler calls the original first with the game's own arguments (clock levels: see OnSetClockLevels), then records, then returns the original's result:
// the game's behaviour does not depend on this module. None logs, allocates or throws (callback_thunk.h rule 1).

int OnSetDisplayRefreshRate(RefreshRateThunk::Fn original, OvrMobileOpaque* ovr, float rate) noexcept {
  const int result = original(ovr, rate);
  g_refreshRequestedMilli.store(ToMilli(rate), std::memory_order_relaxed);
  g_refreshRequestResult.store(result, std::memory_order_relaxed);
  return result;
}

// The one handler that may change an argument: clock_levels::Choose (#928) answers which levels the original is called with,
// the game's own unless the clock-level override is on and the game asked for its stock (2, 2). The levels stored are the ones
// the original was called with, so the line says what the headset was asked for.
int OnSetClockLevels(ClockLevelsThunk::Fn original, OvrMobileOpaque* ovr, int cpuLevel, int gpuLevel) noexcept {
  const clock_levels::Levels applied = clock_levels::Choose(cpuLevel, gpuLevel);
  const int result = original(ovr, applied.cpu, applied.gpu);
  g_cpuLevel.store(applied.cpu, std::memory_order_relaxed);
  g_gpuLevel.store(applied.gpu, std::memory_order_relaxed);
  g_clockResult.store(result, std::memory_order_relaxed);
  return result;
}

int OnSetPropertyInt(SetPropertyThunk::Fn original, const OvrJavaOpaque* java, int property, int value) noexcept {
  const int result = original(java, property, value);
  if (property == kFoveationLevelProperty) g_foveationLevel.store(value, std::memory_order_relaxed);
  if (property == kDynamicFoveationProperty) g_foveationDynamic.store(value, std::memory_order_relaxed);
  return result;
}

float OnGetSystemPropertyFloat(SystemFloatThunk::Fn original, const OvrJavaOpaque* java, int property) noexcept {
  const float result = original(java, property);
  if (property == kRefreshRateSystemProperty) g_refreshReportedMilli.store(ToMilli(result), std::memory_order_relaxed);
  return result;
}

int OnSubmitFrame(SubmitFrameThunk::Fn original, OvrMobileOpaque* ovr,
                  const OvrSubmitFrameDescriptionOpaque* description) noexcept {
  g_frames.fetch_add(1, std::memory_order_relaxed);
  return original(ovr, description);
}

NEVR_HOOK_RECORD(kRefreshRateHook, RefreshRateThunk, &OnSetDisplayRefreshRate);
NEVR_HOOK_RECORD(kClockLevelsHook, ClockLevelsThunk, &OnSetClockLevels);
NEVR_HOOK_RECORD(kSetPropertyHook, SetPropertyThunk, &OnSetPropertyInt);
NEVR_HOOK_RECORD(kSystemFloatHook, SystemFloatThunk, &OnGetSystemPropertyFloat);
NEVR_HOOK_RECORD(kSubmitFrameHook, SubmitFrameThunk, &OnSubmitFrame);

constexpr int kSlotCount = 5;
const char* const kSlotSymbols[kSlotCount] = {
    sentinel::pinned::kSetDisplayRefreshRateSymbol, sentinel::pinned::kSetClockLevelsSymbol,
    sentinel::pinned::kSetPropertyIntSymbol,        sentinel::pinned::kGetSystemPropertyFloatSymbol,
    sentinel::pinned::kSubmitFrame2Symbol,
};
// The slots the hardware dump hooks too (render_config_share.h); the submit-frame slot is not one of them.
constexpr sentinel::RenderConfigSlot kShared[kSlotCount - 1] = {
    sentinel::RenderConfigSlot::kSetDisplayRefreshRate, sentinel::RenderConfigSlot::kSetClockLevels,
    sentinel::RenderConfigSlot::kSetPropertyInt, sentinel::RenderConfigSlot::kGetSystemPropertyFloat};

sentinel::GotHook g_hooks[kSlotCount];

void ArmAll() noexcept {
  RefreshRateThunk::Arm(kRefreshRateHook);
  ClockLevelsThunk::Arm(kClockLevelsHook);
  SetPropertyThunk::Arm(kSetPropertyHook);
  SystemFloatThunk::Arm(kSystemFloatHook);
  SubmitFrameThunk::Arm(kSubmitFrameHook);
}

sentinel::GotStatus RealInstall(int slot) noexcept {
  switch (slot) {
    case 0: return sentinel::InstallThunk<RefreshRateThunk>(g_hooks[0], sentinel::pinned::LibR15SetDisplayRefreshRate());
    case 1: return sentinel::InstallThunk<ClockLevelsThunk>(g_hooks[1], sentinel::pinned::LibR15SetClockLevels());
    case 2: return sentinel::InstallThunk<SetPropertyThunk>(g_hooks[2], sentinel::pinned::LibR15SetPropertyInt());
    case 3: return sentinel::InstallThunk<SystemFloatThunk>(g_hooks[3], sentinel::pinned::LibR15GetSystemPropertyFloat());
    case 4: return sentinel::InstallThunk<SubmitFrameThunk>(g_hooks[4], sentinel::pinned::LibR15SubmitFrame2());
    default: return sentinel::GotStatus::kBadArgument;
  }
}

std::uint64_t ThunkFaults() noexcept {
  return RefreshRateThunk::Faults() + ClockLevelsThunk::Faults() + SetPropertyThunk::Faults() +
         SystemFloatThunk::Faults() + SubmitFrameThunk::Faults();
}

void ResetThunks() noexcept {
  RefreshRateThunk::Reset();
  ClockLevelsThunk::Reset();
  SetPropertyThunk::Reset();
  SystemFloatThunk::Reset();
  SubmitFrameThunk::Reset();
}

// ---- the texture-quality choice the line reports -------------------------------------------------------------------------

std::atomic<const char*> g_textureName{"stock"};
std::atomic<bool> g_textureFromConfig{false};
std::atomic<bool> g_textureReplacesSize{false};

// ---- what the poller keeps ----------------------------------------------------------------------------------------------

Snapshot g_last;
bool g_haveLast = false;
Device g_device;
bool g_haveDevice = false;

// ---- the fields of the line ----------------------------------------------------------------------------------------------

struct FieldName {
  const char* name;
  Value Snapshot::*member;
};
constexpr FieldName kFields[] = {
    {"refresh_requested_hz", &Snapshot::refreshRequestedMilli},
    {"refresh_request_result", &Snapshot::refreshRequestResult},
    {"refresh_reported_hz", &Snapshot::refreshReportedMilli},
    {"cpu_level", &Snapshot::cpuLevel},
    {"gpu_level", &Snapshot::gpuLevel},
    {"clock_result", &Snapshot::clockResult},
    {"foveation_level", &Snapshot::foveationLevel},
    {"foveation_dynamic", &Snapshot::foveationDynamic},
    {"eye_w", &Snapshot::eyeW},
    {"eye_h", &Snapshot::eyeH},
    {"eye_source", &Snapshot::eyeSource},
    {"native_eye_w", &Snapshot::nativeEyeW},
    {"native_eye_h", &Snapshot::nativeEyeH},
    {"msaa_mode", &Snapshot::msaaMode},
    {"thunk_faults", &Snapshot::thunkFaults},
};

const char* EyeSourceName(Value source) noexcept {
  switch (source) {
    case kEyeRuntimeSuggested: return "runtime_suggested";
    case kEyeGameOverride: return "game_rt_size_override";
    case kEyeNevrTextureQuality: return "nevr_texture_quality";
    default: return "unknown";
  }
}

// ---- the production source -----------------------------------------------------------------------------------------------

// True when [vaddr, vaddr + size) lies inside the file-backed or zero-filled part of one PT_LOAD segment.
bool InsideLoadSegment(const sentinel::ElfImage& image, std::uint64_t vaddr, std::size_t size) noexcept {
  for (std::size_t i = 0; i < image.phnum; ++i) {
    const Elf64_Phdr& ph = image.phdr[i];
    if (ph.p_type != PT_LOAD) continue;
    if (vaddr >= ph.p_vaddr && vaddr + size <= ph.p_vaddr + ph.p_memsz) return true;
  }
  return false;
}

bool ReadInt32Pair(const sentinel::ElfImage& image, std::uint64_t vaddr, std::int32_t* first, std::int32_t* second) noexcept {
  if (!InsideLoadSegment(image, vaddr, 2 * sizeof(std::int32_t))) return false;
  std::int32_t pair[2];
  std::memcpy(pair, reinterpret_cast<const void*>(image.base + vaddr), sizeof(pair));
  *first = pair[0];
  *second = pair[1];
  return true;
}

bool ReadGlobalsProduction(Globals* out) noexcept {
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image)) return false;
  return ReadGlobalsFrom(image, sentinel::pinned::LibR15PatchedBuildId(), out);
}

void ReadDeviceProduction(Device* out) noexcept {
  std::snprintf(out->model, sizeof(out->model), "%s", "unknown");
  std::snprintf(out->osBuild, sizeof(out->osBuild), "%s", "unknown");
#if defined(__ANDROID__)
  char value[PROP_VALUE_MAX] = {};
  if (__system_property_get("ro.product.model", value) > 0) std::snprintf(out->model, sizeof(out->model), "%s", value);
  value[0] = '\0';
  if (__system_property_get("ro.build.display.id", value) > 0) std::snprintf(out->osBuild, sizeof(out->osBuild), "%s", value);
#endif
}

// ---- collecting and logging ----------------------------------------------------------------------------------------------

Snapshot Collect(const Source& source) noexcept {
  Snapshot s;
  s.refreshRequestedMilli = g_refreshRequestedMilli.load(std::memory_order_relaxed);
  s.refreshRequestResult = g_refreshRequestResult.load(std::memory_order_relaxed);
  s.refreshReportedMilli = g_refreshReportedMilli.load(std::memory_order_relaxed);
  s.cpuLevel = g_cpuLevel.load(std::memory_order_relaxed);
  s.gpuLevel = g_gpuLevel.load(std::memory_order_relaxed);
  s.clockResult = g_clockResult.load(std::memory_order_relaxed);
  s.foveationLevel = g_foveationLevel.load(std::memory_order_relaxed);
  s.foveationDynamic = g_foveationDynamic.load(std::memory_order_relaxed);
  s.frames = static_cast<Value>(g_frames.load(std::memory_order_relaxed));
  s.thunkFaults = static_cast<Value>(ThunkFaults());
  Globals g;
  if (source.readGlobals != nullptr && source.readGlobals(&g)) {
    s.eyeW = g.eyeW;
    s.eyeH = g.eyeH;
    s.overrideW = g.overrideW;
    s.overrideH = g.overrideH;
    s.msaaMode = g.msaaMode;
    const bool hasOverride = (g.overrideW != kUnset && g.overrideW != 0) || (g.overrideH != kUnset && g.overrideH != 0);
    s.eyeSource = hasOverride                      ? kEyeGameOverride
                  : g_textureReplacesSize.load(std::memory_order_relaxed) ? kEyeNevrTextureQuality
                                                                          : kEyeRuntimeSuggested;
  }
  if (g_textureReplacesSize.load(std::memory_order_relaxed)) {
    const texture_quality::Counts native = texture_quality::CurrentCounts();
    if (native.nativeWidth > 0) s.nativeEyeW = static_cast<Value>(native.nativeWidth);
    if (native.nativeHeight > 0) s.nativeEyeH = static_cast<Value>(native.nativeHeight);
  }
  return s;
}

// Builds the line's fields in a fixed buffer: a field with no value is left out, so a number is always a number.
class Fields {
 public:
  void Text(const char* key, const char* value) noexcept { Put(key, value); }
  void Number(const char* key, Value value) noexcept {
    if (value != kUnset) Put(key, value);
  }
  void Hertz(const char* key, Value milli) noexcept {
    if (milli == kUnset) return;
    FormatHertz(milli, hertz_[hertzUsed_], sizeof(hertz_[0]));
    Put(key, hertz_[hertzUsed_++]);
  }
  const sentinel::LogField* data() noexcept { return reinterpret_cast<const sentinel::LogField*>(storage_); }
  std::size_t size() const noexcept { return count_; }

 private:
  template <typename T>
  void Put(const char* key, T value) noexcept {
    if (count_ >= kMax) return;
    new (storage_ + count_ * sizeof(sentinel::LogField)) sentinel::LogField(key, value);
    ++count_;
  }
  static constexpr std::size_t kMax = 40;
  static constexpr std::size_t kHertzSlots = 4;
  alignas(sentinel::LogField) unsigned char storage_[kMax * sizeof(sentinel::LogField)];
  char hertz_[kHertzSlots][16] = {};
  std::size_t hertzUsed_ = 0;
  std::size_t count_ = 0;
};

void EmitLine(const Snapshot& s, const Device& device, const char* reason, const char* changed) noexcept {
  Fields f;
  f.Text("platform", "quest");
  f.Text("reason", reason);
  if (changed != nullptr && changed[0] != '\0') f.Text("changed", changed);
  f.Text("device_model", device.model);
  f.Text("os_build", device.osBuild);
  f.Hertz("refresh_requested_hz", s.refreshRequestedMilli);
  f.Number("refresh_request_result", s.refreshRequestResult);
  f.Hertz("refresh_reported_hz", s.refreshReportedMilli);
  f.Number("eye_w", s.eyeW);
  f.Number("eye_h", s.eyeH);
  if (s.eyeSource != kUnset) f.Text("eye_source", EyeSourceName(s.eyeSource));
  f.Text("texture_quality", g_textureName.load(std::memory_order_relaxed));
  f.Text("texture_quality_source", g_textureFromConfig.load(std::memory_order_relaxed) ? "config" : "default");
  f.Number("native_eye_w", s.nativeEyeW);
  f.Number("native_eye_h", s.nativeEyeH);
  f.Number("foveation_level", s.foveationLevel);
  f.Number("foveation_dynamic", s.foveationDynamic);
  f.Number("cpu_level", s.cpuLevel);
  f.Number("gpu_level", s.gpuLevel);
  f.Number("clock_result", s.clockResult);
  f.Number("msaa_mode", s.msaaMode);
  f.Text("spacewarp", "unknown");
  if (s.thunkFaults != 0) f.Number("thunk_faults", s.thunkFaults);
  f.Number("frames", s.frames);
  sentinel::LogFieldList(s.thunkFaults != 0 ? sentinel::LogLevel::kWarn : sentinel::LogLevel::kInfo, "render_config", f.data(),
                         f.size());
}

// ---- the poller thread ---------------------------------------------------------------------------------------------------

pthread_mutex_t g_control = PTHREAD_MUTEX_INITIALIZER;  // one Start or Stop at a time
pthread_mutex_t g_wait = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_cond;
bool g_condReady = false;
bool g_running = false;
bool g_stop = false;
pthread_t g_thread;
const Source* g_source = nullptr;
unsigned g_periodMs = 1000;

void* PollerMain(void*) {
  pthread_mutex_lock(&g_wait);
  while (!g_stop) {
    timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += g_periodMs / 1000;
    deadline.tv_nsec += static_cast<long>(g_periodMs % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_nsec -= 1000000000L;
      ++deadline.tv_sec;
    }
    int rc = 0;
    while (!g_stop && rc == 0) rc = pthread_cond_timedwait(&g_cond, &g_wait, &deadline);
    if (g_stop) break;
    const Source* source = g_source;
    pthread_mutex_unlock(&g_wait);  // never log or read game memory under the wait lock
    PollOnce(*source);
    pthread_mutex_lock(&g_wait);
  }
  pthread_mutex_unlock(&g_wait);
  return nullptr;
}

bool StartPoller(const Source& source, unsigned periodMs) noexcept {
  pthread_mutex_lock(&g_control);
  if (g_running) {
    pthread_mutex_unlock(&g_control);
    return true;
  }
  if (!g_condReady) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_cond, &attr);
    pthread_condattr_destroy(&attr);
    g_condReady = true;
  }
  pthread_mutex_lock(&g_wait);
  g_stop = false;
  g_source = &source;
  g_periodMs = periodMs;
  pthread_mutex_unlock(&g_wait);
  const int rc = pthread_create(&g_thread, nullptr, &PollerMain, nullptr);
  if (rc == 0) {
    pthread_setname_np(g_thread, "nevr-renderconfig");
    g_running = true;
  } else {
    sentinel::LogFields(sentinel::LogLevel::kError, "render_config_install", {{"result", "poller_not_started"}, {"errno", rc}});
  }
  pthread_mutex_unlock(&g_control);
  return rc == 0;
}

void StopPoller() noexcept {
  pthread_mutex_lock(&g_control);
  if (g_running) {
    pthread_mutex_lock(&g_wait);
    g_stop = true;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_wait);
    pthread_join(g_thread, nullptr);
    g_running = false;
  }
  pthread_mutex_unlock(&g_control);
}

bool InstallImpl(texture_quality::Level level, bool fromConfig, InstallFn install, const Source* pollerSource,
                 unsigned periodMs) noexcept {
  g_textureName.store(texture_quality::LevelName(level), std::memory_order_relaxed);
  g_textureFromConfig.store(fromConfig, std::memory_order_relaxed);
  g_textureReplacesSize.store(level != texture_quality::Level::kStock, std::memory_order_relaxed);
  ArmAll();
  sentinel::GotStatus status[kSlotCount];
  int installed = 0;
  for (int i = 0; i < kSlotCount; ++i) {
    status[i] = install(i);
    if (status[i] == sentinel::GotStatus::kOk) {
      ++installed;
      if (i < kSlotCount - 1) sentinel::MarkRenderConfigSlotOwned(kShared[i], true);
    }
  }
  const bool all = installed == kSlotCount;
  sentinel::LogFields(all ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "render_config_install",
                      {{"result", all ? "installed" : (installed == 0 ? "not_installed" : "partial")},
                       {"installed", installed},
                       {"wanted", kSlotCount},
                       {kSlotSymbols[0], sentinel::GotStatusName(status[0])},
                       {kSlotSymbols[1], sentinel::GotStatusName(status[1])},
                       {kSlotSymbols[2], sentinel::GotStatusName(status[2])},
                       {kSlotSymbols[3], sentinel::GotStatusName(status[3])},
                       {kSlotSymbols[4], sentinel::GotStatusName(status[4])},
                       {"texture_quality", texture_quality::LevelName(level)}});
  // A partial install still logs what it can see; with none installed there is nothing to wait for.
  if (installed > 0 && pollerSource != nullptr) StartPoller(*pollerSource, periodMs);
  return all;
}

}  // namespace

// ---- public ---------------------------------------------------------------------------------------------------------------

std::size_t ChangedFields(const Snapshot& before, const Snapshot& after, char* out, std::size_t capacity) noexcept {
  if (capacity == 0) return 0;
  out[0] = '\0';
  std::size_t used = 0;
  std::size_t count = 0;
  for (const FieldName& f : kFields) {
    if (before.*(f.member) == after.*(f.member)) continue;
    ++count;
    const std::size_t length = std::strlen(f.name);
    const std::size_t need = length + (used > 0 ? 1 : 0);
    if (used + need + 1 > capacity) continue;  // counted, not named
    if (used > 0) out[used++] = ',';
    std::memcpy(out + used, f.name, length);
    used += length;
    out[used] = '\0';
  }
  return count;
}

void FormatHertz(Value milli, char* out, std::size_t capacity) noexcept {
  if (capacity == 0) return;
  if (milli == kUnset || milli < 0) {
    out[0] = '\0';
    return;
  }
  const long long tenths = (milli + 50) / 100;
  std::snprintf(out, capacity, "%lld.%lld", tenths / 10, tenths % 10);
}

bool ReadGlobalsFrom(const sentinel::ElfImage& image, const char* expectedBuildId, Globals* out) noexcept {
  char buildId[64] = {};
  if (!sentinel::ReadBuildId(image, buildId, sizeof(buildId)) || std::strcmp(buildId, expectedBuildId) != 0) return false;
  std::int32_t w = 0, h = 0, ow = 0, oh = 0;
  if (!ReadInt32Pair(image, kEyeSizeVaddr, &w, &h) || !ReadInt32Pair(image, kOverrideSizeVaddr, &ow, &oh)) return false;
  if (!InsideLoadSegment(image, kMsaaModeVaddr, sizeof(std::int32_t))) return false;
  std::int32_t msaa = 0;
  std::memcpy(&msaa, reinterpret_cast<const void*>(image.base + kMsaaModeVaddr), sizeof(msaa));
  const bool sizeOk = w >= 1 && h >= 1 && w <= kMaxEyeDimension && h <= kMaxEyeDimension;
  out->eyeW = sizeOk ? w : kUnset;
  out->eyeH = sizeOk ? h : kUnset;
  out->overrideW = ow;
  out->overrideH = oh;
  out->msaaMode = msaa;
  return true;
}

const Source& ProductionSource() noexcept {
  static const Source source{&ReadGlobalsProduction, &ReadDeviceProduction};
  return source;
}

bool PollOnce(const Source& source) noexcept {
  const Snapshot now = Collect(source);
  if (now.frames == 0) return false;  // no frame has been submitted: nothing has been rendered yet
  if (!g_haveDevice) {
    if (source.readDevice != nullptr) source.readDevice(&g_device);
    g_haveDevice = true;
  }
  if (!g_haveLast) {
    g_last = now;
    g_haveLast = true;
    EmitLine(now, g_device, "first_frame", nullptr);
    return true;
  }
  char changed[256];
  if (ChangedFields(g_last, now, changed, sizeof(changed)) == 0) return false;
  g_last = now;
  EmitLine(now, g_device, "change", changed);
  return true;
}

bool InstallVia(texture_quality::Level level, bool fromConfig, InstallFn install, const Source* pollerSource,
                unsigned periodMs) noexcept {
  return InstallImpl(level, fromConfig, install, pollerSource, periodMs);
}

bool Install(texture_quality::Level level, bool fromConfig) noexcept {
  return InstallImpl(level, fromConfig, &RealInstall, &ProductionSource(), 1000);
}

void ArmForTest() noexcept { ArmAll(); }

void ResetForTest() noexcept {
  StopPoller();
  g_refreshRequestedMilli.store(kUnset);
  g_refreshRequestResult.store(kUnset);
  g_refreshReportedMilli.store(kUnset);
  g_cpuLevel.store(kUnset);
  g_gpuLevel.store(kUnset);
  g_clockResult.store(kUnset);
  g_foveationLevel.store(kUnset);
  g_foveationDynamic.store(kUnset);
  g_frames.store(0);
  g_textureName.store("stock");
  g_textureFromConfig.store(false);
  g_textureReplacesSize.store(false);
  g_last = Snapshot{};
  g_haveLast = false;
  g_device = Device{};
  g_haveDevice = false;
  ResetThunks();
  for (std::size_t i = 0; i < static_cast<std::size_t>(sentinel::RenderConfigSlot::kCount); ++i) {
    sentinel::MarkRenderConfigSlotOwned(static_cast<sentinel::RenderConfigSlot>(i), false);
  }
}

bool StartPollerForTest(const Source& source, unsigned periodMs) noexcept { return StartPoller(source, periodMs); }
void StopPollerForTest() noexcept { StopPoller(); }

}  // namespace nevr_quest::render_config
