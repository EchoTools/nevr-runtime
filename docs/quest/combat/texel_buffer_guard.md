# Module `texel_buffer_guard` (#1650, #1737)

Files: `src/quest/combat/texel_buffer_guard.{h,cpp}`, `src/quest/combat/texel_buffer_guard_rules.h` (the rules and the guard's own Vulkan types),
`src/quest/combat/texel_buffer_guard_targets.h` (the pins), `src/quest/tests/texel_buffer_guard_test.cpp`,
`src/quest/tests/texel_buffer_guard_pinned_test.cpp`, `src/quest/combat/mutants.d/texel_buffer_guard.sh`, and the device-function requests of
`src/quest/sentinel/vk_ext_window.{h,cpp}` (tested in `src/quest/tests/vk_ext_window_test.cpp`). An early-phase row of the combat install table
(`src/quest/combat/combat_install.h`), after `memsys`: it runs in the `q15t` and `combat` APKs and in no other.

## Origin

The `g40guard` switch of kotorvr's echoquest-combat: its afk_gpuprobe.cpp `fillsets` path, restricted to uniform texel buffers (`NoteFill`,
`FillMake`, `FillNew` and the two hooks that call them), read as text. What is taken is the behaviour: each new descriptor set's uniform texel buffer
bindings hold a zeroed view before the game sees the set, so a binding the game has not written yet reads zeros. The mechanism is this tree's.
Upstream rewrites the function-table words in libr15's data from a thread; this tree has no operation that writes those words (they are `.bss`
words no relocation names, holding driver code pointers: `got_hook.h`, N151), so the guard's wrappers are handed to the game by the lookup that
fills the table, which `vk_ext_window` already owns (ADR 0039: the guard is an input to that owner).

## Switch and default

`combat.texel_buffer_guard` in `config.yaml`, a boolean, default **true** (the reference build ships `g40guard.on`). A value that is not a boolean
keeps the default, with a Warning that names the key. With the key false, or in an APK where combat does not run, nothing is requested and the
game's Vulkan lookups are exactly what they were before this module existed.

## What it hooks

Nothing of the game's code. libr15 imports no Vulkan function: `CGS::Initialize` gets `vkGetInstanceProcAddr` through `CSysModule::FindMethod`
(the slot `vk_ext_window` hooks on every launch), stores it in its table's word 0, asks it for `vkGetDeviceProcAddr`, stores that in word 0x88, and
fills each device slot by asking that for the function's name (`texel_buffer_guard_targets.h` pins every word of that chain, the strings it names,
and the stub that reaches FindMethod's JUMP_SLOT, against the store file).

The early step registers two requests with `vk_ext_window` (`RequestDeviceFunction`): `vkCreateDescriptorSetLayout` and
`vkAllocateDescriptorSets`. With a request registered, `vk_ext_window`'s `vkGetInstanceProcAddr` wrapper answers `vkGetDeviceProcAddr` with its own
wrapper, which answers each requested name with the requester's wrapper (after publishing the driver's function for it) and every other name with
the driver's. So the game's slots 0x290 and 0x2b8 hold the guard's two functions, and every other slot holds what it always held. The step runs
before `install_vk_ext_window` and before the game's first Vulkan lookup; a request that arrives after the game has asked for
`vkGetDeviceProcAddr` is refused (`sealed`) and logged.

The two wrappers:

- `vkCreateDescriptorSetLayout` calls the driver. On success it records the layout's uniform texel bindings (`PickBindings`: type
  `VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER`, at least one descriptor; up to 4 such bindings of up to 64 descriptors each). A layout with a
  binding-flags structure in its chain (a variable count or update-after-bind) is recorded as left alone, as is one with more or larger bindings.
  A handle the driver hands out again replaces its entry, also when the new layout has no such binding, so no write is ever made for a layout that
  no longer has one. The table holds 1024 layouts that have a uniform texel binding.
- `vkAllocateDescriptorSets` calls the driver. On `VK_SUCCESS` it writes the zeroed view into every recorded binding of every new set (descriptor
  count as declared, array element 0), in driver `vkUpdateDescriptorSets` calls of up to 32 writes, then returns the driver's result unchanged. A
  failed allocation is returned untouched and nothing is written.

The zeroed view is made once, with the game's device, the first time a new set has a binding to write: a 64 KiB buffer with
`VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT`, its memory in the first type the buffer allows that is host-visible and host-coherent, zeroed once
through its mapping and left mapped, and a `VK_FORMAT_R32G32B32A32_SFLOAT` view of the whole buffer (a format every Vulkan device supports for
uniform texel buffers). The memory types are read from the instance's physical device; an instance with more than one is not guessed at. Any step
that fails turns the guard off for the process: one Warning names the step and the driver's result, and every later call is the driver's,
unchanged. Nothing the guard makes is destroyed (it lives as long as the process).

## One device

`vk_ext_window`'s `vkGetDeviceProcAddr` wrapper publishes the driver's function for a requested name on every lookup (it overwrites the word each
time). With a second `VkDevice` the published function would be the last lookup's, so the first device's calls would reach another device's
function. The pinned fill passes one device: both `vkGetDeviceProcAddr` calls in `CGS::Initialize` load `x0` from `[x29,#0xb50]` (`ldr x0,[x29,#0xb50]`, word 0xf945aba0, at
ELF 0x18de8a4 and 0x18de930 in the store libr15, read from the file; not a pinned word in `texel_buffer_guard_targets.h`). The guard serves the first non-null device either wrapper is called with. A call with
another device turns the guard off for the process: one Warning (`combat_texel_buffer_guard_second_device`), nothing is written from then on and
every call is the driver's. The guard cannot repair the function the lookup published; it stops writing views and recording layouts. A null device
is passed to the driver and neither served nor counted as a second device.

## Log lines (sentinel log)

- `combat_texel_buffer_guard_install`: `result` (`requested|off_config|refused`), `reason` (`ok`, `off_config`, or the request status: `sealed`,
  `full`, `duplicate`, `null`), `names`, `owner` `vk_ext_window`, `msg`. Info, Warning for a refusal (the guard is off and the game runs).
- `vk_ext_window_device_lookup` (written by `vk_ext_window`): once, when the game first asks for `vkGetDeviceProcAddr`: `result` (`wrapped` with a
  request, `real` without), `requests`.
- `vk_ext_window_device_function` (written by `vk_ext_window`): Info, once per requested name the first time the game takes it: `name`,
  `requester`.
- `combat_texel_buffer_guard_zero_view`: `result` (`made|failed`), `step` (`done`, or the failed step: `entry_points`, `physical_device`,
  `create_buffer`, `memory_type`, `allocate_memory`, `bind_memory`, `map_memory`, `create_view`), `vk_result`, `bytes`, `memory_type`. Info when
  made, Warning when a step failed. `vk_result` is the driver's answer; a step that failed without a driver call (`entry_points`, `memory_type`)
  has none and the line omits the field.
- `combat_texel_buffer_guard_first`: Info, once: `sets` and `bindings` of the first allocation the guard wrote into.
- `combat_texel_buffer_guard_count`: Debug, each time the sets written reach a power of two: `sets`, `bindings`, `unknown_layouts` (new sets whose
  layout the table has no entry for: no uniform texel binding, or not recorded), `skipped_flags` (layouts left alone), `other_device` (wrapper calls with a device other than the first).
- `combat_texel_buffer_guard_second_device`: Warning, once: `entry` (the wrapper a second device called), `sets` (written until then). The guard is
  off for the process.
- `combat_texel_buffer_guard_table_full`: Warning, once: `capacity`.
- `combat_texel_buffer_guard_end`: at the end of the process (the row's end step), when the requests were registered: `wrapped` (1 when the game
  took both wrappers), the counts above and `layouts`, `table_full`. Info, Warning when `wrapped` is 0.

## Arena cost and arena behaviour (arena and combat share the APK)

- **Behaviour.** The guard acts in every level: README rule 9's exception for a default for a value the API leaves undefined until written.
  The API rule: Vulkan leaves a descriptor of a newly allocated set undefined until `vkUpdateDescriptorSets` writes it, and a draw may only use valid
  descriptors (the specification's descriptor-set and draw valid-usage rules; quoted from memory, not re-read in this tree). The view is written
  only into a binding of a set the game has just allocated; the game's own write replaces it. The game may read a binding before it writes it (that is
  the fault the reference build records), so the exception does not rest on the write coming first. The only effect: a read of a binding the game
  has not written yet, or never writes, gets zeros instead of whatever the driver left there (possibly a stale descriptor from a recycled pool).
  A read of a binding the game wrote sees what the game wrote, as before. In arena that is a behaviour change for such a read, from undefined to
  zeros; the specification leaves the read undefined, so this is acceptable. A gate on the level answer (`scope::SeenCombatOrLobby()`) is not used: the layouts are
  created at load, before any level answer exists, so a gated guard would still record them in every level, and when the fault happens against
  the level answer is not measured, so a gate that lags a level change could leave it in place.
- **Cost.** Per layout created: one walk of its bindings and one table write under a mutex. Per allocation: the driver call, one table look-up per
  set under the mutex, and one driver `vkUpdateDescriptorSets` call when a set has a uniform texel binding. Once per process: the buffer, its
  memory and its view (64 KiB of host-visible memory). How often the game allocates sets (at load only, or per frame) is not established: the
  Debug count line at powers of two and the end line's `sets` give the rate. Not measured on a headset.

## Not shown

- That the fault (UCHE, 0x1FF0000000000) happens on the store-based combat APK at all: the reference build enables its guard only under its own
  runtime, and the fault is recorded in its notes, not in a log of this tree.
- Which shader reads which uniform texel binding (the reference notes name set 0 bindings 3 and 4 of vertex shader 407e0930); nothing here reads
  shaders.
- That nothing but `CGS::Initialize`'s fill writes slots 0x290 and 0x2b8 (CGS::Initialize and InitCGS are not reconstructed).
- How often the game allocates descriptor sets, and so the guard's cost per frame.
- That a set the game allocates and never writes is ever bound by a draw (the guard's purpose rests on the reference build's reading of the fault).

## Headset row

Quest 3 and Quest 2, combat APK. First with `combat.texel_buffer_guard: false`, Dyson then Fission, two matches each: is the UCHE fault
(0x1FF0000000000) in logcat at all? Then with the key absent (true): no fault; `combat_texel_buffer_guard_install` `requested`,
`vk_ext_window_device_lookup` `wrapped`, two `vk_ext_window_device_function` lines, `combat_texel_buffer_guard_zero_view` `made`,
`combat_texel_buffer_guard_first`, and `combat_texel_buffer_guard_end` with `wrapped` 1 and its counts; an arena match's frame time against the
last release.

## Platform

PC: not applicable. The PC game renders through Direct3D 12 (`src/runtime/hook/addresses.h`, the D3D12 device-init gate), and the PC runtime
names no Vulkan entry point (`git grep vkCreate -- src/runtime` matches nothing).
