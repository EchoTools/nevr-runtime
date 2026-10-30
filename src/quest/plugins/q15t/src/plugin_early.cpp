// The early plugin (libq15t_early.so): the two modules that must act before android_main reads the heap sizes, heaps and memsys.
//
// They need the loader's early phase (`early: true` on this entry in config.yaml, ADR 0019 moment (a), the sentinel constructor), and an
// entry is one phase, so they cannot share a file with the modules that need the loaded game library (plugin.cpp). Both modules only
// store to data pages through the checked slot swap; a slot that still holds 0 because the dynamic linker has not relocated it yet is
// refused, loudly (docs/quest/q15t/modules/heaps.md section 1). Whether the early phase runs at all on a headset is ADR 0019 P10; if it does not,
// the loader refuses this entry as early_unsupported and nothing here runs.
//
// No namespace-scope object has a constructor: the repack's static-init check refuses a plugin that has one.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "nevr/plugin_interface.h"
#include "heaps.h"
#include "memsys.h"
#include "plugin_log.h"

#ifndef Q15T_BUILD_ID
#define Q15T_BUILD_ID "b243509c08ce677aeb95fa348016949b3fc45230"
#endif
#ifndef Q15T_HEAPS
#define Q15T_HEAPS 1
#endif
#ifndef Q15T_MEMSYS
#define Q15T_MEMSYS 1
#endif

NEVR_DECLARE_SUPPORTED_BUILD_IDS(Q15T_BUILD_ID);

namespace {

bool Covers(const NvrGameContext* ctx, std::size_t end_of_field) { return ctx->ctx_size >= end_of_field; }

// One structured line per module install, in the same shape as plugin.cpp's (module=, status=, reason=, real_resolved=).
[[maybe_unused]] void ReportModule(const char* module, const q15t::InstallResult& r) {
  using namespace q15t;
  const bool ok = r.status == SwapStatus::kOk;
  const bool off = !ok && (std::strcmp(r.reason, "off") == 0 || std::strcmp(r.reason, "off_file") == 0);
  const char* reason = r.reason[0] != 0 || ok ? r.reason : StatusName(r.status);
  const NevrLogField f[] = {{"module", module},
                            {"status", off ? "off" : StatusName(r.status)},
                            {"reason", reason},
                            {"real_resolved", r.real_resolved ? "1" : "0"}};
  Log(ok || off ? NEVR_LOG_INFO : NEVR_LOG_ERROR, ok || off ? "module install" : "module install FAILED", f, 4);
}

}  // namespace

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
  return {"q15t_early", "heap sizes and render-target pool, before android_main reads them", 0, 1, 0};
}
NEVR_PLUGIN_API uint32_t NvrPluginGetApiVersion(void) { return NEVR_PLUGIN_API_VERSION; }
NEVR_PLUGIN_API uint32_t NvrPluginGetCapabilities(void) { return NEVR_PLUGIN_CAP_HOOKS_ENGINE; }

NEVR_PLUGIN_API int NvrPluginInitEx(const NvrGameContext* ctx, const char* /*args_json*/) {
  if (ctx == nullptr) return 1;
  using namespace q15t;
  if (Covers(ctx, offsetof(NvrGameContext, log) + sizeof(ctx->log))) SetLogContext(ctx);
  const bool covers_id = Covers(ctx, offsetof(NvrGameContext, game_build_id) + sizeof(ctx->game_build_id));
  const bool quest = (ctx->flags & NEVR_HOST_PLATFORM_QUEST) != 0;
  const char* id = covers_id && ctx->game_build_id != nullptr ? ctx->game_build_id : "";
  char base_hex[24];
  std::snprintf(base_hex, sizeof(base_hex), "0x%llx", static_cast<unsigned long long>(ctx->base_addr));
  const NevrLogField start[] = {{"game_build_id", id}, {"pinned", Q15T_BUILD_ID}, {"base", base_hex},
                                {"heaps", Q15T_HEAPS ? "1" : "0"}, {"memsys", Q15T_MEMSYS ? "1" : "0"}};
  Log(NEVR_LOG_INFO, "q15t_early init", start, 5);
  if (!quest || std::strcmp(id, Q15T_BUILD_ID) != 0 || ctx->base_addr == 0) {
    Log(NEVR_LOG_WARNING, "refused: not the pinned game build, or the game library base is not known yet", start, 5);
    return 0;  // loaded, nothing installed
  }
#if Q15T_HEAPS
  ReportModule("heaps", InstallHeaps(ctx->base_addr));
#endif
#if Q15T_MEMSYS
  ReportModule("memsys", InstallMemsys(ctx->base_addr));
#endif
  // Always 0 from here: a non-zero return makes the loader dlclose the plugin, which is fatal once any word is swapped.
  return 0;
}

NEVR_PLUGIN_API void NvrPluginShutdown(void) {}
