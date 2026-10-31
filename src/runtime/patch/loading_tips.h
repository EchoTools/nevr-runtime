#pragma once

// The loading-tip script nodes on a server (#1774). Each node's Enter function is hooked at its entry; the detour
// returns at once, so the node does nothing and the script thread moves on. The sites, their bytes and the
// evidence for each identity are in loading_tips.cpp.

#include <array>
#include <cstddef>
#include <cstdint>

namespace nevr_loading_tips {

/// One hook site: a name for the logs, the function's RVA in echovr.exe, and the first 16 bytes the game must hold
/// there. A site whose bytes differ is refused.
struct Site {
  const char* name;
  std::uint32_t rva;
  std::array<std::uint8_t, 16> prologue;
};

/// Hooks each site whose bytes match, in table order, and returns how many were hooked. A site that is not mapped,
/// whose bytes differ, or whose hook fails is logged at Warning and left alone; the others are still hooked.
/// `gameBase` is the image base the RVAs are relative to. Logs one aggregate line for the table.
std::size_t InstallSites(std::uintptr_t gameBase, const Site* sites, std::size_t count);

/// Server only. Installs the three loading-tip hooks of the stock echovr.exe. Call from the server boot path.
void Install(std::uintptr_t gameBase);

}  // namespace nevr_loading_tips
