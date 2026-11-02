"""The shared justfile recipe finder (just_recipe.py) finds a recipe whatever its header form (#1910).

The header forms are the ones the justfile uses now (`name *args: (_heavy-guard "name" args)`, a parameter with a
default, several dependencies) and the plain `name:`. The old finder matched only `^name:\\n`, which is why eleven test
files reported their recipe missing after #1871 changed the headers.
"""
import pathlib
import subprocess
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import just_recipe  # noqa: E402

TEXT = """\
preset := env("PRESET", "mingw-release")

# Plain header.
plain:
    echo plain

# Variadic parameter, the heavy guard as a dependency in parentheses.
[positional-arguments]
guarded *args: (_heavy-guard "guarded" args)
    #!/usr/bin/env bash
    set -euo pipefail

    echo guarded
    echo after blank

# A parameter with a default and two dependencies.
with-default rev="origin/main": plain guarded
    echo "$rev"

# Column-0 comment ends the body above.
test-auth *args: (_heavy-guard "test-auth" args) plain
    echo auth

test-auth-unit *args: (_heavy-guard "test-auth-unit" args)
    echo unit
"""


class HeaderTest(unittest.TestCase):
    def test_a_plain_header_is_found(self):
        self.assertEqual(just_recipe.header(TEXT, "plain"), "plain:")

    def test_a_variadic_header_with_a_guard_dependency_is_found(self):
        self.assertEqual(just_recipe.header(TEXT, "guarded"), 'guarded *args: (_heavy-guard "guarded" args)')

    def test_a_parameter_with_a_default_and_dependencies_is_found(self):
        self.assertEqual(just_recipe.header(TEXT, "with-default"), 'with-default rev="origin/main": plain guarded')

    def test_a_name_that_prefixes_another_recipe_finds_its_own_header(self):
        self.assertEqual(just_recipe.header(TEXT, "test-auth"), 'test-auth *args: (_heavy-guard "test-auth" args) plain')
        self.assertEqual(just_recipe.header(TEXT, "test-auth-unit"),
                         'test-auth-unit *args: (_heavy-guard "test-auth-unit" args)')

    def test_an_assignment_is_not_a_recipe(self):
        self.assertIsNone(just_recipe.header(TEXT, "preset"))
        self.assertNotIn("preset", just_recipe.names(TEXT))

    def test_a_missing_recipe_is_none(self):
        self.assertIsNone(just_recipe.header(TEXT, "absent"))
        self.assertIsNone(just_recipe.body(TEXT, "absent"))
        self.assertIsNone(just_recipe.recipe(TEXT, "absent"))


class BodyTest(unittest.TestCase):
    def test_a_variadic_recipe_keeps_its_shebang_and_its_blank_lines(self):
        self.assertEqual(just_recipe.body(TEXT, "guarded"),
                         "    #!/usr/bin/env bash\n    set -euo pipefail\n\n    echo guarded\n    echo after blank\n")

    def test_the_body_ends_before_an_attribute_line_and_a_column_zero_comment(self):
        self.assertEqual(just_recipe.body(TEXT, "plain"), "    echo plain\n")
        self.assertNotIn("[positional-arguments]", just_recipe.body(TEXT, "plain"))

    def test_the_body_ends_at_the_next_header(self):
        self.assertEqual(just_recipe.body(TEXT, "test-auth"), "    echo auth\n")

    def test_the_recipe_is_the_header_line_and_the_body(self):
        self.assertEqual(just_recipe.recipe(TEXT, "with-default"),
                         'with-default rev="origin/main": plain guarded\n    echo "$rev"\n')

    def test_a_recipe_with_no_body_has_an_empty_body(self):
        self.assertEqual(just_recipe.body("empty:\n\nnext:\n    echo\n", "empty"), "")


class NamesTest(unittest.TestCase):
    def test_the_names_are_the_recipes_in_file_order(self):
        self.assertEqual(just_recipe.names(TEXT),
                         ["plain", "guarded", "with-default", "test-auth", "test-auth-unit"])


class RealJustfileTest(unittest.TestCase):
    """The repository's own justfile, in the forms it has now."""

    def setUp(self):
        self.text = just_recipe.read_justfile()

    def test_the_verify_recipe_is_found_with_its_shebang(self):
        body = just_recipe.body(self.text, "verify")
        self.assertIsNotNone(body)
        self.assertEqual(body.splitlines()[0].strip(), "#!/usr/bin/env bash")

    def test_the_test_auth_unit_recipe_is_found_and_runs_its_binaries(self):
        body = just_recipe.body(self.text, "test-auth-unit")
        self.assertIsNotNone(body)
        self.assertIn('timeout -k 10 900 wine "$1"', body)

    def test_build_android_is_found_and_runs_one_cmake_build_with_the_job_count(self):
        body = just_recipe.body(self.text, "build-android")
        self.assertEqual(body.strip(), 'cmake --build build/android-arm64 -j "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"')

    def test_every_public_recipe_is_named(self):
        summary = subprocess.run(["just", "--summary", "--unsorted"], capture_output=True, text=True,
                                 cwd=just_recipe.REPO).stdout.split()
        self.assertTrue(summary, "just --summary printed no recipes")
        self.assertEqual([name for name in summary if name not in just_recipe.names(self.text)], [])


if __name__ == "__main__":
    unittest.main()
