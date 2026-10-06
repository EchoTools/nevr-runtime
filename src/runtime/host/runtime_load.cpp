#include "runtime/host/runtime_load.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <string>

namespace NevrHost {
namespace {

constexpr wchar_t kRuntimeFileName[] = L"nevr.dll";

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
  return Loaded() ? "dispatch" : "run_original_only";
}

RuntimeLoadResult ClassifyLoad(bool path_ok, const wchar_t* path, DWORD load_error,
                               HMODULE module, bool has_entry, const NevrRuntimeApi* api,
                               NevrAbi::Status abi_status, std::int32_t init_code) {
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
  if (api != nullptr && abi_status != NevrAbi::Status::kHeaderTooSmall &&
      abi_status != NevrAbi::Status::kNullDescriptor) {
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
  if (abi_status != NevrAbi::Status::kOk) {
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

void EmitLoadDiagnostic(BootstrapDiagnostics& diagnostics, const RuntimeLoadResult& result) {
  nlohmann::json record;
  record["event"] = "runtime_load";
  record["outcome"] = result.OutcomeName();
  record["degraded_action"] = result.DegradedAction();
  record["path_ok"] = result.path_ok;
  if (result.path_ok) record["path"] = ToUtf8(result.path);
  record["expected_abi"] = AbiVersion(result.expected_major, result.expected_minor);
  if (result.outcome != RuntimeLoadOutcome::kLoaded) {
    record["actual_abi"] = AbiVersion(result.actual_major, result.actual_minor);
  }
  if (result.win32_error != 0) record["win32_error"] = result.win32_error;
  if (result.abi_status != NevrAbi::Status::kOk) {
    record["abi_status"] = NevrAbi::StatusName(result.abi_status);
  }
  if (result.outcome == RuntimeLoadOutcome::kInitFailed) {
    record["init_code"] = result.init_code;
  }
  diagnostics.Emit(record);
}

void RuntimeLoader::Prepare(HMODULE shim_module) {
  path_ok_ = false;
  path_[0] = L'\0';
  const DWORD length = GetModuleFileNameW(shim_module, path_, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return;
  wchar_t* separator = wcsrchr(path_, L'\\');
  if (separator == nullptr) return;
  const std::size_t directory_length = static_cast<std::size_t>(separator - path_) + 1;
  constexpr std::size_t kNameLength = sizeof(kRuntimeFileName) / sizeof(kRuntimeFileName[0]);
  if (directory_length + kNameLength > MAX_PATH) return;
  std::memcpy(path_ + directory_length, kRuntimeFileName, sizeof(kRuntimeFileName));
  path_ok_ = true;
}

RuntimeLoadResult RuntimeLoader::Load(const NevrHostContext& host,
                                      BootstrapDiagnostics& diagnostics) {
  result_ = RuntimeLoadResult{};
  result_.path_ok = path_ok_;
  CopyPath(result_.path, MAX_PATH, path_);

  HMODULE module = nullptr;
  DWORD load_error = 0;
  bool has_entry = false;
  const NevrRuntimeApi* api = nullptr;
  NevrAbi::Status abi_status = NevrAbi::Status::kNullDescriptor;
  std::int32_t init_code = 0;

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
        GetProcAddress(module, NevrAbi::kGetRuntimeApiExport));
    has_entry = get_api != nullptr;
    if (has_entry) {
      api = get_api();
      abi_status = NevrAbi::ValidateRuntimeApi(api);
      if (abi_status == NevrAbi::Status::kOk) {
        init_code = api->initialize(&host);
      }
    }
  }

  result_ = ClassifyLoad(path_ok_, path_, load_error, module, has_entry, api, abi_status,
                         init_code);
  if (!result_.Loaded()) EmitLoadDiagnostic(diagnostics, result_);
  return result_;
}

}  // namespace NevrHost
