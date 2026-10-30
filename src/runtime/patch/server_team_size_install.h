#pragma once

// #1422: puts network.max_team_size in effect at ServerAssignTeam's two sites (server_team_size.h), once, from
// RunDeferredRuntimeBootstrap (boot.cpp) in both modes, after the config is loaded. A client logs that it does not
// apply (N122/N124). One [NEVR.PATCH] line gives the outcome (InstallLines): Info when the size wanted is in effect,
// Warning with each site's bytes when it is not.

namespace nevr_server_team_size {

void Install(bool isServer);

}  // namespace nevr_server_team_size
