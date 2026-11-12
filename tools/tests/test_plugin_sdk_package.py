"""The plugin SDK as a package (#1263): sdk/ configured on its own, installed, and found by a consumer.

The oracle is CMake itself. The SDK is configured from sdk/ alone (nothing else in the tree on the path), installed to
a prefix, and a separate consumer project finds it with find_package(NevrPluginSdk <api> CONFIG), links
nevr::plugin_sdk and builds one translation unit per portable header with the host compiler, so a portable header
that reaches for <windows.h> or for a file under src/ fails here. The PC headers are compiled against the installed
prefix with MinGW. The installed target must name no source path, and the version file must refuse another major.
The tests skip when cmake or a host C++ compiler is missing; the MinGW test skips without MinGW."""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import work_root  # noqa: E402

SDK = REPO / "sdk"
EXAMPLE = SDK / "examples" / "hello_plugin"
WORKFLOW = REPO / ".github" / "workflows" / "plugin-sdk-example.yml"
INCLUDE = SDK / "include" / "nevr"
PORTABLE = sorted(p.name for p in INCLUDE.glob("*.h"))
PC = sorted(p.name for p in (INCLUDE / "pc").glob("*.h"))
MINHOOK_INCLUDE = REPO / "extern" / "minhook" / "include"

CMAKE = shutil.which("cmake")
HOST_CXX = shutil.which("c++") or shutil.which("g++")
MINGW_CXX = shutil.which("x86_64-w64-mingw32-g++")
NINJA = shutil.which("ninja")


def api_version() -> int:
    text = (INCLUDE / "plugin_interface.h").read_text()
    found = re.findall(r"^#define NEVR_PLUGIN_API_VERSION (\d+)$", text, re.M)
    if len(found) != 1:
        raise AssertionError(f"plugin_interface.h has {len(found)} NEVR_PLUGIN_API_VERSION lines, expected 1")
    return int(found[0])


def run(argv: list, cwd: Path | None = None) -> subprocess.CompletedProcess:
    return subprocess.run([str(a) for a in argv], cwd=cwd, capture_output=True, text=True)


def generator() -> list:
    return ["-G", "Ninja"] if NINJA else []


CONSUMER_CMAKE = """\
cmake_minimum_required(VERSION 3.21)
project(NevrSdkConsumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(NevrPluginSdk {version} CONFIG REQUIRED)
file(GLOB _units "${{CMAKE_CURRENT_SOURCE_DIR}}/*.cpp")
add_executable(consumer ${{_units}})
target_link_libraries(consumer PRIVATE nevr::plugin_sdk)
"""

CONSUMER_MAIN = """\
#include "nevr/plugin_interface.h"
#include "nevr/symbol_hash.h"

static_assert(NEVR_PLUGIN_API_VERSION == {api}, "the installed header is the tree's header");
static_assert(EchoVR::CSymbol64Hash("main_menu") == 0x1509ee27440e1d7bULL, "the installed hash is the game's hash");

int main() {{ return 0; }}
"""


@unittest.skipUnless(CMAKE and HOST_CXX, "cmake and a host C++ compiler are required")
class InstalledPackageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._tmp = work_root.make_test_dir("plugin-sdk-")
        cls.root = Path(cls._tmp.name)
        cls.prefix = cls.root / "prefix"
        build = cls.root / "sdk-build"
        configure = run([CMAKE, "-S", SDK, "-B", build, *generator()])
        if configure.returncode != 0:
            cls._tmp.cleanup()
            raise AssertionError(f"configuring sdk/ on its own failed:\n{configure.stdout}\n{configure.stderr}")
        install = run([CMAKE, "--install", build, "--prefix", cls.prefix])
        if install.returncode != 0:
            cls._tmp.cleanup()
            raise AssertionError(f"installing sdk/ failed:\n{install.stdout}\n{install.stderr}")
        cls.cmake_dir = cls.prefix / "share" / "cmake" / "NevrPluginSdk"

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def consumer(self, name: str, version: str) -> subprocess.CompletedProcess:
        src = self.root / f"{name}-src"
        src.mkdir()
        (src / "CMakeLists.txt").write_text(CONSUMER_CMAKE.format(version=version))
        (src / "main.cpp").write_text(CONSUMER_MAIN.format(api=api_version()))
        for header in PORTABLE:
            # One unit per header, the header first, so each portable header compiles on its own.
            (src / f"unit_{Path(header).stem}.cpp").write_text(f'#include "nevr/{header}"\n')
        build = self.root / f"{name}-build"
        configure = run([CMAKE, "-S", src, "-B", build, *generator(), f"-DCMAKE_PREFIX_PATH={self.prefix}",
                         f"-DCMAKE_CXX_COMPILER={HOST_CXX}"])
        if configure.returncode != 0:
            return configure
        return run([CMAKE, "--build", build])

    def test_every_header_is_installed(self):
        installed = self.prefix / "include" / "nevr"
        self.assertEqual(sorted(p.name for p in installed.glob("*.h")), PORTABLE)
        self.assertEqual(sorted(p.name for p in (installed / "pc").glob("*.h")), PC)
        self.assertTrue((self.cmake_dir / "NevrPluginSdkConfig.cmake").is_file())
        self.assertTrue((self.cmake_dir / "NevrPluginSdkConfigVersion.cmake").is_file())

    def test_the_installed_target_names_no_source_path(self):
        config = (self.cmake_dir / "NevrPluginSdkConfig.cmake").read_text()
        self.assertIn("add_library(nevr::plugin_sdk INTERFACE IMPORTED)", config)
        self.assertRegex(config, r'INTERFACE_INCLUDE_DIRECTORIES "\$\{_IMPORT_PREFIX\}/include"')
        self.assertNotIn(str(REPO), config)
        self.assertNotIn("/src", config)

    def test_a_consumer_finds_the_package_and_builds_every_portable_header(self):
        result = self.consumer("same-major", str(api_version()))
        self.assertEqual(result.returncode, 0, f"{result.stdout}\n{result.stderr}")

    def test_another_major_is_refused(self):
        for version in (api_version() - 1, api_version() + 1):
            with self.subTest(version=version):
                result = self.consumer(f"major-{version}", str(version))
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("NevrPluginSdk", result.stderr)

    def build_example(self, name: str, extra: list) -> subprocess.CompletedProcess:
        # The example is copied to a directory of its own: it sees the installed prefix and nothing of the tree.
        src = self.root / f"{name}-src"
        src.mkdir()
        for f in EXAMPLE.iterdir():
            shutil.copy(f, src / f.name)
        build = self.root / f"{name}-build"
        configure = run([CMAKE, "-S", src, "-B", build, *generator(), f"-DCMAKE_PREFIX_PATH={self.prefix}", *extra])
        if configure.returncode != 0:
            return configure
        return run([CMAKE, "--build", build])

    def test_the_example_default_api_is_the_trees(self):
        self.assertIn(f"NEVR_HELLO_API_VERSION {api_version()} CACHE", (EXAMPLE / "CMakeLists.txt").read_text())

    def test_the_example_plugin_builds_against_the_installed_prefix_on_the_host(self):
        result = self.build_example("example-host", [f"-DCMAKE_CXX_COMPILER={HOST_CXX}"])
        self.assertEqual(result.returncode, 0, f"{result.stdout}\n{result.stderr}")
        self.assertTrue(list((self.root / "example-host-build").glob("hello_plugin.*")))

    def test_the_example_plugin_is_refused_against_another_api(self):
        result = self.build_example("example-api", [f"-DCMAKE_CXX_COMPILER={HOST_CXX}",
                                                   f"-DNEVR_HELLO_API_VERSION={api_version() + 1}"])
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("NevrPluginSdk", result.stderr)

    @unittest.skipUnless(MINGW_CXX, "x86_64-w64-mingw32-g++ is required")
    def test_the_example_plugin_builds_for_windows(self):
        result = self.build_example("example-mingw", ["-DCMAKE_SYSTEM_NAME=Windows", f"-DCMAKE_CXX_COMPILER={MINGW_CXX}"])
        self.assertEqual(result.returncode, 0, f"{result.stdout}\n{result.stderr}")
        self.assertTrue((self.root / "example-mingw-build" / "hello_plugin.dll").is_file())

    @unittest.skipUnless(MINGW_CXX, "x86_64-w64-mingw32-g++ is required")
    def test_the_pc_headers_compile_against_the_installed_prefix(self):
        headers = [h for h in PC if h != "hook_manager.h" or (MINHOOK_INCLUDE / "MinHook.h").is_file()]
        self.assertIn("nevr_common.h", headers)
        for header in headers:
            with self.subTest(header=header):
                unit = self.root / f"pc_{Path(header).stem}.cpp"
                unit.write_text(f'#include "nevr/pc/{header}"\n')
                result = run([MINGW_CXX, "-std=c++17", "-fsyntax-only", f"-I{self.prefix / 'include'}",
                              f"-I{MINHOOK_INCLUDE}", unit])
                self.assertEqual(result.returncode, 0, result.stderr)


class ExampleWorkflowTest(unittest.TestCase):
    def test_the_workflow_builds_the_example_for_windows_and_quest_from_the_installed_prefix(self):
        import yaml
        workflow = yaml.safe_load(WORKFLOW.read_text())
        job = workflow["jobs"]["example"]
        self.assertEqual(set(job["strategy"]["matrix"]["target"]), {"windows-mingw", "quest-arm64-android"})
        text = WORKFLOW.read_text()
        self.assertIn("cmake --install", text)
        self.assertIn("./.github/actions/setup-ndk", text)
        self.assertIn("android.toolchain.cmake", text)
        self.assertIn("x86_64-w64-mingw32-g++", text)
        self.assertIn("hello_plugin.dll", text)
        self.assertIn("hello_plugin.so", text)
        # Out of tree: the example is built from a copy, never with -S pointing into sdk/examples.
        self.assertNotIn("-S sdk/examples", text)


if __name__ == "__main__":
    unittest.main()
