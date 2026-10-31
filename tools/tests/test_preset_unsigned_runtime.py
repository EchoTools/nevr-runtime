"""#1122: which presets build a runtime the host accepts unsigned.

The host verifies nevr-runtime.dll before it maps it and, in every build but the plugin developers' build
(NEVR_ALLOW_UNSIGNED_PLUGINS), stops the game with exit code 78 when the file is unsigned
(DecideVerify in src/runtime/host/runtime_load.cpp, pinned by test_host_runtime_verify). A preset that makes a DLL
pair for a run on the owner's machine without signing it must therefore be the dev build, and a preset that makes
what ships must not be. CMake resolves `inherits` in the same way the helper below does.
"""
import json
import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
DEV_BUILD = "NEVR_ALLOW_UNSIGNED_PLUGINS"
SCENARIO = "NEVR_SCENARIO_CONTROL"

# Presets whose DLLs are packaged or stand in for what is packaged; none of them may be the dev build.
SHIPPING = ("release", "debug", "mingw-release", "mingw-debug", "mingw-dev", "linux-wine-release", "linux-wine-debug")


def presets() -> dict:
    data = json.loads((REPO / "CMakePresets.json").read_text(encoding="utf-8"))
    return {preset["name"]: preset for preset in data["configurePresets"]}


def cache_variables(name: str, by_name: dict) -> dict:
    """The cache variables a configure preset ends up with: its parents' first, its own over them."""
    preset = by_name[name]
    parents = preset.get("inherits", [])
    if isinstance(parents, str):
        parents = [parents]
    merged = {}
    for parent in parents:
        merged.update(cache_variables(parent, by_name))
    for key, value in preset.get("cacheVariables", {}).items():
        merged[key] = value["value"] if isinstance(value, dict) else value
    return merged


def is_on(value) -> bool:
    return str(value).upper() in ("ON", "TRUE", "1")


class ScenarioBuildIsTheDevBuild(unittest.TestCase):
    def test_every_scenario_preset_is_the_dev_build(self):
        by_name = presets()
        scenario = [name for name in by_name if is_on(cache_variables(name, by_name).get(SCENARIO, "OFF"))]
        self.assertIn("mingw-scenario", scenario, "no preset builds the scenario control endpoint any more")
        for name in scenario:
            with self.subTest(preset=name):
                self.assertTrue(
                    is_on(cache_variables(name, by_name).get(DEV_BUILD, "OFF")),
                    f"{name} builds an unsigned nevr-runtime.dll beside the host; without {DEV_BUILD}=ON the host "
                    "refuses it as unsigned and ends the game with exit code 78 before the runtime logs a line",
                )

    def test_a_shipping_preset_is_never_the_dev_build(self):
        by_name = presets()
        for name in SHIPPING:
            if name not in by_name:
                continue
            with self.subTest(preset=name):
                self.assertFalse(is_on(cache_variables(name, by_name).get(DEV_BUILD, "OFF")))

    def test_the_login_test_default_is_the_dev_build(self):
        # #1230: launch-client.sh deploys its default BugSplat64.dll (and the nevr-runtime.dll beside it) into a game copy
        # and runs the login test with it. A default from a shipping preset is refused by the host and the game stops.
        script = (REPO / "launch-client.sh").read_text(encoding="utf-8")
        match = re.search(r"^DLL=build/([^/\s]+)/bin/BugSplat64\.dll$", script, re.MULTILINE)
        self.assertIsNotNone(match, "launch-client.sh has no default DLL of the form build/<preset>/bin/BugSplat64.dll")
        by_name = presets()
        name = match.group(1)
        self.assertIn(name, by_name, f"launch-client.sh's default names {name}, which is not a configure preset")
        self.assertTrue(
            is_on(cache_variables(name, by_name).get(DEV_BUILD, "OFF")),
            f"launch-client.sh's default names {name}, which builds an unsigned nevr-runtime.dll the host refuses "
            f"(exit 78, runtime_refused); its default must be the dev build with {DEV_BUILD}=ON",
        )

    def test_the_resolver_reads_an_inherited_variable(self):
        by_name = presets()
        self.assertTrue(is_on(cache_variables("mingw-plugin-dev", by_name)[DEV_BUILD]))
        self.assertEqual("Release", cache_variables("mingw-scenario", by_name)["CMAKE_BUILD_TYPE"])


if __name__ == "__main__":
    unittest.main()
