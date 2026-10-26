#include "runtime/host/runtime_load.h"

#include "runtime/host/host_identity.h"
#include "runtime/host/host_json.h"
#include "runtime/host/loader_lock.h"
#include "runtime/host/path_buffer.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <atomic>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>

namespace nevr_host {
namespace {

constexpr wchar_t kRuntimeFileName[] = L"nevr-runtime.dll";
constexpr std::size_t kMarkerBytes = 512;

// Bounded copy of a NUL-terminated wide path into a fixed buffer.
void CopyPath(wchar_t* dst, std::size_t dst_size, const wchar_t* src) {
  if (dst == nullptr || dst_size == 0) return;
  if (src == nullptr) {
    dst[0] = L'\0';
    return;
  }
  std::size_t i = 0;
  while (i + 1 < dst_size && src[i] != L'\0') {
    dst[i] = src[i];
    ++i;
  }
  dst[i] = L'\0';
}

// Convert a wide path to UTF-8 for the JSON record (bounded, correct for the
// UTF-16 wchar_t on Windows; nlohmann's own wide adapter is UTF-32 and would
// mangle these paths under MinGW).
std::string ToUtf8(const wchar_t* path) {
  std::string out;
  const int needed = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return out;
  out.resize(static_cast<std::size_t>(needed) - 1);
  WideCharToMultiByte(CP_UTF8, 0, path, -1, out.data(), needed, nullptr, nullptr);
  return out;
}

std::string AbiVersion(std::uint32_t major, std::uint32_t minor) {
  return std::to_string(major) + "." + std::to_string(minor);
}

// True when the outcome reflects a descriptor that was actually read from the
// runtime: NEVR_GetRuntimeApi was resolved and called, so abi_status and
// actual_abi describe a real (possibly null or rejected) descriptor. Outcomes
// where the file was absent, a dependency failed to load, the entry point was
// missing, or the load failed otherwise never read a descriptor — emitting
// abi_status/actual_abi for them would falsely read as "null descriptor / ABI 0.0".
bool DescriptorRead(RuntimeLoadOutcome outcome) {
  switch (outcome) {
    case RuntimeLoadOutcome::kNullDescriptor:
    case RuntimeLoadOutcome::kAbiRejected:
    case RuntimeLoadOutcome::kInitFailed:
    case RuntimeLoadOutcome::kLoaded:
      return true;
    case RuntimeLoadOutcome::kNoPath:
    case RuntimeLoadOutcome::kRuntimeAbsent:
    case RuntimeLoadOutcome::kDependencyLoadFailed:
    case RuntimeLoadOutcome::kLoadFailed:
    case RuntimeLoadOutcome::kMissingEntry:
      return false;
  }
  return false;
}

}  // namespace

const char* RuntimeLoadResult::OutcomeName() const {
  switch (outcome) {
    case RuntimeLoadOutcome::kLoaded: return "loaded";
    case RuntimeLoadOutcome::kNoPath: return "no_path";
    case RuntimeLoadOutcome::kRuntimeAbsent: return "runtime_absent";
    case RuntimeLoadOutcome::kDependencyLoadFailed: return "dependency_load_failed";
    case RuntimeLoadOutcome::kLoadFailed: return "load_failed";
    case RuntimeLoadOutcome::kMissingEntry: return "missing_entry";
    case RuntimeLoadOutcome::kNullDescriptor: return "null_descriptor";
    case RuntimeLoadOutcome::kAbiRejected: return "abi_rejected";
    case RuntimeLoadOutcome::kInitFailed: return "init_failed";
  }
  return "unknown";
}

const char* RuntimeLoadResult::DegradedAction() const {
  return Loaded() ? "dispatch" : "stop_game";
}

RuntimeLoadResult ClassifyLoad(bool path_ok, const wchar_t* path, DWORD load_error,
                               HMODULE module, bool has_entry, const NevrRuntimeApi* api,
                               nevr_runtime_abi::Status abi_status, std::int32_t init_code) {
  RuntimeLoadResult result;
  result.path_ok = path_ok;
  CopyPath(result.path, MAX_PATH, path);
  result.win32_error = load_error;
  result.abi_status = abi_status;
  result.init_code = init_code;
  result.module = module;
  result.api = api;

  // Copy the actual ABI version only when the descriptor's 16-byte header is
  // readable (ValidateRuntimeApi reports kHeaderTooSmall / kNullDescriptor when
  // it is not). Reading abi_major/abi_minor past a short header would fault.
  if (api != nullptr && abi_status != nevr_runtime_abi::Status::kHeaderTooSmall &&
      abi_status != nevr_runtime_abi::Status::kNullDescriptor) {
    result.actual_major = api->header.abi_major;
    result.actual_minor = api->header.abi_minor;
  }

  if (!path_ok) {
    result.outcome = RuntimeLoadOutcome::kNoPath;
    return result;
  }
  if (module == nullptr) {
    if (load_error == ERROR_FILE_NOT_FOUND || load_error == ERROR_PATH_NOT_FOUND) {
      result.outcome = RuntimeLoadOutcome::kRuntimeAbsent;
    } else if (load_error == ERROR_MOD_NOT_FOUND) {
      result.outcome = RuntimeLoadOutcome::kDependencyLoadFailed;
    } else {
      result.outcome = RuntimeLoadOutcome::kLoadFailed;
    }
    return result;
  }
  if (!has_entry) {
    result.outcome = RuntimeLoadOutcome::kMissingEntry;
    return result;
  }
  if (api == nullptr) {
    result.outcome = RuntimeLoadOutcome::kNullDescriptor;
    return result;
  }
  if (abi_status != nevr_runtime_abi::Status::kOk) {
    result.outcome = RuntimeLoadOutcome::kAbiRejected;
    return result;
  }
  if (init_code != 0) {
    result.outcome = RuntimeLoadOutcome::kInitFailed;
    return result;
  }
  result.outcome = RuntimeLoadOutcome::kLoaded;
  return result;
}

#if defined(NEVR_HOST_TESTING)
namespace {
std::atomic<bool> g_fail_emit_for_test{false};
}  // namespace

void SetEmitLoadFailureForTest(bool fail) noexcept { g_fail_emit_for_test.store(fail); }
#endif

void EmitLoadDiagnostic(BootstrapDiagnostics& diagnostics, const RuntimeLoadResult& result) noexcept {
  try {
#if defined(NEVR_HOST_TESTING)
    if (g_fail_emit_for_test.load()) throw std::runtime_error("record builder failed (test seam)");
#endif
    nlohmann::json record;
    record["event"] = "runtime_load";
    record["outcome"] = result.OutcomeName();
    record["degraded_action"] = result.DegradedAction();
    record["path_ok"] = result.path_ok;
    record["loader_lock_held"] = result.loader_lock_held;
    if (result.path_ok) record["path"] = ToUtf8(result.path);
    record["expected_abi"] = AbiVersion(result.expected_major, result.expected_minor);
    const bool descriptor_read = DescriptorRead(result.outcome);
    if (descriptor_read) {
      record["actual_abi"] = AbiVersion(result.actual_major, result.actual_minor);
    }
    if (result.win32_error != 0) record["win32_error"] = result.win32_error;
    if (descriptor_read) record["abi_status"] = nevr_runtime_abi::StatusName(result.abi_status);
    if (result.outcome == RuntimeLoadOutcome::kInitFailed || result.Loaded()) {
      record["init_code"] = result.init_code;
    }
    if (result.init_threw) record["init_threw"] = true;
    diagnostics.Emit(record);
  } catch (const std::exception& error) {
    // Fixed buffer, no allocation: the record the load would have written, reduced to what
    // survives. Emit() has its own marker for a record it cannot write; this one is for a record
    // that could not be built.
    char buffer[kMarkerBytes];
    JsonLine line(buffer, sizeof(buffer), "runtime_load");
    line.Str("error", "record_build_failed")
        .Str("what", error.what())
        .Str("outcome", result.OutcomeName())
        .Bool("loader_lock_held", result.loader_lock_held);
    const std::size_t length = line.Finish();
    if (length != 0) diagnostics.AppendLine(buffer, length);
  }
}

void EmitStopDiagnostic(BootstrapDiagnostics& diagnostics, const RuntimeLoadResult& result) noexcept {
  try {
#if defined(NEVR_HOST_TESTING)
    if (g_fail_emit_for_test.load()) throw std::runtime_error("record builder failed (test seam)");
#endif
    nlohmann::json record;
    record["event"] = "game_stop";
    record["reason"] = result.OutcomeName();
    record["exit_code"] = kRuntimeStopExitCode;
    record["host_identity"] = IdentityLiteral();
    // The path searched: the one thing a player or support needs for a missing file.
    if (result.path_ok) record["runtime_path"] = ToUtf8(result.path);
    if (result.win32_error != 0) record["win32_error"] = result.win32_error;
    diagnostics.Emit(record);
  } catch (const std::exception& error) {
    char buffer[kMarkerBytes];
    JsonLine line(buffer, sizeof(buffer), "game_stop");
    line.Str("error", "record_build_failed")
        .Str("what", error.what())
        .Str("reason", result.OutcomeName())
        .UInt("exit_code", kRuntimeStopExitCode);
    const std::size_t length = line.Finish();
    if (length != 0) diagnostics.AppendLine(buffer, length);
  }
}

bool ResolveRuntimePath(HMODULE host_module, wchar_t* out, std::size_t out_size) noexcept {
  if (out == nullptr || out_size == 0) return false;
  out[0] = L'\0';
  if (!DirOfModule(out, out_size, host_module)) return false;
  const std::size_t directory_length = wcslen(out);
  constexpr std::size_t kNameLength = sizeof(kRuntimeFileName) / sizeof(kRuntimeFileName[0]);
  if (directory_length + kNameLength > out_size) {
    out[0] = L'\0';
    return false;
  }
  std::memcpy(out + directory_length, kRuntimeFileName, sizeof(kRuntimeFileName));
  return true;
}

void RuntimeLoader::Prepare(HMODULE host_module) {
  path_ok_ = ResolveRuntimePath(host_module, path_, MAX_PATH);
  if (!path_ok_) path_[0] = L'\0';
}

std::int32_t RuntimeLoader::CallInitialize(const NevrRuntimeApi* api, const NevrHostContext& host,
                                           bool* threw) noexcept {
  *threw = false;
  try {
    return api->initialize(&host);
  } catch (const std::exception&) {
    *threw = true;
    return kInitThrewCode;
  } catch (...) {  // exceptions must not cross the DLL boundary: a throw of any other type is a load failure too
    *threw = true;
    return kInitThrewCode;
  }
}

RuntimeLoadResult RuntimeLoader::Load(const NevrHostContext& host,
                                      BootstrapDiagnostics& diagnostics) {
  result_ = RuntimeLoadResult{};
  result_.path_ok = path_ok_;
  CopyPath(result_.path, MAX_PATH, path_);
  const bool loader_lock_held = LoaderLockHeldByThisThread();

  HMODULE module = nullptr;
  DWORD load_error = 0;
  bool has_entry = false;
  const NevrRuntimeApi* api = nullptr;
  nevr_runtime_abi::Status abi_status = nevr_runtime_abi::Status::kNullDescriptor;
  std::int32_t init_code = 0;
  bool init_threw = false;

  if (path_ok_) {
    // Distinguish "runtime absent" (the file itself is missing) from a load
    // failure with the file present (e.g. a missing dependency). LoadLibraryExW
    // returns ERROR_MOD_NOT_FOUND for both, so the file must be probed first.
    const DWORD attributes = GetFileAttributesW(path_);
    if (attributes == INVALID_FILE_ATTRIBUTES &&
        (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)) {
      load_error = ERROR_FILE_NOT_FOUND;
    } else {
      module = LoadLibraryExW(path_, nullptr,
                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (module == nullptr) load_error = GetLastError();
    }
  }
  if (module != nullptr) {
    const auto get_api = reinterpret_cast<NevrGetRuntimeApiFn>(
        GetProcAddress(module, nevr_runtime_abi::kGetRuntimeApiExport));
    has_entry = get_api != nullptr;
    if (has_entry) {
      api = get_api();
      abi_status = nevr_runtime_abi::ValidateRuntimeApi(api);
      if (abi_status == nevr_runtime_abi::Status::kOk) {
        init_code = CallInitialize(api, host, &init_threw);
      }
    }
  }

  result_ = ClassifyLoad(path_ok_, path_, load_error, module, has_entry, api, abi_status,
                         init_code);
  result_.loader_lock_held = loader_lock_held;
  result_.init_threw = init_threw;
  EmitLoadDiagnostic(diagnostics, result_);
  return result_;
}

}  // namespace nevr_host
