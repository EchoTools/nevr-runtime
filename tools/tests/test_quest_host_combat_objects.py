"""Every host test program that links the combat install table takes its combat sources from one object library (#2271).

The table (src/quest/combat/combat_install.cpp) names every combat module, so each program that compiles it must also compile
every module it names. Before #2271 each such program repeated that list in its own SRC, so one new module meant editing six or
more lists, and a missed list failed only when that program was built. The check reads the host-test CMake groups as text (no
CMake run is needed) and asserts:

- the combat sources are listed once, in the object library `o_combat_modules`, and that library holds combat_install.cpp;
- every nevr_ht_test or nevr_ht_rerun whose SRC names combat_install.cpp links `o_combat_modules` through OBJS and lists no
  combat module source of its own;
- the programs that name combat_install.cpp are the ones this file knows about, so a new one cannot skip the check.
"""

from __future__ import annotations

import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
GROUPS = REPO / "src" / "quest" / "host_tests" / "groups"
LIBRARY = "o_combat_modules"
COMBAT_INSTALL = "src/quest/combat/combat_install.cpp"

# The six programs that compile the combat install table today (#2271's evidence): combat_install_test, sendguard_test,
# heap_step_test, combat_tick_test, refresh_rate_test and render_config_hook_test.
PROGRAMS = {
    "combat_install_test",
    "sendguard_test",
    "heap_step_test",
    "combat_tick_test",
    "refresh_rate_test",
    "render_config_hook_test",
}


def _calls(text: str):
    """Yield (macro, name, body) for each nevr_ht_test / nevr_ht_rerun / nevr_ht_obj call, the body being its arguments."""
    for m in re.finditer(r"\bnevr_ht_(test|rerun|obj)\(\s*(\w+)", text):
        depth = 0
        start = m.end()
        for i in range(m.start(), len(text)):
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
                if depth == 0:
                    yield m.group(1), m.group(2), text[start:i]
                    break


def _sources(body: str) -> list[str]:
    """The SRC entries of one call, as written (variables unexpanded)."""
    if "SRC" not in body:
        return []
    return body.split("SRC", 1)[1].split()


def _objs(body: str) -> list[str]:
    if "OBJS" not in body:
        return []
    tail = body.split("OBJS", 1)[1]
    return [a or b for a, b in re.findall(r"\$\{(\w+)\}|\b(o_\w+)\b", tail.split("SRC", 1)[0])]


def _all_calls():
    for path in sorted(GROUPS.glob("*.cmake")):
        for macro, name, body in _calls(path.read_text()):
            yield path.name, macro, name, body


class CombatHostObjectsTest(unittest.TestCase):
    def setUp(self):
        self.calls = list(_all_calls())

    def test_the_combat_sources_are_one_object_library_that_holds_the_install_table(self):
        libs = [(file, body) for file, macro, name, body in self.calls if macro == "obj" and name == LIBRARY]
        self.assertEqual(len(libs), 1, "exactly one nevr_ht_obj(o_combat_modules ...) in the host test groups")
        self.assertIn(COMBAT_INSTALL, libs[0][1].split("SRC", 1)[1])

    def test_every_program_that_links_the_library_is_one_of_the_install_table_programs(self):
        linking = {name for _, macro, name, body in self.calls if macro in ("test", "rerun") and LIBRARY in _objs(body)}
        self.assertEqual(linking, PROGRAMS)

    def test_no_other_program_compiles_the_install_table_itself(self):
        for file, macro, name, body in self.calls:
            if macro not in ("test", "rerun") or LIBRARY in _objs(body):
                continue
            with self.subTest(program=name):
                self.assertNotIn(COMBAT_INSTALL, _sources(body), f"{name} ({file}) compiles combat_install.cpp outside {LIBRARY}")

    def test_the_six_programs_keep_their_own_tests_and_no_combat_source(self):
        for _, macro, name, body in self.calls:
            if macro == "test" and name in PROGRAMS:
                with self.subTest(program=name):
                    own = [s for s in _sources(body) if s.startswith("src/quest/combat/")]
                    self.assertEqual(own, [], f"{name} lists combat sources of its own")


if __name__ == "__main__":
    unittest.main()
