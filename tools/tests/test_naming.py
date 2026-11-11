"""docs/standards/naming.md, enforced: `Nevr` is the prefix of the plugin/module C ABI and `Nvr` survives only as
deprecated aliases (and the places that prove the old names still load), and a non-canonical spelling of the
project name does not appear in any file or identifier that does not already carry it. The places `Nvr` may
appear are written out below on purpose: changing one is a visible edit here."""
from __future__ import annotations

import json
import pathlib
import re
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
# `Nvr` followed by a capital, not preceded by a letter; the _fn/_t typedef suffixes are folded away. A
# deprecated alias is `Nvr<Rest>` for the canonical `Nevr<Rest>`.
NVR = re.compile(r"(?<![A-Za-z])Nvr[A-Z][A-Za-z0-9_]*")
# Any spelling of the name made of the letters n-e-v-r in some case, not preceded by a letter.
ANY_CASE = re.compile(r"(?<![A-Za-z])[Nn][Ee][Vv][Rr][A-Za-z0-9_]*")
CANONICAL = re.compile(r"(NEVR|Nevr|nevr)")
# The brand is spelled nEVR in prose (docs/standards/naming.md); these project-name phrasings in the prose of a
# document are the uppercase spelling.
UPPERCASE_BRAND_IN_DOCS = re.compile(
    r"(?<![A-Za-z])NEVR(?: Runtime| project|['’]s| refuses| subsystem|-authored)\b"
)
# These files define the anti-patterns themselves.
SELF = frozenset({"docs/standards/naming.md", "tools/tests/test_naming.py"})
EXCLUDED_PREFIXES = ("extern/", "gen/", "docs/audits/")

# The headers that publish the plugin/module C ABI. Each `Nvr*` name in them is a deprecated alias of the `Nevr*`
# name with the same rest, declared `[[deprecated("use Nevr...")]]`, so code built against the old SDK compiles
# with a warning. The deprecated export spelling (`NvrPluginInit`, ...) is not declared in a header: a plugin
# built before the rename exports it, and the loaders resolve it after the Nevr one.
ALIAS_HEADERS = ("sdk/include/nevr/plugin_interface.h", "src/extension/module_interface.h")
# Every other place a deprecated `Nvr*` name appears, with the names: the loaders name the export they fall
# back to in a failure message, the fixtures export or resolve the old names to prove a plugin built before the
# rename still loads, and one invariant test forbids either spelling in `DllMain`. Nothing is added for a name
# we choose; an entry goes when the name leaves the file.
DEPRECATED_NVR_USES = {
    "src/runtime/ext/plugin_loader.cpp": {"NvrPluginGetInfo"},
    "src/runtime/tests/plugin_deprecated_exports_test_dll.cpp": {
        "NvrPluginGetApiVersion", "NvrPluginGetCapabilities", "NvrPluginGetInfo", "NvrPluginInitEx",
        "NvrPluginOnFrame", "NvrPluginOnGameStateChange", "NvrPluginShutdown", "NvrTestPluginExportsGetCount",
    },
    "src/runtime/tests/test_behavioral.cpp": {"NvrPluginGetCapabilities", "NvrTestPluginExportsGetCount"},
    "src/quest/tests/plugin_loader_test.cpp": {"NvrPluginGetInfo", "NvrPluginInitEx", "NvrPluginShutdown"},
    # The decision record names the old spellings it deprecates.
    "docs/adr/0043-nevr-is-the-plugin-abi-prefix.md": {
        "NvrGameContext", "NvrModuleApiVersionSupported", "NvrModuleContext", "NvrPluginInfo", "NvrPluginInit",
    },
    "tools/tests/test_runtime_lifecycle_invariants.py": {"NvrPluginShutdown"},
    # he_is_the_cat's EchoCombat plugins, ported nearly as he wrote them (#2242): his comments name the plugin entry points.
    "src/runtime/combat/dronefix.cpp": {"NvrPluginInitEx", "NvrPluginShutdown"},
    "src/runtime/combat/powerboost.cpp": {"NvrPluginInitEx", "NvrPluginShutdown"},
    "src/runtime/combat/veilnet.cpp": {"NvrPluginInitEx", "NvrPluginShutdown"},
}
# `using NvrX [[deprecated("use NevrX")]] = NevrX;` and the one deprecated function.
ALIAS_DECL = re.compile(r"using\s+(Nvr\w+)\s*\[\[deprecated\(\"use (Nevr\w+)\"\)\]\]\s*=\s*(Nevr\w+)\s*;")
FUNCTION_ALIAS_DECL = re.compile(r"\[\[deprecated\(\"use (Nevr\w+)\"\)\]\]\s*constexpr\s+\w+\s+(Nvr\w+)\s*\(")
# Plugins other people publish keep their own names (docs/standards/naming.md, "Names that belong to ... third
# parties"). These are named in config examples and docs because a host lists them in config.yaml. Each is one
# third party's file name; nothing here is ours, and nothing is added for a name we choose.
THIRD_PARTY_NVR = frozenset({"NvrContentOverlay"})
# Namespaces that were renamed to nevr_<area> (#131), one JSON file per area under
# tools/tests/renamed_namespaces/ so area PRs do not edit the same lines. The old name is not used as a
# namespace again; a PascalCase namespace in no file is still on the rename list
# (`python3 tools/naming_inventory.py --category pascal-namespaces`).
RENAMED_NAMESPACES = {}
for _area_file in sorted((REPO / "tools/tests/renamed_namespaces").glob("*.json")):
    RENAMED_NAMESPACES.update(json.loads(_area_file.read_text(encoding="utf-8")))
# Existing non-canonical spellings outside prose, per file and exact token. The map only shrinks.
LEGACY_SPELLINGS = {
    "certs/code-signing.conf": {"nEVR"},
    "certs/generate-ca.sh": {"nEVR"},
    "certs/intermediate-ca.conf": {"nEVR"},
    "certs/root-ca.conf": {"nEVR"},
    "cmake/codesign/sign.sh": {"nEVR"},
}
# Documents and comments whose prose spells the brand `nEVR`, per file and exact token.
BRAND_PROSE_SPELLINGS = {
    "AGENTS.md": {"nEVR"},
    "CONTRIBUTING.md": {"nEVR"},
    "README.md": {"nEVR"},
    "docs/README.md": {"nEVR"},
    "RELEASING.md": {"nEVR"},
    "tools/package-release/RELEASE-BODY.template.md": {"nEVR"},
    "tools/tests/test_package_release.py": {"nEVR"},
    # he_is_the_cat's EchoCombat plugins, ported nearly as he wrote them (#2242): his comments and the port's markers are prose.
    "src/runtime/combat/dronefix.cpp": {"nEVR"},
    "src/runtime/combat/powerboost.cpp": {"nEVR"},
    "src/runtime/combat/veilnet.cpp": {"nEVR"},
    "docs/engine/asset-system.md": {"nEVR"},
    "docs/adr/0003-quest-networking-port.md": {"nEVR"},
    "docs/adr/0004-quest-verification-regime.md": {"nEVR"},
    "docs/adr/0010-signed-plugins-only.md": {"nEVR"},
    "docs/adr/0012-static-install-root.md": {"nEVR"},
    "docs/adr/0028-game-derived-packs-on-the-cdn.md": {"nEVR"},
    "docs/reference/content-pack-hosting.md": {"nEVR"},
    "docs/design/2026-09-21-mic-provider-voip-fix.md": {"nEVR"},
    "docs/reference/example-config.yaml": {"nEVR"},
    "docs/reference/launcher-install-root.md": {"nEVR"},
    "docs/reference/verifying-a-release.md": {"nEVR"},
    "docs/standards/logging.md": {"nEVR"},
    "cmake/nevr_version_info.rc.in": {"nEVR"},  # ProductName and FileDescription are brand prose
    "docs/standards/verification.md": {"nEVR"},
    "src/launcher/CMakeLists.txt": {"nEVR"},  # the exe's FileDescription
    "src/runtime/CMakeLists.txt": {"nEVR"},  # the DLL's FileDescription
    "src/core/build_identity.h": {"nEVR"},  # MenuVersionLabel: the text a player sees
    "src/core/build_identity.cpp": {"nEVR"},  # MenuVersionLabel: the text a player sees
    "src/quest/social/menu_version_label.h": {"nEVR"},  # the Quest menu label: the text a player sees
    "src/quest/social/menu_version_label.cpp": {"nEVR"},  # the Quest menu label: the text a player sees
    "src/runtime/ext/plugin_refusal_notice.h": {"nEVR"},
    "src/runtime/host/stop_notice.h": {"nEVR"},  # the player's text when the runtime did not load
    "src/runtime/host/stop_line.cpp": {"nEVR"},  # the detail an operator reads: "no nEVR root"
    "src/runtime/tests/test_host_stop_line.cpp": {"nEVR"},  # the dialog text the tests check
    "src/runtime/tests/test_host_runtime_verify.cpp": {"nEVR"},  # the dialog's version line
    "src/runtime/tests/rendezvous/rendezvous_test.cpp": {"nEVR"},  # the dialog text the harness reads
    "src/runtime/tests/test_host_rendezvous.cpp": {"nEVR"},  # asserts that text
    "src/quest/tests/menu_version_label_test.cpp": {"nEVR"},  # the label text the Quest menu shows
    "src/runtime/tests/test_menu_version_label.cpp": {"nEVR"},  # the label text the menu shows
    "src/runtime/tests/test_behavioral.cpp": {"nEVR"},  # asserts the refusal dialog's player prose
    "tools/package-release/README.template.txt": {"nEVR"},
    "tools/pe_version_info.py": {"nEVR"},  # the pinned ProductName and FileDescription
    "tools/tests/test_pe_version_info.py": {"nEVR"},  # the same pins, read back from built files
    "tools/package-release/RELEASE-NOTES.template.md": {"nEVR"},
    "tools/winvm/README.md": {"nEVR"},
    "src/core/nevr_root.h": {"nEVR"},  # comment prose: where nEVR keeps its files
    "src/runtime/lifecycle/nevr_root_move.h": {"nEVR"},  # comment prose about the one-time move
    "src/runtime/tests/test_nevr_root.cpp": {"nEVR"},  # comment prose and a failure message
    "tools/tests/test_nevr_root_wiring.py": {"nEVR"},  # docstring prose
    "docs/beta/INSTALL.md": {"nEVR"},  # prose: update nEVR through the launcher
    "installer/src/launcher.rs": {"nEVR"},  # comment prose, and the version text the refusal line shows
    "installer/src/session.rs": {"nEVR"},  # the refusal line a player reads: "nEVR 5.4.3 is installed"
    "installer/src/main.rs": {"nEVR"},  # the usage line and the root refusal an operator reads (#1353)
    "installer/tests/nevr_root.rs": {"nEVR"},  # the refusal reason the test asserts (#1353)
    "tools/package-release/install.ps1": {"nEVR"},  # the header comment on -Root (#1353)
}
# Namespaces the game's own binary uses. src/reconstruction takes each from the binary: it is the qualifier of a
# function the file cites by its game address (`// @0x... — NRadEngine::CMemory::Compare`), or of a game API the file
# calls (`CSysWindow::IsVirtual`). docs/standards/naming.md: names that belong to the game keep their own spelling.
GAME_NAMESPACES = frozenset({
    "CBroadcaster", "CGRenderConfig", "CGS", "CHashTable", "CMatSym", "CMemBlock", "CMemory", "CMemoryContext",
    "CSoundEmitter", "CSoundPlayer", "CSysNet", "CSysNet::SAddress", "CSysTime", "CSysWindow", "CTimer", "CVR",
    "NR15RestAPI", "NRadEngine::CGRenderConfig", "NRadEngine::CMemBlock", "NRadEngine::CMemory",
    "NRadEngine::CSoundEmitter", "NRadEngine::CSoundPlayer", "NRadEngine::CSysNet", "NRadEngine::CSysTime",
    "NRadEngine::NRadGame", "NRadEngine::NRadGame::NR15RestAPI", "NRadEngine::SComponentNodeUtils", "NRadGame",
    "NWriteLog", "SBlackboardCD", "SBroadcasterData", "SComponentNodeUtils",
})
# The last component of a namespace is `nevr` or `nevr_<area>`; a game namespace may be the prefix before it, as in
# `NRadEngine::nevr_reconstruction`.
NEVR_LAST_COMPONENT = re.compile(r"nevr(?:_[a-z][a-z0-9_]*)?")


def nevr_named(name):
    """True when the namespace's last component is `nevr` or `nevr_<area>` (docs/standards/naming.md)."""
    return NEVR_LAST_COMPONENT.fullmatch(name.rsplit("::", 1)[-1]) is not None


def project_files():
    """Tracked and untracked-but-not-ignored text files, so a new file is checked before it is added."""
    out = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                         check=True, capture_output=True, text=True).stdout
    for path in (p for p in out.split("\0") if p):
        if path.startswith(EXCLUDED_PREFIXES) or path in SELF:
            continue
        try:
            yield path, (REPO / path).read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue


def noncanonical(text):
    return {m.group() for m in ANY_CASE.finditer(text) if not CANONICAL.match(m.group())}

# Component names, file roles and placement (docs/standards/naming.md). Each check finds its breaks on the tracked tree
# and compares them with a known-breaks list: a break that is not listed fails, and so does a listed entry that breaks
# nothing (its message says to delete it). The lists carry KNOWN-WRONG #2363 and only shrink; the rename that fixes an
# entry deletes it. An entry may be a glob where the rule allows one.
NAME_EXCLUDED_PREFIXES = ("extern/",)  # a vendored submodule path, not a name of ours
DIRECTORY_ROOTS = ("src", "sdk", "installer", "tools")
# A vcpkg overlay port and a Go command keep the name an outside tool gives them (docs/standards/naming.md).
OUTSIDE_THE_DIRECTORY_RULE = ("src/quest/vcpkg-overlay-ports/", "tools/cmd/")
QUEST_RULES_INCLUDE = re.compile(r'#\s*include\s*"(runtime/[^"]*_rules\.h)"')
KNOWN_COLLISIONS = frozenset({
    "src/quest/combat: table_step (file) ~ tablestep (file)",  # KNOWN-WRONG #2363
    "tools: package-release (dir) ~ package_release (file)",  # KNOWN-WRONG #2363
})
KNOWN_ROLE_BREAKS = frozenset({
    "src/quest/combat/bvh_core.h",  # KNOWN-WRONG #2363
    "src/quest/combat/check/powerboost_thunk.want",  # KNOWN-WRONG #2363
    "src/quest/combat/decals_core.h",  # KNOWN-WRONG #2363
    "src/quest/combat/dronekill_core.h",  # KNOWN-WRONG #2363
    "src/quest/combat/ec_core.h",  # KNOWN-WRONG #2363
    "src/quest/combat/ecfx_core.h",  # KNOWN-WRONG #2363
    "src/quest/combat/mutants.d/bvh_core.sh",  # KNOWN-WRONG #2363
    "src/quest/combat/powerboost_core.h",  # KNOWN-WRONG #2363
    "src/quest/combat/powerboost_thunk.inc",  # KNOWN-WRONG #2363
    "src/quest/invite/invite_transport_quest.cpp",  # KNOWN-WRONG #2363
    "src/quest/invite/invite_transport_quest.h",  # KNOWN-WRONG #2363
    "src/quest/invite/join_game_quest.cpp",  # KNOWN-WRONG #2363
    "src/quest/invite/join_game_quest.h",  # KNOWN-WRONG #2363
    "src/quest/invite/ui_text_quest.cpp",  # KNOWN-WRONG #2363
    "src/quest/invite/ui_text_quest.h",  # KNOWN-WRONG #2363
    "src/quest/login/login_thunk.h",  # KNOWN-WRONG #2363
    "src/quest/sentinel/callback_thunk.h",  # KNOWN-WRONG #2363
    "src/quest/sentinel/crash_report_quest.cpp",  # KNOWN-WRONG #2363
    "src/quest/sentinel/crash_report_quest.h",  # KNOWN-WRONG #2363
    "src/quest/tests/compile_fail/fake_thunk.cpp",  # KNOWN-WRONG #2363
    "src/quest/tests/compile_fail/row_fake_thunk.cpp",  # KNOWN-WRONG #2363
    "src/reconstruction/r15/wwise_symbol_bucket_count_thunk.cpp",  # KNOWN-WRONG #2363
    "src/runtime/hook/export_trace_thunk.cpp",  # KNOWN-WRONG #2363
    "src/runtime/hook/export_trace_thunk.h",  # KNOWN-WRONG #2363
    "src/runtime/lifecycle/crash_first_win.cpp",  # KNOWN-WRONG #2363
    "src/runtime/lifecycle/crash_first_win.h",  # KNOWN-WRONG #2363
    "src/runtime/lifecycle/crash_report_win.cpp",  # KNOWN-WRONG #2363
    "src/runtime/lifecycle/crash_report_win.h",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_arena_round_time.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_arena_rules_hook.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_asset_cache.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_asset_registry.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_behavioral.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_boot_log_routing.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_boot_replay.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_bootstrap_diagnostics.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_bounded_thread.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_bridge_line.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_bridge_policy.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_broadcaster_allow_incoming.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_broadcaster_pointer_guard.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_callback_unregistration.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_checked_patch.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_cli_check.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_clock_skew.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_cmdline_strip.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_cmdline_waiver.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_combat_match_type.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_combat_settings.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_combat_shipped_settings.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_combat_write_gate.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_config_yaml_choice.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_content_pack.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_coop_ai_trace.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_crash_first.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_crash_recovery.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_crash_report_pe.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_crash_send.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_credential_cache_check.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_credits_text.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_deadlock_monitor.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_dll_load_hook.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_dot_script.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_dronemuzzle_rules.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_early_quit_feature_flags.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_early_quit_lockout.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_equipment_refire.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_error_dialog_depth.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_evr_codec.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_evrp_package.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_export_trace.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_failure_detail.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_follow_record.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_frame_stats.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_friend_request_check.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_game_data_notice.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_game_image_guard.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_game_line_format.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_gpu_hung_notice.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_gpu_score.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_guild_picker.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_hooking.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_host_crash_capture.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_host_crash_report.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_host_crash_send.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_host_exports.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_host_json.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_host_minidump.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_host_rendezvous.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_host_runtime_load.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_host_runtime_verify.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_host_stop_line.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_host_update_swap.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_http_port_scan.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_installed_levels.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_invite_answer_apply.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_invite_answer_text.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_invite_choices.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_invite_join.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_invite_service.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_invite_transport.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_invite_wizard.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_invite_worker.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_json_escape.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_legacy_codec.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_legacy_session.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_level_gate.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_loading_tips.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_lobby_failure_message.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_lobby_frame.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_lobby_full_backoff.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_lobby_settings_hook.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_lobby_settings_relay.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_log_send.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_login_redirect_override.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_login_refusal.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_login_session.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_main_thread_handoff.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_menu_page.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_menu_version_label.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_messages.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_mic_dsp.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_mic_lifecycle.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_mic_owner_thread.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_netgun_bullet_ids.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_netgun_bullets.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_nevr_config.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_nevr_config_set.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_nevr_root.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_parse_endpoint.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_party_follow_dedup.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_party_leave_check.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_party_share_check.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_plugin_load_plan.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_plugin_log.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_plugin_logger.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_plugin_signature.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_plugin_tree.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_plugin_trust.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_pnsrad_diag_queue.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_pnsrad_load_queue.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_powerboost_rules.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_process_memory.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_protobuf_transport.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_provider_name_redirect.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_queued_join_log.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_quit_check.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_rate_limiter.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_reader_gate.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_registration_roundtrip.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_render_config.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_render_config_hook.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_resource_override.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_resource_registry.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_return_to_lobby_hold.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_return_to_lobby_queue.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_run_log_record.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_runtime_abi.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_runtime_export_guard.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_scenario_early_quit.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_scenario_invite.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_scenario_ui_text.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_schannel_cred_guard.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_script_guard.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_script_hooks.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_self_check.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_self_provenance.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_server_bots.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_server_bringup.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_server_context.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_server_features.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_server_log.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_server_team_size.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_server_timestep.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_serverdb_uri.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_service_config.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_service_map.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_session_unregister.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_shared_magazine.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_social_facade.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_socket_role.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_splash_skip.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_stable_string_pool.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_station_cells.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_status_redirect_check.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_symbol_hash.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_system_module_loader.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_team_ready.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_telemetry_game_properties.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_telemetry_game_type.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_telemetry_snapshot_store.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_timestep_log.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_token_auth.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_ui_text.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_ui_texture_mips.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_update_core_link.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_update_worker.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_url_diagnostics.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_websocket_client_auth.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_websocket_frame.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_window_watch.cpp",  # KNOWN-WRONG #2363
    "src/runtime/tests/test_xpid_patch.cpp",  # KNOWN-WRONG #2363
})
KNOWN_RULES_HOME_BREAKS = frozenset({
    "src/runtime/patch/dronemuzzle_rules.h: no non-test file in src/runtime/patch/ includes it",  # KNOWN-WRONG #2363
    "src/runtime/patch/equipment_refire_rules.h: no non-test file under src/quest/ is named equipment_refire",  # KNOWN-WRONG #2363
    "src/runtime/patch/lobby_failure_message_rules.h: no non-test file under src/quest/ is named lobby_failure_message",  # KNOWN-WRONG #2363: its hook is the lobby-full hook of lobby_full_backoff
    "src/runtime/patch/powerboost_rules.h: no non-test file in src/runtime/patch/ includes it",  # KNOWN-WRONG #2363
    "src/runtime/patch/splash_skip_rules.h: no non-test file under src/quest/ is named splash_skip",  # KNOWN-WRONG #2363
})
KNOWN_DIRECTORY_BREAKS = frozenset({
    "src/libovr-stub",  # KNOWN-WRONG #2363
    "src/modules/platform-compat",  # KNOWN-WRONG #2363
    "src/modules/token-auth",  # KNOWN-WRONG #2363
    "src/quest/vcpkg-overlay-ports",  # KNOWN-WRONG #2363
    "tools/git-hooks",  # KNOWN-WRONG #2363
    "tools/package-release",  # KNOWN-WRONG #2363
    "tools/quest-host-deps",  # KNOWN-WRONG #2363
})

def tree_paths():
    """Tracked and untracked-but-not-ignored paths of the tree, without the trees that are not ours."""
    out = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                         check=True, capture_output=True, text=True).stdout
    return [p for p in out.split("\0") if p and not p.startswith(NAME_EXCLUDED_PREFIXES)]

def tree_texts(paths):
    """The text of each source file among `paths`, for the check that reads an include."""
    texts = {}
    for path in paths:
        if path.startswith("src/"):
            try:
                texts[path] = (REPO / path).read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError):
                continue
    return texts

def name_key(name):
    """Two names are one name when they are equal once case, `_` and `-` are ignored."""
    return name.lower().replace("_", "").replace("-", "")

def directory_entries(paths):
    """Each (directory, kind, name) of the tree: a file's stem (`music.h` and `music.cpp` are one entry) or a directory."""
    entries = set()
    for path in paths:
        pure = pathlib.PurePosixPath(path)
        entries.add((pure.parent.as_posix(), "file", pure.stem))
        for directory in pure.parents:
            if directory.as_posix() != ".":
                entries.add((directory.parent.as_posix(), "dir", directory.name))
    return entries

def collisions(paths):
    """N-COLLIDE: one line per directory and set of entries that are equal by name_key."""
    groups = {}
    for directory, kind, name in directory_entries(paths):
        groups.setdefault((directory, name_key(name)), set()).add((name, kind))
    return {f"{directory}: " + " ~ ".join(f"{name} ({kind})" for name, kind in sorted(members))
            for (directory, _), members in groups.items() if len(members) > 1}

def role_breaks(paths):
    """N-ROLES: a stem the file-role rules forbid, or a C++ file under src/ named test_<stem>.cpp."""
    breaks = set()
    for path in paths:
        if not path.startswith("src/"):
            continue
        pure = pathlib.PurePosixPath(path)
        stem = pure.stem
        if (stem.endswith(("_core", "_thunk"))
                or (path.startswith("src/quest/") and stem.endswith("_quest"))
                or (path.startswith("src/runtime/") and stem.endswith(("_win", "_pc")))
                or (pure.name.startswith("test_") and pure.suffix == ".cpp")):
            breaks.add(path)
    return breaks

def is_test_file(path):
    pure = pathlib.PurePosixPath(path)
    return "tests" in pure.parts or pure.name.startswith("test_") or pure.stem.endswith("_test")

def rules_home_breaks(paths, texts):
    """N-RULES-HOME: a `_rules.h` that src/quest includes needs a non-test PC includer in its own directory under
    src/runtime, and a non-test file under src/quest/ named for it."""
    included = set()
    for path in paths:
        if path.startswith("src/quest/"):
            included.update(QUEST_RULES_INCLUDE.findall(texts.get(path, "")))
    quest_stems = {pathlib.PurePosixPath(p).stem for p in paths if p.startswith("src/quest/") and not is_test_file(p)}
    breaks = set()
    for rel in sorted(included):
        header = "src/" + rel
        pure = pathlib.PurePosixPath(header)
        directory = pure.parent.as_posix()
        stem = pure.stem[:-len("_rules")]
        include = re.compile(r'#\s*include\s*"' + re.escape(rel) + '"')
        if not any(p.startswith("src/runtime/") and pathlib.PurePosixPath(p).parent.as_posix() == directory
                   and not is_test_file(p) and include.search(texts.get(p, "")) for p in paths):
            breaks.add(f"{header}: no non-test file in {directory}/ includes it")
        if stem not in quest_stems:
            breaks.add(f"{header}: no non-test file under src/quest/ is named {stem}")
    return breaks

def directory_breaks(paths):
    """N-DIRS: a directory under DIRECTORY_ROOTS with a `-` in its name, the overlay ports and Go commands aside."""
    breaks = set()
    for path in paths:
        for directory in pathlib.PurePosixPath(path).parents:
            name = directory.as_posix()
            if name == "." or name.split("/", 1)[0] not in DIRECTORY_ROOTS or name.startswith(OUTSIDE_THE_DIRECTORY_RULE):
                continue
            if "-" in directory.name:
                breaks.add(name)
    return breaks

def unlisted_and_stale(found, known):
    """The breaks that no entry of `known` lists, and the entries that name no break."""
    return sorted(set(found) - known), sorted(known - set(found))

class NamingTest(unittest.TestCase):
    def test_the_scan_sees_the_tree(self):
        files = dict(project_files())
        self.assertGreater(len(files), 300, "the sensor is reading almost nothing")
        self.assertIn("sdk/include/nevr/plugin_interface.h", files)

    def test_nvr_is_only_a_deprecated_alias(self):
        files = dict(project_files())
        for header in ALIAS_HEADERS:
            text = files[header]
            aliases = {m.group(1): (m.group(2), m.group(3)) for m in ALIAS_DECL.finditer(text)}
            aliases.update({m.group(2): (m.group(1), m.group(1)) for m in FUNCTION_ALIAS_DECL.finditer(text)})
            used = {re.sub(r"(_fn|_t)$", "", t) for t in NVR.findall(text)}
            self.assertEqual(sorted(used - {re.sub(r"(_fn|_t)$", "", a) for a in aliases}), [],
                             f"{header}: a `Nvr` name that is not a [[deprecated]] alias of its Nevr twin")
            for alias, (message, target) in aliases.items():
                twin = "Nevr" + alias[len("Nvr"):]
                self.assertEqual((message, target), (twin, twin), f"{header}: {alias} must alias {twin}")
                self.assertRegex(text, r"(?:enum|struct|typedef|constexpr)[^;{]*\b" + re.escape(twin) + r"\b",
                                 f"{header}: {twin} is not declared there")
        stray = {}
        for path, text in project_files():
            if path in ALIAS_HEADERS:
                continue
            tokens = {re.sub(r"(_fn|_t)$", "", t) for t in NVR.findall(text)} - THIRD_PARTY_NVR
            tokens -= DEPRECATED_NVR_USES.get(path, set())
            if tokens:
                stray[path] = sorted(tokens)
        self.assertEqual(stray, {}, "`Nevr` is the prefix of the plugin/module C ABI; `Nvr` survives only as the "
                         "deprecated aliases in the SDK headers (docs/standards/naming.md)")

    def test_every_deprecated_nvr_use_is_still_present(self):
        files = dict(project_files())
        gone = {p: sorted(tokens - {re.sub(r"(_fn|_t)$", "", t) for t in NVR.findall(files.get(p, ""))})
                for p, tokens in DEPRECATED_NVR_USES.items()}
        gone = {p: tokens for p, tokens in gone.items() if tokens}
        self.assertEqual(gone, {}, "remove names that left the file from DEPRECATED_NVR_USES")

    def test_the_headers_declare_no_nvr_name_that_is_not_deprecated(self):
        # Every `Nvr` declaration in the published headers is an alias carrying [[deprecated]]; a bare
        # `struct NvrX`, `enum NvrX` or `typedef ... NvrX` would be a new name in the old prefix.
        bare = re.compile(r"\b(?:struct|enum(?:\s+class)?|class|typedef\b[^;]*?)\s+\(?\*?(Nvr\w+)")
        found = {h: sorted(set(bare.findall(dict(project_files())[h]))) for h in ALIAS_HEADERS}
        self.assertEqual({h: n for h, n in found.items() if n}, {}, "a published type or typedef is spelled Nvr")

    def test_no_new_noncanonical_spelling(self):
        offenders = {}
        for path, text in project_files():
            extra = noncanonical(text) - LEGACY_SPELLINGS.get(path, set()) - BRAND_PROSE_SPELLINGS.get(path, set())
            if extra:
                offenders[path] = sorted(extra)
        self.assertEqual(offenders, {}, "use NEVR, Nevr or nevr in a name, and nEVR in prose (docs/standards/naming.md)")

    def test_one_spelling_of_the_lifecycle_namespace(self):
        # The runtime's lifecycle code is in `nevr::lifecycle`; the two older spellings were unified (#131).
        old = re.compile(r"nevr_runtime::lifecycle|Nevr::Lifecycle|nevr::Lifecycle")
        found = {path: sorted(set(old.findall(text))) for path, text in project_files() if old.search(text)}
        self.assertEqual(found, {}, "the lifecycle namespace is nevr::lifecycle (docs/standards/naming.md)")

    def test_cmake_targets_carry_the_prefix(self):
        # A library or executable target is nevr_<name> (#131); the output file name is OUTPUT_NAME and does
        # not move. These four are a file name users touch or a third party's name.
        exceptions = {"echovr_server", "LibOVRPlatform64_1", "ovrplatformloader", "breakpad_client"}
        add = re.compile(r"^\s*add_(?:library|executable)\(\s*([A-Za-z0-9_]+)", re.M)
        stray = {}
        for path, text in project_files():
            if not (path.endswith("CMakeLists.txt") or path.endswith(".cmake")):
                continue
            names = {n for n in add.findall(text)
                     if not n.startswith(("nevr", "test_")) and not n.endswith(("_test", "_probe", "_probe_", "_test_hooks"))}
            names -= exceptions
            if names:
                stray[path] = sorted(names)
        self.assertEqual(stray, {}, "CMake targets are nevr_<name> (docs/standards/naming.md)")

    def test_project_macros_carry_the_prefix(self):
        # The build identity and the hook selector are NEVR_ macros (#131). CMake VARIABLES of the same name
        # (`${PROJECT_VERSION}`, `set(GIT_DESCRIBE ...)`) are CMake's own and stay.
        macro = re.compile(r"(?<![A-Za-z0-9_${])(USE_MINHOOK|PROJECT_VERSION|GIT_COMMIT_HASH|GIT_DESCRIBE)(?![A-Za-z0-9_}])")
        stray = {}
        for path, text in project_files():
            if not path.endswith((".h", ".hpp", ".cpp", ".cc", ".inc")):
                continue
            found = sorted({m.group(1) for m in macro.finditer(text)})
            if found:
                stray[path] = found
        self.assertEqual(stray, {}, "project macros are NEVR_<NAME> (docs/standards/naming.md)")

    def test_renamed_namespaces_do_not_come_back(self):
        def in_string(line, pos):
            quotes, i = 0, 0
            while i < pos:
                if line[i] == "\\":
                    i += 2
                    continue
                quotes += line[i] == '"'
                i += 1
            return quotes % 2 == 1

        # One pattern for every old name, tried only on lines that hold `::` or `namespace`.
        olds = "|".join(re.escape(old) for old in RENAMED_NAMESPACES)
        pattern = re.compile(r"(?<![A-Za-z0-9_:])(" + olds + r")::|\bnamespace\s+(" + olds + r")\b")
        stray = {}
        for path, text in project_files():
            if not path.endswith((".h", ".hpp", ".cpp", ".cc", ".inc")):
                continue
            for n, line in enumerate(text.split("\n"), start=1):
                if "::" not in line and "namespace" not in line:
                    continue
                for m in pattern.finditer(line):
                    if not in_string(line, m.start()):
                        stray.setdefault(path, []).append("%d:%s" % (n, m.group(1) or m.group(2)))
        self.assertEqual(stray, {}, "these namespaces are nevr_<area> now (docs/standards/naming.md)")

    def test_every_namespace_is_nevr_or_the_games(self):
        # The rule is `nevr` or `nevr_<area>` as the last component (#131). The game's own namespaces are
        # GAME_NAMESPACES; the two PascalCase namespaces left are named here with the reason; a new one fails this
        # test, and an entry whose namespace is gone fails it too.
        exceptions = {
            # The hooking abstraction every patch calls; the name predates the rule.
            "src/core/hooking.h": {"Hooking"},
            # Nested in the game's EchoVR namespace and named after the game's own symbol tables.
            "src/abi/symbols.h": {"EchoVR::Symbols", "Social", "Tcp"},
        }
        decl = re.compile(r"^\s*namespace\s+([A-Z][A-Za-z0-9_]*(?:::[A-Za-z0-9_]+)*)\s*\{")
        found = {}
        for path, text in project_files():
            if not path.endswith((".h", ".hpp", ".cpp", ".cc", ".inc")) or path.startswith("plugins/"):
                continue
            for line in text.split("\n"):
                m = decl.match(line)
                if m and m.group(1) not in {"EchoVR", "NRadEngine"}:
                    found.setdefault(path, set()).add(m.group(1))
        declared = set().union(*found.values())
        self.assertEqual(GAME_NAMESPACES - declared, set(),
                         "GAME_NAMESPACES names a namespace the tree no longer declares")
        found = {path: {n for n in names - GAME_NAMESPACES if not nevr_named(n)} for path, names in found.items()}
        found = {path: names for path, names in found.items() if names}
        self.assertEqual(found, exceptions, "namespaces are nevr or nevr_<area> (docs/standards/naming.md)")

    def test_every_legacy_spelling_is_still_present(self):
        files = dict(project_files())
        gone = {p: sorted(tokens - noncanonical(files.get(p, ""))) for p, tokens in LEGACY_SPELLINGS.items()
                if tokens - noncanonical(files.get(p, ""))}
        self.assertEqual(gone, {}, "remove cleaned entries from LEGACY_SPELLINGS")

    def test_every_brand_prose_spelling_is_still_present(self):
        files = dict(project_files())
        gone = {p: sorted(tokens - noncanonical(files.get(p, ""))) for p, tokens in BRAND_PROSE_SPELLINGS.items()
                if tokens - noncanonical(files.get(p, ""))}
        self.assertEqual(gone, {}, "remove cleaned entries from BRAND_PROSE_SPELLINGS")

    def test_project_brand_spelling_in_docs(self):
        offenders = {}
        for path, text in project_files():
            if not path.endswith((".md", ".yaml", ".yml")):
                continue
            matches = sorted({m.group() for m in UPPERCASE_BRAND_IN_DOCS.finditer(text)})
            if matches:
                offenders[path] = matches
        self.assertEqual(offenders, {}, "spell the project brand nEVR in prose (docs/standards/naming.md)")

    def test_n_collide_no_two_entries_of_a_directory_are_equal_by_name(self):
        unlisted, stale = unlisted_and_stale(collisions(tree_paths()), KNOWN_COLLISIONS)
        self.assertEqual(unlisted, [], "N-COLLIDE: two entries of one directory are equal once case, `_` and `-` are "
                         "ignored; rename one of them (docs/standards/naming.md)")
        self.assertEqual(stale, [], "N-COLLIDE: a KNOWN_COLLISIONS entry no longer collides; delete that entry")

    def test_n_roles_no_stem_breaks_a_file_role_rule(self):
        unlisted, stale = unlisted_and_stale(role_breaks(tree_paths()), KNOWN_ROLE_BREAKS)
        self.assertEqual(unlisted, [], "N-ROLES: a stem ends in a suffix the file-role rules forbid, or a C++ file "
                         "under src/ is named test_<stem>.cpp; rename it (docs/standards/naming.md)")
        self.assertEqual(stale, [], "N-ROLES: a KNOWN_ROLE_BREAKS entry breaks no file; delete that entry")

    def test_n_rules_home_each_quest_rules_header_has_a_pc_includer_and_a_quest_half(self):
        paths = tree_paths()
        unlisted, stale = unlisted_and_stale(rules_home_breaks(paths, tree_texts(paths)), KNOWN_RULES_HOME_BREAKS)
        self.assertEqual(unlisted, [], "N-RULES-HOME: a _rules.h that src/quest includes needs a non-test file in its "
                         "directory under src/runtime that includes it, and a non-test file under src/quest/ named "
                         "for it (docs/standards/naming.md)")
        self.assertEqual(stale, [], "N-RULES-HOME: a KNOWN_RULES_HOME_BREAKS entry no longer breaks the rule; delete it")

    def test_n_dirs_no_directory_under_the_source_roots_has_a_hyphen(self):
        unlisted, stale = unlisted_and_stale(directory_breaks(tree_paths()), KNOWN_DIRECTORY_BREAKS)
        self.assertEqual(unlisted, [], "N-DIRS: a directory under src/, sdk/, installer/ or tools/ has a `-` in its "
                         "name; rename it (docs/standards/naming.md)")
        self.assertEqual(stale, [], "N-DIRS: a KNOWN_DIRECTORY_BREAKS entry names no directory with a hyphen; delete it")

    def test_the_name_checks_report_a_planted_break_and_a_stale_entry(self):
        # The checks on small synthetic trees: each reports the break it names and no other, and the comparison
        # reports a break no entry lists and an entry that names no break.
        self.assertEqual(collisions(["src/a/table_step.h", "src/a/table_step.cpp", "src/a/tablestep.h"]),
                         {"src/a: table_step (file) ~ tablestep (file)"})
        self.assertEqual(collisions(["src/a/music.h", "src/a/music.cpp", "src/b/music.h"]), set())
        self.assertEqual(collisions(["tools/package-release/x.py", "tools/package_release.py"]),
                         {"tools: package-release (dir) ~ package_release (file)"})
        self.assertEqual(role_breaks(["src/runtime/patch/x_core.h", "src/quest/a/b_quest.cpp",
                                      "src/runtime/tests/test_x.cpp", "src/quest/a/c_test.cpp",
                                      "src/runtime/patch/z_quest.h", "src/quest/a/d_win.cpp"]),
                         {"src/runtime/patch/x_core.h", "src/quest/a/b_quest.cpp", "src/runtime/tests/test_x.cpp"})
        self.assertEqual(directory_breaks(["src/modules/token-auth/a.cpp",
                                           "src/quest/vcpkg-overlay-ports/yaml-cpp/a.cpp",
                                           "tools/cmd/content-pack-build/main.go"]),
                         {"src/modules/token-auth", "src/quest/vcpkg-overlay-ports"})
        self.assertEqual(rules_home_breaks(["src/runtime/patch/x_rules.h", "src/runtime/patch/x.cpp",
                                            "src/quest/c/x.cpp"],
                                           {"src/quest/c/x.cpp": '#include "runtime/patch/x_rules.h"\n',
                                            "src/runtime/patch/x.cpp": '#include "runtime/patch/x_rules.h"\n'}),
                         set())
        self.assertEqual(rules_home_breaks(["src/runtime/patch/y_rules.h", "src/quest/c/y.cpp",
                                            "src/runtime/patch/test_y.cpp"],
                                           {"src/quest/c/y.cpp": '#include "runtime/patch/y_rules.h"\n',
                                            "src/runtime/patch/test_y.cpp": '#include "runtime/patch/y_rules.h"\n'}),
                         {"src/runtime/patch/y_rules.h: no non-test file in src/runtime/patch/ includes it"})
        self.assertEqual(unlisted_and_stale({"x"}, {"y"}), (["x"], ["y"]))

if __name__ == "__main__":
    unittest.main()
