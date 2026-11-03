// Built with -fno-exceptions (got_hook.h is reached from the hook path; see slot_hook.h).
#include "quest/plugin/slot_hook.h"

#include <pthread.h>

#include <cstdio>
#include <cstring>

#include "hook_log.h"

namespace nevr_quest::slot_hook {

namespace {

constexpr std::size_t kModuleCapacity = 96;
constexpr std::size_t kSymbolCapacity = 64;
constexpr std::size_t kBuildIdCapacity = 80;
constexpr std::size_t kOwnerCapacity = 48;  // "plugin:" and the plugin id

// The names one handle holds. GotTarget keeps the pointers it is given, and a plugin's own buffers may be freed when its
// init returns, so the service copies every name here. A handle is in use from its reservation until its removal.
// `releasing` is set while one unregister takes the handle out of its chain: a second unregister is refused, so one GotHook
// is never removed twice.
struct Held {
  bool used = false;
  bool releasing = false;
  char module[kModuleCapacity] = {};
  char symbol[kSymbolCapacity] = {};
  char buildId[kBuildIdCapacity] = {};
  char owner[kOwnerCapacity] = {};
};

// The handles: one GotHook and one name record per index. The table's lock covers only the reservation, the claim for a
// removal and the release of a handle. The install and the remove run with no lock of this file held: they call
// dl_iterate_phdr, which takes the loader's lock, and that order (loader lock, then the GOT backend's lock) must not be
// inverted here.
sentinel::GotHook g_hooks[kSlotHookHandles];
Held g_held[kSlotHookHandles];
pthread_mutex_t g_tableLock = PTHREAD_MUTEX_INITIALIZER;

// The binding of the loader's call on this thread (Scope), or null outside any call of the loader.
thread_local Binding* g_bound = nullptr;

// The owner a plugin's handle is recorded under. The same text is built for a registration and for a comparison, so a long
// id is truncated the same way in both.
void OwnerOf(const char* pluginId, char* out, std::size_t capacity) noexcept {
  std::snprintf(out, capacity, "plugin:%s", pluginId);
}

// The token of a NevrSlotHookResult, for the Warning line of a refusal the service itself makes.
const char* ResultName(std::int32_t result) noexcept {
  switch (result) {
    case NEVR_SLOT_HOOK_OK: return "ok";
    case NEVR_SLOT_HOOK_BAD_ARGUMENT: return "bad_argument";
    case NEVR_SLOT_HOOK_NOT_FOUND: return "not_found";
    case NEVR_SLOT_HOOK_NO_HANDLE: return "no_handle";
    case NEVR_SLOT_HOOK_REFUSED: return "refused";
    default: return "unknown";
  }
}

// The result of a GOT backend status, for the plugin.
std::int32_t ResultOf(sentinel::GotStatus status) noexcept {
  switch (status) {
    case sentinel::GotStatus::kOk:
      return NEVR_SLOT_HOOK_OK;
    case sentinel::GotStatus::kBadArgument:
      return NEVR_SLOT_HOOK_BAD_ARGUMENT;
    case sentinel::GotStatus::kModuleNotLoaded:
    case sentinel::GotStatus::kSymbolNotFound:
    case sentinel::GotStatus::kWrongRelocationType:
      return NEVR_SLOT_HOOK_NOT_FOUND;
    default:
      return NEVR_SLOT_HOOK_REFUSED;
  }
}

// One Warning for a refusal the service makes itself. The plugin and the names are the caller's text, and the log escapes
// them; a null name reads "(null)".
void LogServiceRefusal(const char* op, std::int32_t result, const char* reason, const char* pluginId, const char* module,
                       const char* symbol, std::uint32_t handle) noexcept {
  sentinel::LogFields(sentinel::LogLevel::kWarn, "plugin_slot_hook",
                      {{"op", op},
                       {"status", ResultName(result)},
                       {"reason", reason},
                       {"plugin", pluginId != nullptr ? pluginId : "(null)"},
                       {"module", module != nullptr ? module : "(null)"},
                       {"symbol", symbol != nullptr ? symbol : "(null)"},
                       {"handle", static_cast<long long>(handle)}});
}

}  // namespace

Scope::Scope(Binding& binding) noexcept : previous_(g_bound) { g_bound = &binding; }

Scope::~Scope() { g_bound = previous_; }

std::int32_t RegisterSlotHookWith(sentinel::ImageLookup lookup, const char* pluginId, const char* module,
                                  const char* symbol, std::int32_t reloc, const char* buildId, void* handler,
                                  void** originalOut, std::uint32_t* outHandle) {
  // Each check names its reason, so a refused plugin's Warning says which argument it got wrong.
  Binding* const bound = g_bound;
  const char* reason = nullptr;
  if (lookup == nullptr || pluginId == nullptr || module == nullptr || symbol == nullptr || handler == nullptr ||
      originalOut == nullptr || outHandle == nullptr) {
    reason = "a required pointer is null";
  } else if (buildId == nullptr || buildId[0] == '\0') {
    reason = "the target names no build ID; the host refuses a plugin hook target without one (ADR 0019 section 5)";
  } else if (std::strlen(module) >= kModuleCapacity || std::strlen(symbol) >= kSymbolCapacity ||
             std::strlen(buildId) >= kBuildIdCapacity) {
    reason = "a name is longer than the service holds";
  } else if (reloc != NEVR_SLOT_RELOC_JUMP_SLOT && reloc != NEVR_SLOT_RELOC_GLOB_DAT) {
    reason = "the relocation is not a NevrSlotReloc";
  } else if (bound != nullptr && std::strcmp(bound->pluginId(), pluginId) != 0) {
    // The loader knows which plugin is calling. A handler recorded under another id would outlive the object that holds it.
    reason = "the plugin_id is not the id the host gave this plugin; the handler is refused";
  }
  if (reason != nullptr) {
    LogServiceRefusal("register", NEVR_SLOT_HOOK_BAD_ARGUMENT, reason, pluginId, module, symbol, 0);
    return NEVR_SLOT_HOOK_BAD_ARGUMENT;
  }

  // Reserve a handle and copy the names into it, under the table's lock. The install runs after the lock is released.
  std::size_t index = kSlotHookHandles;
  pthread_mutex_lock(&g_tableLock);
  for (std::size_t i = 0; i < kSlotHookHandles; ++i) {
    if (!g_held[i].used) {
      index = i;
      break;
    }
  }
  if (index < kSlotHookHandles) {
    Held& held = g_held[index];
    held.used = true;
    held.releasing = false;
    std::snprintf(held.module, sizeof(held.module), "%s", module);
    std::snprintf(held.symbol, sizeof(held.symbol), "%s", symbol);
    std::snprintf(held.buildId, sizeof(held.buildId), "%s", buildId);
    OwnerOf(pluginId, held.owner, sizeof(held.owner));
  }
  pthread_mutex_unlock(&g_tableLock);
  if (index == kSlotHookHandles) {
    LogServiceRefusal("register", NEVR_SLOT_HOOK_NO_HANDLE, "every service handle is in use", pluginId, module, symbol, 0);
    return NEVR_SLOT_HOOK_NO_HANDLE;
  }

  const Held& held = g_held[index];
  const sentinel::RelocKind kind =
      reloc == NEVR_SLOT_RELOC_GLOB_DAT ? sentinel::RelocKind::kGlobDat : sentinel::RelocKind::kJumpSlot;
  const sentinel::GotTarget target = sentinel::GotTarget(held.module, held.symbol, kind, held.buildId).Owned(held.owner);
  // The GOT backend logs its own refusal (the slot, the chain, the reason) and the install line.
  const sentinel::GotStatus status =
      sentinel::PluginHandlerInstaller::Install(g_hooks[index], target, handler, originalOut, lookup);
  if (status != sentinel::GotStatus::kOk) {
    if (g_hooks[index].mayBeLive()) {
      // A failed install whose store was live left the handler reachable through the slot. The handle stays held, so its
      // owner's object stays mapped (HoldsHandleOf) and nothing else takes the entry.
      LogServiceRefusal("register", ResultOf(status),
                        "a failed install left the handler reachable through its slot; the handle stays held", pluginId,
                        module, symbol, 0);
    } else {
      pthread_mutex_lock(&g_tableLock);
      g_held[index] = Held{};
      pthread_mutex_unlock(&g_tableLock);
    }
    return ResultOf(status);
  }
  *outHandle = static_cast<std::uint32_t>(index + 1);
  if (bound != nullptr) bound->NoteRegistered();
  return NEVR_SLOT_HOOK_OK;
}

std::int32_t RegisterSlotHook(const char* pluginId, const char* module, const char* symbol, std::int32_t reloc,
                              const char* buildId, void* handler, void** originalOut, std::uint32_t* outHandle) {
  return RegisterSlotHookWith(&sentinel::FindLoadedImage, pluginId, module, symbol, reloc, buildId, handler, originalOut,
                              outHandle);
}

std::int32_t UnregisterSlotHook(const char* pluginId, std::uint32_t handle) {
  if (pluginId == nullptr) {
    LogServiceRefusal("unregister", NEVR_SLOT_HOOK_BAD_ARGUMENT, "a required pointer is null", nullptr, nullptr, nullptr,
                      handle);
    return NEVR_SLOT_HOOK_BAD_ARGUMENT;
  }
  if (handle == 0 || handle > kSlotHookHandles) {
    LogServiceRefusal("unregister", NEVR_SLOT_HOOK_BAD_ARGUMENT, "the handle is not one the service issued", pluginId,
                      nullptr, nullptr, handle);
    return NEVR_SLOT_HOOK_BAD_ARGUMENT;
  }
  const std::size_t index = handle - 1;
  char owner[kOwnerCapacity];
  OwnerOf(pluginId, owner, sizeof(owner));

  // The claim: under the table's lock, the owner takes the handle into the releasing state, so no second unregister can
  // Remove the same GotHook. The removal itself runs with no lock held (see g_tableLock).
  const char* refusal = nullptr;
  std::int32_t result = NEVR_SLOT_HOOK_OK;
  pthread_mutex_lock(&g_tableLock);
  Held& held = g_held[index];
  if (!held.used) {
    refusal = "the handle is not held";
    result = NEVR_SLOT_HOOK_BAD_ARGUMENT;
  } else if (std::strcmp(held.owner, owner) != 0) {
    refusal = "the handle belongs to another plugin";
    result = NEVR_SLOT_HOOK_BAD_ARGUMENT;
  } else if (held.releasing) {
    refusal = "another unregister of this handle is in progress";
    result = NEVR_SLOT_HOOK_REFUSED;
  } else if (g_hooks[index].mayBeLive()) {
    refusal = "a failed install left the handler reachable through its slot; the handle stays held";
    result = NEVR_SLOT_HOOK_REFUSED;
  } else {
    held.releasing = true;
  }
  pthread_mutex_unlock(&g_tableLock);
  if (refusal != nullptr) {
    LogServiceRefusal("unregister", result, refusal, pluginId, nullptr, nullptr, handle);
    return result;
  }

  // Remove logs its own failure, and names the owner, the position and the chain it left. When the module moved or changed,
  // Remove drops the hook without writing the slot and the handle is released with it; any other failure leaves the handler
  // on its chain and the handle held (releasing is cleared, so the owner may try again).
  const sentinel::GotStatus status = g_hooks[index].Remove();
  const bool stillHeld = g_hooks[index].installed();
  pthread_mutex_lock(&g_tableLock);
  if (stillHeld) {
    g_held[index].releasing = false;
  } else {
    g_held[index] = Held{};
  }
  pthread_mutex_unlock(&g_tableLock);
  if (status == sentinel::GotStatus::kOk) return NEVR_SLOT_HOOK_OK;
  LogServiceRefusal("unregister", NEVR_SLOT_HOOK_REFUSED,
                    stillHeld ? "the host could not take the handler out of its chain; the handler stays chained and the handle stays held"
                              : "the module moved or changed; the hook was dropped without writing the slot and the handle is released",
                    pluginId, nullptr, nullptr, handle);
  return NEVR_SLOT_HOOK_REFUSED;
}

bool HoldsHandleOf(const char* pluginId) {
  if (pluginId == nullptr) return false;
  char owner[kOwnerCapacity];
  OwnerOf(pluginId, owner, sizeof(owner));
  bool holds = false;
  pthread_mutex_lock(&g_tableLock);
  for (std::size_t i = 0; i < kSlotHookHandles && !holds; ++i) {
    holds = g_held[i].used && std::strcmp(g_held[i].owner, owner) == 0;
  }
  pthread_mutex_unlock(&g_tableLock);
  return holds;
}

}  // namespace nevr_quest::slot_hook
