#include "quest/runtime/update_wiring.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>

#include "quest/data/sha256.h"

namespace nevr_quest::update_wiring {
namespace {

// The words of the records this unit writes. The update's own records keep the runtime's tag (docs/standards/logging.md, NEVR.UPDATE).
constexpr char kNotStartedLine[] =
    "[NEVR.UPDATE] update wiring action=not_started reason=core_not_linked";
constexpr char kFrameWorkRefusedLine[] =
    "[NEVR.UPDATE] update wiring action=frame_work_refused reason=registry_full";

// The most a file read for its sha256 may hold: the runtime library is a few megabytes; a larger file is not this runtime.
constexpr std::size_t kMaxRuntimeBytes = 256u * 1024u * 1024u;
// The chunk the file is read in.
constexpr std::size_t kReadChunkBytes = 64u * 1024u;

// How a descriptor's bytes came out: read to the end and hashed, read until a read failed, or longer than kMaxRuntimeBytes.
enum class HashResult : std::uint8_t { kOk, kReadFailed, kTooLarge };

// The size and sha256 of the bytes `fd` holds from offset 0 to its end. Reads through the descriptor, so the caller's open decides what
// file it is (a link is refused by the open, not here).
HashResult HashDescriptor(int fd, std::uint64_t* size_out, std::string* sha256_out) {
  nevr_quest::data::Sha256 hash;
  std::unique_ptr<unsigned char[]> chunk(new unsigned char[kReadChunkBytes]);
  std::uint64_t total = 0;
  for (;;) {
    const ssize_t got = read(fd, chunk.get(), kReadChunkBytes);
    if (got < 0) {
      if (errno == EINTR) continue;
      return HashResult::kReadFailed;
    }
    if (got == 0) break;
    total += static_cast<std::uint64_t>(got);
    if (total > static_cast<std::uint64_t>(kMaxRuntimeBytes)) return HashResult::kTooLarge;
    hash.Update(chunk.get(), static_cast<std::size_t>(got));
  }
  *size_out = total;
  *sha256_out = hash.Hex();
  return HashResult::kOk;
}

// What the frame call needs: the worker and the clock. Leaked with the worker, because the frame call holds both for the life of the
// process.
struct FrameContext {
  nevr_update::Worker* worker = nullptr;
  std::function<std::uint64_t()> now_ms;
};

bool OnFrame(void* context) noexcept {
  const auto* frame = static_cast<const FrameContext*>(context);
  if (frame == nullptr || frame->worker == nullptr || !frame->now_ms) return false;
  frame->worker->OnFrame(frame->now_ms());
  return true;
}

}  // namespace

namespace {

// "host[:port]" of an http(s) base that ResolveSource accepted: the text after the scheme up to the first '/'.
std::string HostOf(const std::string& base) {
  constexpr char kSeparator[] = "://";
  const std::size_t start = base.find(kSeparator) + sizeof(kSeparator) - 1;  // ResolveSource guarantees the scheme
  const std::size_t end = base.find('/', start);
  return base.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

}  // namespace

std::vector<std::string> SourceLines(const nevr_update::SourceResolution& r) {
  using nevr_update::SourceVerdict;
  std::string head = std::string("[NEVR.UPDATE] update source outcome=") + nevr_update::SourceVerdictWord(r.verdict) + " source=config";
  switch (r.verdict) {
    case SourceVerdict::kNone: return {};
    case SourceVerdict::kUsed:
      return {head + " base_host=" + HostOf(r.use.base) + " pins=" + (r.pins_given ? std::to_string(r.pins_count) : "builtin") +
              " consequence=\"the update source is overridden in this development build; releases are read from that base\""};
    case SourceVerdict::kIgnoredRelease:
      return {head + " key=" + r.key + " reason=" + r.reason + " consequence=\"a release build always updates from its own host and pinned keys\""};
    case SourceVerdict::kRejected:
      return {head + " key=" + r.key + " reason=" + r.reason + " consequence=\"no update source override; this development build does not update\""};
  }
  return {};
}

nevr_update::Deps BuildDeps(Inputs inputs) {
  nevr_update::Deps deps;
  const nevr_update::SourceResolution source = nevr_update::ResolveSource(inputs.dev_build, inputs.config_base, inputs.config_pins);
  if (source.verdict == nevr_update::SourceVerdict::kUsed) deps.source = source.use;
  deps.switches.dev_source_override = source.verdict == nevr_update::SourceVerdict::kUsed;
  deps.switches.dev_build = inputs.dev_build;
  deps.switches.flag_no_update = inputs.flag_no_update;
  deps.switches.config_enabled = inputs.config_enabled;
  deps.switches.host_can_swap = inputs.host_can_swap;
  deps.switches.no_root = inputs.root.empty();
  deps.config_channel = std::move(inputs.config_channel);
  deps.server = false;  // the Quest runtime is a client
  deps.platform = kPlatformWord;
  deps.root = std::move(inputs.root);
  deps.fs = inputs.fs;
  deps.read_running = std::move(inputs.read_running);
  deps.core = std::move(inputs.core);
  deps.verify = std::move(inputs.verify);
  deps.log = std::move(inputs.log);
  deps.now_ms = std::move(inputs.now_ms);
  return deps;
}

StartResult Start(Inputs inputs, const RegisterFrameWorkFn& register_frame_work) noexcept {
  StartResult result;
  try {
    if (!inputs.core.open) {
      if (inputs.log) inputs.log(nevr_update::kWarning, kNotStartedLine);
      return result;  // kCoreNotLinked, no worker
    }
    const nevr_update::LogFn log = inputs.log;
    if (log) {
      const nevr_update::SourceResolution source = nevr_update::ResolveSource(inputs.dev_build, inputs.config_base, inputs.config_pins);
      for (const std::string& line : SourceLines(source)) log(nevr_update::kWarning, line);
    }
    std::function<std::uint64_t()> now_ms = inputs.now_ms;
    nevr_update::Worker* worker = new nevr_update::Worker(BuildDeps(std::move(inputs)));  // NOLINT: never destroyed, see above
    result.worker = worker;
    result.decision = worker->Start();
    result.outcome = StartOutcome::kStarted;
    FrameContext* frame = new (std::nothrow) FrameContext{worker, std::move(now_ms)};  // NOLINT: held by the frame call for the process
    if (frame == nullptr) {  // the worker runs; only its frame call is missing
      result.outcome = StartOutcome::kFrameWorkRefused;
      if (log) log(nevr_update::kWarning, kFrameWorkRefusedLine);
      return result;
    }
    if (!register_frame_work || !register_frame_work(&OnFrame, frame)) {
      result.outcome = StartOutcome::kFrameWorkRefused;
      if (log) log(nevr_update::kWarning, kFrameWorkRefusedLine);
    }
  } catch (...) {  // out of memory: no worker, the game runs without an update
    result.outcome = StartOutcome::kCoreNotLinked;
    result.worker = nullptr;
  }
  return result;
}

nevr_update::VerifyOutcome VerifyStagedQuestFile(const std::string& path, const nevr_update::ExpectedFile& expected) {
  nevr_update::VerifyOutcome out;
  // O_NOFOLLOW: a link where the staged file must be is a verdict (ELOOP). Any other open failure is not: the file may be locked or gone.
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    out.refused = errno == ELOOP;
    out.reason = out.refused ? "link" : "open_failed";
    return out;
  }
  struct stat st {};
  if (fstat(fd, &st) != 0) {
    close(fd);
    out.reason = "stat_failed";  // not a verdict on the bytes
    return out;
  }
  if (!S_ISREG(st.st_mode)) {
    close(fd);
    out.refused = true;
    out.reason = "not_a_regular_file";
    return out;
  }
  std::uint64_t size = 0;
  std::string sha256;
  const HashResult result = HashDescriptor(fd, &size, &sha256);
  close(fd);
  if (result == HashResult::kReadFailed) {
    out.reason = "read_failed";  // not a verdict on the bytes
    return out;
  }
  if (result == HashResult::kTooLarge) {
    out.refused = true;
    out.reason = "too_large";
    return out;
  }
  if (size != expected.size) {
    out.refused = true;
    out.reason = "size_mismatch";
    return out;
  }
  if (sha256 != expected.sha256) {
    out.refused = true;
    out.reason = "sha256_mismatch";
    return out;
  }
  out.ok = true;
  out.reason = "accepted";
  return out;
}

bool ReadRunningRuntime(const void* in_library, const char* version, std::string* version_out, std::string* sha256_out,
                        std::string* error) {
  if (version == nullptr || version[0] == '\0') {
    *error = "no_version";
    return false;
  }
  Dl_info info{};
  if (in_library == nullptr || dladdr(in_library, &info) == 0 || info.dli_fname == nullptr || info.dli_fname[0] == '\0') {
    *error = "no_object";
    return false;
  }
  const int fd = open(info.dli_fname, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    *error = "open_failed";
    return false;
  }
  std::uint64_t size = 0;
  std::string sha256;
  const HashResult result = HashDescriptor(fd, &size, &sha256);
  close(fd);
  if (result == HashResult::kTooLarge) {
    *error = "too_large";
    return false;
  }
  if (result == HashResult::kReadFailed) {
    *error = "read_failed";
    return false;
  }
  *version_out = version;
  *sha256_out = sha256;
  return true;
}

}  // namespace nevr_quest::update_wiring
