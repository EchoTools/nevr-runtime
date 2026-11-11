/* The combat area's script redirect (#2189, #1737, docs/adr/0046): the combat APK carries its own build of nine stock script libraries,
 * each named lib<hash>_q15t.so beside the untouched stock lib<hash>.so (script_redirect_list.h), and the game's loads of those nine names
 * open the combat builds. This module asks the host's registry for that redirect (quest/plugin/script_redirect.h, Register), with owner
 * `combat`. The registry registers all nine or none: each redirected file must be in the directory the sentinel was loaded from.
 *
 * When. The combat area's normal install step (combat_install.h), first of its rows. That step runs on the thread that ran the plugin pass,
 * right after the pass returned, at the first non-null dlopen after the sentinel's login and matchmaking installs (integration/post_load.h):
 * the moment a plugin's init would run, so a script load that the plugin pass or a later row causes is already redirected. A script-library load before it is not redirected; its
 * `script_load` line (registry_set=false) shows it. Which APKs: the q15t and combat variants only (combat::Active()); a plain release never
 * reaches the step, registers nothing, and every script loads under its own name.
 *
 * Switch. None: an APK that carries the nine combat builds needs them loaded, so the redirect is not a player's choice.
 *
 * Line (sentinel log), one per process, from the step:
 *   {"event":"combat_script_redirect_install","result":"registered|already_registered|refused","count":9,
 *    "host_result":"<NevrScriptRedirectResult name>","msg":".."}
 * Info when registered. Warning when the registry already holds another owner's registration (`already_armed`: one per process; that one
 * stays in force and redirects the loads it names). Error for every other refusal (nothing is registered, so combat plays with the stock
 * scripts). The host writes its own registry lines beside it.
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstdint>

#include "nevr/plugin_interface.h"
#include "quest/combat/combat_install.h"

namespace nevr_quest::combat::script_redirect {

// The host registry's registration call (quest/plugin/script_redirect.h, Register): owner, hashes, count.
using RegisterFn = NevrScriptRedirectResult (*)(const char* owner, const std::uint64_t* hashes, std::uint32_t count) noexcept;

// The module's normal step against `reg` (the host test passes a fake): registers script_redirect_list.h's nine hashes with owner `combat`
// and writes the line. Installed when the registry answers OK; otherwise not installed, with the registry's result name as the status.
ModuleResult Install(RegisterFn reg) noexcept;

// The same against the host registry (the shipped row).
ModuleResult Install() noexcept;

}  // namespace nevr_quest::combat::script_redirect
