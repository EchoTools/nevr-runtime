"""The tag build compiles through a pinned ccache whose cache is saved and restored by build.yml (#1768).

The ccache launcher is downloaded from its release tarball and checked against a SHA-256 before it runs, and it is
named by its absolute path in $RUNNER_TEMP, never on PATH (PATH is part of every mingw package ABI, #816). The
Configure step hands both compiler launchers to CMake; the Build step points CCACHE_DIR at the workspace; the cache
is saved after the build and before Verify, and a release-event run saves nothing.
"""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
BUILD = ROOT / ".github" / "workflows" / "build.yml"

INSTALL = "Install pinned ccache"
RESTORE = "Restore the ccache"
CONFIGURE = "Configure CMake"
BUILD_STEP = "Build"
SAVE = "Save the ccache"
STATS = "ccache statistics"
ASSERT_VERSION = "Assert the stamped version"
VERIFY = "Verify"
ORDER = [INSTALL, RESTORE, CONFIGURE, BUILD_STEP, SAVE, STATS, ASSERT_VERSION, VERIFY]

CCACHE_DIGEST = "d540ba01f160addbd3dd5e18eda9bd34005591b095c09ac8ae9d0a1b655e7ef5"


def step(text: str, name: str) -> str:
    """The text of the `- name: <name>` step, up to the next step at the same indent."""
    match = re.search(rf"^    - name: {re.escape(name)}\n(.*?)(?=^    - |^    # |\Z)", text, re.S | re.M)
    if match is None:
        raise AssertionError(f"build.yml has no `- name: {name}` step")
    return match.group(0)


def position(text: str, name: str) -> int:
    match = re.search(rf"^    - name: {re.escape(name)}$", text, re.M)
    if match is None:
        raise AssertionError(f"build.yml has no `- name: {name}` step")
    return match.start()


class CcacheStep(unittest.TestCase):
    def setUp(self) -> None:
        self.text = BUILD.read_text(encoding="utf-8")
        self.install = step(self.text, INSTALL)
        self.restore = step(self.text, RESTORE)
        self.configure = step(self.text, CONFIGURE)
        self.build = step(self.text, BUILD_STEP)
        self.save = step(self.text, SAVE)
        self.stats = step(self.text, STATS)

    def test_the_digest_is_checked_before_the_tarball_is_extracted(self) -> None:
        digest = re.search(r"\b([0-9a-f]{64})\b\s+\S*ccache\.tgz", self.install)
        self.assertIsNotNone(digest, "no 64-hex SHA-256 for ccache.tgz in the install step")
        self.assertEqual(digest.group(1), CCACHE_DIGEST, "the ccache digest is not the measured pin")
        self.assertIn("sha256sum -c", self.install, "the install step never runs sha256sum -c")
        self.assertLess(self.install.index("sha256sum -c"), self.install.index("tar -xzf"),
                        "ccache is extracted before its digest is checked")

    def test_the_install_step_does_not_put_ccache_on_path(self) -> None:
        self.assertNotIn("GITHUB_PATH", self.install, "the install step edits PATH through GITHUB_PATH")
        self.assertNotIn("export PATH=", self.install, "the install step exports PATH")

    def test_configure_passes_both_launchers_by_absolute_path(self) -> None:
        self.assertRegex(self.configure, r'-DCMAKE_C_COMPILER_LAUNCHER="\$RUNNER_TEMP/ccache/ccache"',
                         "Configure CMake does not pass the C compiler launcher by absolute path")
        self.assertRegex(self.configure, r'-DCMAKE_CXX_COMPILER_LAUNCHER="\$RUNNER_TEMP/ccache/ccache"',
                         "Configure CMake does not pass the CXX compiler launcher by absolute path")

    def test_the_steps_run_in_order(self) -> None:
        positions = [position(self.text, name) for name in ORDER]
        self.assertEqual(positions, sorted(positions), f"steps are out of order: {ORDER}")
        self.assertEqual(len(set(positions)), len(positions), "a step name is repeated")

    def test_the_save_step_is_skipped_on_a_release_event(self) -> None:
        self.assertEqual(re.findall(r"(?m)^      if: (.+)$", self.save), ["github.event_name != 'release'"])

    def test_the_save_and_restore_steps_use_the_same_directory(self) -> None:
        self.assertEqual(re.findall(r"(?m)^        path: (.+)$", self.save), re.findall(r"(?m)^        path: (.+)$", self.restore))
        self.assertEqual(re.findall(r"(?m)^        path: (.+)$", self.save), [".ccache"])

    def test_the_keys_name_the_toolchain_and_the_commit(self) -> None:
        key = re.search(r"(?m)^        key: (.+)$", self.save).group(1)
        self.assertIn("steps.vcpkg.outputs.gcc", key, "a new compiler would reuse the old objects")
        self.assertIn("github.sha", key)
        self.assertIn("4.14.2", key, "a new ccache version would reuse the old entry")

    def test_the_statistics_step_runs_after_a_failed_build_and_cannot_fail_the_build(self) -> None:
        self.assertIn("if: always()", self.stats)
        self.assertIn("continue-on-error: true", self.stats)
        self.assertIn("--show-stats", self.stats)

    def test_the_save_key_is_the_restore_key_and_the_restore_prefix_is_its_prefix(self) -> None:
        restore_key = re.search(r"^        key: (.+)$", self.restore, re.M)
        save_key = re.search(r"^        key: (.+)$", self.save, re.M)
        self.assertIsNotNone(restore_key, "the restore step has no key")
        self.assertIsNotNone(save_key, "the save step has no key")
        self.assertEqual(save_key.group(1), restore_key.group(1), "save and restore use different keys")
        prefix = re.search(r"restore-keys: \|\n\s+(.+)", self.restore)
        self.assertIsNotNone(prefix, "the restore step has no restore-keys prefix")
        self.assertTrue(restore_key.group(1).startswith(prefix.group(1)),
                        f"restore-keys prefix {prefix.group(1)} is not a prefix of {restore_key.group(1)}")

    def test_every_later_step_gets_the_ccache_settings_from_the_job_environment(self) -> None:
        # The dist target and, off a tag, Verify compile through the same launcher as Build: they need the same settings.
        for line in ('echo "CCACHE_DIR=$GITHUB_WORKSPACE/.ccache"',
                     'echo "CCACHE_CONFIGPATH=$GITHUB_WORKSPACE/tools/ccache.conf"',
                     'echo "CCACHE_MAXSIZE=500M"',
                     'echo "CCACHE_COMPILERCHECK=content"'):
            self.assertIn(line, self.install, line)
        self.assertIn('>> "$GITHUB_ENV"', self.install)
        self.assertNotIn("GITHUB_PATH", self.install)
        for text in (self.build, self.stats):
            self.assertNotIn("export CCACHE_", text, "a step sets its own ccache settings instead of the job's")
        keys = re.findall(r"(?m)^\s+echo \"(CCACHE_DIR)=([^\"]+)\"", self.install)
        self.assertEqual(len(keys), 1)
        self.assertNotIn("github.workspace", keys[0][1], "CCACHE_DIR names the host's workspace path, not the container's")

    def test_the_build_step_zeroes_the_stats_and_keeps_the_build_line(self) -> None:
        self.assertIn("--zero-stats", self.build)
        self.assertIn("cmake --build --preset mingw-release", self.build)


if __name__ == "__main__":
    unittest.main()
