#include "runtime/patch/loading_tips.h"

#include <winsock2.h>
#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/addresses.h"
#include "runtime/hook/hook_liveness.h"
#include "runtime/hook/patching.h"

namespace nevr_loading_tips {
namespace {

using namespace nevr_patch_addresses;

constexpr std::size_t kSiteCount = 3;

// The three loading-tip script nodes, in the order of kDetours and of the liveness ids. Each row is the node's
// Enter function, the RVA the repo's addresses.h names, and the first 16 bytes the game holds there.
//
// Identities (plan E-tips 1.1): the raw decompilations of echovr.exe, matched against libr15.so's symbols,
// which name them (libr15 Enter at 0x20b08c4, 0x20b7f58, 0x20b836c). Structural matches, not PC symbols.
//   - 0x140bd9670: CR15PickLoadingTipNode::Enter. Picks a tip through LoadingTipSelect and writes the outputs.
//   - 0x140be6d10: CR15SelectLoadingTipByIndexNode::Enter. Sets a tip by the 32-bit index in data[9] (LoadingTipSet).
//   - 0x140be7c90: CR15SelectLoadingTipNode::Enter. Selects a tip (LoadingTipSelect) and writes SetInteger too.
const Site kStockSites[kSiteCount] = {
    {"CR15PickLoadingTipNode::Enter", LOADING_TIP_PICK_ENTER,
     {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}},
    {"CR15SelectLoadingTipByIndexNode::Enter", LOADING_TIP_SELECT_BY_INDEX_ENTER,
     {0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41}},
    {"CR15SelectLoadingTipNode::Enter", LOADING_TIP_SELECT_ENTER,
     {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}},
};

// Calls each detour has returned at once on this server, per site. Written with InterlockedIncrement from the
// script thread that runs the node.
volatile LONG g_hits[kSiteCount] = {0, 0, 0};

// The detour's whole job, after its liveness mark: count the call and return. The first call is an Info line
// (the node runs on this server at all); later calls are Debug with the running count. No pointer at Info.
void Skipped(std::size_t site, void* node) {
  const LONG count = InterlockedIncrement(&g_hits[site]);
  if (count == 1) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] loading tip node skipped name=%s count=1 consequence=node_returns_without_tip_outputs",
        kStockSites[site].name);
    return;
  }
  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] loading tip node skipped name=%s node=%p count=%ld",
      kStockSites[site].name, node, count);
}

// The detours. Each returns without touching `data`: the caller reads no return value (void), and the outputs the
// stock function would write are the ones the script's state 0 already put there.
void PickDetour(void* node, void* /*data*/) {
  nevr_hook_liveness::Mark(nevr_hook_liveness::kLoadingTipPick);
  Skipped(0, node);
}

void SelectByIndexDetour(void* node, void* /*data*/) {
  nevr_hook_liveness::Mark(nevr_hook_liveness::kLoadingTipSelectByIndex);
  Skipped(1, node);
}

void SelectDetour(void* node, void* /*data*/) {
  nevr_hook_liveness::Mark(nevr_hook_liveness::kLoadingTipSelect);
  Skipped(2, node);
}

using DetourFn = void (*)(void* node, void* data);
const DetourFn kDetours[kSiteCount] = {&PickDetour, &SelectByIndexDetour, &SelectDetour};

}  // namespace

std::size_t InstallSites(std::uintptr_t gameBase, const Site* sites, std::size_t count) {
  if (sites == nullptr || count > kSiteCount) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] loading tips: site table has %zu rows for %zu detours; nothing hooked", count, kSiteCount);
    return 0;
  }
  std::size_t hooked = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const Site& site = sites[i];
    const auto va = static_cast<unsigned long long>(nevr::ECHOVR_DEFAULT_IMAGE_BASE + site.rva);
    auto* target = static_cast<std::uint8_t*>(nevr::ResolveVA_Checked(gameBase, va));
    if (target == nullptr) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] loading tip hook skipped name=%s va=0x%llX reason=not_mapped; the node keeps the game's "
          "behaviour",
          site.name, va);
      continue;
    }
    if (!nevr::ValidatePrologue(target, site.prologue.data(), site.prologue.size())) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] hook failed name=%s va=0x%llX expected=%02x%02x%02x%02x actual=%02x%02x%02x%02x "
          "reason=prologue_mismatch; the node keeps the game's behaviour",
          site.name, va, site.prologue[0], site.prologue[1], site.prologue[2], site.prologue[3], target[0],
          target[1], target[2], target[3]);
      continue;
    }
    // PatchDetour reads the target from *original before it attaches, and logs its own Warning when the attach
    // fails (N126), so a failed site needs nothing more here.
    void* original = target;
    if (!PatchDetour(&original, reinterpret_cast<void*>(kDetours[i]), site.name)) continue;
    ++hooked;
    Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] loading tip hook installed name=%s va=0x%llX", site.name, va);
  }
  const std::size_t failed = count - hooked;
  Log(failed == 0 ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning,
      "[NEVR.PATCH] Disabled loading tips system for server mode hooks=%zu/%zu failed=%zu (hooked nodes return at "
      "once and choose no tip; a node not hooked keeps the game's behaviour)",
      hooked, count, failed);
  return hooked;
}

void Install(std::uintptr_t gameBase) { InstallSites(gameBase, kStockSites, kSiteCount); }

}  // namespace nevr_loading_tips
