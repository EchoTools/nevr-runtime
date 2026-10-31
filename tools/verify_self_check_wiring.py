#!/usr/bin/env python3
"""Fail `just verify` when the self-checks stop being unconditional or the runtime adds a debug query (#451).

Self-checks are on in every build since 2026-10-11, so nothing in a build may behave differently
because of how it was stamped, and the runtime never adds `debug=true` to a connection by build type: whether a
player sends the FULL remote logs is the game service's decision (profile.EnableAllRemoteLogs,
serviceSettings.EnableSessionDebug). This gate keeps both true:

* the PC bridge turns the unit on unconditionally (WireSelfChecks) and the Quest bridge wires it through
  ApplySelfCheck with no feature in between;
* no switch by build type or feature exists (no NEVR_SELF_CHECKS define, no Quest `self_check` feature) and
  nothing appends a debug query (no AppendRemoteDebugParam / WantsRemoteDebug / remoteDebugQuery, no
  `debug=true` literal in the runtime sources).

Source checks, because the connect path itself runs only in a live session.
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# (file, regex that must match, message)
REQUIRED = [
    ("src/runtime/compat/ws_bridge.cpp",
     r"static void WireSelfChecks\(\)\s*\{\s*nevr_self_check::SetEnabled\(true\);",
     "WireSelfChecks does not turn the unit on as its first, unconditional statement"),
    ("src/runtime/compat/ws_bridge.cpp", r"WireSelfChecks\(\);",
     "InstallWebSocketBridge does not wire the self-check sender"),
    ("src/quest/integration/production_steps.cpp", r"ApplySelfCheck\(\s*&config\.tap\s*,",
     "the Quest bridge config is not wired through ApplySelfCheck"),
    ("src/quest/integration/self_check_wiring.cpp", r"nevr_self_check::SetEnabled\(true\);",
     "ApplySelfCheck does not turn the unit on"),
    ("src/runtime/compat/ws_bridge.cpp", r"if \(fromServer\) nevr_party_share_check::OnServerMessage\(sym\);\s*else nevr_party_share_check::OnClientMessage\(sym,",
     "the PC observer does not hand both directions to the party_data_share check"),
    ("src/runtime/compat/ws_bridge.cpp", r"if \(fromServer\) nevr_party_leave_check::OnServerMessage\(sym, payload,",
     "the PC observer does not hand the service's messages to the party_member_leave check"),
    ("src/quest/integration/production_steps.cpp", r"nevr_party_leave_check::OnServerMessage\(message\.symbol, message\.payload,",
     "the Quest observer does not hand the service's messages to the party_member_leave check"),
    ("src/runtime/compat/ws_bridge.cpp", r"if \(fromServer\) nevr_friend_request_check::OnServerMessage\(sym, payload,",
     "the PC observer does not hand the service's messages to the friend_request check"),
    ("src/quest/integration/production_steps.cpp", r"nevr_friend_request_check::OnServerMessage\(message\.symbol, message\.payload,",
     "the Quest observer does not hand the service's messages to the friend_request check"),
    ("src/runtime/lifecycle/crash_recovery.cpp", r"nevr_quit_check::OnGameLoopReturned\(gameResult\);[\s\S]{0,120}return gameResult;",
     "the client's game loop return does not report the pc_quit check before the process exits"),
    ("src/quest/integration/production_steps.cpp", r"SetProcessEndCallback\(&OnGameProcessEnd\);",
     "the constructor step InstallProcessEnd does not register OnGameProcessEnd with the process-end hook"),
    ("src/quest/integration/production_steps.cpp",
     r"void OnGameProcessEnd\(\) noexcept \{[\s\S]{0,200}FinishPlugins\(nevr_quest::plugin::reason::kGameExitedFirst\);[\s\S]{0,40}ReportAndroidMainReturned\(\);",
     "the process-end callback does not end the pending plugin entries and send the android_main_returned report"),
    ("src/quest/sentinel/entry.cpp", r"bool InstallProcessEndHook\(\) noexcept \{ return nevr_quest::process_end::Install\(\); \}",
     "InstallProcessEndHook does not return the result of the process-end hook's install"),
    ("src/quest/integration/production_steps.cpp", r"const bool ok = nevr_quest::integration::InstallProcessEndHook\(\);",
     "the constructor step InstallProcessEnd does not install through InstallProcessEndHook"),
    ("src/runtime/compat/ws_bridge.cpp", r"nevr_credential_cache_check::Register\(\);",
     "WireSelfChecks does not register the credential_cache and credential_cache_refused checks"),
    ("src/core/auth_token.h", r"nevr_credential_stats::NoteSaved\(protection\);[\s\S]*nevr_credential_stats::NoteRefused\(why\);|nevr_credential_stats::NoteRefused\(why\);[\s\S]*nevr_credential_stats::NoteSaved\(protection\);",
     "SaveAuthToken does not note both its outcomes (written, refused) for the credential_cache check"),
    ("src/modules/token-auth/src/token_auth.cpp", r"nevr_credential_stats::NoteCacheReused\(\);[\s\S]*nevr_credential_stats::NoteSignInRequired\(whyNoCache\);",
     "the token-auth module does not note whether the cached credentials were reused or a sign-in was needed"),
    ("src/runtime/lifecycle/config.cpp", r"nevr_status_redirect_check::NoteConnect\(gameUri, uri\);",
     "the PC connect hook does not count the game's connects for the status_redirect check"),
    ("src/runtime/compat/ws_bridge.cpp", r"nevr_status_redirect_check::Register\(&nevr_status_redirect_check::ProcessProbe\);",
     "WireSelfChecks does not register the status_redirect check"),
    ("src/quest/redirect/service_redirector.cpp", r"nevr_status_redirect_check::NoteConnect\(url, url\);[\s\S]*nevr_status_redirect_check::NoteConnect\(url, chosen\);",
     "ApplyUrl does not count every connect (left alone and decided) for the status_redirect check"),
    ("src/quest/integration/production_steps.cpp", r"nevr_status_redirect_check::Register\(&nevr_status_redirect_check::ProcessProbe\);",
     "the Quest bridge does not register the status_redirect check"),
    ("src/quest/integration/production_steps.cpp", r"ObserveForSelfChecks\(serverToGame, data, len\);",
     "the Quest tap's observer does not hand frames to the self-checks"),
    ("src/quest/integration/production_steps.cpp", r"ObserveForSelfChecks\(false, reinterpret_cast<const std::uint8_t\*>\(frame\.data\(\)\)",
     "the Quest facade's own requests (SendSocialFrame) do not reach the party_data_share check"),
]

# Tokens that must not appear in the runtime sources: a switch by build type or feature, or a debug query.
FORBIDDEN_TOKENS = [
    "AppendRemoteDebugParam",
    "WantsRemoteDebug",
    "remoteDebugQuery",
    "NEVR_SELF_CHECKS",
    "nevr_self_checks_by_stamp",
    "Feature::kSelfCheck",
    "Features::selfCheck",
    "effective.selfCheck",
    "requested.selfCheck",
    "bool selfCheck",
]
FORBIDDEN_REGEX = [
    (re.compile(r"\bdebug=true\b"), "a `debug=true` query literal", False,
     "the game service decides who sends the full remote logs"),
    (re.compile(r"\{\s*\"debug\"\s*,"), "a `debug` query parameter being appended", False,
     "the game service decides who sends the full remote logs"),
    # the unit stays on once the bridge turned it on; a test may switch it off
    (re.compile(r"nevr_self_check::SetEnabled\(\s*false\s*\)"), "a later SetEnabled(false)", True,
     "self-checks are on in every build"),
]
SCAN_SUFFIXES = {".cpp", ".h", ".cmake", ".txt", ".in"}
SCAN_ROOTS = ["src", "cmake", "CMakeLists.txt", "justfile"]


def scan_files(root: Path):
    for entry in SCAN_ROOTS:
        base = root / entry
        if base.is_file():
            yield base
        elif base.is_dir():
            for path in sorted(base.rglob("*")):
                if path.is_file() and (path.suffix in SCAN_SUFFIXES or path.name == "justfile"):
                    yield path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=REPO)
    args = parser.parse_args()
    failed = 0
    for rel, pattern, message in REQUIRED:
        path = args.root / rel
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as error:
            print(f"verify: FAIL — self-check wiring: cannot read {rel}: {error}", file=sys.stderr)
            failed += 1
            continue
        if not re.search(pattern, text, re.S):
            print(f"verify: FAIL — self-check wiring: {rel}: {message}", file=sys.stderr)
            failed += 1
    scanned = 0
    for path in scan_files(args.root):
        scanned += 1
        text = path.read_text(encoding="utf-8", errors="replace")
        rel = path.relative_to(args.root).as_posix()
        for token in FORBIDDEN_TOKENS:
            if token in text:
                print(f"verify: FAIL — self-check wiring: {rel} names `{token}`: self-checks are on in every build "
                      "and the runtime adds no debug query by build type (#451)", file=sys.stderr)
                failed += 1
        for regex, what, tests_may, reason in FORBIDDEN_REGEX:
            if tests_may and "/tests/" in "/" + rel:
                continue
            if regex.search(text):
                print(f"verify: FAIL — self-check wiring: {rel} has {what}: {reason} (#451)", file=sys.stderr)
                failed += 1
    if scanned == 0:
        print("verify: FAIL — self-check wiring: nothing was scanned", file=sys.stderr)
        return 1
    if failed:
        return 1
    print(f"verify: self-check wiring OK ({len(REQUIRED)} required, {scanned} files scanned)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
