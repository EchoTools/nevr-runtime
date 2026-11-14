// The PC's CDN start (docs/adr/0045-cdn-texture-delivery.md). Client only: the caller gates it, because a server serves no asset and
// fetches nothing.
#pragma once

namespace nevr_cdn {
struct Mounted;
}

namespace nevr_cdn_start {

// Starts the CDN for this run: the cache is <nevr root>\cache\cdn, the mounted sets are published as they are ready, and one background
// thread does the fetch. False when no start happened (no nevr root, or a start already ran in this process); it logs why.
bool Start() noexcept;

// The last mounted set that Start published, or null before the first one. The PC read hook (the detour on
// CResourceManager::FillResourceFileRequest in patch/plugin_assets.cpp) will read it; until that hook exists nothing serves it.
const nevr_cdn::Mounted* Published() noexcept;

}  // namespace nevr_cdn_start
