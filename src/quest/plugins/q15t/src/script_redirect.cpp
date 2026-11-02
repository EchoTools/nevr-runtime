#include "script_redirect.h"

#include <cstddef>

#include "plugin_log.h"

namespace q15t {

namespace {

constexpr const char kPluginName[] = "q15t";
constexpr std::size_t kCount = sizeof kScriptRedirectHashes / sizeof kScriptRedirectHashes[0];

// The host's NevrScriptRedirectResult, as its log names it (src/quest/plugin/script_redirect.cpp, ResultName).
const char* ResultText(std::int32_t result) {
  switch (result) {
    case NEVR_SCRIPT_REDIRECT_OK: return "ok";
    case NEVR_SCRIPT_REDIRECT_BAD_ARGUMENT: return "bad_argument";
    case NEVR_SCRIPT_REDIRECT_ALREADY_ARMED: return "already_armed";
    case NEVR_SCRIPT_REDIRECT_MISSING_LIBRARY: return "missing_library";
    case NEVR_SCRIPT_REDIRECT_NO_LIBRARY_DIR: return "no_library_dir";
    default: return "unknown";
  }
}

}  // namespace

int RegisterScriptRedirect(const NvrGameContext* ctx) {
  if (ctx == nullptr) return -1;
  const bool covered = ctx->ctx_size >= offsetof(NvrGameContext, quest) + sizeof(ctx->quest);
  const NevrQuestHostApi* quest = covered ? ctx->quest : nullptr;
  const bool hasRedirect = quest != nullptr && quest->struct_size >= offsetof(NevrQuestHostApi, register_script_redirect) +
                                                  sizeof(quest->register_script_redirect);
  if (!hasRedirect || quest->register_script_redirect == nullptr) {
    const NevrLogField fields[] = {{"result", "no_host_api"}, {"consequence", "the combat libraries stay unredirected"}};
    Log(NEVR_LOG_ERROR, "script redirect not registered", fields, 2);
    return -1;
  }
  const bool hasId = ctx->ctx_size >= offsetof(NvrGameContext, plugin_id) + sizeof(ctx->plugin_id);
  const char* const id = hasId && ctx->plugin_id != nullptr ? ctx->plugin_id : kPluginName;
  const std::int32_t result = quest->register_script_redirect(id, kScriptRedirectHashes, static_cast<std::uint32_t>(kCount));
  const NevrLogField fields[] = {{"result", ResultText(result)}};
  if (result == NEVR_SCRIPT_REDIRECT_OK) {
    Log(NEVR_LOG_INFO, "script redirect registered", fields, 1);
  } else {
    Log(NEVR_LOG_ERROR, "script redirect not registered: no combat library is redirected", fields, 1);
  }
  return result;
}

}  // namespace q15t
