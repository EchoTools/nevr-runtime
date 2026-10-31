"""#1139 / ADR 0022: the PC render_config line is wired where the game cannot be asked. The model is tested by
src/runtime/tests/test_render_config.cpp and the hooks by test_render_config_hook.cpp; these are source-order
sensors for what neither can see: the per-frame call sits inside DispatchPerFrameWork (N86: no new Tick* site, no
thread), the boot call hands Install the server flag (Install owns the client-only gate and the logged skip), both
hooks are named, prologue-validated and installed with PatchDetour, nothing on the game's path logs, and the tag is
registered. Each check takes the text it reads, so a test can hand it a broken copy and see it fail."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOOT = ROOT / "src/runtime/lifecycle/boot.cpp"
TICK = ROOT / "src/runtime/frame/tick.cpp"
HOOK = ROOT / "src/runtime/patch/render_config_hook.cpp"
MODEL = ROOT / "src/runtime/patch/render_config.cpp"
MODEL_H = ROOT / "src/runtime/patch/render_config.h"
LOGGING_DOC = ROOT / "docs/standards/logging.md"
FILTER = ROOT / "src/runtime/log/builtin_filter.cpp"

POLL_CALL = "nevr_render_config_hook::Poll(nowUs);"
INSTALL_CALL = ("nevr_render_config_hook::Install(reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr)), "
                "g_isServer != FALSE, g_isHeadless != FALSE);")
SERVER_LOG = "[NEVR.RENDER] render_config skipped reason=server_mode"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def normalize(text: str) -> str:
    """Comments stripped and every run of whitespace made one space, so a call wrapped at the 120-column limit reads as one."""
    return re.sub(r"\s+", " ", strip_comments(text))


def function_body(code: str, signature: str) -> str | None:
    """The text between the braces of the function whose definition starts with `signature` (comments stripped)."""
    at = code.find(signature)
    if at < 0:
        return None
    open_at = code.find("{", at)
    if open_at < 0:
        return None
    depth = 0
    for i in range(open_at, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[open_at + 1:i]
    return None


def poll_call_problems(tick: str) -> list[str]:
    """Poll is called once, inside DispatchPerFrameWork, and nowhere else in tick.cpp."""
    code = strip_comments(tick)
    if code.count(POLL_CALL) != 1:
        return [f"expected exactly one `{POLL_CALL}` in tick.cpp, found {code.count(POLL_CALL)}"]
    body = function_body(code, "void DispatchPerFrameWork(")
    if body is None:
        return ["DispatchPerFrameWork not found in tick.cpp"]
    if POLL_CALL not in body:
        return ["the Poll call is not inside DispatchPerFrameWork (N86: per-frame work runs only there)"]
    return []


def install_call_problems(boot: str) -> list[str]:
    """Install is called once from boot.cpp with the server flag; the gate and the skip log live in Install."""
    code = normalize(boot)
    if code.count(INSTALL_CALL) != 1:
        return [f"expected exactly one `{INSTALL_CALL}` in boot.cpp, found {code.count(INSTALL_CALL)}"]
    return []


def install_gate_problems(hook: str) -> list[str]:
    """Install returns on a server before it resolves an address or calls PatchDetour, and logs the skip."""
    code = strip_comments(hook)
    body = function_body(code, "bool Install(")
    if body is None:
        return ["Install not found in render_config_hook.cpp"]
    gate = re.search(r"if\s*\(\s*isServer\s*\)\s*\{(.*?)return\s+false;\s*\}", body, flags=re.S)
    if not gate:
        return ["Install has no `if (isServer) { ... return false; }` gate"]
    if SERVER_LOG not in gate.group(1):
        return [f"the server gate does not log `{SERVER_LOG}`"]
    for later in ("PatchDetour(", "ResolveVA_Checked("):
        if later in body and body.index(later) < gate.end():
            return [f"`{later}` comes before the server gate"]
    return []


def hook_problems(hook: str) -> list[str]:
    """Both hooks are named, prologue-validated, resolved with the checked resolver and installed with PatchDetour."""
    code = strip_comments(hook)
    problems = []
    for name in ("ApplyRuntimeGfxSettings", "GfxSettingsAutoSelect"):
        if not re.search(r'PatchDetour\(\s*&g_original\w+\s*,[^;]*"' + name + r'"\s*\)', code):
            problems.append(f'no PatchDetour(..., "{name}") naming the hook')
        if not re.search(r'PrologueMatches\(\s*"' + name + '"', code):
            problems.append(f"{name} is not prologue-validated before it is hooked")
    if len(re.findall(r"\bResolveVA_Checked\(", code)) != 2:
        problems.append("the two hook targets are not both resolved with ResolveVA_Checked")
    if "nevr::ValidatePrologue(" not in code:
        problems.append("PrologueMatches does not call nevr::ValidatePrologue")
    # The validation result gates the install: neither hook goes in when either prologue differs.
    if not re.search(r"if\s*\(\s*!applyOk\s*\|\|\s*!selectorOk\s*\)\s*return\s+false;", code):
        problems.append("a prologue mismatch does not stop both installs")
    if re.search(r"\bResolveVA_Unchecked\(|\bResolveVA\(", code):
        problems.append("an unchecked resolver is used for a hook target")
    return problems


SELECTOR_SIGNATURE = "std::uint64_t SelectorHook("
# The selector's detour fills the GPU score before the original selector reads it (#1419): that fill is its one step before
# the call, wrapped in the same try/catch every hook uses. Moving it after the call would make the selector read a zero score.
SCORE_FILL = "FillGpuScore();"
SCORE_FILL_WRAPPER = re.compile(r"\btry\s*\{\s*FillGpuScore\(\);\s*\}\s*catch\s*\(const std::exception&\)\s*\{[^}]*\}")


def game_path_problems(hook: str) -> list[str]:
    """The two detours call the original, do not log, sleep or throw on the game's path, and return its result. ApplyHook
    does nothing before the call; SelectorHook does only the score fill before it (#1419)."""
    code = strip_comments(hook)
    problems = []
    for signature, original in (("std::uint64_t ApplyHook(", "g_originalApply("),
                                (SELECTOR_SIGNATURE, "g_originalSelector(")):
        body = function_body(code, signature)
        if body is None:
            problems.append(f"{signature} not found")
            continue
        if original not in body:
            problems.append(f"{signature} does not call the original")
            continue
        before = body[: body.index(original)].replace("const std::uint64_t result =", "")
        if signature == SELECTOR_SIGNATURE:
            if SCORE_FILL not in before:
                problems.append(f"{signature} does not fill the GPU score before it calls the original (#1419)")
            elif SCORE_FILL_WRAPPER.sub("", before).strip():
                problems.append(f"{signature} does work before it calls the original other than the score fill (#1419)")
        elif "try" in before:
            problems.append(f"{signature} does work before it calls the original")
        for forbidden in ("Log(", "Sleep(", "throw ", "catch (...)", "catch(...)"):
            if forbidden in body:
                problems.append(f"{signature} uses `{forbidden}` on the game's path")
        if "return result;" not in body:
            problems.append(f"{signature} does not return the original's result")
    return problems


def model_purity_problems(files: dict[str, str]) -> list[str]:
    """The model is testable without Windows: no Windows header, no log call."""
    problems = []
    for path, text in files.items():
        code = strip_comments(text)
        for forbidden in ("<windows.h>", "<winsock2.h>", "Log(", "GetProcAddress", "ReadProcessMemory"):
            if forbidden in code:
                problems.append(f"{path} contains `{forbidden}`; the model is the pure half")
    return problems


def tag_problems(doc: str) -> list[str]:
    return [] if re.search(r"^\|\s*`\[NEVR\.RENDER\]`", doc, flags=re.M) else ["[NEVR.RENDER] is not in the tag table"]


def line_cap_problems(filter_source: str) -> list[str]:
    """The built-in filter's 500-byte line cap must not reach a [NEVR.] line: the render_config line is longer than 500
    bytes and a cap loses its last fields. ApplyTruncation returns the full length for a `[NEVR.` line before it applies
    max_line_length."""
    code = strip_comments(filter_source)
    body = function_body(code, "static int ApplyTruncation(")
    if body is None:
        return ["ApplyTruncation not found in builtin_filter.cpp"]
    exempt = body.find("if (IsNevrLine(message)) return len;")
    cap = body.find("g_config.max_line_length")
    if exempt < 0:
        return ["ApplyTruncation does not return a [NEVR.] line whole"]
    if cap < 0 or exempt > cap:
        return ["the [NEVR.]-line exemption must come before the max_line_length cap"]
    return []


class RenderConfigWiring(unittest.TestCase):
    def test_poll_is_called_once_inside_the_per_frame_dispatcher(self):
        self.assertEqual(poll_call_problems(TICK.read_text(encoding="utf-8")), [])

    def test_the_poll_check_fails_when_the_call_leaves_the_dispatcher(self):
        tick = TICK.read_text(encoding="utf-8")
        self.assertIn(POLL_CALL, tick)
        outside = tick.replace(POLL_CALL, "", 1).replace("}  // namespace nevr_frame",
                                                          "void Other(uint64_t nowUs) { " + POLL_CALL + " }\n\n}  // namespace nevr_frame")
        self.assertTrue(poll_call_problems(outside))
        self.assertTrue(poll_call_problems(tick.replace(POLL_CALL, "")))
        self.assertTrue(poll_call_problems(tick + "\n" + POLL_CALL + "\n"))
        self.assertTrue(poll_call_problems(tick.replace(POLL_CALL, "// " + POLL_CALL)))

    def test_install_is_called_once_from_boot_with_the_server_flag(self):
        self.assertEqual(install_call_problems(BOOT.read_text(encoding="utf-8")), [])

    def test_the_install_call_check_fails_when_the_flag_is_dropped_or_the_call_doubled(self):
        boot = BOOT.read_text(encoding="utf-8")
        flag_mutant = boot.replace("(nullptr)), g_isServer != FALSE,\n", "(nullptr)), false,\n", 1)
        self.assertNotEqual(flag_mutant, boot, "the anchor did not match the Install call, so the mutant was never made")
        self.assertTrue(install_call_problems(flag_mutant))
        self.assertTrue(install_call_problems(boot.replace("g_isHeadless != FALSE);", "false);", 1)))
        self.assertTrue(install_call_problems(normalize(boot).replace(INSTALL_CALL, "")))
        self.assertTrue(install_call_problems(boot + "\n" + INSTALL_CALL + "\n"))
        self.assertTrue(install_call_problems(normalize(boot).replace(INSTALL_CALL, "// " + INSTALL_CALL)))

    def test_install_returns_on_a_server_before_any_hook_work_and_logs_the_skip(self):
        self.assertEqual(install_gate_problems(HOOK.read_text(encoding="utf-8")), [])

    def test_the_gate_check_fails_without_the_gate_the_log_or_the_order(self):
        hook = HOOK.read_text(encoding="utf-8")
        self.assertTrue(install_gate_problems(hook.replace("if (isServer) {", "if (false) {", 1)))
        self.assertTrue(install_gate_problems(hook.replace(SERVER_LOG, "[NEVR.RENDER] something else")))
        moved = hook.replace("bool Install(std::uintptr_t gameBase, bool isServer, bool headless) {",
                             "bool Install(std::uintptr_t gameBase, bool isServer, bool headless) {\n  nevr::ResolveVA_Checked(gameBase, 0x140000000ULL);", 1)
        self.assertNotEqual(moved, hook, "the anchor did not match, so the mutant was never made")
        self.assertTrue(install_gate_problems(moved))

    def test_both_hooks_are_named_prologue_validated_and_installed_with_patchdetour(self):
        self.assertEqual(hook_problems(HOOK.read_text(encoding="utf-8")), [])

    def test_the_hook_check_fails_without_each_piece(self):
        hook = HOOK.read_text(encoding="utf-8")
        removals = (
            '"ApplyRuntimeGfxSettings");\n  }',
            'PrologueMatches("GfxSettingsAutoSelect"',
            "nevr::ValidatePrologue(",
            "if (!applyOk || !selectorOk) return false;",
        )
        for needle in removals:
            self.assertIn(needle, hook, needle)
        self.assertTrue(hook_problems(hook.replace('"ApplyRuntimeGfxSettings");\n  }', '"x");\n  }', 1)))
        self.assertTrue(hook_problems(hook.replace('PrologueMatches("GfxSettingsAutoSelect"', 'Other("GfxSettingsAutoSelect"', 1)))
        self.assertTrue(hook_problems(hook.replace("nevr::ValidatePrologue(", "Other(", 1)))
        self.assertTrue(hook_problems(hook.replace("if (!applyOk || !selectorOk) return false;", "", 1)))
        self.assertTrue(hook_problems(hook.replace("nevr::ResolveVA_Checked(gameBase, nevr_render_config::kApplyVa)",
                                                  "nevr::ResolveVA_Unchecked(gameBase, nevr_render_config::kApplyVa)", 1)))

    def test_the_detours_call_the_original_first_and_never_log_or_throw(self):
        self.assertEqual(game_path_problems(HOOK.read_text(encoding="utf-8")), [])

    def test_the_game_path_check_fails_on_each_violation(self):
        hook = HOOK.read_text(encoding="utf-8")
        marker = "const std::uint64_t result = g_originalApply(settings, game, logFlag);"
        self.assertIn(marker, hook)
        self.assertTrue(game_path_problems(hook.replace(marker, 'Log(EchoVR::LogLevel::Info, "x");\n  ' + marker, 1)))
        self.assertTrue(game_path_problems(hook.replace(marker, "Sleep(1);\n  " + marker, 1)))
        # ApplyHook's tail: the anchor must match, or the mutant is never made and the assertion passes on nothing.
        tail = "  return result;\n}\n\n// Writes the score"
        self.assertIn(tail, hook)
        self.assertTrue(game_path_problems(hook.replace(tail, "  return 0;\n}\n\n// Writes the score", 1)))
        early = hook.replace("  const auto returnAddress = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));\n  " + marker,
                             "  try { g_swallowed.fetch_add(0); } catch (const std::exception&) {}\n  " + marker, 1)
        self.assertNotEqual(early, hook)
        self.assertTrue(game_path_problems(early))
        selector_marker = "  const std::uint64_t result = g_originalSelector(settings, game);"
        self.assertIn(selector_marker, hook)
        self.assertIn(SCORE_FILL + "\n", hook)
        self.assertTrue(game_path_problems(hook.replace(selector_marker, "  Other();\n" + selector_marker, 1)))
        self.assertTrue(game_path_problems(hook.replace("    " + SCORE_FILL + "\n", "", 1)))

    def test_the_model_is_the_pure_half(self):
        files = {str(p.relative_to(ROOT)): p.read_text(encoding="utf-8") for p in (MODEL, MODEL_H)}
        self.assertEqual(model_purity_problems(files), [])

    def test_the_purity_check_fails_on_a_windows_call(self):
        files = {"m.cpp": "#include <windows.h>\n"}
        self.assertTrue(model_purity_problems(files))
        self.assertTrue(model_purity_problems({"m.cpp": 'void F() { Log(x); }'}))
        self.assertEqual(model_purity_problems({"m.cpp": "// Log( and <windows.h> in a comment only"}), [])

    def test_the_tag_is_registered(self):
        self.assertEqual(tag_problems(LOGGING_DOC.read_text(encoding="utf-8")), [])
        self.assertTrue(tag_problems("| `[NEVR.HOST]` | x |"))

    def test_an_nevr_line_is_not_cut_by_the_line_cap(self):
        text = FILTER.read_text(encoding="utf-8")
        self.assertEqual(line_cap_problems(text), [])

    def test_the_line_cap_check_fails_without_the_exemption_or_after_the_cap(self):
        text = FILTER.read_text(encoding="utf-8")
        gone = text.replace("if (IsNevrLine(message)) return len;", "")
        self.assertNotEqual(line_cap_problems(gone), [])
        moved = text.replace("if (IsNevrLine(message)) return len;", "").replace(
            "    return len;\n}\n\n/* ----", "    if (IsNevrLine(message)) return len;\n    return len;\n}\n\n/* ----", 1)
        self.assertNotEqual(line_cap_problems(moved), [])


if __name__ == "__main__":
    unittest.main()
