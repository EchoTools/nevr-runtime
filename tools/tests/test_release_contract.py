"""Source-level release workflow contracts that run without building artifacts."""

import pathlib
import json
import os
import re
import subprocess
import tempfile
import unittest
import zipfile

REPO = pathlib.Path(__file__).resolve().parents[2]


class ReleaseContractTest(unittest.TestCase):
    def test_telemetry_snapshot_lease_covers_header_frame_and_previous_copy(self):
        header = (REPO / "src/runtime/server/telemetry_streamer.h").read_text()
        store = (REPO / "src/runtime/server/telemetry_snapshot_store.h").read_text()
        streamer = (REPO / "src/runtime/server/telemetry_streamer.cpp").read_text()
        run = streamer.split("void TelemetryStreamer::Run()", 1)[1].split(
            "// Frame assembly + sending", 1
        )[0]

        self.assertIn("three-slot snapshot store", header)
        self.assertIn("kSlotCount = 3", store)
        self.assertIn("TryAcquireRead(m_lastReadSnapshotSequence, headerRequired)", run)
        self.assertLess(run.index("SendHeaderWithSnapshot(snapshot)"), run.index("BuildAndSendFrame(snapshot)"))
        self.assertLess(run.index("BuildAndSendFrame(snapshot)"), run.index("m_prevSnapshot = snapshot"))
        self.assertIn("m_lastReadSnapshotSequence = lease.sequence()", run)
        self.assertNotIn("m_snapshotReady", streamer)
        self.assertNotIn("m_writeIndex", streamer)

    def test_new_runtime_gtests_are_built_and_run_by_auth_unit_gate(self):
        justfile = (REPO / "justfile").read_text()
        recipe = justfile.split("test-auth-unit:\n", 1)[1].split("# Run auth integration", 1)[0]
        targets = (
            "test_system_module_loader",
            "test_websocket_frame",
            "test_protobuf_transport",
            "test_websocket_client_auth",
            "test_url_diagnostics",
            "test_winhttp_stub",
            "test_callback_unregistration",
            "test_server_context",
            "test_session_unregister",
            "test_mic_lifecycle",
            "test_telemetry_snapshot_store",
        )
        for target in targets:
            self.assertIn(f"--target {target}", recipe)
            self.assertIn(target, recipe.split("for test_name in ", 1)[1].split("; do", 1)[0])
        # Every binary runs through run_test, which bounds it with timeout so a hang fails the gate: no
        # `wine` command may appear outside run_test's body, at any indentation.
        match = re.search(r"^ *run_test\(\) \{\n.*?^ *\}\n", recipe, re.S | re.M)
        self.assertIsNotNone(match, "run_test is not defined in test-auth-unit")
        outside = (recipe[:match.start()] + recipe[match.end():])
        code = "\n".join(line for line in outside.splitlines() if not line.lstrip().startswith("#"))
        self.assertIsNone(re.search(r"(^|[;&|(]|\s)wine\s", code),
                          "a wine command runs outside run_test, so it has no time limit")
        self.assertGreaterEqual(code.count('run_test "$bin"'), 15)
        self.assertIn('timeout -k 10 900 wine "$1"', match.group(0))
        # run_test must carry a failing exit status out of the recipe: run it with a wine that fails.
        body = match.group(0)
        with tempfile.TemporaryDirectory(dir="/var/tmp") as tmp:
            shim = pathlib.Path(tmp) / "wine"
            shim.write_text("#!/bin/sh\nexit 7\n")
            shim.chmod(0o755)
            env = dict(os.environ, PATH=f"{tmp}:{os.environ['PATH']}")
            failing = subprocess.run(["bash", "-c", body + '\nrun_test x.exe; echo reached'],
                                     env=env, capture_output=True, text=True)
            self.assertEqual(failing.returncode, 7, failing.stderr)
            self.assertNotIn("reached", failing.stdout)
            shim.write_text("#!/bin/sh\nexit 0\n")
            passing = subprocess.run(["bash", "-c", body + '\nrun_test x.exe; echo reached'],
                                     env=env, capture_output=True, text=True)
            self.assertEqual(passing.returncode, 0, passing.stderr)
            self.assertIn("reached", passing.stdout)
        self.assertIn('if [[ ! -f "$bin" ]]; then', recipe)

    def test_url_diagnostic_sinks_use_redaction_and_hide_reasons(self):
        websocket = (REPO / "src/runtime/server/websocket_client.cpp").read_text()
        telemetry = (REPO / "src/runtime/server/telemetry_streamer.cpp").read_text()
        winhttp = (REPO / "src/runtime/compat/winhttp_stub.cpp").read_text()
        gameserver = (REPO / "src/runtime/server/gameserver.cpp").read_text()
        self.assertIn("FormatRedactedUrlDiagnostic", websocket)
        self.assertIn("FormatWebSocketCloseDiagnostic", websocket)
        self.assertIn("FormatWebSocketErrorDiagnostic", websocket)
        self.assertNotIn("msg->errorInfo.reason.c_str()", websocket)
        self.assertNotIn("msg->closeInfo.reason.c_str()", websocket)
        self.assertIn("FormatRedactedUrlDiagnostic", telemetry)
        self.assertIn("FormatWebSocketCloseDiagnostic", telemetry)
        self.assertIn("FormatWebSocketErrorDiagnostic", telemetry)
        self.assertNotIn("msg->errorInfo.reason.c_str()", telemetry)
        self.assertIn("FormatRedactedUrlDiagnostic", winhttp)
        self.assertIn("IsCredentialHeaderName", winhttp)
        self.assertNotIn("curl_easy_strerror", winhttp)
        self.assertNotIn("url=%ls", winhttp)
        self.assertNotIn("sessionSuccess.endpoint().c_str()", gameserver)

    def test_build_and_distribution_recipes_propagate_cmake_failures(self):
        justfile = (REPO / "justfile").read_text()
        build = justfile.split("# Build all components\n", 1)[1].split("# Build only", 1)[0]
        dist = justfile.split("# Create distribution packages\n", 1)[1].split("# Create distribution with full output", 1)[0]
        self.assertIn("cmake --build --preset {{ preset }}", build)
        self.assertNotIn("|", build)
        self.assertIn("cmake --build --preset {{ preset }} --target dist", dist)
        self.assertNotIn("|| true", dist)

    def test_distribution_targets_build_required_runtime_and_validate_packages(self):
        cmake = (REPO / "CMakeLists.txt").read_text()
        self.assertIn("add_dependencies(dist-prepare nevr_runtime)", cmake)
        self.assertIn("add_dependencies(dist-lite-prepare nevr_runtime)", cmake)
        self.assertIn("add_dependencies(dist-prepare echovr_server)", cmake)
        self.assertIn("add_dependencies(dist-lite-prepare echovr_server)", cmake)
        self.assertIn("find_program(MINGW_STRIP_TOOL NAMES x86_64-w64-mingw32-strip llvm-strip strip)", cmake)
        self.assertGreaterEqual(cmake.count('$<TARGET_FILE:echovr_server>'), 2)
        self.assertIn("VerifyDistribution.cmake", cmake)
        lite_distribution = cmake.split("# Lite Distribution Package Target", 1)[1].split(
            "# Legacy Distribution Package Target", 1)[0]
        self.assertEqual(lite_distribution.count("add_custom_command(TARGET dist-lite POST_BUILD"), 1)
        lite_post_build = lite_distribution.split("add_custom_command(TARGET dist-lite POST_BUILD", 1)[1]
        self.assertLess(lite_post_build.index("VerifyDistribution.cmake"),
                        lite_post_build.index('COMMAND ${CMAKE_COMMAND} -E echo "Lite distribution packages'))
        validator = (REPO / "cmake/VerifyDistribution.cmake").read_text()
        self.assertIn("echovr_server.exe", validator)
        distribution = cmake.split("# Distribution Package Target", 1)[1].split("# Legacy Distribution Package Target", 1)[0]
        executable_lines = re.sub(r"(?m)^\s*#.*$", "", distribution)
        self.assertNotRegex(executable_lines, r"\|\|\s*(true|\$\{CMAKE_COMMAND\}\s+-E\s+true)")

    def test_release_ci_installs_tools_and_uploads_the_actual_artifact_directory(self):
        workflow = (REPO / ".github/workflows/build.yml").read_text()
        self.assertIn("just-version:", workflow)
        # The build runs on the current toolchain, the one development uses (#81): Arch's MinGW-w64
        # from pacman in an archlinux container, with every version logged per run.
        self.assertIn("image: archlinux:base-devel", workflow)
        pacman = re.search(r"pacman -Syu --noconfirm --needed \\\n(?P<pkgs>(?:[^\n]*\\\n)*[^\n]*)", workflow)
        self.assertIsNotNone(pacman, "the toolchain comes from one pacman -Syu")
        for pkg in ("mingw-w64-gcc", "cmake", "ninja", "wine", "zstd", "openssl", "python-yaml"):
            self.assertRegex(pacman.group("pkgs"), rf"(?<![\w-]){re.escape(pkg)}(?![\w-])", pkg)
        self.assertIn("mtrojnar/osslsigncode.git", workflow)  # not in Arch's official repos: pinned build
        self.assertIn("- name: Toolchain versions", workflow)
        # Only the build job's toolchain has to come from one pacman -Syu (#81/#82); the publish job
        # runs on ubuntu-latest (not the Arch container) and installs zstd with apt-get there (#79).
        build_job = workflow.split("\n  sign:", 1)[0]
        self.assertNotIn("apt-get", build_job)
        self.assertIn("bufbuild/buf/cmd/buf@v1.47.2", workflow)
        self.assertIn("--host-triplet=x64-linux", workflow)
        self.assertIn("x64-linux/tools/protobuf/protoc", workflow)
        self.assertLess(workflow.index("just proto"), workflow.index("- name: Configure CMake"))
        self.assertIn("NEVR_CODESIGN_REQUIRED", workflow)
        # Release signing is Microsoft Artifact Signing in the `codesign` environment (#78): the only
        # environment Azure's federated credential trusts. The private-CA secrets are gone from CI.
        self.assertIn("azure/artifact-signing-action@v2", workflow)
        self.assertIn("environment: codesign", workflow)
        self.assertIn("id-token: write", workflow)
        self.assertIn("Get-AuthenticodeSignature", workflow)
        self.assertNotIn("CODESIGN_PASS", workflow)
        self.assertNotIn("CODESIGN_PFX_BASE64", workflow)
        self.assertIn("          dist/*.zip", workflow)
        self.assertIn("          dist/*.tar.zst", workflow)
        self.assertIn("Verify distribution archives", workflow)

    def test_distribution_signs_before_archiving_and_keeps_existing_outputs(self):
        cmake = (REPO / "CMakeLists.txt").read_text()
        driver = (REPO / "tools/build_distribution.py").read_text()
        self.assertIn('option(NEVR_CODESIGN_REQUIRED', cmake)
        self.assertIn("--required-signing", cmake)
        self.assertIn('"${CMAKE_SOURCE_DIR}/dist/.staging/${DIST_NAME}"', cmake)
        self.assertIn('"${CMAKE_SOURCE_DIR}/dist/.staging/${DIST_LITE_NAME}"', cmake)
        self.assertIn("verify_archives(tar_candidate, zip_candidate, package_candidate, signing)", driver)
        self.assertIn("moved_old", driver)
        self.assertIn("CODESIGN_SKIP is forbidden in required signing mode", driver)
        self.assertIn("-require-leaf-hash", driver)

    def test_vcpkg_install_errors_are_not_masked_and_proto_precedes_configure(self):
        justfile = (REPO / "justfile").read_text()
        install = justfile.split("_vcpkg-mingw:", 1)[1]
        self.assertIn("set -euo pipefail", install)
        self.assertNotIn("|| true", install)
        self.assertIn("--host-triplet=x64-linux", install)
        self.assertIn("before CMake configure", justfile.split("# Regenerate C++ protobuf", 1)[1].split("proto:", 1)[0])

    def test_just_sign_uses_required_transactional_distribution_signing(self):
        justfile = (REPO / "justfile").read_text()
        recipe = justfile.split("# Build normal and lite archives with required local Authenticode signing.\n", 1)[1].split(
            "# Verify Authenticode signature on a file", 1)[0]
        self.assertIn("-DNEVR_CODESIGN_REQUIRED=ON", recipe)
        self.assertIn("--target dist dist-lite", recipe)
        self.assertNotIn("osslsigncode sign", recipe)

    def test_auth_unit_recipe_builds_and_runs_mic_dsp(self):
        justfile = (REPO / "justfile").read_text()
        self.assertIn("--target test_mic_dsp", justfile)
        self.assertIn("test_mic_dsp.exe", justfile)

    def test_wine_preset_uses_one_build_directory_component(self):
        presets = json.loads((REPO / "CMakePresets.json").read_text())
        wine = next(p for p in presets["configurePresets"] if p["name"] == "linux-wine-base")
        self.assertEqual(wine["binaryDir"], "${sourceDir}/build/${presetName}")

    def test_docs_describe_static_modules_and_cmake_minimum(self):
        readme = (REPO / "README.md").read_text()
        agents = (REPO / "AGENTS.md").read_text()
        self.assertRegex(readme, r"statically linked into\s+`BugSplat64\.dll`")
        self.assertNotIn("`platform_compat.dll`", readme)
        self.assertNotIn("`token_auth.dll`", readme)
        self.assertIn("CMake 4.0+", readme)
        self.assertIn("CMake 4.0+", agents)
        self.assertIn("echovr_server.exe", readme)
        self.assertTrue((REPO / "docs/design/2026-09-21-mic-provider-voip-fix.md").is_file())

    def test_sample_config_sensor_uses_tracked_fixture_not_ignored_local_state(self):
        justfile = (REPO / "justfile").read_text()
        sensor = justfile.split("# S7d — the tracked sample config", 1)[1].split("# N112", 1)[0]
        executable = "\n".join(line for line in sensor.splitlines()
                                 if not line.lstrip().startswith("#"))
        self.assertIn("docs/reference/example-config.yaml", executable)
        self.assertNotIn("echovr/_local/config.yaml", executable)
        sample = (REPO / "docs/reference/example-config.yaml").read_text()
        for section in ("auth:", "services:", "identity:", "version:"):
            self.assertRegex(sample, rf"(?m)^\s*{section}")

    def test_dllmain_and_launcher_both_enter_the_validated_game_module_route(self):
        dllmain = (REPO / "src/runtime/lifecycle/dllmain.cpp").read_text()
        initialize = (REPO / "src/runtime/lifecycle/initialize.cpp").read_text()
        self.assertIn("InitializeGameModule(GetModuleHandle(NULL))", dllmain)
        self.assertIn("InitializeGameModule(hGame)", dllmain)
        route = initialize.split("void InitializeGameModule(HMODULE module)", 1)[1].split("}", 1)[0]
        self.assertIn("RunWithSupportedGameModule", route)
        self.assertNotIn("VerifyGameVersion", initialize)


class DistributionArchiveTest(unittest.TestCase):
    def setUp(self):
        scratch = pathlib.Path("/var/tmp/work-nevr-runtime")
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.root = pathlib.Path(self.temp.name)
        self.validator = REPO / "cmake/VerifyDistribution.cmake"

    def tearDown(self):
        self.temp.cleanup()

    def make_archives(self, directory: pathlib.Path, files: tuple[str, ...] =
                      ("BugSplat64.dll", "echovr_server.exe"), corrupt_tar: bool = False):
        payload = directory / "payload"
        payload.mkdir(parents=True)
        for name in files:
            (payload / name).write_bytes(b"fixture PE data")
        tar_archive = directory / "package.tar.zst"
        zip_archive = directory / "package.zip"
        if corrupt_tar:
            tar_archive.write_bytes(b"not a zstd tar archive")
        else:
            subprocess.run(["tar", "--zstd", "-cf", str(tar_archive), "-C",
                            str(directory), "payload"], check=True, capture_output=True)
        with zipfile.ZipFile(zip_archive, "w") as archive:
            for path in payload.iterdir():
                archive.write(path, f"payload/{path.name}")
        return tar_archive, zip_archive

    def run_validator(self, tar_archive: pathlib.Path, zip_archive: pathlib.Path):
        return subprocess.run([
            "cmake", f"-DARCHIVE_TAR={tar_archive}", f"-DARCHIVE_ZIP={zip_archive}",
            "-P", str(self.validator),
        ], capture_output=True, text=True)

    def assert_failed_build_does_not_publish(self, fixture_dir: pathlib.Path,
                                              tar_archive: pathlib.Path,
                                              zip_archive: pathlib.Path):
        build_dir = fixture_dir / "build"
        marker = fixture_dir / "uploadable-artifact.txt"
        cmakelists = fixture_dir / "CMakeLists.txt"
        cmakelists.write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(DistributionGate NONE)\n"
            "add_custom_target(dist COMMAND ${CMAKE_COMMAND} -E echo packaging)\n"
            "add_custom_command(TARGET dist POST_BUILD\n"
            f'  COMMAND ${{CMAKE_COMMAND}} "-DARCHIVE_TAR={tar_archive}" '
            f'"-DARCHIVE_ZIP={zip_archive}" -P "{self.validator}"\n'
            "  VERBATIM)\n"
            f'add_custom_target(uploadable COMMAND ${{CMAKE_COMMAND}} -E touch "{marker}" DEPENDS dist)\n'
        )
        configured = subprocess.run(["cmake", "-S", str(fixture_dir), "-B", str(build_dir),
                                     "-G", "Ninja"], capture_output=True, text=True)
        self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
        built = subprocess.run(["cmake", "--build", str(build_dir), "--target", "uploadable"],
                               capture_output=True, text=True)
        self.assertNotEqual(built.returncode, 0, built.stdout + built.stderr)
        self.assertFalse(marker.exists(), "failed distribution validation reached the upload target")

    def test_normal_and_lite_archive_pairs_pass_runtime_validation(self):
        for package in ("normal", "lite"):
            with self.subTest(package=package):
                package_dir = self.root / package
                package_dir.mkdir()
                tar_archive, zip_archive = self.make_archives(package_dir)
                result = self.run_validator(tar_archive, zip_archive)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_corrupt_and_incomplete_archives_fail_closed_without_upload(self):
        variants = ("missing-archive", "corrupt-archive", "missing-bugsplat", "missing-launcher")
        for variant in variants:
            with self.subTest(variant=variant):
                case = self.root / variant
                case.mkdir()
                files = {
                    "missing-archive": ("BugSplat64.dll", "echovr_server.exe"),
                    "corrupt-archive": ("BugSplat64.dll", "echovr_server.exe"),
                    "missing-bugsplat": ("echovr_server.exe",),
                    "missing-launcher": ("BugSplat64.dll",),
                }[variant]
                tar_archive, zip_archive = self.make_archives(
                    case, files, corrupt_tar=variant == "corrupt-archive")
                if variant == "missing-archive":
                    zip_archive.unlink()
                result = self.run_validator(tar_archive, zip_archive)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assert_failed_build_does_not_publish(case, tar_archive, zip_archive)


if __name__ == "__main__":
    unittest.main()
