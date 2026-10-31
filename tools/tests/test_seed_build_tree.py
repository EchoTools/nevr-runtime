"""tools/seed_build_tree.py on a throwaway ninja project: a "main checkout" built with real ninja, then seeded into a
worktree. The fixture never touches the real repository or its build trees."""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from gitenv import git  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[2]
TOOL = REPO / "tools/seed_build_tree.py"


def load_tool():
    spec = importlib.util.spec_from_file_location("seed_build_tree", TOOL)
    module = importlib.util.module_from_spec(spec)
    sys.modules["seed_build_tree"] = module
    spec.loader.exec_module(module)
    return module


def tree_digest(root, skip=None):
    """Every path, its bytes and its mtime: a change anywhere under root changes the digest (except under `skip`)."""
    h = hashlib.sha256()
    for path in sorted(pathlib.Path(root).rglob("*")):
        if skip is not None and (path == skip or skip in path.parents):
            continue
        st = path.lstat()
        h.update(f"{path.relative_to(root)}|{st.st_mtime_ns}|{st.st_size}\n".encode())
        if path.is_file() and not path.is_symlink():
            h.update(path.read_bytes())
    return h.hexdigest()


REGEN_SCRIPT = """\
import os, pathlib, re, subprocess, sys
build_ninja, checkout = pathlib.Path(sys.argv[1]), sys.argv[2]
env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
head = subprocess.run(["git", "-C", checkout, "rev-parse", "HEAD"], check=True, capture_output=True, text=True, env=env).stdout.strip()
text = re.sub(r'(NEVR_GIT_COMMIT_FULL=..)[0-9a-f]{40}', lambda m: m.group(1) + head, build_ninja.read_text())
build_ninja.write_text(text)
"""


def regenerate(build_dir):
    """Run the configure step alone: the generator edge that rebuilds build.ninja, which reads the checkout's commit again
    (what CMake does when build.ninja is older than a CMakeLists.txt). Returns what `ninja -n` printed before it ran."""
    planned = subprocess.run(["ninja", "-C", str(build_dir), "-n"], check=True, capture_output=True, text=True).stdout
    subprocess.run(["ninja", "-C", str(build_dir), "build.ninja"], check=True, capture_output=True)
    return planned


def pending_steps(build_dir):
    """The edges `ninja -n` would run, as the output names of its [n/m] lines."""
    out = subprocess.run(["ninja", "-C", str(build_dir), "-n"], check=True, capture_output=True, text=True).stdout
    return sorted(m.group(1) for m in re.finditer(r"^\[\d+/\d+\] \S+ (?:.* )?(\S+)$", out, re.M)), out


class SeedFixture(unittest.TestCase):
    PRESET = "p"
    WORKTREE_NAME = "wt"
    DESCRIBE = None  # -DNEVR_GIT_DESCRIBE="<this>" on the compile commands, when set
    COMMIT_DEFINE = True  # the compile commands carry -DNEVR_GIT_COMMIT_FULL="<HEAD>", as the root CMakeLists.txt does

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="seed-build-tree-test-"))
        self.addCleanup(shutil.rmtree, self.tmp)
        self.tool = load_tool()
        self.main = self.tmp / "main"
        (self.main / "src").mkdir(parents=True)
        (self.main / "inc").mkdir()
        (self.main / "inc/h.h").write_text("#define VAL 1\n")
        (self.main / "src/a.c").write_text('#include "h.h"\nint a(void) { return VAL; }\n')
        (self.main / "src/c.c").write_text('#include "h.h"\nint c(void) { return VAL + 1; }\n')
        (self.main / "src/b.c").write_text("int a(void);\nint c(void);\nint main(void) { return a() - c() + 1; }\n")
        (self.main / "CMakeLists.txt").write_text("project(fixture)\n")
        (self.main / "regen.py").write_text(REGEN_SCRIPT)
        git(self.main, "init", "-q", "-b", "main")
        git(self.main, "add", "-A")
        git(self.main, "commit", "-q", "--no-gpg-sign", "-m", "init")
        self.build = self.main / "build" / self.PRESET
        self.build.mkdir(parents=True)
        define = ""
        if self.DESCRIBE:
            define += f' -DNEVR_GIT_DESCRIBE=\\"{self.DESCRIBE}\\"'
        # The commit is defined on the one source that reads it (b.o), as the root CMakeLists.txt does for the identity
        # readers; no other edge carries it, so a commit change rebuilds b.o and what links it.
        commit = ""
        if self.COMMIT_DEFINE:
            commit = f' -DNEVR_GIT_COMMIT_FULL=\\"{git(self.main, "rev-parse", "HEAD").stdout.strip()}\\"'
        # An output spelled as an absolute path, as CMake does for some custom commands. build.ninja has the generator
        # edge CMake gives it: it is rebuilt when it is older than CMakeLists.txt, and the rebuild reads the commit again.
        (self.build / "build.ninja").write_text(
            "rule regen\n  command = python3 " + f"{self.main}/regen.py build.ninja {self.main}\n  generator = 1\n"
            f"build build.ninja: regen {self.main}/CMakeLists.txt\n"
            f"rule cc\n  command = cc -MD -MF $out.d -I{self.main}/inc{define} -c $in -o $out\n"
            "  depfile = $out.d\n  deps = gcc\n"
            f"rule cc_identity\n  command = cc -MD -MF $out.d -I{self.main}/inc{define}{commit} -c $in -o $out\n"
            "  depfile = $out.d\n  deps = gcc\n"
            "rule link\n  command = cc $in -o $out\n"
            f"build a.o: cc {self.main}/src/a.c\n"
            f"build b.o: cc_identity {self.main}/src/b.c\n"
            f"build {self.build}/c.o: cc {self.main}/src/c.c\n"
            f"build app: link a.o b.o {self.build}/c.o\n")
        # A path that has the checkout path as a prefix but is another directory: it must survive a rewrite.
        (self.build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.main}\nCMAKE_CACHEFILE_DIR:INTERNAL={self.build}\n"
            f"SCRATCH:PATH={self.main}-work/scratch\n")
        subprocess.run(["ninja", "-C", str(self.build)], check=True, capture_output=True)
        self.assertEqual(pending_steps(self.build)[0], [])
        time.sleep(0.05)
        self.wt = self.tmp / self.WORKTREE_NAME
        git(self.main, "worktree", "add", "-q", "--no-track", "-b", "feature", str(self.wt))

    def seed(self, **kw):
        return self.tool.seed(self.main, self.wt, self.PRESET, **kw)

    def wt_build(self):
        return self.wt / "build" / self.PRESET

    def touch_later(self, path, text):
        path.write_text(text)
        t = time.time_ns() + 5_000_000_000
        os.utime(path, ns=(t, t))


class SeededTree(SeedFixture):
    def test_seeded_tree_has_nothing_to_rebuild(self):
        result = self.seed()
        self.assertEqual(result.status, "seeded", result)
        steps, out = pending_steps(self.wt_build())
        self.assertEqual(steps, [], out)
        self.assertIn("no work to do", out)

    def test_a_header_edit_rebuilds_only_its_dependents(self):
        self.assertEqual(self.seed().status, "seeded")
        self.touch_later(self.wt / "inc/h.h", "#define VAL 2\n")
        steps, out = pending_steps(self.wt_build())
        self.assertEqual(steps, sorted(["a.o", f"{self.wt_build()}/c.o", "app"]), out)

    def test_a_source_edit_rebuilds_one_object_and_the_link(self):
        self.assertEqual(self.seed().status, "seeded")
        self.touch_later(self.wt / "src/b.c", "int a(void);\nint main(void) { return a(); }\n")
        steps, out = pending_steps(self.wt_build())
        self.assertEqual(steps, ["app", "b.o"], out)

    def test_a_file_that_differs_from_main_is_rebuilt(self):
        (self.wt / "src/a.c").write_text('#include "h.h"\nint a(void) { return VAL + 40; }\n')
        self.assertEqual(self.seed().status, "seeded")
        steps, out = pending_steps(self.wt_build())
        self.assertEqual(steps, ["a.o", "app"], out)

    def test_a_differing_file_older_than_the_objects_is_still_rebuilt(self):
        (self.wt / "src/a.c").write_text('#include "h.h"\nint a(void) { return VAL + 40; }\n')
        os.utime(self.wt / "src/a.c", ns=(1, 1))  # a checkout older than the main checkout's build of a.o
        self.assertEqual(self.seed().status, "seeded")
        self.assertIn("a.o", pending_steps(self.wt_build())[0])

    def test_a_file_of_the_same_size_but_other_content_is_rebuilt(self):
        original = (self.main / "src/a.c").read_text()
        changed = original.replace("return VAL;", "return 777;")
        self.assertEqual(len(changed), len(original))
        (self.wt / "src/a.c").write_text(changed)
        self.assertEqual(self.seed().status, "seeded")
        self.assertEqual(pending_steps(self.wt_build())[0], ["a.o", "app"])

    def test_the_cache_points_at_the_worktree_and_leaves_other_directories_alone(self):
        self.assertEqual(self.seed().status, "seeded")
        cache = (self.wt_build() / "CMakeCache.txt").read_text()
        self.assertIn(f"CMAKE_HOME_DIRECTORY:INTERNAL={self.wt}\n", cache)
        self.assertIn(f"CMAKE_CACHEFILE_DIR:INTERNAL={self.wt_build()}\n", cache)
        self.assertIn(f"SCRATCH:PATH={self.main}-work/scratch\n", cache)
        self.assertNotIn(f"={self.main}\n", cache)

    def test_the_main_checkout_is_not_modified(self):
        before = tree_digest(self.main, skip=self.wt)  # the worktree may sit inside main (SeededTreeWithTheWorktreeInsideMain)
        self.assertEqual(self.seed().status, "seeded")
        self.assertEqual(tree_digest(self.main, skip=self.wt), before)

    def test_no_staging_directory_is_left_behind(self):
        self.assertEqual(self.seed().status, "seeded")
        self.assertEqual(sorted(p.name for p in (self.wt / "build").iterdir()), [self.PRESET])

    def test_seeding_again_keeps_what_is_there(self):
        self.assertEqual(self.seed().status, "seeded")
        marker = self.wt_build() / "marker"
        marker.write_text("mine\n")
        again = self.seed()
        self.assertEqual(again.status, "kept", again)
        self.assertEqual(marker.read_text(), "mine\n")


    def test_files_that_are_not_text_are_copied_as_they_are(self):
        blob = self.build / "blob.o"  # an object embeds the checkout path in debug info and is carried as it is
        blob.write_bytes(b"\0\1" + str(self.main).encode() + b"/x\0")
        self.assertEqual(self.seed().status, "seeded")
        self.assertEqual((self.wt_build() / "blob.o").read_bytes(), blob.read_bytes())

    def test_a_rewritten_text_file_keeps_its_mtime(self):
        before = (self.build / "CMakeCache.txt").stat().st_mtime_ns
        self.assertEqual(self.seed().status, "seeded")
        self.assertEqual((self.wt_build() / "CMakeCache.txt").stat().st_mtime_ns, before)
        self.assertEqual((self.wt_build() / "build.ninja").stat().st_mtime_ns, (self.build / "build.ninja").stat().st_mtime_ns)

    def test_a_link_into_the_main_checkout_points_into_the_worktree(self):
        (self.build / "app-link").symlink_to(self.build / "app")
        (self.build / "dir-link").symlink_to(self.build / "CMakeFiles", target_is_directory=True)
        (self.build / "relative-link").symlink_to("app")
        self.assertEqual(self.seed().status, "seeded")
        self.assertEqual(os.readlink(self.wt_build() / "app-link"), str(self.wt_build() / "app"))
        self.assertEqual(os.readlink(self.wt_build() / "dir-link"), str(self.wt_build() / "CMakeFiles"))
        self.assertEqual(os.readlink(self.wt_build() / "relative-link"), "app")

    def test_a_log_entry_of_an_edge_no_longer_in_the_manifest_is_dropped(self):
        with open(self.build / ".ninja_log", "a") as f:
            f.write("1\t2\t3\tstale.o\tdeadbeef\n")
        result = self.seed()
        self.assertEqual(result.status, "seeded", result)
        self.assertEqual(result.counts["log_entries_dropped"], 1)
        self.assertNotIn("stale.o", (self.wt_build() / ".ninja_log").read_text())
        self.assertEqual(pending_steps(self.wt_build())[0], [])


class SeededTreeWithTheWorktreeInsideMain(SeededTree):
    """The layout of the real repository: linked worktrees live under the main checkout's .claude/worktrees."""
    WORKTREE_NAME = "main/.claude/worktrees/wt"


class SeededTreeWithAnotherPathLength(SeededTree):
    """The deps log pads each path to four bytes: a worktree path of another length than main's must work too."""
    WORKTREE_NAME = "worktree-with-a-longer-name-x"


class CommitGuard(SeedFixture):
    COMMIT_DEFINE = True

    def test_a_worktree_at_the_commit_the_tree_was_built_at_is_seeded(self):
        self.assertEqual(self.seed().status, "seeded")
        self.assertEqual(pending_steps(self.wt_build())[0], [])

    def test_a_worktree_at_another_commit_is_seeded_and_only_its_identity_readers_and_differing_sources_rebuild(self):
        (self.wt / "src/a.c").write_text('#include "h.h"\nint a(void) { return VAL + 40; }\n')
        git(self.wt, "commit", "-q", "-a", "--no-gpg-sign", "-m", "branch change")
        result = self.seed()
        self.assertEqual(result.status, "seeded", result)
        self.assertTrue(result.counts["configure_rerun"], result)
        self.assertEqual((self.wt_build() / "build.ninja").stat().st_mtime_ns, 0)
        # The epoch mtime makes the configure the first thing the build does, and the configure reads the new commit.
        self.assertIn("regen.py", regenerate(self.wt_build()))
        # b.o reads the commit; a.o's source differs from main's; app links both. c.o reads neither and is kept.
        self.assertEqual(pending_steps(self.wt_build())[0], sorted(["a.o", "app", "b.o"]))

    def test_a_worktree_at_the_same_commit_keeps_the_configure_as_it_is(self):
        result = self.seed()
        self.assertEqual(result.status, "seeded", result)
        self.assertFalse(result.counts["configure_rerun"], result)
        self.assertEqual((self.wt_build() / "build.ninja").stat().st_mtime_ns, (self.build / "build.ninja").stat().st_mtime_ns)


class UnreadableCommit(SeedFixture):
    """A tree whose commit cannot be read from build.ninja has its configure re-run like any other tree at another commit."""
    COMMIT_DEFINE = False

    def test_a_build_ninja_that_names_no_commit_is_seeded_with_its_configure_rerun(self):
        result = self.seed()
        self.assertEqual(result.status, "seeded", result)
        self.assertTrue(result.counts["configure_rerun"], result)
        self.assertEqual((self.wt_build() / "build.ninja").stat().st_mtime_ns, 0)

    def test_the_cli_prints_the_configure_rerun_exits_zero_and_records_it(self):
        log = self.tmp / "seed.jsonl"
        proc = subprocess.run([sys.executable, "-I", str(TOOL), "--main", str(self.main), "--worktree", str(self.wt),
                               "--preset", self.PRESET, "--log", str(log)], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("seeded", proc.stdout)
        self.assertIn("configure re-runs first", proc.stdout)
        record = json.loads(log.read_text().splitlines()[-1])
        self.assertEqual(record["status"], "seeded")
        self.assertTrue(record["configure_rerun"])


class AnotherSpellingOfMain(SeedFixture):
    """A tree configured through a link to the main checkout names it by that spelling, which the rewrite does not
    know: build.ninja would still regenerate the main checkout's tree. Resolved paths are equal, so the cache check
    passes; the copy is scanned for what is left."""

    def setUp(self):
        super().setUp()
        self.alias = self.tmp / "alias"
        self.alias.symlink_to(self.main)
        for name in ("build.ninja", "CMakeCache.txt"):
            path = self.build / name
            path.write_text(path.read_text().replace(str(self.main), str(self.alias)))

    def assert_refused(self, result):
        self.assertEqual(result.status, "not-seeded", result)
        self.assertIn(str(self.alias), result.reason)
        self.assertFalse(self.wt_build().exists())
        self.assertEqual(list((self.wt / "build").glob(".seed-*")), [])

    def test_a_copy_that_still_names_the_main_checkout_by_another_spelling_is_not_moved_into_place(self):
        self.assert_refused(self.seed())

    def test_the_same_when_main_is_given_by_that_spelling(self):
        self.assert_refused(self.tool.seed(self.alias, self.wt, self.PRESET))

    def test_the_reason_names_the_file(self):
        self.assertRegex(self.seed().reason, r"build\.ninja|CMakeCache\.txt")


class SpellingMainWasGivenBy(SeedFixture):
    """The cache names the checkout by its real path; the caller names it through a link, and a file of the copy
    (here the manifest, or a link) uses that spelling too."""

    def setUp(self):
        super().setUp()
        self.alias = self.tmp / "alias"
        self.alias.symlink_to(self.main)

    def test_a_manifest_that_uses_the_spelling_main_was_given_by_is_not_moved_into_place(self):
        ninja = self.build / "build.ninja"
        ninja.write_text(ninja.read_text().replace(str(self.main), str(self.alias)))
        result = self.tool.seed(self.alias, self.wt, self.PRESET)
        self.assertEqual(result.status, "not-seeded", result)
        self.assertIn(str(self.alias), result.reason)
        self.assertFalse(self.wt_build().exists())

    def test_a_link_through_that_spelling_is_found_too(self):
        (self.build / "a-link").symlink_to(self.alias / "src" / "a.c")
        result = self.tool.seed(self.alias, self.wt, self.PRESET)
        self.assertEqual(result.status, "not-seeded", result)
        self.assertIn("a-link", result.reason)
        self.assertFalse(self.wt_build().exists())
        self.assertEqual(list((self.wt / "build").glob(".seed-*")), [])


class UnknownBinaryFile(SeedFixture):
    def test_a_file_with_no_binary_suffix_that_names_the_main_checkout_is_not_carried_over_unrewritten(self):
        (self.build / "blob.dat").write_bytes(b"\0\1" + str(self.main).encode() + b"/x\0")
        result = self.seed()
        self.assertEqual(result.status, "not-seeded", result)
        self.assertIn("blob.dat", result.reason)
        self.assertFalse(self.wt_build().exists())

    def test_the_worktrees_own_path_is_not_taken_for_the_main_checkouts_when_it_is_inside_it(self):
        inside = self.main / ".claude" / "worktrees" / "inside"
        git(self.main, "worktree", "add", "-q", "--no-track", "-b", "inside", str(inside))
        self.assertEqual(self.tool.seed(self.main, inside, self.PRESET).status, "seeded")

    def test_a_sibling_directory_that_starts_with_the_checkouts_name_is_not_the_checkout(self):
        (self.build / "sibling.txt").write_text(f"{self.main}-work/scratch\n")
        self.assertEqual(self.seed().status, "seeded")


class MainGivenThroughALink(SeedFixture):
    def test_a_link_to_the_main_checkout_given_as_its_path_does_not_stop_a_tree_configured_at_the_real_path(self):
        alias = self.tmp / "alias"
        alias.symlink_to(self.main)
        result = self.tool.seed(alias, self.wt, self.PRESET)
        self.assertEqual(result.status, "seeded", result)
        self.assertEqual(pending_steps(self.wt_build())[0], [])


class GitEnvironment(SeedFixture):
    def test_variables_naming_another_repository_do_not_redirect_the_seeders_git_calls(self):
        sentinel = self.tmp / "sentinel"
        sentinel.mkdir()
        git(sentinel, "init", "-q", "-b", "main")
        git(sentinel, "commit", "-q", "--allow-empty", "--no-gpg-sign", "-m", "another repository")
        leak = {"GIT_DIR": str(sentinel / ".git"), "GIT_WORK_TREE": str(sentinel),
                "GIT_INDEX_FILE": str(sentinel / ".git/index"), "GIT_COMMON_DIR": str(sentinel / ".git")}
        with mock.patch.dict(os.environ, leak):
            result = self.seed()
        self.assertEqual(result.status, "seeded", result)
        self.assertFalse(result.counts["configure_rerun"], result)  # HEAD was read from the worktree, not the sentinel
        self.assertEqual(pending_steps(self.wt_build())[0], [])  # and ls-files listed the worktree's files


class ModifiedTree(SeedFixture):
    COMMIT_DEFINE = True
    DESCRIBE = "v1.2.3-4-gabcd-dirty"

    def test_a_tree_configured_with_uncommitted_changes_is_seeded_with_its_configure_rerun(self):
        result = self.seed()
        self.assertEqual(result.status, "seeded", result)
        self.assertTrue(result.counts["configure_rerun"], result)
        self.assertEqual((self.wt_build() / "build.ninja").stat().st_mtime_ns, 0)


class CleanDescribe(SeedFixture):
    COMMIT_DEFINE = True
    DESCRIBE = "v1.2.3-4-gabcd"

    def test_a_clean_tree_at_the_same_commit_is_seeded(self):
        self.assertEqual(self.seed().status, "seeded")
        self.assertEqual(pending_steps(self.wt_build())[0], [])


class NotSeeded(SeedFixture):
    def test_a_destination_with_content_is_kept_untouched(self):
        (self.wt_build()).mkdir(parents=True)
        (self.wt_build() / "mine.txt").write_text("keep\n")
        result = self.seed()
        self.assertEqual(result.status, "kept", result)
        self.assertEqual(sorted(p.name for p in self.wt_build().iterdir()), ["mine.txt"])

    def test_an_empty_destination_directory_is_filled(self):
        self.wt_build().mkdir(parents=True)
        self.assertEqual(self.seed().status, "seeded")
        self.assertEqual(pending_steps(self.wt_build())[0], [])

    def test_a_main_checkout_without_the_preset_says_so(self):
        shutil.rmtree(self.build)
        result = self.seed()
        self.assertEqual(result.status, "not-seeded", result)
        self.assertIn(f"build/{self.PRESET}", result.reason)
        self.assertFalse(self.wt_build().exists())

    def test_a_tree_configured_for_another_checkout_is_refused(self):
        cache = self.build / "CMakeCache.txt"
        cache.write_text(cache.read_text().replace(f"HOME_DIRECTORY:INTERNAL={self.main}", "HOME_DIRECTORY:INTERNAL=/elsewhere"))
        result = self.seed()
        self.assertEqual(result.status, "not-seeded", result)
        self.assertIn("CMAKE_HOME_DIRECTORY", result.reason)
        self.assertFalse(self.wt_build().exists())

    def test_a_shell_sitting_in_main_s_build_directory_does_not_stop_it(self):
        proc = subprocess.Popen(["sleep", "60"], cwd=self.build)
        self.addCleanup(proc.wait)
        self.addCleanup(proc.terminate)
        self.assertEqual(self.seed().status, "seeded")

    def test_a_build_running_in_main_is_not_copied(self):
        fake_ninja = self.tmp / "ninja"
        shutil.copy(shutil.which("sleep"), fake_ninja)
        proc = subprocess.Popen([str(fake_ninja), "60"], cwd=self.build)
        self.addCleanup(proc.wait)
        self.addCleanup(proc.terminate)
        result = self.seed()
        self.assertEqual(result.status, "not-seeded", result)
        self.assertIn("running", result.reason)
        self.assertFalse(self.wt_build().exists())

    def test_a_corrupt_deps_log_is_an_error_that_leaves_nothing_behind(self):
        (self.build / ".ninja_deps").write_bytes(b"not a ninja deps log")
        result = self.seed()
        self.assertEqual(result.status, "error", result)
        self.assertIn(".ninja_deps", result.reason)
        self.assertFalse(self.wt_build().exists())
        self.assertEqual(list((self.wt / "build").glob(".seed-*")), [])

    def test_a_failed_copy_is_an_error_that_leaves_nothing_behind(self):
        real_run = self.tool.subprocess.run

        def failing_cp(cmd, *args, **kwargs):
            if cmd[0] == "cp":
                return subprocess.CompletedProcess(cmd, 1, "", "cp: no space left on device")
            return real_run(cmd, *args, **kwargs)

        with mock.patch.object(self.tool.subprocess, "run", failing_cp):
            result = self.seed()
        self.assertEqual(result.status, "error", result)
        self.assertIn("no space left", result.reason)
        self.assertFalse(self.wt_build().exists())
        self.assertEqual(list((self.wt / "build").glob(".seed-*")), [])

    def test_a_worktree_whose_head_cannot_be_read_is_not_seeded_and_the_reason_says_so(self):
        plain = self.tmp / "not-a-repository"
        plain.mkdir()
        result = self.tool.seed(self.main, plain, self.PRESET)
        self.assertEqual(result.status, "not-seeded", result)
        self.assertIn("cannot read the HEAD", result.reason)
        self.assertFalse((plain / "build").exists())

    def test_a_worktree_inside_the_tree_it_would_be_copied_from_is_refused(self):
        inside = self.build / "sub"
        inside.mkdir()
        result = self.tool.seed(self.main, inside, self.PRESET)
        self.assertEqual(result.status, "not-seeded", result)
        self.assertIn("inside", result.reason)

    def test_a_main_tree_that_changes_while_it_is_copied_is_not_used(self):
        real_run = self.tool.subprocess.run

        def run_then_touch(cmd, *args, **kwargs):
            done = real_run(cmd, *args, **kwargs)
            if cmd[0] == "cp":  # a build in the main checkout appends to its log while the copy is made
                with open(self.build / ".ninja_log", "a") as f:
                    f.write("\n")
            return done

        with mock.patch.object(self.tool.subprocess, "run", run_then_touch):
            result = self.seed()
        self.assertEqual(result.status, "not-seeded", result)
        self.assertIn("changed while", result.reason)
        self.assertFalse(self.wt_build().exists())
        self.assertEqual(list((self.wt / "build").glob(".seed-*")), [])

    def test_the_worktree_must_not_be_the_main_checkout(self):
        result = self.tool.seed(self.main, self.main, self.PRESET)
        self.assertEqual(result.status, "not-seeded", result)


class DepsLog(SeedFixture):
    def records(self, data):
        import struct
        off = len(b"# ninjadeps\n") + 4
        while off < len(data):
            (word,) = struct.unpack_from("<I", data, off)
            yield bool(word & 0x80000000), word & 0x7FFFFFFF
            off += 4 + (word & 0x7FFFFFFF)

    def test_every_rewritten_record_keeps_ninjas_four_byte_alignment(self):
        data = (self.build / ".ninja_deps").read_bytes()
        rewrite = self.tool.path_rewriter(str(self.main), str(self.tmp / "a-new-root-of-another-length"))
        new, changed = self.tool.rewrite_deps_log(data, rewrite)
        self.assertGreater(changed, 0)
        sizes = list(self.records(new))
        self.assertEqual(len(sizes), len(list(self.records(data))))
        self.assertTrue(all(size % 4 == 0 for _is_deps, size in sizes), sizes)


class CommandHash(SeedFixture):
    def test_the_hash_is_the_one_ninja_records(self):
        log = [line.rstrip("\n").split("\t") for line in (self.build / ".ninja_log").read_text().splitlines()
               if not line.startswith("#")]
        self.assertGreaterEqual(len(log), 4)
        for _start, _end, _mtime, output, recorded in log:
            command = subprocess.run(["ninja", "-C", str(self.build), "-t", "commands", "-s", output],
                                     check=True, capture_output=True, text=True).stdout.rstrip("\n")
            self.assertEqual(f"{self.tool.command_hash(command.encode()):x}", recorded, output)

    def test_every_input_length_class_hashes_like_ninja(self):
        """rapidhash branches on the input length (0-3, 4-16, 17-48, 49 and over, and per 48-byte block): make ninja
        record commands of each class and compare."""
        lengths = (1, 2, 3, 4, 5, 8, 9, 12, 15, 16, 17, 24, 32, 33, 48, 49, 50, 96, 97, 98, 145, 500)
        d = self.tmp / "lengths"
        d.mkdir()
        manifest = []
        commands = {}
        for n in lengths:
            if n < 16:
                command = (":;" * n)[:n]  # a shell no-op of exactly n bytes
            else:
                out = f"o{n}"
                command = f"touch {out} #"
                command += "x" * (n - len(command))
            self.assertEqual(len(command), n)
            commands[f"o{n}"] = command
            manifest.append(f"rule r{n}\n  command = {command}\nbuild o{n}: r{n}\n")
        (d / "build.ninja").write_text("".join(manifest))
        subprocess.run(["ninja", "-C", str(d)], check=True, capture_output=True)
        log = {f[3]: f[4] for f in (line.split("\t") for line in (d / ".ninja_log").read_text().splitlines()
                                    if not line.startswith("#"))}
        self.assertEqual(sorted(log), sorted(commands))
        for out, command in commands.items():
            self.assertEqual(f"{self.tool.command_hash(command.encode()):x}", log[out], f"{len(command)}-byte command")


class Cli(SeedFixture):
    def run_cli(self, *extra):
        log = self.tmp / "seed.jsonl"
        proc = subprocess.run([sys.executable, "-I", str(TOOL), "--main", str(self.main), "--worktree", str(self.wt),
                               "--preset", self.PRESET, "--log", str(log), *extra], capture_output=True, text=True)
        return proc, log

    def test_seeding_prints_a_summary_and_appends_a_record(self):
        proc, log = self.run_cli()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn(f"seeded build/{self.PRESET}", proc.stdout)
        record = json.loads(log.read_text().splitlines()[-1])
        self.assertEqual(record["status"], "seeded")
        self.assertEqual(record["preset"], self.PRESET)
        self.assertGreater(record["log_entries_kept"], 0)
        self.assertEqual(record["log_entries_dropped"], 0)

    def test_not_seeding_says_why_on_stderr_and_is_recorded(self):
        shutil.rmtree(self.build)
        proc, log = self.run_cli()
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("not seeded", proc.stderr)
        self.assertIn(f"build/{self.PRESET}", proc.stderr)
        self.assertEqual(json.loads(log.read_text().splitlines()[-1])["status"], "not-seeded")

    def test_a_log_that_cannot_be_written_is_an_error(self):
        proc, _ = self.run_cli("--log", str(self.tmp / "no-such-dir/x/seed.jsonl"))
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("cannot write", proc.stderr)


if __name__ == "__main__":
    unittest.main()
