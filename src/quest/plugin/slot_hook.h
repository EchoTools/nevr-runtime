/* The plugin hook service's host side (#2001, ADR 0019 section 5, ADR 0034): the functions behind
 * NevrQuestHostApi::register_slot_hook and NevrQuestHostApi::unregister_slot_hook.
 *
 * The service holds kSlotHookHandles handles. A handle is one GotHook, so one registry entry (hook_budget.h), and it stays
 * on its slot until its plugin unregisters it. A plugin's handler goes onto the same chain the sentinel's own hooks use
 * (got_hook.h), so a plugin's hook and a sentinel hook on one slot run as one chain, in registration order.
 *
 * Ownership: a handle belongs to the plugin that registered it (its plugin_id, recorded as the owner "plugin:<id>"). Only that
 * plugin may unregister it. HoldsHandleOf tells the loader whether a plugin still has a handler on a chain, so the loader
 * never unmaps that plugin's code (plugin_loader.cpp).
 *
 * Logging: an install writes the GOT backend's own line (event "got_hook", op "install", with the owner "plugin:<id>" and
 * the position on the chain). A refusal of the service itself (a bad argument, no free handle, a handle not held, a handle
 * of another plugin) writes one Warning, event "plugin_slot_hook", with op, status and reason. A refusal from the GOT backend
 * is logged there, with the slot, the chain and the reason, and the service returns its status.
 *
 * Built with -fno-exceptions (got_hook.h is reached from the hook path). This file writes no page protection: the page
 * writes are got_hook.cpp's (N151).
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "nevr/plugin_interface.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::slot_hook {

// The handles the service holds at once. Each one is a GotHook, declared once in slot_hook.cpp (hook_budget.h counts it).
inline constexpr std::size_t kSlotHookHandles = 8;

// register_slot_hook, with the module lookup the caller names (production: sentinel::FindLoadedImage; a test passes its
// own image). Returns a NevrSlotHookResult and writes *outHandle on NEVR_SLOT_HOOK_OK only.
std::int32_t RegisterSlotHookWith(sentinel::ImageLookup lookup, const char* pluginId, const char* module,
                                  const char* symbol, std::int32_t reloc, const char* buildId, void* handler,
                                  void** originalOut, std::uint32_t* outHandle);

// The two NevrQuestHostApi members, production (sentinel::FindLoadedImage).
std::int32_t RegisterSlotHook(const char* pluginId, const char* module, const char* symbol, std::int32_t reloc,
                              const char* buildId, void* handler, void** originalOut, std::uint32_t* outHandle);
// Takes `handle` out of its chain. Only the plugin that registered it (`pluginId`) may do so; any other caller is refused
// with NEVR_SLOT_HOOK_BAD_ARGUMENT and the handle stays where it is.
std::int32_t UnregisterSlotHook(const char* pluginId, std::uint32_t handle);

// True while a handle the service issued to `pluginId` is held: its handler may be on a chain (including a handler whose
// failed install left it reachable). The loader keeps such a plugin mapped (plugin_loader.cpp).
bool HoldsHandleOf(const char* pluginId);

}  // namespace nevr_quest::slot_hook
