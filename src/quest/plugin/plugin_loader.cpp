#include "quest/plugin/plugin_loader.h"

#include <atomic>
#include <exception>
#include <utility>

#include "quest/data/sha256.h"
#include "runtime/ext/plugin_log.h"

namespace nevr_quest::plugin {

namespace {

// The one loader the plugins' C callbacks (ctx->get_plugin_count, ctx->log) reach. A process has one.
std::atomic<Loader*> g_active{nullptr};

using IdsFn = const char* (*)(void);

template <typename Fn>
Fn Resolve(Platform& platform, void* handle, const char* name) {
  return reinterpret_cast<Fn>(platform.Symbol(handle, name));
}

LoadRecord BaseRecord(const PlanEntry& entry) {
  LoadRecord record;
  record.name = entry.item.name;
  record.file = entry.item.file;
  record.phase = entry.phase;
  record.required = entry.item.required;
  record.argKeys = ArgKeys(entry.item.args_json);
  return record;
}

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
    LoadRecord record = BaseRecord(entry);
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
    LoadRecord record = BaseRecord(entry);
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

NvrGameContext Loader::ContextFor(const PlanEntry& entry) const {
  NvrGameContext ctx = {};
  ctx.base_addr = gameBase_;
  ctx.net_game = nullptr;
  ctx.game_state = 0;
  ctx.flags = NEVR_HOST_IS_CLIENT | NEVR_HOST_PLATFORM_QUEST;
  ctx.ctx_size = sizeof(NvrGameContext);
  ctx.get_plugin_count = &Loader::GetLoadedPluginCount;
  ctx.get_plugin_info = &Loader::GetLoadedPluginInfo;
  ctx.plugin_id = entry.item.name.c_str();
  ctx.log = &Loader::HostLog;
  ctx.level_symbol = 0;
  ctx.level_name = nullptr;
  ctx.game_build_id = gameBuildId_.empty() ? nullptr : gameBuildId_.c_str();
  return ctx;
}

void Loader::Load(PlanEntry& entry) {
  LoadRecord record = BaseRecord(entry);
  const auto end = [&](Outcome outcome, const char* reasonToken) {
    record.outcome = outcome;
    record.reason = reasonToken;
    Settle(entry, record);
  };
  try {
    if (gameBuildId_.empty()) gameBuildId_ = platform_.GameBuildId();
    if (gameBase_ == 0) gameBase_ = platform_.GameBase();
    if (!gameBuildId_.empty()) record.gameBuildId = gameBuildId_;

    std::vector<std::uint8_t> bytes;
    if (platform_.ReadFile(entry.item.file, &bytes)) {
      record.sha256 = data::Sha256Hex(bytes.data(), bytes.size());
      const std::string id = BuildIdOf(bytes.data(), bytes.size());
      record.pluginBuildId = id.empty() ? "none" : id;
      if (!IsSharedObjectFor(bytes.data(), bytes.size(), options_.machine)) return end(Outcome::kRefused, reason::kMachineType);
    } else {
      record.sha256 = "unavailable";
      record.pluginBuildId = "unavailable";
    }

    void* const handle = platform_.Open(entry.item.file);
    if (handle == nullptr) return end(Outcome::kFailed, reason::kDlopenFailed);

    const auto getInfo = Resolve<NvrPluginGetInfo_fn>(platform_, handle, "NvrPluginGetInfo");
    if (getInfo == nullptr) return end(Outcome::kFailed, reason::kNoPluginInfo);

    // The plugin's own list of the libr15 builds its addresses hold for, against the build that is running. Its
    // constructors have run by now (dlopen ran them); the refusal is before its init.
    const auto buildIds = Resolve<IdsFn>(platform_, handle, "NEVR_GetSupportedBuildIds");
    switch (DecideBuildId(buildIds != nullptr ? buildIds() : nullptr, gameBuildId_)) {
      case BuildIdVerdict::kSupported: break;
      case BuildIdVerdict::kUndeclared: return end(Outcome::kRefused, reason::kBuildIdsUndeclared);
      case BuildIdVerdict::kUnknownGame: return end(Outcome::kRefused, reason::kGameBuildIdUnavailable);
      case BuildIdVerdict::kUnsupported: return end(Outcome::kRefused, reason::kBuildIdUnsupported);
    }

    const NvrPluginInfo info = getInfo();
    LoadedPlugin plugin;
    plugin.name = info.name != nullptr ? info.name : "";
    plugin.description = info.description != nullptr ? info.description : "";
    plugin.info.version_major = info.version_major;
    plugin.info.version_minor = info.version_minor;
    plugin.info.version_patch = info.version_patch;
    if (const auto apiVersion = Resolve<NvrPluginGetApiVersion_fn>(platform_, handle, "NvrPluginGetApiVersion")) {
      plugin.info.api_version = apiVersion();
    }
    if (const auto capabilities = Resolve<NvrPluginGetCapabilities_fn>(platform_, handle, "NvrPluginGetCapabilities")) {
      plugin.info.capabilities = capabilities();
    }

    contexts_.push_back(ContextFor(entry));
    const NvrGameContext* ctx = &contexts_.back();
    const auto initEx = Resolve<NvrPluginInitEx_fn>(platform_, handle, "NvrPluginInitEx");
    const auto init = Resolve<NvrPluginInit_fn>(platform_, handle, "NvrPluginInit");
    int rc = 0;
    switch (ChoosePluginInit(initEx != nullptr, init != nullptr)) {
      case PluginInitKind::Ex: rc = initEx(ctx, entry.item.args_json.c_str()); break;
      case PluginInitKind::Legacy: rc = init(ctx); break;
      case PluginInitKind::None: break;
    }
    if (rc != 0) {
      // The documented lifecycle: a failed init is followed by the plugin's Shutdown, when it has one.
      if (const auto shutdown = Resolve<NvrPluginShutdown_fn>(platform_, handle, "NvrPluginShutdown")) shutdown();
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

const NvrLoadedPluginInfo* Loader::GetLoadedPluginInfo(int index) {
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
