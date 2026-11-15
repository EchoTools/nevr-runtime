#pragma once

// The Quest's update start, run by the shim at its constructor (docs/adr/0024-pc-self-update.md, sections 4 and 6; the
// Quest design, piece 15). The shared start decision (core/update/engine_start.h) runs with the Quest's edges
// (update_start.cpp): it recovers an interrupted swap, swaps a staged runtime into the update root, and judges a
// pending one. This unit then loads what the decision chose (runtime_load.h) and does what a failed load needs: a
// release that never ran is put back in the same start, and a release whose initialize failed ends the process with
// kUpdateFailedExitCode, so the next start puts the previous one back.
//
// Without an update root, and when the update folder does not exist, the start is the baseline exactly as before this
// unit: the APK's libnevr-runtime.so, loaded beside the shim.
//
// The fence: this unit runs the engine; it does not verify a proof pair, check ELF structure, or confirm a release
// (6.19 and the runtime's worker). The runtime is the update root's libnevr-runtime.so (StartContext::runtime_file).

#include <cstdint>
#include <string>

#include "quest/shim/runtime_load.h"

namespace nevr_quest::update_start {

/// The exit code of a release whose initialize failed: the host's stop code (docs/standards/logging.md, the NEVR.STOP
/// table).
inline constexpr int kUpdateFailedExitCode = 78;

/// What the start is given. `host` is the host inputs of the load; the start sets its update root and its
/// before-initialize hook.
struct Inputs {
  /// The folder that holds update\ (update_root::Resolve()); empty when the process has none, which means no update.
  std::string root;
  /// An address inside the shim: the baseline is read from the folder that holds the shim (runtime_load.h).
  const void* in_shim = nullptr;
  /// The folder the baseline is read from, when it is not the folder that holds the shim (the host tests have no shim
  /// folder to give). Empty in the shim itself. The caller then sets host.shim_handle, which the production start takes
  /// from in_shim.
  std::string baseline_dir;
  runtime_load::HostInputs host;
};

/// Where the loaded runtime came from.
enum class Origin : std::uint8_t {
  kBaseline,  ///< the APK's libnevr-runtime.so
  kUpdate,    ///< the update root's libnevr-runtime.so, put there by the swap
};

struct Result {
  /// The outcome of the load that ended the start (kLoaded when a runtime runs).
  runtime_load::Outcome outcome = runtime_load::Outcome::kAbsent;
  Origin origin = Origin::kBaseline;
  /// The file that loaded when origin is kUpdate; empty otherwise.
  std::string path;
  /// True when the process must end with kUpdateFailedExitCode: a release's initialize failed, and game_stop was
  /// written.
  bool stop = false;
};

/// Runs the start and loads the runtime it chooses. Never throws; a failure that is not the release's is the
/// baseline's.
Result Start(const Inputs& inputs) noexcept;

}  // namespace nevr_quest::update_start
