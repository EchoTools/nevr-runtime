#include "quest/runtime/update_wiring.h"

#include <dlfcn.h>

#include <cstdio>
#include <exception>
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

nevr_update::Deps BuildDeps(Inputs inputs) {
  nevr_update::Deps deps;
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
    std::function<std::uint64_t()> now_ms = inputs.now_ms;
    nevr_update::Worker* worker = new nevr_update::Worker(BuildDeps(std::move(inputs)));  // NOLINT: never destroyed, see above
    result.worker = worker;
    result.decision = worker->Start();
    result.outcome = StartOutcome::kStarted;
    FrameContext* frame = new FrameContext{worker, std::move(now_ms)};  // NOLINT: held by the frame call for the process
    if (!register_frame_work || !register_frame_work(&OnFrame, frame)) {
      result.outcome = StartOutcome::kFrameWorkRefused;
      if (log) log(nevr_update::kWarning, kFrameWorkRefusedLine);
    }
  } catch (const std::exception&) {  // out of memory: no worker, the game runs without an update
    result.outcome = StartOutcome::kCoreNotLinked;
    result.worker = nullptr;
  }
  return result;
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
  std::FILE* file = std::fopen(info.dli_fname, "rb");
  if (file == nullptr) {
    *error = "open_failed";
    return false;
  }
  nevr_quest::data::Sha256 hash;
  std::unique_ptr<unsigned char[]> chunk(new unsigned char[kReadChunkBytes]);
  std::size_t total = 0;
  bool ok = true;
  for (;;) {
    const std::size_t got = std::fread(chunk.get(), 1, kReadChunkBytes, file);
    if (got > 0) {
      total += got;
      if (total > kMaxRuntimeBytes) {
        ok = false;
        *error = "too_large";
        break;
      }
      hash.Update(chunk.get(), got);
    }
    if (got < kReadChunkBytes) {
      if (std::ferror(file) != 0) {
        ok = false;
        *error = "read_failed";
      }
      break;
    }
  }
  std::fclose(file);
  if (!ok) return false;
  *version_out = version;
  *sha256_out = hash.Hex();
  return true;
}

}  // namespace nevr_quest::update_wiring
