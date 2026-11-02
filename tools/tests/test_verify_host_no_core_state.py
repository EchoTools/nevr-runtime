"""verify_host_no_core_state.py: the host shares no writable data symbol with the core or ABI library."""

import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import verify_host_no_core_state as sensor  # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parent))
import just_recipe  # noqa: E402

TOOL = Path(__file__).resolve().parents[1] / "verify_host_no_core_state.py"

HOST = """\
0000000180001000 T DllMain
0000000180002000 D _ZN8nevr_host10g_recorderE
0000000180003000 b _ZL8g_active
0000000180004000 B __mingw_state
0000000180005000 d .data
"""
CORE = """\
0000000000000000 B g_isServer
0000000000000004 b _ZL13g_bootLogSink
0000000000000010 d _ZZ8GetRunIdvE7s_runId
0000000000000000 T Log
                 U memcpy
"""
ABI = """\
0000000000000000 B g_pGame
0000000000000008 D g_GameBaseAddress
"""


class DataSymbolsTest(unittest.TestCase):
    def test_only_writable_data_is_read(self):
        self.assertEqual(sensor.data_symbols(CORE), {"g_isServer", "_ZL13g_bootLogSink", "_ZZ8GetRunIdvE7s_runId"})

    def test_section_symbols_and_code_and_undefined_are_not_names(self):
        self.assertEqual(sensor.data_symbols(HOST), {"_ZN8nevr_host10g_recorderE", "_ZL8g_active", "__mingw_state"})

    def test_common_symbols_count(self):
        self.assertEqual(sensor.data_symbols("0000000000000004 C g_shared\n"), {"g_shared"})


class CheckTest(unittest.TestCase):
    def test_a_host_that_shares_nothing_passes(self):
        host_count, counts = sensor.check(HOST, {"core": CORE, "abi": ABI})
        self.assertEqual(host_count, 3)
        self.assertEqual(counts, {"core": 3, "abi": 2})

    def test_a_core_global_linked_into_the_host_fails_and_is_named(self):
        host = HOST + "0000000180006000 B g_isServer\n"
        with self.assertRaisesRegex(sensor.SensorError, r"core: g_isServer"):
            sensor.check(host, {"core": CORE, "abi": ABI})

    def test_an_abi_global_fails_too(self):
        with self.assertRaisesRegex(sensor.SensorError, r"abi: g_pGame"):
            sensor.check(HOST + "0000000180006000 b g_pGame\n", {"core": CORE, "abi": ABI})

    def test_a_function_local_static_of_the_core_fails(self):
        with self.assertRaisesRegex(sensor.SensorError, "_ZZ8GetRunIdvE7s_runId"):
            sensor.check(HOST + "0000000180006000 d _ZZ8GetRunIdvE7s_runId\n", {"core": CORE})

    def test_a_stripped_host_is_a_check_that_saw_nothing(self):
        with self.assertRaisesRegex(sensor.SensorError, "defines no writable data symbols: it is stripped"):
            sensor.check("0000000180001000 T DllMain\n", {"core": CORE})

    def test_a_library_with_no_data_is_a_check_that_saw_nothing(self):
        with self.assertRaisesRegex(sensor.SensorError, "core defines no writable data symbols"):
            sensor.check(HOST, {"core": "0000000000000000 T Log\n"})


class CommandLineTest(unittest.TestCase):
    """The tool as the recipe runs it, with a fake nm that prints a canned listing per path."""

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="host-core-state-"))
        self.addCleanup(lambda: __import__("shutil").rmtree(self.dir, ignore_errors=True))

    def fake_nm(self, listings, exit_code=0):
        for name, text in listings.items():
            (self.dir / name).write_text(text)
        script = self.dir / "nm"
        script.write_text('#!/bin/sh\nfor last; do :; done\nfile="$last.listing"\n'
                          f'if [ {exit_code} -ne 0 ]; then echo "nm: bad file" >&2; exit {exit_code}; fi\n'
                          'cat "$file"\n')
        script.chmod(script.stat().st_mode | stat.S_IEXEC)
        return script

    def run_tool(self, nm, *extra):
        return subprocess.run([sys.executable, "-I", str(TOOL), "--nm", str(nm), "--host-dll", str(self.dir / "host.dll"),
                               "--lib", str(self.dir / "core.a"), *extra], capture_output=True, text=True)

    def test_clean_passes_with_exit_0(self):
        nm = self.fake_nm({"host.dll.listing": HOST, "core.a.listing": CORE})
        result = self.run_tool(nm)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("verify_host_no_core_state: OK", result.stdout)

    def test_shared_state_exits_1(self):
        nm = self.fake_nm({"host.dll.listing": HOST + "0000000180006000 B g_isServer\n", "core.a.listing": CORE})
        result = self.run_tool(nm)
        self.assertEqual(result.returncode, 1)
        self.assertIn("g_isServer", result.stdout)

    def test_nm_failing_is_a_failure_not_a_pass(self):
        nm = self.fake_nm({"host.dll.listing": HOST, "core.a.listing": CORE}, exit_code=2)
        result = self.run_tool(nm)
        self.assertEqual(result.returncode, 1)
        self.assertIn("nm exited 2", result.stdout)

    def test_a_missing_nm_is_a_failure_not_a_pass(self):
        result = self.run_tool(self.dir / "no-such-nm")
        self.assertEqual(result.returncode, 1)
        self.assertIn("verify_host_no_core_state: FAIL", result.stdout)


class RecipeWiringTest(unittest.TestCase):
    """`just verify` runs the check on the unstripped build output, against both libraries, with no escape hatch."""

    def recipe(self):
        recipe = just_recipe.recipe((Path(__file__).resolve().parents[2] / "justfile").read_text(), "verify")
        if recipe is None:
            raise AssertionError("justfile has no verify recipe")
        return recipe

    def test_verify_runs_the_tool_on_the_unstripped_host_against_core_and_abi(self):
        lines = [line for line in self.recipe().splitlines() if "tools/verify_host_no_core_state.py" in line]
        self.assertEqual(len(lines), 1, lines)
        line = lines[0].strip()
        self.assertTrue(line.startswith("python3 tools/verify_host_no_core_state.py "), line)
        self.assertIn('--host-dll "build/{{ preset }}/bin/BugSplat64.dll"', line)
        self.assertIn('--lib "build/{{ preset }}/src/core/libnevr_core.a"', line)
        self.assertIn('--lib "build/{{ preset }}/src/abi/libnevr_abi.a"', line)
        for escape in ("||", "true", "dist/", "strip"):
            self.assertNotIn(escape, line)


if __name__ == "__main__":
    unittest.main()
