#include "core/system_info.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#include <vector>
#endif

namespace SystemInfo {
namespace {

#ifdef _WIN32

/// CPUID leaves 0x80000002..0x80000004 hold the 48-byte brand string.
/// Leaf 0x80000000 reports the highest extended leaf supported; if it is below
/// 0x80000004 the brand string is not available and we return empty rather than
/// reading whatever happens to be in the registers.
std::string CpuBrand() {
  int regs[4] = {0, 0, 0, 0};
  __cpuid(regs, 0x80000000);
  if (static_cast<unsigned>(regs[0]) < 0x80000004u) return {};

  char raw[49] = {0};
  for (unsigned leaf = 0; leaf < 3; ++leaf) {
    __cpuid(regs, static_cast<int>(0x80000002u + leaf));
    memcpy(raw + leaf * 16, regs, 16);
  }
  raw[48] = '\0';

  // The brand string is space-padded on both sides on many parts.
  const char* begin = raw;
  while (*begin == ' ') ++begin;
  std::string out(begin);
  while (!out.empty() && (out.back() == ' ' || out.back() == '\0')) out.pop_back();
  return out;
}

/// Physical core count from the processor-relation table. Returns 0 rather than
/// falling back to the logical count — a wrong number here is indistinguishable
/// from a right one downstream, so "unknown" has to stay expressible.
uint32_t PhysicalCores() {
  DWORD bytes = 0;
  GetLogicalProcessorInformation(nullptr, &bytes);
  if (bytes == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return 0;

  std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(
      bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
  if (info.empty()) return 0;
  if (!GetLogicalProcessorInformation(info.data(), &bytes)) return 0;

  uint32_t cores = 0;
  for (const auto& e : info) {
    if (e.Relationship == RelationProcessorCore) ++cores;
  }
  return cores;
}

/// ntdll exports these under Wine and nowhere else, which is the canonical
/// detection. An empty version string therefore MEANS native Windows — it is
/// the answer, not a failure to get one.
void ProbeWine(std::string& version, std::string& host_os) {
  HMODULE ntdll = GetModuleHandleA("ntdll.dll");
  if (!ntdll) return;

  using wine_get_version_fn = const char* (*)(void);
  auto get_version = reinterpret_cast<wine_get_version_fn>(
      reinterpret_cast<void*>(GetProcAddress(ntdll, "wine_get_version")));
  if (!get_version) return;  // native Windows
  if (const char* v = get_version()) version = v;

  using wine_get_host_version_fn = void (*)(const char**, const char**);
  auto get_host = reinterpret_cast<wine_get_host_version_fn>(
      reinterpret_cast<void*>(GetProcAddress(ntdll, "wine_get_host_version")));
  if (!get_host) return;
  const char* sysname = nullptr;
  const char* release = nullptr;
  get_host(&sysname, &release);
  if (sysname) {
    host_os = sysname;
    if (release && *release) {
      host_os += " ";
      host_os += release;
    }
  }
}

Host Measure() {
  Host h;

  SYSTEM_INFO si = {};
  GetSystemInfo(&si);
  h.logical_cores  = si.dwNumberOfProcessors;
  h.physical_cores = PhysicalCores();

  MEMORYSTATUSEX ms = {};
  ms.dwLength = sizeof(ms);
  if (GlobalMemoryStatusEx(&ms)) {
    const uint64_t kMb = 1024ull * 1024ull;
    h.memory_total_mb = ms.ullTotalPhys / kMb;
    // "Used" is derived, not reported: total minus available physical.
    h.memory_used_mb =
        (ms.ullTotalPhys >= ms.ullAvailPhys) ? (ms.ullTotalPhys - ms.ullAvailPhys) / kMb : 0;
  }

  h.cpu_brand = CpuBrand();
  ProbeWine(h.wine_version, h.wine_host_os);

  // Build number via the PEB rather than GetVersionEx: the latter is subject to
  // the manifest-based compatibility shims and lies on modern Windows.
  if (HMODULE ntdll = GetModuleHandleA("ntdll.dll")) {
    using RtlGetVersion_fn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    auto rtl_get_version = reinterpret_cast<RtlGetVersion_fn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    if (rtl_get_version) {
      RTL_OSVERSIONINFOW vi = {};
      vi.dwOSVersionInfoSize = sizeof(vi);
      if (rtl_get_version(&vi) == 0) h.os_build = vi.dwBuildNumber;
    }
  }

  return h;
}

#else  // !_WIN32

Host Measure() { return Host{}; }

#endif

}  // namespace

const Host& Get() {
  static const Host cached = Measure();
  return cached;
}

}  // namespace SystemInfo
