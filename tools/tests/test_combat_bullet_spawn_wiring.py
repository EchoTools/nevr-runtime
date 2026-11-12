"""The bullet spawn's one detour (#2336, src/runtime/combat/bullet_spawn_hook.h): the wiring a host test cannot reach.

The detour, its order (PowerBoost's drop, the game's spawn, DroneFix's after filter) and its refusals are driven by
src/runtime/tests/test_combat_bullet_spawn.cpp against a fake game image. What stays here is read as source: each of the two
ported modules sets its filter through the one owner and no longer puts a detour of its own on the spawn, which is what the
write gate refused (OVERCHARGE and MEDIGUN never started); each module takes its filter off again when its install fails.

Python because every test in tools/tests is Python; the file reads source text and runs nothing."""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import strip_comments

ROOT = Path(__file__).resolve().parents[2]
COMBAT = ROOT / "src/runtime/combat"
DRONEFIX = COMBAT / "dronefix.cpp"
POWERBOOST = COMBAT / "powerboost.cpp"
OWNER = COMBAT / "bullet_spawn_hook.h"


def code(path):
    return strip_comments(path.read_text())


def body(text, signature):
    """The text of the function whose definition starts with `signature`, up to its closing brace at column 0."""
    start = text.index(signature)
    return text[start : text.index("\n}\n", start)]


class OneDetour(unittest.TestCase):
    def test_no_module_detours_the_spawn_itself(self):
        for path in (DRONEFIX, POWERBOOST):
            text = code(path)
            self.assertNotIn("Real_BulletSpawn", text, path.name)
            self.assertNotIn("Hooked_BulletSpawn", text, path.name)
            self.assertIsNone(re.search(r"\b0xce2020\b", text, re.I), f"{path.name}: the RVA is the owner's (kRva)")

    def test_the_owner_names_the_spawn_rva_and_its_prologue(self):
        text = code(OWNER)
        self.assertIn("kRva = 0xce2020;", text)
        self.assertIn("kPrologue[12] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};", text)


class DroneFixFilter(unittest.TestCase):
    def test_install_chase_sets_the_after_filter_and_clears_it_when_the_transaction_fails(self):
        chase = body(code(DRONEFIX), "void InstallChase(uintptr_t base)")
        set_at = chase.index("bullet_spawn::SetAfter(base, AfterBulletSpawn, kCatModule)")
        commit_at = chase.index("DetourTransactionCommit()")
        self.assertLess(set_at, commit_at, "the filter is set before the transaction, so a failure can take it out")
        self.assertRegex(chase, r"if \(rc != NO_ERROR\) nevr_combat::bullet_spawn::SetAfter\(base, nullptr, kCatModule\);")

    def test_the_after_filter_is_the_tracer_rule(self):
        after = body(code(DRONEFIX), "void AfterBulletSpawn(const nevr_combat::bullet_spawn::Call &call)")
        self.assertIn("g_bullet_cs = sys;", after)
        self.assertIn("shooter_pos[i] = pose->p[i]", after)


class PowerBoostFilter(unittest.TestCase):
    def test_install_medigun_sets_the_drop_filter_with_medigun_on(self):
        medigun = body(code(POWERBOOST), "void InstallMedigun(uint8_t *base)")
        self.assertRegex(
            medigun,
            r"if \(g_medigun_on && Check\(base \+ GUN_FIRE_RVA[^\n]*\n(?:[^\n]*\n){2}\s*g_spawn_filter_set = "
            r"nevr_combat::bullet_spawn::SetDrop\(reinterpret_cast<uintptr_t>\(base\), DropBulletSpawn, kCatModule\);",
        )
        self.assertNotIn("AttachTracked(&(PVOID &)Real_BulletSpawn", medigun)

    def test_a_failed_install_and_uninstall_clear_the_drop_filter(self):
        text = code(POWERBOOST)
        install = body(text, "bool Install(bool server)")
        self.assertRegex(
            install,
            r"if \(rc != NO_ERROR && g_spawn_filter_set\) \{\s*nevr_combat::bullet_spawn::SetDrop\("
            r"reinterpret_cast<uintptr_t>\(base\), nullptr, kCatModule\);\s*g_spawn_filter_set = false;",
        )
        uninstall = body(text, "void Uninstall()")
        self.assertRegex(uninstall, r"if \(g_spawn_filter_set\) nevr_combat::bullet_spawn::SetDrop\([^;]*, nullptr, kCatModule\);")

    def test_the_drop_filter_keeps_the_party_rule(self):
        drop = body(code(POWERBOOST), "bool DropBulletSpawn(const nevr_combat::bullet_spawn::Call &)")
        self.assertIn('GetPrivateProfileIntA("party", "drop_bullet", 1, g_ini_path)', drop)
        self.assertIn("return true;", drop)
        self.assertIn("return false;", drop)


if __name__ == "__main__":
    unittest.main()
