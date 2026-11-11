// A no-op plugin for the Quest loader's host test (src/quest/tests/plugin_loader_test.cpp). One source, built several
// times with different macros into lib<variant>.so: each variant is one way a plugin can be right or wrong. It records what
// it saw in `fx_seen`, which the test reads with dlsym; it logs through ctx->log the way a real plugin does.
#include <cstddef>
#include <cstdint>
#include <cstring>
#ifdef FX_SLOT_HOOK
#include <cstdlib>
#include <pthread.h>
#include <unistd.h>
#endif

#include "nevr/plugin_interface.h"
#include "quest/tests/plugin_fixture_seen.h"

#ifndef FX_NAME
#define FX_NAME "fx"
#endif
// The build IDs this plugin says it supports, as the ELF note the loader reads before it maps the file (the test's fake running build is the second one).
#ifndef FX_IDS
#define FX_IDS "1111111111111111111111111111111111111111 2222222222222222222222222222222222222222"
#endif
#ifndef FX_INIT_RC
#define FX_INIT_RC 0
#endif

extern "C" {
__attribute__((visibility("default"))) FxSeen fx_seen = {};
}

#ifndef FX_NO_IDS
NEVR_DECLARE_SUPPORTED_BUILD_IDS(FX_IDS);
#endif

namespace {
#ifndef FX_NO_CTOR
// A namespace-scope constructor, which a shipped plugin must not have (tools/check_quest_static_init.sh --plugin); the
// loader tests keep it to show that a refused plugin's constructors never ran. The device plugin is built without it.
struct Ctor {
  Ctor() { fx_seen.ctor_ran = 1; }
} g_ctor;
#endif

void Copy(char* to, std::size_t cap, const char* from) {
  std::strncpy(to, from != nullptr ? from : "", cap - 1);
  to[cap - 1] = '\0';
}

void Record(const NevrGameContext* ctx, const char* args) {
  ++fx_seen.init_count;
  fx_seen.ctx_size = ctx->ctx_size;
  fx_seen.flags = ctx->flags;
  fx_seen.base_addr = ctx->base_addr;
  fx_seen.net_game = ctx->net_game;
  fx_seen.game_state = ctx->game_state;
  Copy(fx_seen.plugin_id, sizeof(fx_seen.plugin_id), ctx->plugin_id);
  Copy(fx_seen.args, sizeof(fx_seen.args), args);
  if (ctx->ctx_size >= offsetof(NevrGameContext, level_name) + sizeof(ctx->level_name)) {
    fx_seen.level_symbol = ctx->level_symbol;
    fx_seen.level_name = ctx->level_name;
  }
  fx_seen.covers_game_build_id =
      ctx->ctx_size >= offsetof(NevrGameContext, game_build_id) + sizeof(ctx->game_build_id) ? 1 : 0;
  if (fx_seen.covers_game_build_id != 0) Copy(fx_seen.game_build_id, sizeof(fx_seen.game_build_id), ctx->game_build_id);
  fx_seen.covers_quest = ctx->ctx_size >= offsetof(NevrGameContext, quest) + sizeof(ctx->quest) ? 1 : 0;
  if (fx_seen.covers_quest != 0) fx_seen.quest = ctx->quest;
  fx_seen.plugin_count_at_init = ctx->get_plugin_count != nullptr ? ctx->get_plugin_count() : -1;
  if (fx_seen.plugin_count_at_init > 0) {
    const NevrLoadedPluginInfo* first = ctx->get_plugin_info(0);
    Copy(fx_seen.first_plugin_name, sizeof(fx_seen.first_plugin_name), first != nullptr ? first->name : "");
  }
  const NevrLogField fields[] = {{"k", "v"}};
  ctx->log(ctx->plugin_id, NEVR_LOG_INFO, "fixture init", fields, 1);
}
}  // namespace

#ifdef FX_SLOT_HOOK
// The slot-hook probes (#2001, round 3). The handler, its init and its Shutdown are in this object, so a loader that unmaps
// the object while the handler is still chained runs code that is gone: that is the crash the probes look for.
//   FX_SLOT_CLAIM: the plugin_id the handler registers under (default: ctx->plugin_id, the id the host gave the plugin).
//   FX_SLOT_WAIT: the init starts a thread that calls the chained function until the handler is entered; the handler then
//   holds that call inside it, and the init returns once it is entered. Shutdown unregisters the handler while the call is
//   still inside it. The thread's id is in fx_b2_tid, so the test can join it before it unmaps the object.
//   FX_SLOT_THREAD: the init registers nothing itself. It starts a thread that registers the handler, joins it, and returns.
//   FX_SLOT_SHUTDOWN: the init registers nothing. Its Shutdown registers the handler; fx_b2_register_rc is that result.
// args_json: {"build":"<lowercase hex GNU build ID of the module>","call":"<decimal address of the module's call>"}.
extern "C" {
__attribute__((visibility("default"))) volatile int fx_b2_entered = 0;
__attribute__((visibility("default"))) volatile int fx_b2_done = 0;
__attribute__((visibility("default"))) pthread_t fx_b2_tid = 0;
__attribute__((visibility("default"))) std::int32_t fx_b2_register_rc = -1;
__attribute__((visibility("default"))) std::int32_t fx_b2_unregister_rc = -1;
__attribute__((visibility("default"))) std::uint32_t fx_b2_handle = 0;
}

namespace {
void* fx_b2_original = nullptr;  // the game's function, which the handler calls (register publishes it)
const NevrQuestHostApi* fx_b2_quest = nullptr;
char fx_b2_claim[96] = {};
char fx_b2_build[96] = {};

int FxB2Handler(int a, int b) noexcept {
  fx_b2_entered = 1;
  // Holds the call inside the handler, so an unmap of this object happens while the thread is running its code.
  for (volatile long spin = 0; spin < 100000000L; spin = spin + 1) {
  }
  return reinterpret_cast<int (*)(int, int)>(fx_b2_original)(a, b) + 100;
}

// The value of `"build":"` in args_json, copied into out. False when the key is absent.
bool ArgBuildId(const char* args, char* out, std::size_t cap) {
  const char* at = std::strstr(args, "\"build\":\"");
  if (at == nullptr) return false;
  at += 9;
  std::size_t n = 0;
  while (at[n] != '\0' && at[n] != '"' && n + 1 < cap) {
    out[n] = at[n];
    ++n;
  }
  out[n] = '\0';
  return n > 0;
}

#ifdef FX_SLOT_WAIT
// The value of `"call":"` in args_json, as an address (0 when absent).
std::uintptr_t ArgCallAddress(const char* args) {
  const char* at = std::strstr(args, "\"call\":\"");
  return at == nullptr ? 0 : static_cast<std::uintptr_t>(std::strtoull(at + 8, nullptr, 10));
}

// Calls the chained function until its handler is entered: the call that enters the handler is the one it holds. The bound
// keeps a refused registration from looping forever.
void* CallUntilEntered(void* arg) {
  const auto call = reinterpret_cast<int (*)(int, int)>(reinterpret_cast<std::uintptr_t>(arg));
  for (long i = 0; i < 2000000 && fx_b2_entered == 0; i = i + 1) call(3, 4);
  fx_b2_done = 1;
  return nullptr;
}
#endif

// Registers the handler under the claim, against the module the args name. The result is fx_b2_register_rc.
void RegisterHandler() {
  fx_b2_register_rc = fx_b2_quest->register_slot_hook(fx_b2_claim, "libgotfx_consumer_now.so", "fx_add", NEVR_SLOT_RELOC_JUMP_SLOT,
                                                      fx_b2_build, reinterpret_cast<void*>(&FxB2Handler), &fx_b2_original,
                                                      &fx_b2_handle);
}

#ifdef FX_SLOT_THREAD
void* RegisterOnThread(void*) {
  RegisterHandler();
  return nullptr;
}
#endif

void SlotHookInit(const NevrGameContext* ctx, const char* args_json) {
  if (!ArgBuildId(args_json, fx_b2_build, sizeof(fx_b2_build))) return;
#ifdef FX_SLOT_CLAIM
  const char* claim = FX_SLOT_CLAIM;
#else
  const char* claim = ctx->plugin_id;
#endif
  Copy(fx_b2_claim, sizeof(fx_b2_claim), claim);
  const NevrQuestHostApi* quest = ctx->quest;
  if (quest == nullptr || quest->struct_size < offsetof(NevrQuestHostApi, unregister_slot_hook) + sizeof(quest->unregister_slot_hook)) {
    return;
  }
  fx_b2_quest = quest;
#if defined(FX_SLOT_THREAD)
  // The registration runs on a thread this init starts; the init waits for it and does not register itself.
  pthread_t registrar{};
  if (pthread_create(&registrar, nullptr, RegisterOnThread, nullptr) == 0) pthread_join(registrar, nullptr);
  return;
#elif defined(FX_SLOT_SHUTDOWN)
  return;  // the registration is the Shutdown's
#else
  RegisterHandler();
#endif
#ifdef FX_SLOT_WAIT
  if (fx_b2_register_rc == NEVR_SLOT_HOOK_OK) {
    const std::uintptr_t call = ArgCallAddress(args_json);
    if (call != 0 && pthread_create(&fx_b2_tid, nullptr, CallUntilEntered, reinterpret_cast<void*>(call)) == 0) {
      for (int i = 0; i < 5000 && fx_b2_entered == 0; ++i) usleep(1000);
    }
  }
#endif
}

#ifdef FX_SLOT_WAIT
void SlotHookShutdown() {
  if (fx_b2_quest != nullptr && fx_b2_register_rc == NEVR_SLOT_HOOK_OK) {
    fx_b2_unregister_rc = fx_b2_quest->unregister_slot_hook(fx_b2_claim, fx_b2_handle);
  }
}
#endif
#ifdef FX_SLOT_SHUTDOWN
void SlotHookShutdownRegister() {
  if (fx_b2_quest != nullptr) RegisterHandler();
}
#endif
}  // namespace
#endif  // FX_SLOT_HOOK

#ifndef FX_NO_INFO
NEVR_PLUGIN_API NevrPluginInfo NevrPluginGetInfo(void) { return {FX_NAME, "loader test fixture", 1, 2, 3}; }
#endif
NEVR_PLUGIN_API uint32_t NevrPluginGetApiVersion(void) { return NEVR_PLUGIN_API_VERSION; }
NEVR_PLUGIN_API uint32_t NevrPluginGetCapabilities(void) { return NEVR_PLUGIN_CAP_OBSERVES_ONLY; }
#ifdef FX_LEGACY_INIT
NEVR_PLUGIN_API int NevrPluginInit(const NevrGameContext* ctx) {
  Record(ctx, "");
  return FX_INIT_RC;
}
#else
NEVR_PLUGIN_API int NevrPluginInitEx(const NevrGameContext* ctx, const char* args_json) {
  Record(ctx, args_json);
#ifdef FX_SLOT_HOOK
  SlotHookInit(ctx, args_json);
#endif
  return FX_INIT_RC;
}
#endif
NEVR_PLUGIN_API void NevrPluginShutdown(void) {
  ++fx_seen.shutdown_count;
#ifdef FX_SLOT_WAIT
  SlotHookShutdown();
#endif
#ifdef FX_SLOT_SHUTDOWN
  SlotHookShutdownRegister();
#endif
}
