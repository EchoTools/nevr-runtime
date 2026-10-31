"""`just test-auth-unit` builds every test binary it runs.

The recipe lists the binaries it builds (`--target`) apart from the ones it runs (`bin=...`, or a `for test_name in`
loop), so a binary added to one list and not the other makes the recipe stop with `GTest binary not found` after
every earlier binary has run (#1805, test_early_quit_feature_flags in #1803).
"""
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]


def recipe_text():
    justfile = (REPO / "justfile").read_text(encoding="utf-8")
    match = re.search(r"^test-auth-unit:\n((?:[ \t]+.*\n|\n)*)", justfile, re.M)
    if match is None:
        raise AssertionError("no test-auth-unit recipe in the justfile")
    return match.group(1)


def code_lines(recipe):
    return "\n".join(line for line in recipe.splitlines() if not line.lstrip().startswith("#"))


def built_targets(code):
    targets = set(re.findall(r"--target\s+([A-Za-z0-9_]+)", code))
    # `--target a --target b test_c test_d`: cmake takes the words after the last flag as further targets.
    for line in code.splitlines():
        if "cmake --build" in line and "--target" in line:
            targets.update(re.findall(r"(?<=\s)(test_[A-Za-z0-9_]+)(?=\s|$)", line))
    return targets


def run_binaries(code):
    names = set(re.findall(r'bin="[^"]*/bin/(?:[A-Za-z0-9_-]+/)?(test_[A-Za-z0-9_]+)\.exe"', code))
    for loop in re.findall(r"for test_name in ([^;]+); do", code):
        names.update(loop.split())
    for loop in re.findall(r"for bin in ([^;]+); do", code):
        names.update(re.findall(r"/bin/(test_[A-Za-z0-9_]+)\.exe", loop))
    return names


class AuthUnitBuildsWhatItRuns(unittest.TestCase):
    def test_every_binary_the_recipe_runs_is_a_build_target_of_the_recipe(self):
        code = code_lines(recipe_text())
        built = built_targets(code)
        run = run_binaries(code)
        self.assertGreater(len(run), 60, "the scan reads too few run lines; it is watching nothing")
        self.assertGreater(len(built), 60, "the scan reads too few --target words; it is watching nothing")
        # A few binaries are built by an earlier recipe step in a directory of their own; those are not in the loop.
        self.assertEqual(sorted(run - built), [], "run but never built by test-auth-unit")


if __name__ == "__main__":
    unittest.main()
