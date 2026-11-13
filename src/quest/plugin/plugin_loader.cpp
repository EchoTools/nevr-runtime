#include "quest/plugin/plugin_loader.h"

#include <atomic>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "quest/data/sha256.h"
#include "quest/plugin/slot_hook.h"
#include "runtime/ext/plugin_log.h"

namespace nevr_quest::plugin {

namespace {

// The one loader the plugins' C callbacks (ctx->get_plugin_count, ctx->log) reach. A process has one.
std::atomic<Loader*> g_active{nullptr};

// A plugin entry point, under its Nevr name first and, when the plugin does not export that, under the deprecated Nvr
// name a plugin built before the prefix rename exports. `suffix` is the name without the prefix ("PluginInitEx").
// `deprecatedUsed` collects the deprecated names that resolved, so the loader can say once that the plugin loaded
// through them.
template <typename Fn>
Fn Resolve(Platform& platform, void* handle, const char* suffix, std::vector<std::string>* deprecatedUsed) {
  if (void* canonical = platform.Symbol(handle, (std::string("Nevr") + suffix).c_str())) {
    return reinterpret_cast<Fn>(canonical);
  }
  const std::string old = std::string("Nvr") + suffix;
  void* deprecated = platform.Symbol(handle, old.c_str());
  if (deprecated != nullptr) deprecatedUsed->push_back(old);
  return reinterpret_cast<Fn>(deprecated);
}

LoadRecord BaseRecord(const PlanEntry& entry, bool configRejected) {
  LoadRecord record;
  record.name = entry.item.name;
  record.file = entry.item.file;
  record.phase = entry.phase;
  record.required = entry.item.required;
  record.argKeys = ArgKeys(entry.item.args_json);
  record.source = SourceName(entry.source);
  record.overridden = entry.overridden;
  record.expectedSha256 = entry.expectedSha256;
  record.configRejected = configRejected;
  return record;
}

// The descriptor ReadFile returned for a load. It is closed when the load ends on any path, after the object has been mapped
// from it (mapping does not need it open afterwards).
class FileHold {
 public:
  explicit FileHold(Platform& platform) : platform_(platform) {}
  ~FileHold() { Release(); }
  FileHold(const FileHold&) = delete;
  FileHold& operator=(const FileHold&) = delete;

  int* slot() { return &fd_; }
  int fd() const { return fd_; }

 private:
  void Release() {
    if (fd_ < 0) return;
    const int fd = fd_;
    fd_ = -1;
    try {
      platform_.CloseFile(fd);
    } catch (const std::exception&) {
      // A descriptor the platform cannot close is not a reason to fail the load that is already settled.
    }
  }

  Platform& platform_;
  int fd_ = -1;
};

}  // namespace

Loader::Loader(Platform& platform, Options options) : platform_(platform), options_(options) { g_active.store(this); }

Loader::~Loader() {
  Loader* self = this;
  g_active.compare_exchange_strong(self, nullptr);
}

void Loader::Settle(PlanEntry& entry, const LoadRecord& record) {
  entry.pending = false;
  entry.outcome = record.outcome;
  entry.reasonToken = record.reason;
  try {
    platform_.Emit(record);
  } catch (const std::exception&) {
    // The platform reports its own write failures; the loader goes on to the next entry.
  }
}

void Loader::Plan(std::vector<PluginLoadItem> items) {
  std::vector<PlannedItem> planned;
  planned.reserve(items.size());
  for (PluginLoadItem& item : items) {
    PlannedItem one;
    one.item = std::move(item);
    planned.push_back(std::move(one));
  }
  Plan(std::move(planned));
}

void Loader::Plan(std::vector<PlannedItem> items) {
  if (planned_) return;
  planned_ = true;
  entries_ = BuildPlan(std::move(items), options_.earlySupported);
  std::size_t early = 0, normal = 0, refused = 0, disabled = 0;
  for (const PlanEntry& entry : entries_) {
    if (entry.outcome == Outcome::kDisabled) {
      ++disabled;
    } else if (entry.outcome == Outcome::kRefused && !entry.pending) {
      ++refused;
    } else if (entry.phase == PluginPhase::Early) {
      ++early;
    } else {
      ++normal;
    }
  }
  try {
    platform_.EmitPlan(entries_.size(), early, normal, refused, disabled);
  } catch (const std::exception&) {
  }
  for (PlanEntry& entry : entries_) {
    if (entry.pending) continue;
    LoadRecord record = BaseRecord(entry, options_.configRejected);
    record.outcome = entry.outcome;
    record.reason = entry.reasonToken;
    Settle(entry, record);
  }
}

void Loader::RunPhase(PluginPhase phase) {
  if (phase == PluginPhase::Early) earlyRan_ = true;
  for (PlanEntry& entry : entries_) {
    if (entry.pending && PluginInPhase(entry.item, phase, earlyRan_)) Load(entry);
  }
}

void Loader::Finish(const char* reasonToken) {
  for (PlanEntry& entry : entries_) {
    if (!entry.pending) continue;
    LoadRecord record = BaseRecord(entry, options_.configRejected);
    record.outcome = Outcome::kNotStarted;
    record.reason = reasonToken;
    Settle(entry, record);
  }
}

std::size_t Loader::PendingFor(PluginPhase phase) const {
  std::size_t count = 0;
  for (const PlanEntry& entry : entries_) {
    if (entry.pending && PluginInPhase(entry.item, phase, earlyRan_)) ++count;
  }
  return count;
}

NevrGameContext Loader::ContextFor(const PlanEntry& entry, const char* pluginViewBuildId) const {
  NevrGameContext ctx = {};
  ctx.base_addr = gameBase_;
  ctx.net_game = nullptr;
  ctx.game_state = 0;
  ctx.flags = NEVR_HOST_IS_CLIENT | NEVR_HOST_PLATFORM_QUEST;
  ctx.ctx_size = sizeof(NevrGameContext);
  ctx.get_plugin_count = &Loader::GetLoadedPluginCount;
  ctx.get_plugin_info = &Loader::GetLoadedPluginInfo;
  ctx.plugin_id = entry.item.name.c_str();
  ctx.log = &Loader::HostLog;
  const MainLevel level = platform_.CurrentMainLevel();
  ctx.level_symbol = level.symbol;
  ctx.level_name = level.name;
  ctx.game_build_id = pluginViewBuildId;
  ctx.quest = platform_.QuestHostApi();
  return ctx;
}

std::vector<PluginManifestEntry> Loader::Report() const {
  std::vector<PluginManifestEntry> report;
  report.reserve(entries_.size());
  for (const PlanEntry& entry : entries_) {
    PluginManifestEntry out;
    out.name = entry.item.name;
    out.file = entry.item.file;
    out.enabled = entry.outcome != Outcome::kDisabled;
    out.required = entry.item.required;
    out.loaded = !entry.pending && entry.outcome == Outcome::kLoaded;
    if (out.loaded) {
      for (const LoadedPlugin& plugin : loaded_) {
        if (plugin.entryName != entry.item.name) continue;
        out.version_major = plugin.info.version_major;
        out.version_minor = plugin.info.version_minor;
        out.version_patch = plugin.info.version_patch;
        out.api_version = plugin.info.api_version;
        out.capabilities = plugin.info.capabilities;
        break;
      }
    } else if (out.enabled) {
      out.error = entry.pending ? "pending" : entry.reasonToken;
    }
    report.push_back(std::move(out));
  }
  return report;
}

void Loader::RefreshMainLevel() {
  if (contexts_.empty()) return;  // no plugin holds a context yet: nothing to keep current, and no read of the game
  const MainLevel level = platform_.CurrentMainLevel();
  for (NevrGameContext& ctx : contexts_) {
    ctx.level_symbol = level.symbol;
    ctx.level_name = level.name;
  }
}

void Loader::Load(PlanEntry& entry) {
  LoadRecord record = BaseRecord(entry, options_.configRejected);
  void* handle = nullptr;  // set once the object is mapped; closed again on every path that does not keep the plugin
  FileHold file(platform_);  // the one descriptor every check reads and the mapping uses (#1521)
  // The plugin's id as the host knows it: bound around its init and the Shutdown after a failed init, so a handler it registers
  // on the loader's thread is recorded under this id and no other (slot_hook.h).
  slot_hook::Binding binding(entry.item.name.c_str());
  const auto close = [&] {
    if (handle == nullptr) return;
    void* const mapped = handle;
    handle = nullptr;
    // A slot hook this plugin registered may still be on its chain (ADR 0034): unmapping the object would leave the game
    // calling code that is gone. The object stays mapped for the life of the process, and the record says why. A plugin
    // that registered a handler during its init keeps its object mapped even after it unregisters it: the call that was
    // inside the handler when it unregistered may still be running that code.
    if (binding.registered() || slot_hook::HoldsHandleOf(entry.item.name.c_str())) {
      record.keptMapped = "slot_hook";
      return;
    }
    try {
      platform_.Close(mapped);
    } catch (const std::exception&) {
      // The object stays mapped; the record below still says why the plugin was not kept.
    }
  };
  const auto end = [&](Outcome outcome, const char* reasonToken) {
    if (outcome != Outcome::kLoaded) close();
    record.outcome = outcome;
    record.reason = reasonToken;
    Settle(entry, record);
  };
  try {
    // The running libr15 is read again for every decision (ADR 0021 section 12): the plugin identity is the stock ID only
    // while the live note, read now, is the one the patch verification covered, and a note read once and kept could
    // outlive that. `view` points into `live` or at the stock ID; both live to the end of this function.
    const std::string live = platform_.GameBuildId();
    const Libr15PatchState patch = platform_.Libr15Patch();
    const PluginView view = ResolvePluginView(live.c_str(), patch.verified, patch.verifiedBuildId.c_str());
    if (gameBase_ == 0) gameBase_ = platform_.GameBase();
    if (!live.empty()) {
      record.gameBuildId = live;
      record.pluginViewBuildId = view.buildId;
    }
    record.libr15Patched = view.patched;

    // Everything that decides whether the plugin may run is decided from the file's bytes, before dlopen: mapping a
    // shared object runs its constructors, so a refusal after the mapping would come after the plugin's own code. The bytes
    // and the descriptor come from one open, and the mapping is made from that descriptor (#1521): a later write under the
    // name cannot change what is mapped.
    std::vector<std::uint8_t> bytes;
    if (!platform_.ReadFile(entry.item.file, &bytes, file.slot())) {
      record.sha256 = "unavailable";
      record.pluginBuildId = "unavailable";
      return end(Outcome::kFailed, reason::kFileUnreadable);
    }
    record.sha256 = data::Sha256Hex(bytes.data(), bytes.size());
    // A packaged file is the one the repack hashed: other bytes under its name are refused before anything else reads them.
    if (!entry.expectedSha256.empty() && record.sha256 != entry.expectedSha256) {
      return end(Outcome::kRefused, reason::kManifestHashMismatch);
    }
    const std::string id = BuildIdOf(bytes.data(), bytes.size());
    record.pluginBuildId = id.empty() ? "none" : id;
    if (!IsSharedObjectFor(bytes.data(), bytes.size(), options_.machine)) return end(Outcome::kRefused, reason::kMachineType);

    // The plugin's own list of the libr15 builds its addresses hold for (an ELF note), against the build that is running.
    const std::optional<std::string> declared = SupportedBuildIdsOf(bytes.data(), bytes.size());
    switch (DecideBuildIdForView(declared.has_value() ? declared->c_str() : nullptr, live, view)) {
      case BuildIdVerdict::kSupported: break;
      case BuildIdVerdict::kUndeclared: return end(Outcome::kRefused, reason::kBuildIdsUndeclared);
      case BuildIdVerdict::kUnknownGame: return end(Outcome::kRefused, reason::kGameBuildIdUnavailable);
      case BuildIdVerdict::kUnsupported: return end(Outcome::kRefused, reason::kBuildIdUnsupported);
    }
    // A matching note is not enough: a plugin's addresses are properties of the stock file, so it runs only on the file that
    // attach verified as our patch. On any other libr15 (the stock file, or a note with no verified stubs) it is refused.
    if (!view.patched) return end(Outcome::kRefused, reason::kLibR15NotVerified);

    handle = platform_.Open(file.fd(), entry.item.file);
    if (handle == nullptr) return end(Outcome::kFailed, reason::kDlopenFailed);

    std::vector<std::string> deprecatedUsed;
    const auto getInfo = Resolve<NevrPluginGetInfo_fn>(platform_, handle, "PluginGetInfo", &deprecatedUsed);
    if (getInfo == nullptr) return end(Outcome::kFailed, reason::kNoPluginInfo);

    const NevrPluginInfo info = getInfo();
    LoadedPlugin plugin;
    plugin.name = info.name != nullptr ? info.name : "";
    plugin.description = info.description != nullptr ? info.description : "";
    plugin.entryName = entry.item.name;
    plugin.info.version_major = info.version_major;
    plugin.info.version_minor = info.version_minor;
    plugin.info.version_patch = info.version_patch;
    if (const auto apiVersion = Resolve<NevrPluginGetApiVersion_fn>(platform_, handle, "PluginGetApiVersion", &deprecatedUsed)) {
      plugin.info.api_version = apiVersion();
    }
    if (const auto capabilities = Resolve<NevrPluginGetCapabilities_fn>(platform_, handle, "PluginGetCapabilities", &deprecatedUsed)) {
      plugin.info.capabilities = capabilities();
    }

    viewIds_.push_back(view.buildId);  // a deque: the context keeps this pointer for the life of the process
    contexts_.push_back(ContextFor(entry, viewIds_.back().c_str()));
    const NevrGameContext* ctx = &contexts_.back();
    const auto initEx = Resolve<NevrPluginInitEx_fn>(platform_, handle, "PluginInitEx", &deprecatedUsed);
    const auto init = Resolve<NevrPluginInit_fn>(platform_, handle, "PluginInit", &deprecatedUsed);
    const auto shutdown = Resolve<NevrPluginShutdown_fn>(platform_, handle, "PluginShutdown", &deprecatedUsed);
    // Every entry point is resolved above, Shutdown included, so the line names every deprecated export the plugin uses.
    if (!deprecatedUsed.empty()) {
      std::string names;
      for (const std::string& n : deprecatedUsed) names += (names.empty() ? "" : ", ") + n;
      platform_.PluginLine(NEVR_LOG_INFO, "[NEVR.PLUGIN] plugin=" + entry.item.name + " file=" + entry.item.file +
                                              " loaded with deprecated Nvr exports (" + names +
                                              "); rebuild against the current SDK to export the Nevr names");
    }
    int rc = 0;
    {
      const slot_hook::Scope scope(binding);
      switch (ChoosePluginInit(initEx != nullptr, init != nullptr)) {
        case PluginInitKind::Ex: rc = initEx(ctx, entry.item.args_json.c_str()); break;
        case PluginInitKind::Legacy: rc = init(ctx); break;
        case PluginInitKind::None: break;
      }
    }
    if (rc != 0) {
      // The documented lifecycle: a failed init is followed by the plugin's Shutdown, when it has one. It runs under the same
      // binding, so an unregister there is the owner's and the handler it takes out is the one the init registered.
      if (shutdown != nullptr) {
        const slot_hook::Scope scope(binding);
        shutdown();
      }
      return end(Outcome::kFailed, reason::kInitFailed);
    }
    loaded_.push_back(std::move(plugin));
    LoadedPlugin& kept = loaded_.back();
    kept.info.name = kept.name.c_str();
    kept.info.description = kept.description.c_str();
    end(Outcome::kLoaded, "");
  } catch (const std::exception&) {
    if (entry.pending) end(Outcome::kFailed, reason::kException);
  }
}

int Loader::GetLoadedPluginCount() {
  const Loader* loader = g_active.load();
  return loader != nullptr ? static_cast<int>(loader->loaded_.size()) : 0;
}

const NevrLoadedPluginInfo* Loader::GetLoadedPluginInfo(int index) {
  const Loader* loader = g_active.load();
  if (loader == nullptr || index < 0 || static_cast<std::size_t>(index) >= loader->loaded_.size()) return nullptr;
  return &loader->loaded_[static_cast<std::size_t>(index)].info;
}

void Loader::HostLog(const char* pluginId, std::uint32_t level, const char* message, const NevrLogField* fields,
                     std::uint32_t fieldCount) {
  Loader* loader = g_active.load();
  if (loader == nullptr) return;
  try {
    const nevr_plugin_log::Formatted line = nevr_plugin_log::Format(pluginId, level, message, fields, fieldCount);
    loader->platform_.PluginLine(line.level, line.line);
  } catch (const std::exception&) {
    // A plugin's log call never throws into the plugin.
  }
}

}  // namespace nevr_quest::plugin
