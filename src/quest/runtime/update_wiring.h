#pragma once

// The Quest runtime's start of the update worker (docs/adr/0024-pc-self-update.md, section 3; design piece quest-worker-and-confirm).
// The worker is the engine's (core/update/worker.h): a check thread, a confirmation thread and the per-frame call that fires the
// confirmation after kConfirmAfterMs of frames. PC starts it in src/runtime/lifecycle/update_process.cpp, StartProcessWorker. This
// unit is the Quest counterpart: it turns what the runtime knows (the build, the config's `update:` section, the host's
// capabilities, the update root) into the worker's inputs, starts it, and registers the frame call.
//
// No sentinel, config or frame-dispatch header is included: the caller (runtime_entry.cpp) supplies the values and the registration
// function, so this unit builds and is tested on the host with a fake core.
//
// Held, and stated here so it is not read as done: the check and the stage need the Rust core (Core) and the verifier (verify) on
// Quest. Neither is linked in this build, so the runtime's caller passes neither, and Start then does not start the worker
// (StartOutcome::kCoreNotLinked). `-no-update` has no source on Quest (the process has no argument list); `flag_no_update` is for a
// caller that has one.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "core/update/engine_check.h"
#include "core/update/fs.h"
#include "core/update/worker.h"

namespace nevr_quest::update_wiring {

/// The core's platform word for Quest (the PC's is "pc"): ADR 0024, section 3.
inline constexpr char kPlatformWord[] = "quest";

/// What the runtime knows when it starts the worker.
struct Inputs {
  bool dev_build = false;                    ///< nevr_build::kIsDev: a developer's runtime is never replaced
  bool flag_no_update = false;               ///< the `-no-update` token, from a caller that has an argument list
  std::optional<bool> config_enabled;        ///< `update.enabled` (config.yaml); unset means on
  std::optional<std::string> config_channel; ///< `update.channel`; unset means the core's default
  bool host_can_swap = false;                ///< the host's capability bit 0 (nevr_runtime_abi::kHostCapabilitySwap)
  std::string root;                          ///< the nevr root, UTF-8 (the folder that holds update/); empty when there is none
  const nevr_update::UpdateFs* fs = nullptr; ///< every file operation on update/ goes through it; required
  /// The version and the sha256 of the file this process mapped; see ReadRunningRuntime.
  std::function<bool(std::string* version, std::string* sha256, std::string* error)> read_running;
  nevr_update::Core core;                    ///< the Rust core's job functions; empty when the core is not linked
  std::function<nevr_update::VerifyOutcome(const std::string& path)> verify;
  nevr_update::LogFn log;                    ///< called from the worker's threads: it must be thread-safe
  std::function<std::uint64_t()> now_ms;     ///< a monotonic clock in milliseconds
};

/// The worker's inputs for `inputs`: the switches, the channel, the platform word "quest", the root, and the functions as given.
/// `no_root` is set when the root is empty.
nevr_update::Deps BuildDeps(Inputs inputs);

/// The frame call (frame_dispatch.h WorkFn): runs on the game thread once per frame.
using FrameWorkFn = bool (*)(void* context) noexcept;
/// frame_dispatch.h RegisterFrameWork, or a test's stand-in: false when the registry is full.
using RegisterFrameWorkFn = std::function<bool(FrameWorkFn work, void* context)>;

enum class StartOutcome : std::uint8_t {
  kStarted,            ///< the worker was started (the decision it took is in StartResult::decision)
  kCoreNotLinked,      ///< no Core was given: the worker is not started, and one record says so
  kFrameWorkRefused,   ///< the worker was started but the frame call could not be registered: the confirmation cannot fire
};

struct StartResult {
  StartOutcome outcome = StartOutcome::kCoreNotLinked;
  nevr_update::Disabled decision = nevr_update::Disabled::kNone;
  nevr_update::Worker* worker = nullptr;  ///< null when not started; owned by the frame call for the life of the process
};

/// Builds the inputs, starts the worker once and registers its frame call. The worker is never destroyed: the frame call holds it for
/// the life of the process, as PC's does (update_process.cpp g_worker_for_frames). Never throws.
StartResult Start(Inputs inputs, const RegisterFrameWorkFn& register_frame_work) noexcept;

/// For Inputs::read_running: the file the code at `in_library` was loaded from (dladdr), its sha256 and `version`. False with `error`
/// set when the address is in no loaded object, the file cannot be read, or the version is empty. Reads the whole file: call it from
/// the worker's thread (Deps::read_running is).
bool ReadRunningRuntime(const void* in_library, const char* version, std::string* version_out, std::string* sha256_out,
                        std::string* error);

}  // namespace nevr_quest::update_wiring
