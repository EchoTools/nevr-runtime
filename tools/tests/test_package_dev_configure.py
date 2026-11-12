"""package-dev configures the Windows dev tree through the `configure` recipe, not by its own vcpkg and cmake lines (#1959).

`configure` depends on generate-symcache, _proto-current and _build-inputs; a package that configures without them can
ship from a stale gen/ or a stale symcache. The check reads the justfile text, so it fails on the recipe as written.
"""

from __future__ import annotations

import unittest

import just_recipe


class PackageDevConfigureTest(unittest.TestCase):
    def setUp(self):
        self.text = just_recipe.read_justfile()
        self.body = just_recipe.body(self.text, "package-dev")

    def test_package_dev_has_a_body(self):
        self.assertTrue(self.body, "package-dev is not found in the justfile")

    def test_package_dev_configures_through_the_configure_recipe(self):
        self.assertIn("just preset=mingw-dev configure", self.body)

    def test_package_dev_does_not_run_its_own_vcpkg_or_cmake_configure(self):
        self.assertNotIn("just preset=mingw-dev _vcpkg-mingw", self.body)
        self.assertNotIn("cmake --preset mingw-dev", self.body)


if __name__ == "__main__":
    unittest.main()
