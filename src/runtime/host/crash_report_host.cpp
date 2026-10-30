#include "runtime/host/crash_report_host.h"

#include "core/crash_report.h"
#include "runtime/host/host_json.h"
#include "runtime/host/host_paths.h"
#include "runtime/host/path_buffer.h"
#include "runtime/lifecycle/crash_report_pe.h"
#include "runtime/lifecycle/crash_report_win_common.h"
#include "runtime/lifecycle/readable_memory.h"

#include <cstdio>
#include <cstring>
#include <ctime>

namespace nevr_host {
namespace {

constexpr std::size_t kMaxModules = 256;
constexpr std::size_t kModuleNameBytes = 64;
constexpr std::size_t kPathCapacity = MAX_PATH * 4 + 64;

struct ModuleEntry {
  std::uint64_t base;
  std::uint64_t end;
  char name[kModuleNameBytes];
  char build_id[nevr_crash::kBuildIdBytes];
};

ModuleEntry g_modules[kMaxModules];
volatile LONG g_module_count = 0;
char g_stem[kPathCapacity];  // `<crashes dir>\nevr-host-<pid>-<time>`; empty when there is no usable directory
wchar_t g_dir[MAX_PATH] = {};
volatile LONG g_report_count = 0;
BootstrapDiagnostics* g_diagnostics = nullptr;

bool Readable(const void* address, std::size_t length) {
  return nevr_crash_recovery::IsReadableMemory(address, length);
}

const ModuleEntry* FindModule(std::uint64_t address) {
  const LONG count = g_module_count;
  for (LONG i = 0; i < count; ++i) {
    if (address >= g_modules[i].base && address < g_modules[i].end) return &g_modules[i];
  }
  return nullptr;
}

bool KnownAddress(std::uint64_t address) { return FindModule(address) != nullptr; }

bool ResolveAddress(std::uint64_t address, nevr_crash::Location* out) {
  const ModuleEntry* module = FindModule(address);
  if (module == nullptr) return false;
  nevr_crash::CopyText(out->module, sizeof(out->module), module->name);
  nevr_crash::CopyText(out->build_id, sizeof(out->build_id), module->build_id);
  out->module_base = module->base;
  const auto* image = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(module->base));
  const std::uint64_t rva = address - module->base;
  std::uint64_t offset = 0;
  if (rva <= 0xffffffffull && nevr_crash_pe::NearestExport(image, &Readable, static_cast<std::uint32_t>(rva), out->symbol,
                                                           sizeof(out->symbol), &offset)) {
    out->symbol_offset = offset;
  }
  return true;
}

void Describe(const nevr_crash::CrashInfo& info, nevr_crash::Report* report) {
  nevr_crash_win::DescribeException(info, report, &Readable, &KnownAddress);
}

// `<stem>-<n>` + the report suffix: one file per report, never appended to. The folder is made here when it is missing.
bool ReportPath(const nevr_crash::CrashInfo&, char* buffer, std::size_t capacity) {
  if (g_stem[0] == '\0') return false;
  wchar_t dir[MAX_PATH];
  std::memcpy(dir, g_dir, sizeof(dir));
  CreateDirectories(dir);
  const LONG number = InterlockedIncrement(&g_report_count);
  const int length = std::snprintf(buffer, capacity, "%s-%ld%s", g_stem, static_cast<long>(number), nevr_crash::kReportSuffix);
  return length > 0 && static_cast<std::size_t>(length) < capacity;
}

// The one `crash_report` record per report, through the host log's crash-safe append.
void LogSummary(const char* line) {
  if (g_diagnostics == nullptr) return;
  char buffer[nevr_crash::kSummaryCapacity + 128];
  JsonLine record(buffer, sizeof(buffer), "crash_report");
  record.Str("summary", line);
  const std::size_t length = record.Finish();
  if (length != 0) g_diagnostics->AppendLine(buffer, length);
}

const nevr_crash::Platform kPlatform = {&ResolveAddress,
                                        &Describe,
                                        &ReportPath,
                                        &nevr_crash_win::OpenReportFile,
                                        &nevr_crash_win::WriteReportFile,
                                        &nevr_crash_win::CloseReportFile,
                                        &nevr_crash_win::RenameReportFile,
                                        &LogSummary};

using EnumProcessModulesFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);

}  // namespace

void PrepareCrashReport(BootstrapDiagnostics* diagnostics) noexcept {
  g_diagnostics = diagnostics;
  g_stem[0] = '\0';
  g_dir[0] = L'\0';
  wchar_t local_app_data[MAX_PATH] = {};
  if (!CopyEnv(local_app_data, MAX_PATH, L"LOCALAPPDATA") || !AppendPath(g_dir, MAX_PATH, local_app_data, kCrashesSubdirectory)) {
    g_dir[0] = L'\0';
  } else {
    // The shared report writes through ANSI file calls: a folder the ANSI code page cannot spell leaves the report unwritten
    // (and logged), as the runtime's does.
    char narrow[MAX_PATH * 4] = {};
    BOOL lossy = FALSE;
    const int converted = WideCharToMultiByte(CP_ACP, 0, g_dir, -1, narrow, sizeof(narrow), nullptr, &lossy);
    if (converted > 0 && lossy == FALSE) {
      const unsigned long long stamp = static_cast<unsigned long long>(std::time(nullptr));
      const int length = std::snprintf(g_stem, sizeof(g_stem), "%s\\nevr-host-%lu-%llx", narrow,
                                       static_cast<unsigned long>(GetCurrentProcessId()), stamp);
      if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(g_stem)) g_stem[0] = '\0';
    }
  }
  nevr_crash::SetPlatform(&kPlatform);
}

void SnapshotModulesForCrashReport() noexcept {
  const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  if (kernel32 == nullptr) return;
  // psapi.dll is not an import of the host (tools/verify_pe_contract.py); kernel32 exports the same function.
  const auto enumerate =
      reinterpret_cast<EnumProcessModulesFn>(reinterpret_cast<void*>(GetProcAddress(kernel32, "K32EnumProcessModules")));
  if (enumerate == nullptr) return;
  HMODULE handles[kMaxModules];
  DWORD needed = 0;
  if (!enumerate(GetCurrentProcess(), handles, sizeof(handles), &needed)) return;
  const std::size_t found = needed / sizeof(HMODULE);
  const std::size_t count = found < kMaxModules ? found : kMaxModules;
  LONG stored = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const auto* image = reinterpret_cast<const std::uint8_t*>(handles[i]);
    nevr_crash_pe::ImageIdentity identity{};
    if (!nevr_crash_pe::ReadIdentity(image, &Readable, &identity) || identity.size_of_image == 0) continue;
    ModuleEntry& entry = g_modules[stored];
    char path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(handles[i], path, MAX_PATH);
    const char* name = path;
    for (DWORD at = 0; at < length; ++at) {
      if (path[at] == '\\' || path[at] == '/') name = path + at + 1;
    }
    entry.base = reinterpret_cast<std::uintptr_t>(handles[i]);
    entry.end = entry.base + identity.size_of_image;
    nevr_crash::CopyText(entry.name, sizeof(entry.name), length == 0 ? "unknown" : name);
    nevr_crash_pe::FormatIdentity(identity, entry.build_id, sizeof(entry.build_id));
    ++stored;
  }
  InterlockedExchange(&g_module_count, stored);
}

bool WriteCrashReport(PEXCEPTION_POINTERS exception_pointers) noexcept {
  nevr_crash::CrashInfo info{};
  info.context = exception_pointers;
  info.thread_id = GetCurrentThreadId();
  if (exception_pointers != nullptr && exception_pointers->ExceptionRecord != nullptr) {
    const EXCEPTION_RECORD* record = exception_pointers->ExceptionRecord;
    info.code = static_cast<std::int64_t>(record->ExceptionCode);
    info.pc = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(record->ExceptionAddress));
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
      info.fault_address = static_cast<std::uint64_t>(record->ExceptionInformation[1]);
    }
    if (exception_pointers->ContextRecord != nullptr) info.pc = exception_pointers->ContextRecord->Rip;
  }
  return nevr_crash::WriteReport(info);
}

}  // namespace nevr_host
