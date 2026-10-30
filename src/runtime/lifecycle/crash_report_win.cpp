#include "runtime/lifecycle/crash_report_win.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string.h>

#include "core/crash_report.h"
#include "core/logging.h"
#include "runtime/lifecycle/crash_recovery.h"
#include "runtime/lifecycle/crash_recovery_sites.h"
#include "runtime/lifecycle/crash_report_pe.h"
#include "runtime/lifecycle/crash_report_win_common.h"
#include "runtime/lifecycle/readable_memory.h"

namespace nevr_crash_win {

namespace {

constexpr std::size_t kPathCapacity = MAX_PATH + 32;

char g_stem[kPathCapacity];  // `<crashes dir>\nevr-crash-<run id>`; empty when there is no directory
ModuleLookupFn g_lookup = nullptr;
volatile LONG g_reportCount = 0;

bool Readable(const void* address, std::size_t length) {
  return nevr_crash_recovery::IsReadableMemory(address, length);
}

bool ResolveAddress(std::uint64_t address, nevr_crash::Location* out) {
  ModuleSpan span{};
  if (g_lookup == nullptr || !g_lookup(address, &span)) return false;
  nevr_crash::CopyText(out->module, sizeof(out->module), span.name);
  out->module_base = span.base;
  const auto* image = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(span.base));
  nevr_crash_pe::ImageIdentity identity{};
  if (nevr_crash_pe::ReadIdentity(image, &Readable, &identity)) {
    nevr_crash_pe::FormatIdentity(identity, out->build_id, sizeof(out->build_id));
  }
  const std::uint64_t rva = address - span.base;
  std::uint64_t offset = 0;
  if (rva <= 0xffffffffull && nevr_crash_pe::NearestExport(image, &Readable, static_cast<std::uint32_t>(rva),
                                                           out->symbol, sizeof(out->symbol), &offset)) {
    out->symbol_offset = offset;
    return true;
  }
  // The game has no exports. The server-mode attribution table names the functions the null-dereference family hits;
  // an exact function start is the only match it is allowed to claim.
  if (_stricmp(span.name, "echovr.exe") == 0) {
    if (const char* site = nevr_crash_recovery::LookupKnownNullDerefSite(static_cast<std::int64_t>(rva))) {
      nevr_crash::CopyText(out->symbol, sizeof(out->symbol), site);
      out->symbol_offset = 0;
    }
  }
  return true;
}

bool KnownAddress(std::uint64_t address) {
  ModuleSpan span{};
  return g_lookup != nullptr && g_lookup(address, &span);
}

void Describe(const nevr_crash::CrashInfo& info, nevr_crash::Report* report) {
  DescribeException(info, report, &Readable, &KnownAddress);
  if (info.thread_id == GameLoopThreadId()) {
    nevr_crash::CopyText(report->thread_name, sizeof(report->thread_name), "game-loop");
  }
}

// `<stem>-<n>` + the report suffix: one file per report, never appended to.
bool ReportPath(const nevr_crash::CrashInfo&, char* buffer, std::size_t capacity) {
  if (g_stem[0] == '\0') return false;
  const LONG number = InterlockedIncrement(&g_reportCount);
  const int length = std::snprintf(buffer, capacity, "%s-%ld%s", g_stem, number, nevr_crash::kReportSuffix);
  return length > 0 && static_cast<std::size_t>(length) < capacity;
}

// The same two sinks every [NEVR.CRASH] line has: standard error and the crash record, through the handler's own writer.
void LogSummary(const char* line) {
  char text[nevr_crash::kSummaryCapacity + 16];
  const int length = std::snprintf(text, sizeof(text), "[NEVR.CRASH] %s", line);
  if (length > 0) WriteCrashRecordLine(text);
}

const nevr_crash::Platform kPlatform = {&ResolveAddress,   &Describe,         &ReportPath,      &OpenReportFile,
                                        &WriteReportFile, &CloseReportFile, &RenameReportFile, &LogSummary};

}  // namespace

void RegisterCrashReportPlatform(const char* crashes_dir, ModuleLookupFn lookup) {
  g_lookup = lookup;
  g_stem[0] = '\0';
  if (crashes_dir != nullptr && crashes_dir[0] != '\0') {
    const int length = std::snprintf(g_stem, sizeof(g_stem), "%s\\nevr-crash-%s", crashes_dir, GetRunId());
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(g_stem)) g_stem[0] = '\0';
  }
  nevr_crash::SetPlatform(&kPlatform);
}

}  // namespace nevr_crash_win
