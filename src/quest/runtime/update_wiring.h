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
// The runtime's caller passes the Rust core (core/update/core_ffi.h, the quest-runtime profile below) and VerifyStagedQuestFile. A
// caller that passes no core gets StartOutcome::kCoreNotLinked and no worker. `-no-update` has no source on Quest (the process has no
// argument list); `flag_no_update` is for a caller that has one.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/update/engine_check.h"
#include "core/update/fs.h"
#include "core/update/worker.h"

namespace nevr_quest::update_wiring {

/// The core's platform word for Quest (the PC's is "pc"): ADR 0024, section 3.
inline constexpr char kPlatformWord[] = "quest";

/// The stage profile of the Quest runtime: the artifact kind and the one member, the same text as installer/src/stage.rs QUEST_PROFILE.
/// A release lists the runtime as `quest-runtime`, one file with no zip (installer/include/nevr_install.h, NevrStage).
inline constexpr char kQuestStageProfile[] = R"({"kind":"quest-runtime","members":["libnevr-runtime.so"]})";

/// What the runtime knows when it starts the worker.
struct Inputs {
  bool dev_build = false;                    ///< nevr_build::kIsDev: a developer's runtime is never replaced, unless it has an `update.base`
  bool flag_no_update = false;               ///< the `-no-update` token, from a caller that has an argument list
  std::optional<bool> config_enabled;        ///< `update.enabled` (config.yaml); unset means on
  std::optional<std::string> config_channel; ///< `update.channel`; unset means the core's default
  /// `update.base` and `update.pins` as the file gave them (unchecked). Only a dev build uses them (nevr_update::ResolveSource): a release
  /// build ignores both and Start logs that it did; they never reach the core's open-config JSON there.
  std::optional<std::string> config_base;
  std::optional<std::string> config_pins;
  bool host_can_swap = false;                ///< the host's capability bit 0 (nevr_runtime_abi::kHostCapabilitySwap)
  std::string root;                          ///< the nevr root, UTF-8 (the folder that holds update/); empty when there is none
  const nevr_update::UpdateFs* fs = nullptr; ///< every file operation on update/ goes through it; required
  /// The version and the sha256 of the file this process mapped; see ReadRunningRuntime.
  std::function<bool(std::string* version, std::string* sha256, std::string* error)> read_running;
  nevr_update::Core core;                    ///< the Rust core's job functions; empty when the core is not linked
  std::function<nevr_update::VerifyOutcome(const std::string& path, const nevr_update::ExpectedFile& expected)> verify;
  nevr_update::LogFn log;                    ///< called from the worker's threads: it must be thread-safe
  std::function<std::uint64_t()> now_ms;     ///< a monotonic clock in milliseconds
};

/// The worker's inputs for `inputs`: the switches, the channel, the platform word "quest", the root, and the functions as given.
/// `no_root` is set when the root is empty. The update source (`update.base`, `update.pins`) is `Deps::source` and the dev switch
/// `dev_source_override` only in a dev build whose base and pins are valid (nevr_update::ResolveSource); otherwise neither is set.
nevr_update::Deps BuildDeps(Inputs inputs);

/// The records of the update source decision (`update source`): none for a build with neither key. A release build with a key writes one
/// Warning (outcome=ignored); a dev build writes one Warning that the source is overridden (outcome=used), or one Warning for a value
/// it refused (outcome=rejected, the update stays disabled). The base is named by its host and port only; a base cannot carry
/// credentials, a query or a fragment (ResolveSource refuses them), so no secret can reach the line.
std::vector<std::string> SourceLines(const nevr_update::SourceResolution& resolution);

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

/// The Quest verifier of one staged file (Inputs::verify). The core checked the file against the signed manifest when it staged it
/// (ADR 0024, section 4.1). Here the bytes the engine staged must still be those: the file is opened without following a link, must be
/// a regular file, and its size and sha256 must equal `expected` (the stage result's entry). A link or a size or hash mismatch is a
/// verdict (`refused`); a file that cannot be opened or read is not (a scanner's lock, a missing file).
/// KNOWN-WRONG #TBD: the stored proof pair (NevrProofVerify, DESIGN section 2) is not checked here; that is the piece after this one
/// (6.19), so a release the signer later revokes still verifies against its own bytes until then.
nevr_update::VerifyOutcome VerifyStagedQuestFile(const std::string& path, const nevr_update::ExpectedFile& expected);

/// For Inputs::read_running: the file the code at `in_library` was loaded from (dladdr), its sha256 and `version`. False with `error`
/// set when the address is in no loaded object, the file cannot be read, or the version is empty. Reads the whole file: call it from
/// the worker's thread (Deps::read_running is).
bool ReadRunningRuntime(const void* in_library, const char* version, std::string* version_out, std::string* sha256_out,
                        std::string* error);

}  // namespace nevr_quest::update_wiring
