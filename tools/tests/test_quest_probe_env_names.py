"""#2189: the Quest checks' environment variables and the frame sensor's probe symbols carry no name of the deleted q15t plugin.

docs/standards/naming.md: an environment variable is NEVR_<NAME>. Two variables were spelled Q15T_*: the switch that lets a combat
check skip its qemu run (src/quest/combat/check/*.sh) and the one that makes a host test dump its module's log records
(src/quest/tests/dronekill_test.cpp, dronemuzzle_test.cpp). The assembly probes of the frame sensor
(src/quest/tests/frames_asm_probe_real.cpp) carry symbols the sensor test names (tests/quest/frames_asm_probes_test.go); a symbol
renamed on one side only is an undefined symbol at link time, or a sensor that checks nothing.
"""

import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CHECKS = ["dronemuzzle", "headset_gates", "powerboost"]
VERBOSE_TESTS = ["dronekill_test.cpp", "dronemuzzle_test.cpp"]
PROBE = ROOT / "src/quest/tests/frames_asm_probe_real.cpp"
SENSOR_TEST = ROOT / "tests/quest/frames_asm_probes_test.go"
OLD_VARIABLES = ("Q15T_SKIP_QEMU", "Q15T_TEST_VERBOSE")
PROBE_SYMBOLS = [
    "probe_dm_tp_ret", "probe_dm_tp_next", "probe_dm_fire_ret", "probe_dm_fire_next", "probe_pb_cone_ret", "probe_pb_resolve_real",
    "probe_dm_teleport", "probe_dm_fire", "probe_pb_cone_at", "probe_dm_tp_thunk", "probe_dm_fire_thunk", "probe_pb_resolve_thunk",
]


def tracked(*paths: str) -> list[Path]:
    out = subprocess.run(["git", "-C", str(ROOT), "ls-files", "-z", "--", *paths], check=True, capture_output=True).stdout
    return [ROOT / p for p in out.decode().split("\0") if p]


class EnvironmentVariableNames(unittest.TestCase):
    def test_no_source_names_a_q15t_variable_of_the_two_renamed(self):
        for path in tracked("src", "tools", "justfile", "tests", ".github"):
            if path == Path(__file__).resolve() or not path.is_file():
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            for old in OLD_VARIABLES:
                self.assertNotIn(old, text, f"{path.relative_to(ROOT)} still names {old}")

    def test_each_combat_check_reads_the_skip_variable_and_says_so(self):
        for name in CHECKS:
            text = (ROOT / f"src/quest/combat/check/{name}.sh").read_text()
            self.assertIn('"${NEVR_SKIP_QEMU:-0}" = 1', text, name)
            self.assertIn("SKIPPED (NEVR_SKIP_QEMU=1): qemu-aarch64 not found", text, name)
            self.assertIn("(NEVR_SKIP_QEMU=1 to skip explicitly)", text, name)

    def test_each_verbose_test_reads_the_verbose_variable(self):
        for name in VERBOSE_TESTS:
            text = (ROOT / "src/quest/tests" / name).read_text()
            self.assertIn('std::getenv("NEVR_TEST_VERBOSE")', text, name)


class ProbeSymbols(unittest.TestCase):
    def test_the_probe_names_no_q15t_symbol_and_defines_each_probe_symbol(self):
        text = PROBE.read_text()
        self.assertNotRegex(text, r"(?i)q15t")
        for sym in PROBE_SYMBOLS:
            self.assertRegex(text, rf"\b{sym}\b", sym)

    def test_the_sensor_test_expects_the_renamed_entries(self):
        text = SENSOR_TEST.read_text()
        self.assertNotRegex(text, r"(?i)q15t")
        for sym in ("probe_dm_tp_thunk", "probe_dm_fire_thunk", "probe_pb_resolve_thunk"):
            self.assertRegex(text, rf"\b{sym}\b", sym)

    def test_every_string_the_probe_asm_names_is_a_defined_symbol(self):
        text = PROBE.read_text()
        words = re.findall(r'"(probe_[a-z_]+)"', text)
        self.assertEqual(len(set(words)), 9, "the asm bodies name nine symbols as string literals")
        for word in words:
            self.assertIn(word, PROBE_SYMBOLS)
            self.assertRegex(text, rf"\b{word}\s*(=|\()", word)


if __name__ == "__main__":
    unittest.main()
