#pragma once

// The platform edges of the self-update start (docs/adr/0024-pc-self-update.md, sections 4 and 6).
// The engine (engine_start.h) decides what to do and asks the platform through these interfaces. This header declares
// interfaces only: the PC implementations live in runtime/host/update_swap.cpp and runtime/host/runtime_load.cpp (the
// Win32 calls, the Authenticode check and LoadLibraryExW, all unchanged in behaviour). Pure: no platform header.
//
// Not written here, and held for the owner's trust review (the Quest updater: docs/adr/0023-core-install-method.md, section 7): the Quest
// ArtifactVerifier (the proof pair), the Quest RuntimeLoaderEdge (what loads on Quest), and the Quest RollbackTarget
// (the APK baseline). Their interfaces are these.

#include "core/update/fs.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace nevr_update {

// ---- ArtifactVerifier: is this file one the release signer stands behind (ADR 0024, section 4.3)-------------------------

/// What the engine asks of one file. With check_digest the size and sha256 (from staged.json) must equal these before the
/// signature is asked; without it only the signature is asked (a runtime that is already in the root).
struct ArtifactExpect {
  bool check_digest = false;
  std::uint64_t size = 0;
  std::string sha256;
};

/// The verdict. `reason` is empty when `ok`; otherwise it is "unreadable", "size", "sha256", or the verifier's own words.
struct ArtifactCheck {
  bool ok = false;
  std::string reason;
};

/// What the engine records about one file: whether it is there, whether it could be read, its sha256 and its NEVR-BUILD
/// version. `unreadable` is never the same as absent (ADR 0024, section 4.9).
struct ArtifactFacts {
  bool present = false;
  bool unreadable = false;
  std::string sha256;
  std::string version;
};

class ArtifactVerifier {
 public:
  virtual ~ArtifactVerifier() = default;
  /// The file at `path` against `expect`, and the signature under the platform's policy.
  virtual ArtifactCheck Verify(const std::string& path, const ArtifactExpect& expect) const = 0;
  /// The facts the engine records about the file at `path` (the bytes it reads are the same bytes Verify checks).
  virtual ArtifactFacts Measure(const std::string& path) const = 0;
};

// ---- InUseCheck: does another process run the root's runtime (ADR 0024, section 4.4) --------------------------------

enum class InUse : std::uint8_t {
  kFree,   ///< nothing refuses a write to the runtime (or there is none)
  kInUse,  ///< another process maps it: the write is refused by a sharing violation
  kError,  ///< refused for another cause (access denied, ...): nothing moves until it is gone
};

struct InUseAnswer {
  InUse state = InUse::kFree;
  /// The OS error of the refusal; 0 when kFree.
  int os_error = 0;
};

// (Named InUseCheck: tools/verify_host_seams.py rejects a symbol of the production host units whose demangled name holds a word from
// its seam list, and this interface is compiled into them.)
class InUseCheck {
 public:
  virtual ~InUseCheck() = default;
  virtual InUseAnswer Query(const std::string& runtime_path) const = 0;
};

// ---- RollbackTarget: where a failed or unconfirmed release goes back to (ADR 0024, section 6) -------------------------

enum class RollbackState : std::uint8_t {
  kUsable,      ///< the previous release's runtime is there and verifies
  kAbsent,      ///< nothing to put back
  kUnverified,  ///< there, and it does not verify: `reason` says why
};

struct RollbackCheck {
  RollbackState state = RollbackState::kAbsent;
  std::string reason;
};

class RollbackTarget {
 public:
  virtual ~RollbackTarget() = default;
  /// Whether the previous release's runtime at `runtime_path` can be put back. PC answers from update\previous\<from>\.
  virtual RollbackCheck Check(const std::string& runtime_path) const = 0;
};

// ---- RuntimeLoaderEdge: what the load of the verified file did before its entry ---------------------------------------

class RuntimeLoaderEdge {
 public:
  virtual ~RuntimeLoaderEdge() = default;
  /// True when the runtime's own code ran before its entry was looked up. PC maps the DLL under the loader lock, so its
  /// DllMain runs at the map: true. A runtime with no constructors answers false (the Quest edge, not written here).
  virtual bool CodeBeforeEntry() const noexcept = 0;
};

// ---- UpdateLog and UpdateClock: one record per decision, and the elapsed time of a start ------------------------------

class UpdateLog {
 public:
  virtual ~UpdateLog() = default;
  /// One structured record (the event, the outcome and its fields), written to the platform's log.
  virtual void Emit(const nlohmann::json& record) = 0;
};

class UpdateClock {
 public:
  virtual ~UpdateClock() = default;
  /// A monotonic millisecond count; differences are elapsed times.
  virtual std::uint64_t TickMs() const noexcept = 0;
  /// The current UTC time as "YYYY-MM-DDTHH:MM:SSZ", recorded in quarantine entries.
  virtual std::string UtcIso() const = 0;
};

}  // namespace nevr_update
