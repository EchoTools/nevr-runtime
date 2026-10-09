#include "quest/integration/stage_log.h"

#include <cstring>

namespace nevr_quest::integration {

const char* StageForStep(const char* step) noexcept {
  struct Map { const char* step; const char* stage; };
  static const Map kMap[] = {
      {"resolve_config", "config_loaded"},       {"install_clock_hook", "clock_hook_installed"},
      {"start_token_auth", "token_auth_state"},  {"start_bridge", "router_listening"},
      {"install_redirect", "redirect_installed"}, {"install_social", "social_hook_installed"},
      {"install_dlopen_hook", "dlopen_hook_installed"},  {"install_login_prompt", "login_prompt_hook_installed"},
  };
  for (const Map& m : kMap) {
    if (std::strcmp(m.step, step) == 0) return m.stage;
  }
  return nullptr;
}

namespace {
bool Has(std::string_view line, std::string_view needle) { return line.find(needle) != std::string_view::npos; }
}  // namespace

StepLevel StepLogLevel(const char* state, const char* reason) noexcept {
  const std::string_view s = state != nullptr ? state : "";
  const std::string_view r = reason != nullptr ? reason : "";
  if (s == "ok") return StepLevel::kInfo;
  if (s != "skipped") return StepLevel::kError;
  if (r == "counters_refused") return StepLevel::kError;
  const std::string_view off = "_off";
  const bool featureOff = r.size() >= off.size() && r.substr(r.size() - off.size()) == off;
  if (featureOff || r == "nothing_to_install_after_load") return StepLevel::kInfo;
  return StepLevel::kWarn;
}

std::optional<StageEvent> ClassifyRouterLine(std::string_view line) noexcept {
  if (Has(line, "[remote] ") && Has(line, " connected")) return StageEvent{"router_remote_connected", "ok", "connected"};
  if (Has(line, "[remote] ") && Has(line, "connect failed")) {
    const char* cls = "connect_failed";
    if (Has(line, "tls verification failed")) cls = "tls_verification_failed";
    else if (Has(line, "tls handshake failed")) cls = "tls_error";
    else if (Has(line, "websocket upgrade rejected")) cls = "handshake_rejected";
    else if (Has(line, "network error")) cls = "network_error";
    else if (Has(line, "refused by transport policy")) cls = "policy_refused";
    return StageEvent{"router_remote_failed", "failed", cls};
  }
  if (Has(line, "[bridge] ") && Has(line, "neither an account JWT")) return StageEvent{"router_remote_failed", "failed", "no_jwt"};
  if (Has(line, "[remote] ") && Has(line, "not started: no connect request"))
    return StageEvent{"router_remote_failed", "failed", "no_connect_request"};
  if (Has(line, "[remote] ") && Has(line, "refused: the remote URL"))
    return StageEvent{"router_remote_failed", "failed", "remote_url_not_wss"};
  if (Has(line, "[router] LOGIN SUCCESS")) return StageEvent{"login_accepted", "ok", "login_success"};
  if (Has(line, "[router] LOGIN FAILURE")) return StageEvent{"login_refused", "failed", "login_failure"};
  return std::nullopt;
}

}  // namespace nevr_quest::integration
