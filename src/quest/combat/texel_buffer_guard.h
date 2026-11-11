/* A new descriptor set's uniform texel buffer bindings hold a zeroed view before the game sees the set (#1650, #1737;
 * docs/quest/combat/texel_buffer_guard.md).
 *
 * The fault this answers. The working combat build's notes record a GPU read fault on the Quest (UCHE, address 0x1FF0000000000) that they
 * trace to a draw reading a uniform texel buffer binding before a sublevel's stream-in writes it (the light-UV texel buffers of one vertex
 * shader, set 0 bindings 3 and 4). A binding the game has not written yet holds no view; a read of it reads an address the GPU cannot map.
 * Not established here: that the fault happens on the store-based combat APK at all (the headset row in the doc asks), and which shader
 * reads which binding (texel_buffer_guard_targets.h, "Not established").
 *
 * What this does. Two device functions, wrapped through vk_ext_window (the owner of the game's Vulkan lookups, ADR 0039; this module is an
 * input to it, RequestDeviceFunction): libr15 fills its function table from vkGetDeviceProcAddr in CGS::Initialize, so the game's
 * vkCreateDescriptorSetLayout and vkAllocateDescriptorSets are these wrappers (texel_buffer_guard_targets.h pins the stores).
 *   - vkCreateDescriptorSetLayout calls the driver and, on success, records the layout's uniform texel bindings (texel_buffer_guard_rules.h,
 *     PickBindings: type VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, at least one descriptor; a layout with a binding-flags structure is left
 *     alone).
 *   - vkAllocateDescriptorSets calls the driver and, on VK_SUCCESS, writes one zeroed uniform texel buffer view into each recorded binding of
 *     each new set, with one driver vkUpdateDescriptorSets call (more when the writes outgrow kWriteBatch), then returns the driver's result
 *     unchanged. A failed allocation is returned untouched with no write.
 *   The game's own writes replace the view, so the guard decides only what a binding reads before the game has written it: zeros, not an
 *   unmapped address. The zeroed buffer (64 KiB, host-visible and host-coherent, zeroed once through its mapping, view format
 *   R32G32B32A32_SFLOAT) is made once, with the game's device, the first time a new set has a binding to write. Any step that fails turns
 *   the guard off for the process (one Warning names the step and the driver's result) and every later call is the driver's, unchanged.
 *
 * Arena (same APK). It acts in every level: README rule 9's exception for a default the game replaces before any read. A view is written
 * only into a binding the game has not written yet, in a set the game has just allocated, and the game's own write replaces it; the game
 * never reads what the guard wrote except where it would otherwise read an unwritten binding. Cost per allocation: one driver call and a
 * table look-up per set under one mutex; per layout created: one walk of its bindings. Not per frame unless the game allocates sets per frame
 * (not established: the count line at powers of two shows how often).
 *
 * Which APKs (ADR 0046 rule 5). The q15t and combat variants: the early step of combat_install.h registers the request, before
 * install_vk_ext_window and before the game's first Vulkan lookup. Switch: `combat.texel_buffer_guard`, default true (the reference build
 * ships it on). With the key false, or in an APK where combat does not run, nothing is requested and the game's lookups are unchanged.
 *
 * Owner (ADR 0039). vk_ext_window owns the lookups; this file owns what the two wrappers do.
 *
 * Origin. The `g40guard` switch of kotorvr's echoquest-combat: its afk_gpuprobe.cpp `fillsets` path restricted to uniform texel buffers
 * (NoteFill, FillMake, FillNew, H_CreateDescriptorSetLayout, H_AllocateDescriptorSets), read as text and rebuilt on vk_ext_window's lookup.
 * Upstream rewrites the table words in libr15's data; this tree has no operation for those words (got_hook.h, N151), so the guard is
 * handed out by the lookup that fills them.
 *
 * Lines (sentinel log):
 *   install   {"event":"combat_texel_buffer_guard_install","result":"requested|off_config|refused","reason":"ok|off_config|<request status>",
 *             "names":"vkCreateDescriptorSetLayout vkAllocateDescriptorSets","owner":"vk_ext_window","msg":".."}  Info, Error for a refusal
 *   zero view {"event":"combat_texel_buffer_guard_zero_view","result":"made|failed","step":"..","vk_result":<n>,"bytes":<n>,"memory_type":<n>,
 *             "msg":".."}  Info when made, Warning when a step failed (the guard is then off for the process)
 *   first     {"event":"combat_texel_buffer_guard_first","sets":<n>,"bindings":<n>,"msg":".."}  Info, the first allocation the guard wrote into
 *   count     {"event":"combat_texel_buffer_guard_count","sets":..,"bindings":..,"unknown_layouts":..,"skipped_flags":..,"other_device":..}
 *             Debug, each time the sets written reach a power of two
 *   full      {"event":"combat_texel_buffer_guard_table_full","capacity":1024,"msg":".."}  Warning, once
 *   end       {"event":"combat_texel_buffer_guard_end","wrapped":0|1,"layouts":..,"sets":..,"bindings":..,"unknown_layouts":..,
 *             "skipped_flags":..,"other_device":..,"table_full":..,"msg":".."}  Info, Warning when requested and never handed out (wrapped 0)
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/texel_buffer_guard_rules.h"

namespace nevr_quest::combat::texel_buffer_guard {

inline constexpr const char* kCreateLayoutName = "vkCreateDescriptorSetLayout";
inline constexpr const char* kAllocateName = "vkAllocateDescriptorSets";
inline constexpr std::uint32_t kWriteBatch = 32;  // writes per driver vkUpdateDescriptorSets call

// The early step (combat_install.cpp): `on` is `combat.texel_buffer_guard`. Registers the two wrappers with vk_ext_window.
ModuleResult Early(bool on) noexcept;
// The end-of-process step (the row's `end`): the end line, when the wrappers were requested.
void End() noexcept;

// The two wrappers, as vk_ext_window hands them to the game.
vk::Result CreateDescriptorSetLayout(void* device, const vk::DescriptorSetLayoutCreateInfo* info, const void* allocator,
                                     vk::Handle* layout) noexcept;
vk::Result AllocateDescriptorSets(void* device, const vk::DescriptorSetAllocateInfo* info, vk::Handle* sets) noexcept;

struct Counts {
  std::uint64_t layouts = 0;          // layouts recorded with a uniform texel binding
  std::uint64_t sets = 0;             // new sets the guard wrote into
  std::uint64_t bindings = 0;         // writes made (one per recorded binding of each such set)
  std::uint64_t unknownLayouts = 0;   // new sets whose layout the table has no entry for (no uniform texel binding, or not recorded)
  std::uint64_t skippedFlags = 0;     // new sets whose layout was left alone (binding flags, or more bindings than the guard writes)
  std::uint64_t otherDevice = 0;      // allocations on a device other than the one the zeroed view was made with
  std::uint64_t tableFull = 0;        // layouts not recorded because the table was full
};
Counts CurrentCounts() noexcept;
// Whether the zeroed view exists (made), and whether the guard turned itself off (a step failed).
bool ZeroViewMade() noexcept;
bool TurnedOff() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------
// Forgets the table, the zeroed view, the counts and the lines' once-only state, and the real functions.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::texel_buffer_guard
