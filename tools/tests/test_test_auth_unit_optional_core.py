"""`just test-auth-unit` builds and runs `test_update_core_link` only when the configured tree defines it (#2287).

`test_update_core_link` is added to the build only under `NEVR_UPDATE_CORE_ENABLED` (src/runtime/CMakeLists.txt). The
CI builder image has no Rust, so the target is absent there, and an unconditional `--target` stops the recipe with
`ninja: error: unknown target` before any later test binary runs. The recipe must ask the build graph first, build the
target only when it is there, and skip it with one line when it is not.
"""
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import just_recipe  # noqa: E402

CORE = "test_update_core_link"


def code_lines(recipe):
    return [line for line in recipe.splitlines() if not line.lstrip().startswith("#")]


class OptionalCoreTest(unittest.TestCase):
    def setUp(self):
        body = just_recipe.body(just_recipe.read_justfile(), "test-auth-unit")
        self.assertIsNotNone(body, "no test-auth-unit recipe in the justfile")
        self.code = code_lines(body)

    def test_the_recipe_asks_the_build_graph_whether_the_core_target_exists(self):
        text = "\n".join(self.code)
        self.assertIn("-t targets all", text, "the recipe does not ask the build graph for its targets")
        self.assertIn(f"grep -qw {CORE}", text, "the recipe does not check for the core target before using it")

    def test_no_cmake_build_line_builds_the_core_target_unconditionally(self):
        builds = [line for line in self.code if "cmake --build" in line]
        self.assertTrue(builds, "the recipe builds nothing with cmake")
        for line in builds:
            self.assertNotIn(CORE, line, f"unconditional build of {CORE}: {line.strip()}")

    def test_a_missing_core_target_is_skipped_with_one_line_naming_it(self):
        text = "\n".join(self.code)
        self.assertIn(f"skipping {CORE}", text, "a tree without the core target does not say it skipped it")


if __name__ == "__main__":
    unittest.main()
