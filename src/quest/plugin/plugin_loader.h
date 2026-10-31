// The Quest plugin loader (docs/adr/0019-quest-plugin-loader.md, slice 1).
//
// Pure policy over a `Platform`: the host test gives it a fake platform and real fixture shared objects, the sentinel gives it
// the one that reads the native library directory and calls dlopen (plugin_platform.h). No Android header is included here.
//
// One `plugin_load` record ends every plan entry, whatever its outcome (loaded, refused, disabled, not_started, failed). The
// loader never throws and never ends the process: `required` on a client is recorded and the game goes on.
//
// What a plugin sees is the existing C ABI (nevr/plugin_interface.h): the same NvrGameContext as on PC, with
// base_addr the libr15 load base, net_game null, game_state 0, flags IS_CLIENT | PLATFORM_QUEST, the main level
// (level_symbol, level_name) the platform reports now and RefreshMainLevel keeps current (#1106), and game_build_id the
// plugin identity (plugin_identity.h: the stock libr15 ID when the running libr15 is our verified patched file, else the
// live note, ADR 0021 section 12). The per-frame and state-change exports are not called in slice 1: nothing ticks them yet.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "nevr/plugin_interface.h"
#include "quest/plugin/plugin_identity.h"
#include "quest/plugin/plugin_image.h"
#include "quest/plugin/plugin_plan.h"

namespace nevr_quest::plugin {

// The one `plugin_load` record of a plan entry. Unknown values are fixed tokens, never empty: "none" (the file was not
// read: the plan refused or disabled the entry first), "unavailable" (the file could not be read).
struct LoadRecord {
  std::string name;
  std::string file;
  std::string sha256 = "none";           // of the bytes the loader read
  std::string pluginBuildId = "none";    // the plugin file's GNU build ID
  std::string gameBuildId = "none";      // the running libr15's live note
  std::string pluginViewBuildId = "none";  // the ID the plugin's list was compared with (the plugin identity)
  bool libr15Patched = false;            // the running libr15 is our verified patched file
  PluginPhase phase = PluginPhase::Normal;
  Outcome outcome = Outcome::kNotStarted;
  std::string reason;                    // a reason:: token; empty for `loaded`
  bool required = false;
  std::string argKeys;                   // the keys of the entry's args, never the values
  std::string source = "config";         // "packaged" (in the APK, from the manifest) or "config" (config.yaml only)
  bool overridden = false;               // a config.yaml entry named this packaged file
  std::string expectedSha256;            // the manifest's hash for the file; empty when the file has none to be checked against
  bool configRejected = false;           // config.yaml was rejected as a whole: this plugin loaded with its manifest values alone
};

// The game's main level, as a plugin context carries it (level_symbol, level_name). `name` is null when the symbol is 0 or the
// generated name table does not know it (a level a plugin registered itself is not in the table: compare the symbol).
struct MainLevel {
  std::uint64_t symbol = 0;
  const char* name = nullptr;
};

// The loader's view of the world. Every method may throw std::exception; the loader contains it.
class Platform {
 public:
  virtual ~Platform() = default;
  // The game's main level now (#1106, the PC's `main level` read from the game context). The default is none: a platform
  // with no source for it (a host test) says so, and the context carries level_symbol 0.
  virtual MainLevel CurrentMainLevel() { return {}; }
  // The bytes of `file` in the native library directory; false when they cannot be read.
  virtual bool ReadFile(const std::string& file, std::vector<std::uint8_t>* bytes) = 0;
  // dlopen of `file`; null on failure.
  virtual void* Open(const std::string& file) = 0;
  // dlclose of a handle Open returned, for a plugin that is refused or fails after it was mapped.
  virtual void Close(void* handle) = 0;
  virtual void* Symbol(void* handle, const char* name) = 0;
  // The running libr15's build ID, lowercase hex; "" when it cannot be read.
  virtual std::string GameBuildId() = 0;
  // What the process recorded about the libr15 patch (ADR 0021 section 12): whether `Attach` verified it, and the ID it
  // verified. The loader reads it with the live note at every decision.
  virtual Libr15PatchState Libr15Patch() = 0;
  virtual std::uintptr_t GameBase() = 0;
  virtual void Emit(const LoadRecord& record) = 0;
  // One line of the plan: how many entries, and how each stands at the end of the plan step.
  virtual void EmitPlan(std::size_t entries, std::size_t early, std::size_t normal, std::size_t refused,
                        std::size_t disabled) = 0;
  // A line a plugin logged through ctx->log, already in the `[NEVR.PLUGIN] plugin=<id> ...` shape. `level` is an
  // NevrLogLevel value. May be called from any thread.
  virtual void PluginLine(std::uint32_t level, const std::string& line) = 0;
  // The Quest host services a plugin reaches through ctx->quest (#1202, ADR 0025).
  virtual const NevrQuestHostApi* QuestHostApi() = 0;
};

struct Options {
  std::uint16_t machine = kMachineAarch64;  // the host test builds its fixtures for x86-64
  bool earlySupported = true;               // ADR 0019 P10, decided on a headset: false refuses `early` entries
  bool configRejected = false;              // config.yaml was rejected as a whole; every record of this run says so
};

class Loader {
 public:
  Loader(Platform& platform, Options options);
  ~Loader();
  Loader(const Loader&) = delete;
  Loader& operator=(const Loader&) = delete;

  // Builds the plan and writes the record of every entry the plan alone settled (disabled, refused), then the plan line.
  // Runs no plugin code. Call once.
  void Plan(std::vector<PluginLoadItem> items);
  // The same over merged entries (MergePackaged): a packaged entry's expected hash is checked before its file is mapped.
  void Plan(std::vector<PlannedItem> items);

  // Loads the pending entries of `phase`, in list order, each ending in its one record. The normal pass also takes the
  // `early` entries when no early pass ran (PluginInPhase), so a boot path that never reaches the early moment still loads
  // them.
  void RunPhase(PluginPhase phase);

  // Ends every still-pending entry as `not_started` with `reasonToken`.
  void Finish(const char* reasonToken);

  // How many entries still wait for `phase` (counting the early ones in the normal phase when no early pass ran).
  std::size_t PendingFor(PluginPhase phase) const;
  const std::vector<PlanEntry>& Entries() const { return entries_; }

  // Writes the platform's current main level into every context already handed out, so a plugin that kept its pointer sees
  // a level change (#1106). Game thread, once per frame. A plugin reading the two fields while they are stored sees the old
  // or the new value: each is one aligned 8-byte store, and nothing else in the context changes.
  void RefreshMainLevel();

 private:
  // What the query API (ctx->get_plugin_info) hands out: the strings are copies, the struct is process-lifetime stable
  // (a deque never moves an element).
  struct LoadedPlugin {
    std::string name;
    std::string description;
    NvrLoadedPluginInfo info{};
  };

  void Load(PlanEntry& entry);
  // Ends `entry` with `record`'s outcome and writes the record. A platform that throws while writing it does not stop
  // the loader.
  void Settle(PlanEntry& entry, const LoadRecord& record);
  NvrGameContext ContextFor(const PlanEntry& entry, const char* pluginViewBuildId) const;

  static int GetLoadedPluginCount();
  static const NvrLoadedPluginInfo* GetLoadedPluginInfo(int index);
  static void HostLog(const char* pluginId, std::uint32_t level, const char* message, const NevrLogField* fields,
                      std::uint32_t fieldCount);

  Platform& platform_;
  Options options_;
  std::vector<PlanEntry> entries_;
  bool planned_ = false;
  bool earlyRan_ = false;
  std::uintptr_t gameBase_ = 0;
  std::deque<LoadedPlugin> loaded_;
  std::deque<std::string> viewIds_;      // the plugin identity each context points at (ctx.game_build_id)
  std::deque<NvrGameContext> contexts_;  // one per plugin that reached its init; a plugin may keep the pointer
};

}  // namespace nevr_quest::plugin
