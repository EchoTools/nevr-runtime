#pragma once

// #1139, docs/adr/0022 "PCVR": the render configuration of a PC client as one log line.
//
// This header and render_config.cpp are the pure half: a snapshot of what the game holds (its GPU, its headset, the
// render target it created, its graphics settings), the ordered list of fields that differ between two snapshots, the
// one-line formatter, and the once-a-second check that decides when the line is written. No Windows call, no game
// header and no log call is made here, so a unit test feeds it a fake game image
// (src/runtime/tests/test_render_config.cpp). The process half (the two observer hooks, the production memory reader,
// the DXGI and LibOVR queries, the log call) is render_config_hook.cpp.
//
// Every address is a ReVault VA in echovr.exe (image base 0x140000000); the value read at run time is
// `base + (VA - kImageBase)`. The ones marked "read back" were read back from the disassembly while writing this file.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace nevr_render_config {

inline constexpr std::uint64_t kImageBase = 0x140000000ULL;

// ---- What the game holds (STATIC: read from echovr.exe through ReVault) ----

/// 128-byte NUL-terminated GPU name; getter fcn.1400df2d0 trims it in place (read back: LEA RBX,[0x14209cf70]).
inline constexpr std::uint64_t kGpuNameVa = 0x14209cf70ULL;
inline constexpr std::size_t kGpuNameBytes = 128;
/// qword, bytes of video memory; getter fcn.1400df770 (read back: MOV RAX,[0x14209cff0]; RET).
inline constexpr std::uint64_t kGpuVramVa = 0x14209cff0ULL;
/// dword, vendor code the auto-selector compares with 1 and 2; getter fcn.1400df7b0 (read back).
inline constexpr std::uint64_t kGpuVendorVa = 0x14209cff8ULL;
/// double, the GPU score the auto-selector picks the quality level from; getter fcn.1400df7a0 (read back:
/// MOVSD XMM0,[0x14209d000]; RET), setter fcn.1400dfeb0. In echovr.exe its only two writers are in the cgs_dx12 device
/// init fcn.14058e7f0 (the script DLLs were not searched): NVIDIA cores x base clock (kHz) x 2 x 1e-9 through NVAPI
/// (0x14058fa04); AMD's AGS teraFlops, or 5.095 when AGS gives 0 and the adapter name contains "Radeon RX 570 Series"
/// (0x14058ecb3). When the rating fails otherwise it stays 0. The selector reads it at 0x140c31e21 against 8.0 and 4.5
/// (vendor 1) or 10.0 and 6.0 (vendor 2); below the lower one it picks quality level 1 (#1419, read back).
inline constexpr std::uint64_t kGpuScoreVa = 0x14209d000ULL;

/// The ovrHmdDesc copy CVR init makes (FUN_14072be80 copies 0x108 bytes to 0x1420c77a8). The layout is the LibOVR SDK's:
/// Type@0, ProductName[64]@8, Resolution{int w,h}@0xF8, DisplayRefreshRate(float)@0x100. The 0x100 offset is the one
/// fcn_14072af50 reads (read back: MOVSS XMM0,[0x1420c78a8]); DefaultEyeFov at +0xB8 is the one CVRSystem::UpdateDisplay
/// reads from 0x1420c7860 (read back), which fixes the layout up to there; Resolution is the SDK's next-but-one field.
inline constexpr std::uint64_t kHmdDescVa = 0x1420c77a8ULL;
inline constexpr std::size_t kHmdDescBytes = 0x108;
inline constexpr std::size_t kHmdTypeOffset = 0;
inline constexpr std::size_t kHmdProductNameOffset = 8;
inline constexpr std::size_t kHmdProductNameBytes = 64;
inline constexpr std::size_t kHmdResolutionOffset = 0xF8;
inline constexpr std::size_t kHmdRefreshOffset = 0x100;

/// Per-eye render target, packed `w | (h << 32)`, stored by CVRSystem::UpdateDisplay (read back: `OR RDX,RAX;
/// MOV [0x1420bb568],RDX` and `MOV [0x1420bb568],RAX`). Zero until UpdateDisplay has run.
inline constexpr std::uint64_t kRenderTargetVa = 0x1420bb568ULL;
/// CGRenderConfig supersampling table, `{float x, float y}` per entry; UpdateDisplay reads entry 0 through
/// fcn.1405625e0 and multiplies the HMD's size by its x (read back).
inline constexpr std::uint64_t kResolutionScaleVa = 0x1420afac0ULL;

/// ApplyRuntimeGfxSettings(SR15GfxSettings*, CR15Game*, int logFlag) (read back).
inline constexpr std::uint64_t kApplyVa = 0x140c31870ULL;
/// The first 27 bytes of ApplyRuntimeGfxSettings, up to and including `SUB RSP,0x60`. Nothing in them is position dependent.
/// The render_config observer and the headless replacement both validate the game's bytes against these.
inline constexpr std::array<std::uint8_t, 27> kApplyPrologue = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24,
                                                                0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x55, 0x41, 0x56,
                                                                0x41, 0x57, 0x48, 0x8b, 0xec, 0x48, 0x83, 0xec, 0x60};
/// The GPU-based auto-selector fcn.140c31e00(SR15GfxSettings*, CR15Game*) (read back).
inline constexpr std::uint64_t kSelectorVa = 0x140c31e00ULL;
/// The return address of the boot call to ApplyRuntimeGfxSettings: FUN_1401090c0 `CALL` at 0x140109209 (read back).
inline constexpr std::uint64_t kBootApplyReturnVa = 0x14010920eULL;

/// A VA in the image, as an address in the running process.
inline std::uintptr_t Resolve(std::uintptr_t imageBase, std::uint64_t va) {
  return imageBase + static_cast<std::uintptr_t>(va - kImageBase);
}

/// How the line reads the game. Production reads its own process with ReadProcessMemory; a test serves a fake image.
class MemoryReader {
 public:
  virtual ~MemoryReader() = default;
  /// True when all `length` bytes at `address` were copied to `out`; false (and `out` unspecified) otherwise.
  virtual bool Read(std::uintptr_t address, void* out, std::size_t length) const = 0;
};

// ---- The game's SR15GfxSettings, as ApplyRuntimeGfxSettings reads it ----

/// Offsets read back from the disassembly of ApplyRuntimeGfxSettings. Types are the ones the function loads them as:
/// MOVSS for the floats, MOV r32 for the integers.
inline constexpr std::size_t kSettingsFlagsOffset = 0x0;           // u32: bit0 fullscreen, bit1 adaptive res, bit2 TAA, bit3 multires
inline constexpr std::size_t kSettingsDisplayOffset = 0x4;         // u32
inline constexpr std::size_t kSettingsResolutionScaleOffset = 0x8; // float
inline constexpr std::size_t kSettingsAdaptiveMinOffset = 0xC;     // float
inline constexpr std::size_t kSettingsAdaptiveMaxOffset = 0x10;    // float
inline constexpr std::size_t kSettingsAdaptiveTargetOffset = 0x14; // i32
inline constexpr std::size_t kSettingsMsaaOffset = 0x1C;           // i32
inline constexpr std::size_t kSettingsQualityLevelOffset = 0x24;   // i32
/// Bytes read: through the end of qualitylevel.
inline constexpr std::size_t kSettingsBytes = 0x28;

inline constexpr std::uint32_t kFlagFullscreen = 1U << 0;
inline constexpr std::uint32_t kFlagAdaptiveResolution = 1U << 1;
inline constexpr std::uint32_t kFlagTaa = 1U << 2;
inline constexpr std::uint32_t kFlagMultiRes = 1U << 3;

struct GfxSettings {
  std::uint32_t flags = 0;
  std::uint32_t display = 0;
  float resolutionScale = 0.0F;
  float adaptiveMin = 0.0F;
  float adaptiveMax = 0.0F;
  std::int32_t adaptiveTarget = 0;
  std::int32_t msaa = 0;
  std::int32_t qualityLevel = 0;
};

/// Reads the settings struct at `address`. False when the read fails.
bool ReadGfxSettings(const MemoryReader& reader, std::uintptr_t address, GfxSettings* out);

/// What set the settings the game last applied.
enum class SettingsSource {
  Unknown,        // no ApplyRuntimeGfxSettings call seen
  AutoSelected,   // the boot call, on the struct the GPU auto-selector had just filled
  SavedOrDefault, // the boot call, with no selector run on that struct
  MenuOrJson,     // any other caller
};
const char* SettingsSourceName(SettingsSource source);

/// What the two observer hooks saw. The hooks fire at boot and when a setting changes, never per frame; the state is
/// mutex-guarded because the hook and the check can run on different threads.
class SettingsObserver {
 public:
  struct View {
    bool seen = false;  // an Apply call was seen and its settings were read
    GfxSettings settings;
    SettingsSource source = SettingsSource::Unknown;
    std::uint64_t applyCalls = 0;
    std::uint64_t readFailures = 0;  // Apply calls whose settings struct could not be read
  };

  /// An ApplyRuntimeGfxSettings call. `returnAddress` is the call's return address in the game; `bootReturnAddress` is
  /// where the boot call returns to. The source is the boot call after a selector run on this same struct
  /// (AutoSelected), the boot call without one (SavedOrDefault), or any other caller (MenuOrJson).
  void OnApply(const MemoryReader& reader, std::uintptr_t settingsAddress, std::uintptr_t returnAddress,
               std::uintptr_t bootReturnAddress);
  /// The auto-selector ran on the struct at `settingsAddress`.
  void OnSelector(std::uintptr_t settingsAddress);
  View Snapshot() const;
  /// Forgets everything seen (tests).
  void Reset();

 private:
  mutable std::mutex mutex_;
  View view_;
  std::uintptr_t selectorAddress_ = 0;
};

// ---- The snapshot ----

/// A value that was read, or was not.
template <typename T>
struct Reading {
  bool ok = false;
  T value{};
};

struct Snapshot {
  Reading<std::string> gpu;
  Reading<std::uint32_t> gpuVendor;
  Reading<double> gpuScore;
  Reading<std::uint64_t> vramMb;
  std::string gpuDriver = "unknown";
  Reading<std::string> headset;
  Reading<std::int32_t> hmdType;
  std::string runtimeVersion = "unknown";
  Reading<float> refreshHz;
  Reading<std::int32_t> hmdW;
  Reading<std::int32_t> hmdH;
  Reading<std::int32_t> renderW;  // ok only when the packed target is non-zero
  Reading<std::int32_t> renderH;
  Reading<float> resolutionScale;
  SettingsObserver::View settings;
};

/// Reads everything the game holds. A field that cannot be read is left `ok == false`; the driver and the runtime
/// version are not game memory and are filled by the caller.
Snapshot ReadSnapshot(const MemoryReader& reader, std::uintptr_t imageBase, const SettingsObserver::View& settings);

/// A string safe to put inside double quotes on one line: control bytes become spaces, `"` becomes `'`, `\` becomes
/// `/`, and any byte above 0x7E becomes `?`. Leading and trailing spaces are removed.
std::string CleanText(const std::string& text);

/// The snapshot's fields in line order, as name and formatted value. The line and the diff both come from this list, so
/// a field cannot be on the line and missing from the diff.
std::vector<std::pair<std::string, std::string>> Fields(const Snapshot& snapshot);

/// Names of the fields whose value differs between the two snapshots, in the later one's line order, then any the
/// earlier one had and the later one lacks. Empty when the line would read the same.
std::vector<std::string> ChangedFields(const Snapshot& before, const Snapshot& after);

enum class Reason { FirstFrame, Change };

/// `[NEVR.RENDER] render_config reason=<first_frame|change> changed=<a,b|-> platform=pcvr gpu="..." ...`: one line, no
/// newline inside.
std::string FormatLine(const Snapshot& snapshot, Reason reason, const std::vector<std::string>& changed);

/// What the check asks of the process for the two values that are not game memory.
struct Externals {
  /// The user-mode driver version of the adapter whose description equals the GPU name; "unknown" on any failure.
  std::function<std::string(const std::string& gpuName)> gpuDriver;
  /// The LibOVR runtime's version string; "unknown" on any failure.
  std::function<std::string()> runtimeVersion;
};

/// The once-a-second check. It writes the line at the first frame and again whenever the snapshot differs from the
/// last one written.
class Collector {
 public:
  /// At most one evaluation per second.
  static constexpr std::uint64_t kIntervalMs = 1000;
  /// The first-frame line is written without a render target once this long has passed since the first evaluation.
  static constexpr std::uint64_t kFirstFrameGraceMs = 5000;

  Collector(const MemoryReader& reader, std::uintptr_t imageBase, const SettingsObserver& settings, Externals externals);

  /// `nowMs` is a monotonic clock. Returns the line to log, or an empty string when there is nothing to say.
  std::string Poll(std::uint64_t nowMs);

 private:
  const MemoryReader& reader_;
  std::uintptr_t imageBase_;
  const SettingsObserver& settings_;
  Externals externals_;

  bool started_ = false;
  std::uint64_t startMs_ = 0;
  std::uint64_t lastEvalMs_ = 0;
  bool emitted_ = false;
  Snapshot last_;
  std::string gpuDriver_ = "unknown";
  std::string runtimeVersion_ = "unknown";
};

}  // namespace nevr_render_config
