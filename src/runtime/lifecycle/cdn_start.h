// The PC's CDN start (docs/adr/0045-cdn-texture-delivery.md). Start is the client's and StartServer the dedicated server's: the caller gates
// them. A dedicated server also resolves each session's demand through ResolveDemandForSession (PLAN-v5 R6-a-2b), when the session names
// its level and mode.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace nevr_cdn {
struct Mounted;
}

namespace nevr_cdn_start {

// Starts the CDN for this run: the cache is <nevr root>\cache\cdn, the mounted sets are published as they are ready, and one background
// thread does the fetch. False when no start happened (no nevr root, or a start already ran in this process); it logs why.
bool Start() noexcept;

// Starts the CDN for a dedicated server (nevr_cdn::StartServer, PLAN-v7 5.4 "Server"), on the calling thread: the EchoCombat set is fetched
// before the one boot publish (at most nevr_cdn::kServerBootFetchMs), and the server's cargo pin is the boot set's pin (nevr_cargo_pin::
// BindServerFeed). No background thread. False when no start happened (no nevr root, or a server start already ran); it logs why.
bool StartServer() noexcept;

// The last mounted set that Start, StartServer or a session's demand resolve published, or null before the first one. Each published shadow is also handed to the PC read hook
// (the detour on CResourceManager::FillResourceFileRequest in patch/plugin_assets.cpp, PublishCdnShadow), which fills a request whose
// key the shadow holds from it through the shared nevr_cdn::Resolver, as the Quest's does.
const nevr_cdn::Mounted* Published() noexcept;

// The outcome of one session's demand resolve on a dedicated server. `reason` is the demand's token when !ok (the core's, or
// "no_nevr_root" when the nEVR root cannot be formed).
struct SessionDemandOutcome {
  bool ok = false;
  std::string reason;
};

// Resolves the objects of one session's level and game type (nevr_cdn::ResolveDemand, role server, the manifest path with the v2 to v1
// fallback). A set is published through the read hook only when it is complete; the announcer's callouts are not fed from a server's
// sets (announcer_callouts.h). `keep_going` returns false when the caller no longer wants the result.
SessionDemandOutcome ResolveDemandForSession(std::uint64_t level, std::string_view game_type, const std::function<bool()>& keep_going);

}  // namespace nevr_cdn_start
