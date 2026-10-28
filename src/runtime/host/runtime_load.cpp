#include "runtime/host/runtime_load.h"

#include "runtime/host/host_identity.h"
#include "runtime/host/host_json.h"
#include "runtime/host/loader_lock.h"
#include "runtime/host/path_buffer.h"
#include "core/nevr_root.h"

#include "runtime/ext/plugin_pins.h"
#include "runtime/ext/plugin_signature.h"

#include <nlohmann/json.hpp>

#include <wincrypt.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <cstring>
#include <ctime>
#include <iterator>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

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

#if defined(NEVR_HOST_TESTING)
std::atomic<bool> g_verify_for_test{false};
std::atomic<const nevr_plugin_signature::Policy*> g_policy_for_test{nullptr};
std::atomic<int> g_dev_build_for_test{-1};
std::atomic<void (*)(const wchar_t*)> g_before_load_for_test{nullptr};
std::atomic<const wchar_t*> g_runtime_dir_for_test{nullptr};
#endif

// The policy the verdict is taken against: the pins, the same constants the runtime's own policy starts from.
const nevr_plugin_signature::Policy& VerifyPolicy() {
#if defined(NEVR_HOST_TESTING)
  if (const nevr_plugin_signature::Policy* policy = g_policy_for_test.load()) return *policy;
#endif
  return nevr_plugin_pins::PinnedPolicy();
}

bool DevBuild() {
#if defined(NEVR_HOST_TESTING)
  const int mode = g_dev_build_for_test.load();
  if (mode >= 0) return mode != 0;
#endif
  return nevr_plugin_pins::IsDevBuild();
}

// The verdict is taken in production. The test targets' harnesses (the crash and attach shims) load unsigned fixture
// runtimes, so a test build verifies only when a test asks it to (SetVerifyRuntimeForTest).
bool VerifyEnabled() noexcept {
#if defined(NEVR_HOST_TESTING)
  return g_verify_for_test.load();
#else
  return true;
#endif
}

// What the versions record states about one file: its SHA-256 and, for nevr-runtime.dll, the version its NEVR-BUILD
// literal carries. An empty field is a fact that could not be read, and the record says "unavailable".
struct FileFacts {
  std::string sha256;
  std::string version;
};

constexpr char kUnavailable[] = "unavailable";

// The whole file, read from offset 0 through `file`. False on any read failure or an empty file.
bool ReadWholeFile(HANDLE file, std::vector<unsigned char>* out) {
  LARGE_INTEGER size{};
  LARGE_INTEGER start{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) return false;
  if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) return false;
  out->resize(static_cast<std::size_t>(size.QuadPart));
  std::size_t done = 0;
  while (done < out->size()) {
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out->size() - done, std::size_t{1} << 20));
    DWORD got = 0;
    if (!ReadFile(file, out->data() + done, want, &got, nullptr) || got == 0) return false;
    done += got;
  }
  return true;
}

// SHA-256 of `bytes` as 64 lowercase hex characters, through CryptoAPI (advapi32, which the host links for the
// verifier). Empty on any failure.
std::string Sha256Hex(const std::vector<unsigned char>& bytes) {
  HCRYPTPROV provider = 0;
  if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return {};
  std::string hex;
  HCRYPTHASH hash = 0;
  if (CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
    bool ok = true;
    std::size_t done = 0;
    while (ok && done < bytes.size()) {
      const std::size_t chunk = std::min<std::size_t>(bytes.size() - done, std::size_t{1} << 20);
      ok = CryptHashData(hash, bytes.data() + done, static_cast<DWORD>(chunk), 0) != FALSE;
      done += chunk;
    }
    BYTE digest[32] = {};
    DWORD length = sizeof(digest);
    if (ok && CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0) && length == sizeof(digest)) {
      constexpr char kDigits[] = "0123456789abcdef";
      for (const BYTE b : digest) {
        hex.push_back(kDigits[b >> 4]);
        hex.push_back(kDigits[b & 0x0f]);
      }
    }
    CryptDestroyHash(hash);
  }
  CryptReleaseContext(provider, 0);
  return hex;
}

// The version field of the file's "NEVR-BUILD <version> <commit40>" literal (the NUL-bounded one, src/core and
// host_identity.cpp). Empty when the file has no such literal.
std::string BuildVersion(const std::vector<unsigned char>& bytes) {
  static constexpr unsigned char kPrefix[] = {0, 'N', 'E', 'V', 'R', '-', 'B', 'U', 'I', 'L', 'D', ' '};
  const auto found = std::search(bytes.begin(), bytes.end(), std::begin(kPrefix), std::end(kPrefix));
  if (found == bytes.end()) return {};
  std::string version;
  for (auto at = found + static_cast<std::ptrdiff_t>(sizeof(kPrefix));
       at != bytes.end() && *at != 0 && *at != ' ' && version.size() < 64; ++at) {
    version.push_back(static_cast<char>(*at));
  }
  return version;
}

FileFacts FactsOf(HANDLE file, bool want_version) {
  FileFacts facts;
  std::vector<unsigned char> bytes;
  if (!ReadWholeFile(file, &bytes)) return facts;
  facts.sha256 = Sha256Hex(bytes);
  if (want_version) facts.version = BuildVersion(bytes);
  return facts;
}

// The host's own file, opened by name and read with the share mode the verifier uses. Empty facts when it cannot be.
FileFacts FactsOfPath(const wchar_t* path, bool want_version) {
  if (path == nullptr || path[0] == L'\0') return {};
  const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  FileFacts facts = FactsOf(file, want_version);
  CloseHandle(file);
  return facts;
}

// The host's sha256: the file the game loaded this host from. Its own path is the module's (GetModuleFileNameW).
std::string HostSha256(HMODULE host_module) {
  if (host_module == nullptr) return {};
  wchar_t path[MAX_PATH * 4] = {};
  if (GetModuleFileNameW(host_module, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0]))) == 0) return {};
  return FactsOfPath(path, false).sha256;
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
    case RuntimeLoadOutcome::kRuntimeRefused:
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
    case RuntimeLoadOutcome::kRuntimeRefused: return "runtime_refused";
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

void SetVerifyRuntimeForTest(bool enabled) noexcept { g_verify_for_test.store(enabled); }

void SetVerifyPolicyForTest(const nevr_plugin_signature::Policy* policy) noexcept { g_policy_for_test.store(policy); }

void SetDevBuildForTest(int mode) noexcept { g_dev_build_for_test.store(mode); }

void SetBeforeLoadHookForTest(void (*hook)(const wchar_t* path)) noexcept { g_before_load_for_test.store(hook); }

void SetRuntimeDirForTest(const wchar_t* directory) noexcept { g_runtime_dir_for_test.store(directory); }
#endif

VerifyDecision DecideVerify(RuntimeFile file, bool verify_enabled, bool accepted, bool dev_build) noexcept {
  VerifyDecision decision;
  switch (file) {
    case RuntimeFile::kAbsent:
      return decision;  // nothing was checked: the load fails by itself, as runtime_absent
    case RuntimeFile::kUnreadable:
      decision.refused = true;
      decision.verify = "refused";
      return decision;  // refused in every build: a dev build allows an unsigned file, not one it cannot read
    case RuntimeFile::kReadable:
      break;
  }
  if (!verify_enabled) {
    decision.may_load = true;
    decision.verify = "not_checked";
    return decision;
  }
  if (accepted) {
    decision.may_load = true;
    decision.verify = "accepted";
  } else if (dev_build) {
    decision.may_load = true;
    decision.verify = "unsigned_allowed";
    decision.reason_is_verdict = true;
  } else {
    decision.refused = true;
    decision.verify = "refused";
    decision.reason_is_verdict = true;
  }
  return decision;
}

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
    if (result.path_ok) record["root"] = RuntimeRootSourceName(result.root_source);
    // NEVR_ROOT was set and this build does not honour it: said in the record, never silently dropped.
    if (result.root_override_ignored) record["root_override"] = "ignored";
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
    if (result.verify != nullptr) record["verify"] = result.verify;
    if (!result.verify_reason.empty()) record["verify_reason"] = result.verify_reason;
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

void EmitStopDiagnostic(BootstrapDiagnostics& diagnostics, const RuntimeLoadResult& result,
                        const char* notice) noexcept {
  try {
#if defined(NEVR_HOST_TESTING)
    if (g_fail_emit_for_test.load()) throw std::runtime_error("record builder failed (test seam)");
#endif
    nlohmann::json record;
    record["event"] = "game_stop";
    record["reason"] = result.OutcomeName();
    record["exit_code"] = kRuntimeStopExitCode;
    record["host_identity"] = IdentityLiteral();
    if (notice != nullptr) record["notice"] = notice;
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
        .Str("notice", notice != nullptr ? notice : "")
        .UInt("exit_code", kRuntimeStopExitCode);
    const std::size_t length = line.Finish();
    if (length != 0) diagnostics.AppendLine(buffer, length);
  }
}

namespace {

// The `versions` record (docs/adr/0012-static-install-root.md, "Names this record fixes"): the host's version and sha256
// and nevr-runtime.dll's, written once per process right after the verdict on the runtime and before it is loaded, so
// a player's report names exactly what ran. Never throws; a record that cannot be built is a marker line.
void EmitVersionsRecord(BootstrapDiagnostics& diagnostics, const std::string& host_sha256, const FileFacts& runtime,
                        const RuntimeLoadResult& result) noexcept {
  try {
    nlohmann::json record;
    record["event"] = "versions";
    record["host_version"] = nevr_host::Version();
    record["host_sha256"] = host_sha256.empty() ? kUnavailable : host_sha256;
    record["runtime_version"] = runtime.version.empty() ? kUnavailable : runtime.version;
    record["runtime_sha256"] = runtime.sha256.empty() ? kUnavailable : runtime.sha256;
    if (result.path_ok) record["runtime_path"] = ToUtf8(result.path);
    diagnostics.Emit(record);
  } catch (const std::exception& error) {
    char buffer[kMarkerBytes];
    JsonLine line(buffer, sizeof(buffer), "versions");
    line.Str("error", "record_build_failed").Str("what", error.what());
    const std::size_t length = line.Finish();
    if (length != 0) diagnostics.AppendLine(buffer, length);
  }
}

}  // namespace

namespace {

// A directory that is spelled from a drive or a share: X:\ or X:/ or \\server. A relative NEVR_ROOT would resolve against
// whatever the working directory is, and the runtime must not be found that way.
bool IsAbsoluteDirectory(const wchar_t* path) noexcept {
  if (path == nullptr) return false;
  const bool drive = ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' &&
                     (path[2] == L'\\' || path[2] == L'/');
  const bool share = path[0] == L'\\' && path[1] == L'\\';
  return drive || share;
}

std::size_t Length(const wchar_t* text) noexcept { return text == nullptr ? 0 : wcslen(text); }

// directory + one backslash (none added when it already ends in a separator) + child, into out. Appends to the
// `used` characters already in out. False, with out emptied, when it does not fit with its NUL.
bool Append(wchar_t* out, std::size_t out_size, std::size_t* used, const wchar_t* piece) noexcept {
  const std::size_t length = Length(piece);
  if (*used + length + 1 > out_size) {
    out[0] = L'\0';
    return false;
  }
  std::memcpy(out + *used, piece, length * sizeof(wchar_t));
  *used += length;
  out[*used] = L'\0';
  return true;
}

bool AppendChild(wchar_t* out, std::size_t out_size, std::size_t* used, const wchar_t* child) noexcept {
  if (*used != 0 && out[*used - 1] != L'\\' && out[*used - 1] != L'/') {
    if (!Append(out, out_size, used, L"\\")) return false;
  }
  return Append(out, out_size, used, child);
}

// An environment value into a fixed buffer; empty when it is unset or does not fit.
void ReadEnvironment(const wchar_t* name, wchar_t* out, std::size_t out_size) noexcept {
  out[0] = L'\0';
  const DWORD n = GetEnvironmentVariableW(name, out, static_cast<DWORD>(out_size));
  if (n == 0 || n >= out_size) out[0] = L'\0';
}

}  // namespace

const char* RuntimeRootSourceName(RuntimeRootSource source) noexcept {
  switch (source) {
    case RuntimeRootSource::kLocalAppData: return "local_appdata";
    case RuntimeRootSource::kOverride: return "nevr_root_override";
    case RuntimeRootSource::kUnresolved: break;
  }
  return "unresolved";
}

RuntimePathChoice ResolveRuntimePathFrom(const wchar_t* local_app_data, const wchar_t* nevr_root_override,
                                         bool dev_build, wchar_t* out, std::size_t out_size) noexcept {
  RuntimePathChoice choice;
  if (out == nullptr || out_size == 0) return choice;
  out[0] = L'\0';
  std::size_t used = 0;
  const bool override_set = nevr_root_override != nullptr && nevr_root_override[0] != L'\0';
  const bool honoured = override_set && dev_build && IsAbsoluteDirectory(nevr_root_override);
  choice.override_ignored = override_set && !honoured;
  if (honoured) {
    if (!Append(out, out_size, &used, nevr_root_override)) return choice;
    choice.source = RuntimeRootSource::kOverride;
  } else {
    if (local_app_data == nullptr || local_app_data[0] == L'\0') return choice;
    if (!Append(out, out_size, &used, local_app_data)) return choice;
    if (!AppendChild(out, out_size, &used, nevr_root::Names<wchar_t>::kRoot)) return choice;
    choice.source = RuntimeRootSource::kLocalAppData;
  }
  if (!AppendChild(out, out_size, &used, kRuntimeFileName)) {
    choice.source = RuntimeRootSource::kUnresolved;
  }
  return choice;
}

RuntimePathChoice ResolveRuntimePath(wchar_t* out, std::size_t out_size) noexcept {
  wchar_t local_app_data[MAX_PATH] = {};
  wchar_t nevr_root_override[MAX_PATH] = {};
  ReadEnvironment(L"LOCALAPPDATA", local_app_data, MAX_PATH);
  ReadEnvironment(L"NEVR_ROOT", nevr_root_override, MAX_PATH);
  const wchar_t* override_root = nevr_root_override;
  bool dev_build = DevBuild();
#if defined(NEVR_HOST_TESTING)
  // The seam's directory is an absolute root in a dev build's terms, through the same resolver (one straight path, no
  // branch of its own).
  if (const wchar_t* directory = g_runtime_dir_for_test.load()) {
    override_root = directory;
    dev_build = true;
    local_app_data[0] = L'\0';
  }
#endif
  return ResolveRuntimePathFrom(local_app_data, override_root, dev_build, out, out_size);
}

void RuntimeLoader::Prepare() {
  choice_ = ResolveRuntimePath(path_, MAX_PATH);
  path_ok_ = choice_.source != RuntimeRootSource::kUnresolved;
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
  result_.root_source = choice_.source;
  result_.root_override_ignored = choice_.override_ignored;
  const bool loader_lock_held = LoaderLockHeldByThisThread();

  HMODULE module = nullptr;
  DWORD load_error = 0;
  bool has_entry = false;
  const NevrRuntimeApi* api = nullptr;
  nevr_runtime_abi::Status abi_status = nevr_runtime_abi::Status::kNullDescriptor;
  std::int32_t init_code = 0;
  bool init_threw = false;

  // The verdict on the file (ADR 0012, "The host verifies nevr-runtime.dll before it loads it"). The file is held open
  // with FILE_SHARE_READ from the check through the load, so nothing can write it in between; the verifier reads it
  // from offset 0 through this handle, and the load maps the same bytes.
  const std::string host_sha256 = HostSha256(host.host_module);
  VerifyDecision decision;
  std::string verify_reason;
  std::string verdict_text;
  nevr_plugin_signature::Reason verify_reason_code = nevr_plugin_signature::Reason::Accepted;
  FileFacts runtime_facts;
  HANDLE file = INVALID_HANDLE_VALUE;

  if (path_ok_) {
    // Distinguish "runtime absent" (the file itself is missing) from a load
    // failure with the file present (e.g. a missing dependency). LoadLibraryExW
    // returns ERROR_MOD_NOT_FOUND for both, so the file must be probed first.
    const DWORD attributes = GetFileAttributesW(path_);
    if (attributes == INVALID_FILE_ATTRIBUTES &&
        (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)) {
      load_error = ERROR_FILE_NOT_FOUND;
      decision = DecideVerify(RuntimeFile::kAbsent, VerifyEnabled(), false, DevBuild());
    } else {
      file = CreateFileW(path_, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) {
        decision = DecideVerify(RuntimeFile::kUnreadable, VerifyEnabled(), false, DevBuild());
        verify_reason = "unreadable";
        verify_reason_code = nevr_plugin_signature::Reason::Unreadable;
      } else {
        bool accepted = false;
        if (VerifyEnabled()) {
          const nevr_plugin_signature::Verdict verdict = nevr_plugin_signature::VerifyPluginFile(
              file, VerifyPolicy(), static_cast<std::int64_t>(std::time(nullptr)));
          accepted = verdict.accepted();
          verify_reason_code = verdict.reason;
          verdict_text = verdict.Describe();
        }
        decision = DecideVerify(RuntimeFile::kReadable, VerifyEnabled(), accepted, DevBuild());
        if (decision.reason_is_verdict) verify_reason = verdict_text;
        if (!decision.refused) runtime_facts = FactsOf(file, true);
      }
    }
  }
  const bool refused = decision.refused;
  const bool may_load = decision.may_load;
  const char* verify = decision.verify;
  // The versions record: after the verdict, before the runtime can run a line. The runtime's facts are "unavailable"
  // when it is missing or refused.
  EmitVersionsRecord(diagnostics, host_sha256, runtime_facts, result_);
  if (may_load) {
#if defined(NEVR_HOST_TESTING)
    if (const auto hook = g_before_load_for_test.load()) hook(path_);
#endif
    module = LoadLibraryExW(path_, nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) load_error = GetLastError();
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

  // Released after the load and the runtime's initialize: the file is held for as long as the verdict must hold.
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);

  result_ = ClassifyLoad(path_ok_, path_, load_error, module, has_entry, api, abi_status,
                         init_code);
  if (refused) result_.outcome = RuntimeLoadOutcome::kRuntimeRefused;
  result_.root_source = choice_.source;
  result_.root_override_ignored = choice_.override_ignored;
  result_.verify = verify;
  result_.verify_reason = verify_reason;
  result_.verify_reason_code = verify_reason_code;
  result_.loader_lock_held = loader_lock_held;
  result_.init_threw = init_threw;
  EmitLoadDiagnostic(diagnostics, result_);
  return result_;
}

}  // namespace nevr_host
