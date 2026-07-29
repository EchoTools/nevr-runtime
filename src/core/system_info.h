#pragma once

#include <cstdint>
#include <string>

/// Host description — measured, never invented.
///
/// This exists because the login payload used to carry a `system_info` block
/// made entirely of string literals: "cpu":"Wine", "video_card":"Wine D3D12",
/// 4 physical cores, 8 logical, 16384 MB. None of it measured, all of it sent
/// as though it were. Fabricated telemetry is worse than absent telemetry —
/// absent data is obviously absent, while fabricated data is indistinguishable
/// from a real reading and gets acted on.
///
/// Every field here is either measured or explicitly empty. A value this code
/// cannot determine is left empty/zero and the caller decides what to send; it
/// is never filled with a plausible-looking guess.
namespace SystemInfo {

struct Host {
  /// Physical/logical CPU counts. 0 means "could not determine".
  uint32_t physical_cores = 0;
  uint32_t logical_cores  = 0;

  /// Physical RAM in MB. 0 means "could not determine".
  uint64_t memory_total_mb = 0;
  uint64_t memory_used_mb  = 0;

  /// CPUID brand string, e.g. "AMD Ryzen 9 5950X 16-Core Processor".
  /// Empty if CPUID is unavailable.
  std::string cpu_brand;

  /// Wine version from ntdll!wine_get_version, e.g. "10.0".
  /// EMPTY means native Windows — that is the detection, not a failure.
  std::string wine_version;

  /// Host OS as Wine reports it (ntdll!wine_get_host_version), e.g. "Linux".
  /// Empty on native Windows, or if the export is unavailable.
  std::string wine_host_os;

  /// Windows build number as reported to the process. 0 if unavailable.
  uint32_t os_build = 0;

  bool IsWine() const { return !wine_version.empty(); }
};

/// Measured once on first call and cached — these values do not change during a
/// process lifetime, and the probes (CPUID, GetLogicalProcessorInformation)
/// are not free enough to want on a hot path.
const Host& Get();

}  // namespace SystemInfo
