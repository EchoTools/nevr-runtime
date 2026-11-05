#include <atomic>
#include <thread>

#include "quest/cargo/cargo_service.h"
#include "quest/cargo/posix_env.h"
#include "quest/data/curl_transport.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/resource_request_hook.h"

// The production entry of the CDN texture delivery: the libcurl transport with the data host's roots, the POSIX env, the resource
// request hook's publish and one detached thread. Kept apart from cargo_service.cpp so the host tests of the service need no libcurl.
namespace nevr_quest::cargo {

namespace {

std::atomic<bool> g_started{false};

struct State {
  nevr_quest::data::CurlTransport transport;
  PosixEnv env;
  nevr_cargo::RunConfig config;
  State() : env(&transport) {}
};

}  // namespace

bool Start(const AssetsChoice& assets) noexcept {
  if (g_started.exchange(true)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "cargo_start", {{"result", "refused"}, {"reason", "already_started"}});
    return false;
  }
  // Never freed: the thread below and the published sets outlive every caller.
  State* const state = new State();
  state->config = MakeRunConfig(assets);
  if (!state->env.MakeDirs(state->config.dir)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "cargo_start",
                        {{"result", "off"},
                         {"reason", "no_cache_dir"},
                         {"consequence", "no CDN texture is served this run; the stock textures show"}});
    return true;
  }
  const nevr_cargo::Publish give = [](std::unique_ptr<nevr_cargo::Mounted> set) {
    nevr_quest::resource_request::PublishCargo(set.release());
  };
  nevr_cargo::MountFromCache(state->env, state->config, give);
  std::thread([state, give]() {
    nevr_cargo::FetchAndMount(state->env, state->config, give, [] { return true; });
  }).detach();
  return true;
}

}  // namespace nevr_quest::cargo
