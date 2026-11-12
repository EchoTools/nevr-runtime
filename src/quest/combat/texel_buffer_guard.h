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
 * Arena (same APK). It acts in every level: README rule 9's exception for a default for a value the API leaves undefined until written. The API
 * rule: Vulkan leaves a descriptor of a newly allocated set undefined until vkUpdateDescriptorSets writes it, and a draw may only use valid
 * descriptors (the specification's descriptor-set and draw valid-usage rules; quoted from memory, not re-read in this tree). A view is written
 * only into a binding of a set the game has just allocated, and the game's own write replaces it. The game may read a binding before it writes
 * it (the fault above is exactly that), so the exception does not rest on the write coming first: the guard's only effect is that a read of a
 * binding the game has not written, or never writes, gets zeros instead of whatever the driver left there (possibly a stale descriptor from a
 * recycled pool); a read of a binding the game wrote sees what the game wrote. In arena that is a change for such a read, from undefined to
 * zeros, which the API's rule makes acceptable. Cost per allocation: one driver call and a
 * table look-up per set under one mutex; per layout created: one walk of its bindings. Not per frame unless the game allocates sets per frame
 * (not established: the count line at powers of two shows how often). Each call of either wrapper, in every level and also after the guard is
 * off, first runs ServesDevice: a compare-exchange on the first-device slot (a read-modify-write on one shared cache line). A plain load before
 * it would skip the store once the device is taken; that is not made, because no test can observe the store. Calls per frame: not measured.
 *
 * Which APKs (ADR 0046 rule 5). The combat variants: the early step of combat_install.h registers the request, before
 * install_vk_ext_window and before the game's first Vulkan lookup. Switch: `combat.texel_buffer_guard`, default true (the reference build
 * ships it on). With the key false, or in an APK where combat does not run, nothing is requested and the game's lookups are unchanged.
 *
 * Owner (ADR 0039). vk_ext_window owns the lookups; this file owns what the two wrappers do.
 *
 * One device. vk_ext_window's vkGetDeviceProcAddr wrapper overwrites the published real function on every lookup, so with a second VkDevice
 * the real function would be the last device's lookup and the first device's calls would reach it. The pinned fill passes one device
 * (`ldr x0,[x29,#0xb50]`, word 0xf945aba0, at ELF 0x18de8a4 and 0x18de930 in the store libr15, read from the file; not a pinned word in texel_buffer_guard_targets.h). The guard serves the first non-null device a wrapper is
 * called with; a call with another device turns the guard off for the process (one Warning, combat_texel_buffer_guard_second_device), after
 * which every call is the driver's. It cannot repair the real function the lookup published; it stops writing.
 *
 * Origin. The `g40guard` switch of kotorvr's echoquest-combat: its afk_gpuprobe.cpp `fillsets` path restricted to uniform texel buffers
 * (NoteFill, FillMake, FillNew, H_CreateDescriptorSetLayout, H_AllocateDescriptorSets), read as text and rebuilt on vk_ext_window's lookup.
 * Upstream rewrites the table words in libr15's data; this tree has no operation for those words (got_hook.h, N151), so the guard is
 * handed out by the lookup that fills them.
 *
 * Lines (sentinel log):
 *   install   {"event":"combat_texel_buffer_guard_install","result":"requested|off_config|refused","reason":"ok|off_config|<request status>",
 *             "names":"vkCreateDescriptorSetLayout vkAllocateDescriptorSets","owner":"vk_ext_window","msg":".."}  Info, Warning for a refusal
 *             (the guard is off and the game runs)
 *   zero view {"event":"combat_texel_buffer_guard_zero_view","result":"made|failed","step":"..","vk_result":<n>,"bytes":<n>,"memory_type":<n>,
 *             "msg":".."}  Info when made, Warning when a step failed (the guard is then off for the process); `vk_result` is the driver's
 *             answer and is absent for a step that failed without a driver call (`entry_points`, `memory_type`)
 *   second device {"event":"combat_texel_buffer_guard_second_device","entry":"<wrapper>","sets":<n>,"msg":".."}  Warning, once: the guard is off
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
  std::uint64_t otherDevice = 0;      // wrapper calls with a device other than the first (the first of them turned the guard off)
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
