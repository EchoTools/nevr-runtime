// The baseline start of the Quest runtime (runtime_load.h): map the APK's libnevr-runtime.so by descriptor, validate what
// NEVR_GetRuntimeApi returns, call initialize once, and write one runtime_load record for the outcome.

#include "quest/shim/runtime_load.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(__ANDROID__)
#include <android/dlext.h>
#endif

namespace nevr_quest::runtime_load {
namespace {

inline constexpr char kRecordTag[] = "NEVR.HOST";
inline constexpr char kRecordEvent[] = "runtime_load";
inline constexpr char kSourceBaseline[] = "baseline";
inline constexpr char kVerifyNone[] = "none";
// The baseline is not checked by this shim: the APK signature covers its bytes at install. Piece 12 replaces this word.
inline constexpr char kVerifyReasonBaseline[] = "apk_signature_covers_baseline";
inline constexpr char kPathFormat[] = "%s/%s";
// PATH_MAX on Linux and on Bionic.
inline constexpr std::size_t kPathCapacity = 4096;
#if !defined(__ANDROID__)
// The host tests have no android_dlopen_ext: they map the descriptor's inode through its /proc name.
inline constexpr char kProcFdFormat[] = "/proc/self/fd/%d";
// "/proc/self/fd/" plus the digits of an int and the terminator.
inline constexpr std::size_t kProcFdPathCapacity = 32;
#endif
// The dynamic linker's message, kept for the record. Longer text is cut.
inline constexpr std::size_t kDetailCapacity = 256;
// Digits of an int, a sign and the terminator.
inline constexpr std::size_t kNumberCapacity = 24;
// The most fields one record carries: outcome, source, file, verify, verify_reason, reason, detail, initialize_status, errno.
inline constexpr std::uint32_t kMaxRecordFields = 12;
// The capability word the context carries. Bit 0 (the staged swap) stays clear until the start decision is wired (piece 15), so the
// runtime's worker never stages a release under this shim.
inline constexpr std::uint64_t kCapabilitiesNone = 0;

// The fields of one record, in the order given; the sink copies every string before it returns.
struct FieldList {
  NevrQuestLogField items[kMaxRecordFields] = {};
  std::uint32_t count = 0;

  void Add(const char* key, const char* value) noexcept {
    if (count < kMaxRecordFields) items[count++] = NevrQuestLogField{key, value};
  }
};

// The descriptor the start owns: closed when the start ends, after initialize has returned, never before.
struct FdGuard {
  int fd;
  ~FdGuard() {
    if (fd >= 0) ::close(fd);
  }
};

// What one start did, filled in by Run and reported by Report.
struct Attempt {
  Outcome outcome = Outcome::kLoaded;
  const char* reason = "";
  char detail[kDetailCapacity] = {};
  std::int32_t status = 0;
};

void CopyText(char* out, std::size_t capacity, const char* text) noexcept {
  if (text == nullptr) {
    out[0] = '\0';
    return;
  }
  std::snprintf(out, capacity, "%s", text);
}

// The mapping: by descriptor, so the file the linker maps is the one the caller holds (Android: USE_LIBRARY_FD; the host
// tests have no android_dlopen_ext and map the same inode through /proc/self/fd).
void* MapFd(int fd) noexcept {
#if defined(__ANDROID__)
  android_dlextinfo info = {};
  info.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
  info.library_fd = fd;
  return android_dlopen_ext(kBaselineFileName, RTLD_NOW, &info);
#else
  char path[kProcFdPathCapacity];
  std::snprintf(path, sizeof path, kProcFdFormat, fd);
  return dlopen(path, RTLD_NOW);
#endif
}

void Refuse(Attempt* out, const char* reason) noexcept {
  out->outcome = Outcome::kHostContextRefused;
  out->reason = reason;
}

// Maps, resolves, validates and initializes. Returns at the first step that does not hold, with its outcome and reason.
void Run(int fd, const HostInputs& host, Attempt* out) noexcept {
  if (host.shim_handle == nullptr) return Refuse(out, "no_shim_handle");
  if (host.libr15_base == nullptr) return Refuse(out, "no_libr15_base");
  if (host.libr15_build_id == nullptr) return Refuse(out, "no_build_id");

  void* handle = MapFd(fd);
  if (handle == nullptr) {
    out->outcome = Outcome::kLoadFailed;
    out->reason = "dlopen";
    CopyText(out->detail, sizeof out->detail, dlerror());
    return;
  }
  void* symbol = dlsym(handle, nevr_runtime_abi::kGetRuntimeApiExport);
  if (symbol == nullptr) {
    out->outcome = Outcome::kMissingEntry;
    out->reason = "no_export";
    return;
  }
  const auto get_api = reinterpret_cast<NevrQuestGetRuntimeApiFn>(symbol);
  const NevrQuestRuntimeApi* api = get_api();
  if (api == nullptr) {
    out->outcome = Outcome::kNullDescriptor;
    out->reason = "null";
    return;
  }
  // ValidateRuntimeApi reads the header first and the initialize field only after the header passes.
  const nevr_runtime_abi::Status status = nevr_quest_runtime_abi::ValidateRuntimeApi(api);
  if (status != nevr_runtime_abi::Status::kOk) {
    out->outcome = Outcome::kAbiRejected;
    out->reason = nevr_runtime_abi::StatusName(status);
    return;
  }

  NevrQuestHostContext context = {};
  context.header.struct_size = nevr_quest_runtime_abi::kHostContextV1Size;
  context.header.abi_major = nevr_quest_runtime_abi::kMajor;
  context.header.abi_minor = nevr_quest_runtime_abi::kMinor;
  context.shim_handle = host.shim_handle;
  context.libr15_base = host.libr15_base;
  context.libr15_build_id = host.libr15_build_id;
  context.update_root = host.update_root;
  context.shim_version = host.shim_version;
  context.log = host.log;
  context.variant = host.variant;
  context.capabilities = kCapabilitiesNone;

  const std::int32_t result = api->initialize(&context);
  if (result != 0) {
    out->outcome = Outcome::kInitFailed;
    out->reason = "nonzero";
    out->status = result;
    return;
  }
  out->outcome = Outcome::kLoaded;
}

// One record of the start, through the shim's sink. Nothing is written when there is no sink.
void Report(const char* file, const HostInputs& host, const Attempt& attempt, int err) noexcept {
  if (host.log == nullptr) return;
  const bool loaded = attempt.outcome == Outcome::kLoaded;
  char status_text[kNumberCapacity] = {};
  char errno_text[kNumberCapacity] = {};
  std::snprintf(status_text, sizeof status_text, "%d", static_cast<int>(attempt.status));
  std::snprintf(errno_text, sizeof errno_text, "%d", err);

  FieldList fields;
  fields.Add("outcome", OutcomeName(attempt.outcome));
  fields.Add("source", kSourceBaseline);
  fields.Add("file", file);
  fields.Add("verify", kVerifyNone);
  fields.Add("verify_reason", kVerifyReasonBaseline);
  if (!loaded) {
    fields.Add("reason", attempt.reason);
    if (attempt.detail[0] != '\0') fields.Add("detail", attempt.detail);
  }
  if (attempt.outcome == Outcome::kInitFailed) fields.Add("initialize_status", status_text);
  if (err != 0) fields.Add("errno", errno_text);
  host.log(loaded ? kLogLevelInfo : kLogLevelError, kRecordTag, kRecordEvent, nullptr, fields.items, fields.count);
}

}  // namespace

const char* OutcomeName(Outcome outcome) noexcept {
  switch (outcome) {
    case Outcome::kLoaded: return "loaded";
    case Outcome::kAbsent: return "absent";
    case Outcome::kLoadFailed: return "load_failed";
    case Outcome::kMissingEntry: return "missing_entry";
    case Outcome::kNullDescriptor: return "null_descriptor";
    case Outcome::kAbiRejected: return "abi_rejected";
    case Outcome::kInitFailed: return "init_failed";
    case Outcome::kHostContextRefused: return "host_context_refused";
  }
  return "unknown";
}

int OpenBaselineIn(const char* dir) noexcept {
  char path[kPathCapacity];
  const int written = std::snprintf(path, sizeof path, kPathFormat, dir, kBaselineFileName);
  if (written < 0 || static_cast<std::size_t>(written) >= sizeof path) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return ::open(path, O_RDONLY | O_CLOEXEC);
}

Outcome LoadRuntimeFromFd(int fd, const char* file, const HostInputs& host) noexcept {
  const FdGuard guard = {fd};
  Attempt attempt;
  Run(fd, host, &attempt);
  Report(file, host, attempt, 0);
  return attempt.outcome;
}

Outcome LoadBaseline(const char* dir, const HostInputs& host) noexcept {
  const int fd = OpenBaselineIn(dir);
  if (fd < 0) {
    const int err = errno;
    Attempt attempt;
    attempt.outcome = Outcome::kAbsent;
    attempt.reason = "open_failed";
    Report(kBaselineFileName, host, attempt, err);
    return Outcome::kAbsent;
  }
  return LoadRuntimeFromFd(fd, kBaselineFileName, host);
}

Outcome StartBaselineBesideShim(const void* in_shim, HostInputs host) noexcept {
  host.shim_handle = ShimHandleOf(in_shim);
  char dir[kPathCapacity];
  if (!NativeLibraryDirOf(in_shim, dir, sizeof dir)) {
    Attempt attempt;
    attempt.outcome = Outcome::kAbsent;
    attempt.reason = "no_native_dir";
    Report(kBaselineFileName, host, attempt, 0);
    return Outcome::kAbsent;
  }
  return LoadBaseline(dir, host);
}

bool NativeLibraryDirOf(const void* in_shim, char* out, std::size_t capacity) noexcept {
  Dl_info info = {};
  if (dladdr(in_shim, &info) == 0 || info.dli_fname == nullptr) return false;
  const char* slash = std::strrchr(info.dli_fname, '/');
  if (slash == nullptr || slash == info.dli_fname) return false;
  const std::size_t length = static_cast<std::size_t>(slash - info.dli_fname);
  if (length + 1 > capacity) return false;
  std::memcpy(out, info.dli_fname, length);
  out[length] = '\0';
  return true;
}

void* ShimHandleOf(const void* in_shim) noexcept {
  Dl_info info = {};
  if (dladdr(in_shim, &info) == 0 || info.dli_fname == nullptr) return nullptr;
  return dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
}

}  // namespace nevr_quest::runtime_load
