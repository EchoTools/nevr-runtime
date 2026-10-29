#include "runtime/patch/render_config.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>

namespace nevr_render_config {

namespace {

template <typename T>
bool ReadValue(const MemoryReader& reader, std::uintptr_t address, T* out) {
  return reader.Read(address, out, sizeof(T));
}

/// The bytes up to the first NUL (or all of them), cleaned. Empty when there is no text.
std::string TextFrom(const unsigned char* bytes, std::size_t capacity) {
  std::size_t length = 0;
  while (length < capacity && bytes[length] != 0) ++length;
  return CleanText(std::string(reinterpret_cast<const char*>(bytes), length));
}

std::string Quoted(const std::string& text) { return "\"" + CleanText(text) + "\""; }

/// An unquoted value: cleaned, with no space left in it, "unknown" when nothing is left.
std::string Token(const std::string& text) {
  std::string out = CleanText(text);
  std::replace(out.begin(), out.end(), ' ', '_');
  return out.empty() ? std::string("unknown") : out;
}

std::string Number(long long value) { return std::to_string(value); }

std::string Float(float value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value));
  return buffer;
}

/// The shortest text that reads back as the same double. The GPU score sits next to the selector's thresholds (4.5, 6,
/// 8, 10), which it compares exactly; any fixed precision prints a score close enough below one as the threshold
/// itself, while the selector picks the level below.
std::string Double(double value) {
  char buffer[32];
  const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return std::string(buffer, result.ptr);
}

const char* Bool(bool value) { return value ? "true" : "false"; }

constexpr const char* kUnavailable = "unavailable";

}  // namespace

const char* SettingsSourceName(SettingsSource source) {
  switch (source) {
    case SettingsSource::Unknown: return "unknown";
    case SettingsSource::AutoSelected: return "auto_selected";
    case SettingsSource::SavedOrDefault: return "saved_or_default";
    case SettingsSource::MenuOrJson: return "menu_or_json";
  }
  return "unknown";
}

bool ReadGfxSettings(const MemoryReader& reader, std::uintptr_t address, GfxSettings* out) {
  unsigned char raw[kSettingsBytes];
  if (!reader.Read(address, raw, sizeof(raw))) return false;
  std::memcpy(&out->flags, raw + kSettingsFlagsOffset, sizeof(out->flags));
  std::memcpy(&out->display, raw + kSettingsDisplayOffset, sizeof(out->display));
  std::memcpy(&out->resolutionScale, raw + kSettingsResolutionScaleOffset, sizeof(out->resolutionScale));
  std::memcpy(&out->adaptiveMin, raw + kSettingsAdaptiveMinOffset, sizeof(out->adaptiveMin));
  std::memcpy(&out->adaptiveMax, raw + kSettingsAdaptiveMaxOffset, sizeof(out->adaptiveMax));
  std::memcpy(&out->adaptiveTarget, raw + kSettingsAdaptiveTargetOffset, sizeof(out->adaptiveTarget));
  std::memcpy(&out->msaa, raw + kSettingsMsaaOffset, sizeof(out->msaa));
  std::memcpy(&out->qualityLevel, raw + kSettingsQualityLevelOffset, sizeof(out->qualityLevel));
  return true;
}

void SettingsObserver::OnApply(const MemoryReader& reader, std::uintptr_t settingsAddress,
                               std::uintptr_t returnAddress, std::uintptr_t bootReturnAddress) {
  GfxSettings settings;
  const bool read = ReadGfxSettings(reader, settingsAddress, &settings);
  std::lock_guard<std::mutex> lock(mutex_);
  ++view_.applyCalls;
  if (!read) {
    ++view_.readFailures;
    return;
  }
  const bool boot = returnAddress == bootReturnAddress;
  const bool selected = selectorAddress_ != 0 && selectorAddress_ == settingsAddress;
  view_.seen = true;
  view_.settings = settings;
  if (!boot) {
    view_.source = SettingsSource::MenuOrJson;
  } else {
    view_.source = selected ? SettingsSource::AutoSelected : SettingsSource::SavedOrDefault;
  }
}

void SettingsObserver::OnSelector(std::uintptr_t settingsAddress) {
  std::lock_guard<std::mutex> lock(mutex_);
  selectorAddress_ = settingsAddress;
}

SettingsObserver::View SettingsObserver::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return view_;
}

void SettingsObserver::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  view_ = View();
  selectorAddress_ = 0;
}

std::string CleanText(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F) {
      out.push_back(' ');
    } else if (c == '"') {
      out.push_back('\'');
    } else if (c == '\\') {
      out.push_back('/');
    } else if (u > 0x7E) {
      out.push_back('?');
    } else {
      out.push_back(c);
    }
  }
  const std::size_t first = out.find_first_not_of(' ');
  if (first == std::string::npos) return std::string();
  const std::size_t last = out.find_last_not_of(' ');
  return out.substr(first, last - first + 1);
}

Snapshot ReadSnapshot(const MemoryReader& reader, std::uintptr_t imageBase, const SettingsObserver::View& settings) {
  Snapshot s;
  s.settings = settings;

  unsigned char name[kGpuNameBytes];
  if (reader.Read(Resolve(imageBase, kGpuNameVa), name, sizeof(name))) {
    std::string text = TextFrom(name, sizeof(name));
    if (!text.empty()) {
      s.gpu.ok = true;
      s.gpu.value = std::move(text);
    }
  }
  s.gpuVendor.ok = ReadValue(reader, Resolve(imageBase, kGpuVendorVa), &s.gpuVendor.value);
  s.gpuScore.ok = ReadValue(reader, Resolve(imageBase, kGpuScoreVa), &s.gpuScore.value);
  std::uint64_t vramBytes = 0;
  if (ReadValue(reader, Resolve(imageBase, kGpuVramVa), &vramBytes)) {
    s.vramMb.ok = true;
    s.vramMb.value = vramBytes >> 20;
  }

  // The HMD descriptor is filled once, when VR starts; before that (and on a run with no headset) it is all zero and the
  // product name is empty, so the whole group reads as unavailable.
  unsigned char desc[kHmdDescBytes];
  if (reader.Read(Resolve(imageBase, kHmdDescVa), desc, sizeof(desc))) {
    std::string product = TextFrom(desc + kHmdProductNameOffset, kHmdProductNameBytes);
    if (!product.empty()) {
      s.headset.ok = true;
      s.headset.value = std::move(product);
      s.hmdType.ok = true;
      std::memcpy(&s.hmdType.value, desc + kHmdTypeOffset, sizeof(s.hmdType.value));
      s.hmdW.ok = true;
      std::memcpy(&s.hmdW.value, desc + kHmdResolutionOffset, sizeof(s.hmdW.value));
      s.hmdH.ok = true;
      std::memcpy(&s.hmdH.value, desc + kHmdResolutionOffset + 4, sizeof(s.hmdH.value));
      s.refreshHz.ok = true;
      std::memcpy(&s.refreshHz.value, desc + kHmdRefreshOffset, sizeof(s.refreshHz.value));
    }
  }

  std::uint64_t packed = 0;
  if (ReadValue(reader, Resolve(imageBase, kRenderTargetVa), &packed) && packed != 0) {
    s.renderW.ok = true;
    s.renderW.value = static_cast<std::int32_t>(packed & 0xFFFFFFFFULL);
    s.renderH.ok = true;
    s.renderH.value = static_cast<std::int32_t>(packed >> 32);
  }

  s.resolutionScale.ok = ReadValue(reader, Resolve(imageBase, kResolutionScaleVa), &s.resolutionScale.value);
  return s;
}

std::vector<std::pair<std::string, std::string>> Fields(const Snapshot& s) {
  std::vector<std::pair<std::string, std::string>> f;
  const auto text = [&](const char* name, const Reading<std::string>& r) {
    f.emplace_back(name, r.ok ? Quoted(r.value) : std::string(kUnavailable));
  };
  const auto integer = [&](const char* name, bool ok, long long v) {
    f.emplace_back(name, ok ? Number(v) : std::string(kUnavailable));
  };
  const auto real = [&](const char* name, bool ok, float v) {
    f.emplace_back(name, ok ? Float(v) : std::string(kUnavailable));
  };
  const SettingsObserver::View& v = s.settings;
  const auto setting = [&](const char* name, const std::string& value) {
    f.emplace_back(name, v.seen ? value : std::string(kUnavailable));
  };

  text("gpu", s.gpu);
  integer("gpu_vendor", s.gpuVendor.ok, s.gpuVendor.value);
  f.emplace_back("gpu_score", s.gpuScore.ok ? Double(s.gpuScore.value) : std::string(kUnavailable));
  integer("vram_mb", s.vramMb.ok, static_cast<long long>(s.vramMb.value));
  f.emplace_back("gpu_driver", Token(s.gpuDriver));
  text("headset", s.headset);
  integer("hmd_type", s.hmdType.ok, s.hmdType.value);
  f.emplace_back("runtime", "libovr");
  f.emplace_back("runtime_version", Quoted(s.runtimeVersion));
  real("refresh_hz", s.refreshHz.ok, s.refreshHz.value);
  integer("hmd_w", s.hmdW.ok, s.hmdW.value);
  integer("hmd_h", s.hmdH.ok, s.hmdH.value);
  if (s.renderW.ok && s.renderH.ok) {
    integer("render_w", true, s.renderW.value);
    integer("render_h", true, s.renderH.value);
  } else {
    f.emplace_back("render_target", kUnavailable);
  }
  real("resolution_scale", s.resolutionScale.ok, s.resolutionScale.value);
  f.emplace_back("settings_source", SettingsSourceName(v.seen ? v.source : SettingsSource::Unknown));
  setting("quality_level", Number(v.settings.qualityLevel));
  setting("msaa", Number(v.settings.msaa));
  setting("settings_resolution_scale", Float(v.settings.resolutionScale));
  setting("adaptive_res", Bool((v.settings.flags & kFlagAdaptiveResolution) != 0));
  setting("taa", Bool((v.settings.flags & kFlagTaa) != 0));
  setting("multires", Bool((v.settings.flags & kFlagMultiRes) != 0));
  setting("fullscreen", Bool((v.settings.flags & kFlagFullscreen) != 0));
  setting("adaptive_min", Float(v.settings.adaptiveMin));
  setting("adaptive_max", Float(v.settings.adaptiveMax));
  setting("adaptive_target", Number(v.settings.adaptiveTarget));
  setting("display", Number(v.settings.display));
  return f;
}

std::vector<std::string> ChangedFields(const Snapshot& before, const Snapshot& after) {
  const auto was = Fields(before);
  const auto now = Fields(after);
  const auto find = [](const std::vector<std::pair<std::string, std::string>>& list,
                       const std::string& name) -> const std::string* {
    for (const auto& entry : list) {
      if (entry.first == name) return &entry.second;
    }
    return nullptr;
  };
  std::vector<std::string> changed;
  for (const auto& entry : now) {
    const std::string* old = find(was, entry.first);
    if (old == nullptr || *old != entry.second) changed.push_back(entry.first);
  }
  for (const auto& entry : was) {
    if (find(now, entry.first) == nullptr) changed.push_back(entry.first);
  }
  return changed;
}

std::string FormatLine(const Snapshot& snapshot, Reason reason, const std::vector<std::string>& changed) {
  std::string line = "[NEVR.RENDER] render_config reason=";
  line += reason == Reason::FirstFrame ? "first_frame" : "change";
  line += " changed=";
  if (changed.empty()) {
    line += "-";
  } else {
    for (std::size_t i = 0; i < changed.size(); ++i) {
      if (i != 0) line += ",";
      line += changed[i];
    }
  }
  line += " platform=pcvr";
  for (const auto& field : Fields(snapshot)) {
    line += " ";
    line += field.first;
    line += "=";
    line += field.second;
  }
  return line;
}

Collector::Collector(const MemoryReader& reader, std::uintptr_t imageBase, const SettingsObserver& settings,
                     Externals externals)
    : reader_(reader), imageBase_(imageBase), settings_(settings), externals_(std::move(externals)) {}

std::string Collector::Poll(std::uint64_t nowMs) {
  if (started_ && nowMs >= lastEvalMs_ && nowMs - lastEvalMs_ < kIntervalMs) return std::string();
  if (!started_) {
    started_ = true;
    startMs_ = nowMs;
  }
  lastEvalMs_ = nowMs;

  Snapshot now = ReadSnapshot(reader_, imageBase_, settings_.Snapshot());
  now.gpuDriver = gpuDriver_;
  now.runtimeVersion = runtimeVersion_;

  if (!emitted_) {
    const bool targetExists = now.renderW.ok;
    const bool graceOver = nowMs >= startMs_ && nowMs - startMs_ >= kFirstFrameGraceMs;
    if (!targetExists && !graceOver) return std::string();
    // The two values that are not game memory are asked for once, here, and kept: the DXGI walk and the library lookup
    // stay off every later pass.
    if (externals_.gpuDriver && now.gpu.ok) gpuDriver_ = externals_.gpuDriver(now.gpu.value);
    if (externals_.runtimeVersion) runtimeVersion_ = externals_.runtimeVersion();
    now.gpuDriver = gpuDriver_;
    now.runtimeVersion = runtimeVersion_;
    emitted_ = true;
    last_ = now;
    return FormatLine(now, Reason::FirstFrame, {});
  }

  const std::vector<std::string> changed = ChangedFields(last_, now);
  if (changed.empty()) return std::string();
  last_ = now;
  return FormatLine(now, Reason::Change, changed);
}

}  // namespace nevr_render_config
