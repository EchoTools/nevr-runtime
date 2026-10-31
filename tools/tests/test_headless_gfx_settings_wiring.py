"""#1774 P3: the headless detour of ApplyRuntimeGfxSettings is wired where the NOP was, and render_config_hook leaves the
address to it on a headless run. Source-order sensors; the behaviour is in src/runtime/tests/test_headless_gfx_settings.cpp.
Each check takes the text it reads, so a test can hand it a broken copy and see it fail."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODE_PATCHES = ROOT / "src/runtime/patch/mode_patches.cpp"
HOOK = ROOT / "src/runtime/patch/headless_gfx_settings.cpp"
RENDER_HOOK = ROOT / "src/runtime/patch/render_config_hook.cpp"
ADDRESSES = ROOT / "src/runtime/hook/addresses.h"

INSTALL_IN_HEADLESS = "nevr_headless_gfx_settings::Install(reinterpret_cast<std::uintptr_t>(EchoVR::g_GameBaseAddress));"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


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


def headless_install_problems(mode_patches: str, addresses: str) -> list[str]:
    """PatchEnableHeadless installs the detour, and the NOP over the call site is gone from it and from addresses.h."""
    code = strip_comments(mode_patches)
    body = function_body(code, "VOID PatchEnableHeadless(PVOID pGame)")
    if body is None:
        return ["PatchEnableHeadless not found in mode_patches.cpp"]
    problems = []
    if body.count(INSTALL_IN_HEADLESS) != 1:
        problems.append("PatchEnableHeadless does not install the headless apply detour exactly once")
    if "HEADLESS_APPLY_GRAPHICS" in code or "graphicsNop" in code:
        problems.append("the call-site NOP of ApplyRuntimeGfxSettings is still in mode_patches.cpp")
    if "HEADLESS_APPLY_GRAPHICS" in strip_comments(addresses):
        problems.append("addresses.h still defines the NOP's address")
    return problems


def hook_problems(hook: str) -> list[str]:
    """The detour returns 0 without entering the original, counts itself in liveness, and validates the prologue before
    it detours the address."""
    code = strip_comments(hook)
    problems = []
    apply_hook = function_body(code, "__attribute__((noinline)) std::uint64_t ApplyHook(")
    if apply_hook is None:
        return ["ApplyHook not found"]
    if "Mark(nevr_hook_liveness::kApplyGfxSettings)" not in apply_hook:
        problems.append("ApplyHook does not count itself in HookLiveness")
    if "return 0;" not in apply_hook:
        problems.append("ApplyHook does not return 0")
    if "g_original(" in apply_hook:
        problems.append("ApplyHook enters the original on a headless run")
    install = function_body(code, "bool Install(std::uintptr_t gameBase)")
    if install is None:
        return problems + ["Install not found"]
    validate = install.find("nevr::ValidatePrologue(")
    detour = install.find("PatchDetour(&g_original")
    if validate < 0 or detour < 0 or validate > detour:
        problems.append("Install does not validate the prologue before the detour")
    if '"ApplyRuntimeGfxSettings")' not in install:
        problems.append("the detour is not named ApplyRuntimeGfxSettings")
    return problems


def render_gate_problems(render_hook: str) -> list[str]:
    """On a headless run the render observer does not detour ApplyRuntimeGfxSettings; the selector detour is unconditional."""
    code = strip_comments(render_hook)
    if not re.search(r"if\s*\(\s*!headless\s*\)\s*\{[^}]*PatchDetour\(\s*&g_originalApply", code, flags=re.S):
        return ["the Apply detour is not inside `if (!headless)`"]
    if re.search(r"if\s*\(\s*!headless\s*\)\s*\{[^}]*PatchDetour\(\s*&g_originalSelector", code, flags=re.S):
        return ["the selector detour is inside `if (!headless)`, so a headless run would lose the GPU-score fill (#1419)"]
    if "PatchDetour(&g_originalSelector" not in code:
        return ["the selector detour is missing"]
    return []


class HeadlessGfxSettingsWiring(unittest.TestCase):
    def test_the_headless_detour_replaces_the_call_site_nop(self):
        self.assertEqual(
            headless_install_problems(MODE_PATCHES.read_text(encoding="utf-8"), ADDRESSES.read_text(encoding="utf-8")), [])

    def test_the_install_check_fails_when_the_nop_returns_or_the_detour_is_dropped(self):
        mode = MODE_PATCHES.read_text(encoding="utf-8")
        addresses = ADDRESSES.read_text(encoding="utf-8")
        self.assertTrue(headless_install_problems(mode.replace(INSTALL_IN_HEADLESS, "", 1), addresses))
        nop = "  ApplyPatch(HEADLESS_APPLY_GRAPHICS, graphicsNop, sizeof(graphicsNop));\n"
        self.assertTrue(headless_install_problems(mode.replace(INSTALL_IN_HEADLESS, INSTALL_IN_HEADLESS + "\n" + nop, 1), addresses))
        self.assertTrue(headless_install_problems(mode, addresses + "\nconstexpr uintptr_t HEADLESS_APPLY_GRAPHICS = 0x109209;\n"))

    def test_the_detour_counts_itself_returns_zero_and_validates_before_it_detours(self):
        self.assertEqual(hook_problems(HOOK.read_text(encoding="utf-8")), [])

    def test_the_hook_check_fails_on_each_violation(self):
        hook = HOOK.read_text(encoding="utf-8")
        self.assertTrue(hook_problems(hook.replace("Mark(nevr_hook_liveness::kApplyGfxSettings);", "", 1)))
        self.assertTrue(hook_problems(hook.replace("  return 0;\n}", "  return g_original(settings, game, logFlag);\n}", 1)))
        self.assertTrue(hook_problems(hook.replace("nevr::ValidatePrologue(", "Other(", 1)))
        self.assertTrue(hook_problems(hook.replace('"ApplyRuntimeGfxSettings");', '"x");', 1)))

    def test_the_render_observer_leaves_the_apply_to_the_headless_detour(self):
        self.assertEqual(render_gate_problems(RENDER_HOOK.read_text(encoding="utf-8")), [])

    def test_the_render_gate_check_fails_without_the_gate(self):
        render = RENDER_HOOK.read_text(encoding="utf-8")
        self.assertTrue(render_gate_problems(render.replace("if (!headless) {", "if (true) {", 1)))
        self.assertTrue(render_gate_problems(render.replace("  if (!headless) {\n    g_originalApply", "  {\n    g_originalApply", 1)))
        self.assertTrue(render_gate_problems(render.replace("  BOOL applyHooked = TRUE;", "  BOOL applyHooked = TRUE;\n  if (!headless) { PatchDetour(&g_originalSelector, nullptr, \"x\"); }", 1)))


if __name__ == "__main__":
    unittest.main()
