"""#1122: the host says whether it is a dev build, and the option reaches that literal through CMake.

The literal's value and the record that carries it are pinned by test_host_runtime_load and test_host_runtime_verify;
the release path's refusal of a dev host by test_package_release. This reads the one place a GTest cannot: the CMake
definition that makes the dev build's host say `dev`.
"""
import pathlib
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]


class DevBuildMarksTheHost(unittest.TestCase):
    """The host carries no plugin-trust literal, so the option reaches its NEVR-HOST-BUILD literal through the
    definition CMake puts on host/host_identity.cpp; the release path (package_release.py) reads that literal."""

    def test_the_option_defines_itself_on_the_hosts_identity_unit(self):
        text = (REPO / "src/runtime/CMakeLists.txt").read_text(encoding="utf-8")
        start = text.index("if(NEVR_ALLOW_UNSIGNED_PLUGINS)")
        block = text[start:text.index("endif()", start)]
        self.assertRegex(block, r"set_property\(SOURCE host/host_identity\.cpp APPEND PROPERTY "
                                r"COMPILE_DEFINITIONS NEVR_ALLOW_UNSIGNED_PLUGINS=1\)")


if __name__ == "__main__":
    unittest.main()
