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

REPO = pathlib.Path(__file__).resolve().parents[2]
TOOL = REPO / "tools/seed_build_tree.py"


def load_tool():
    spec = importlib.util.spec_from_file_location("seed_build_tree", TOOL)
    module = importlib.util.module_from_spec(spec)
    sys.modules["seed_build_tree"] = module
    spec.loader.exec_module(module)
    return module


def git(cwd, *args):
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-C", str(cwd), *args],
                          check=True, capture_output=True, text=True)


def tree_digest(root):
    """Every path, its bytes and its mtime: a change anywhere under root changes the digest."""
    h = hashlib.sha256()
    for path in sorted(pathlib.Path(root).rglob("*")):
        st = path.lstat()
        h.update(f"{path.relative_to(root)}|{st.st_mtime_ns}|{st.st_size}\n".encode())
        if path.is_file() and not path.is_symlink():
            h.update(path.read_bytes())
    return h.hexdigest()


def pending_steps(build_dir):
    """The edges `ninja -n` would run, as the output names of its [n/m] lines."""
    out = subprocess.run(["ninja", "-C", str(build_dir), "-n"], check=True, capture_output=True, text=True).stdout
    return sorted(m.group(1) for m in re.finditer(r"^\[\d+/\d+\] \S+ (?:.* )?(\S+)$", out, re.M)), out


class SeedFixture(unittest.TestCase):
    PRESET = "p"
    WORKTREE_NAME = "wt"

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
        git(self.main, "init", "-q", "-b", "main")
        git(self.main, "add", "-A")
        git(self.main, "commit", "-q", "--no-gpg-sign", "-m", "init")
        self.build = self.main / "build" / self.PRESET
        self.build.mkdir(parents=True)
        # An output spelled as an absolute path, as CMake does for some custom commands.
        (self.build / "build.ninja").write_text(
            f"rule cc\n  command = cc -MD -MF $out.d -I{self.main}/inc -c $in -o $out\n"
            "  depfile = $out.d\n  deps = gcc\n"
            "rule link\n  command = cc $in -o $out\n"
            f"build a.o: cc {self.main}/src/a.c\n"
            f"build b.o: cc {self.main}/src/b.c\n"
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
        git(self.wt, "commit", "-q", "-a", "--no-gpg-sign", "-m", "branch change")
        self.assertEqual(self.seed().status, "seeded")
        steps, out = pending_steps(self.wt_build())
        self.assertEqual(steps, ["a.o", "app"], out)

    def test_the_cache_points_at_the_worktree_and_leaves_other_directories_alone(self):
        self.assertEqual(self.seed().status, "seeded")
        cache = (self.wt_build() / "CMakeCache.txt").read_text()
        self.assertIn(f"CMAKE_HOME_DIRECTORY:INTERNAL={self.wt}\n", cache)
        self.assertIn(f"CMAKE_CACHEFILE_DIR:INTERNAL={self.wt_build()}\n", cache)
        self.assertIn(f"SCRATCH:PATH={self.main}-work/scratch\n", cache)
        self.assertNotIn(f"={self.main}\n", cache)

    def test_the_main_checkout_is_not_modified(self):
        before = tree_digest(self.main)
        self.assertEqual(self.seed().status, "seeded")
        self.assertEqual(tree_digest(self.main), before)

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


class SeededTreeWithAnotherPathLength(SeededTree):
    """The deps log pads each path to four bytes: a worktree path of another length than main's must work too."""
    WORKTREE_NAME = "worktree-with-a-longer-name-x"


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

    def test_a_build_running_in_main_is_not_copied(self):
        proc = subprocess.Popen(["sleep", "60"], cwd=self.build)
        self.addCleanup(proc.wait)
        self.addCleanup(proc.terminate)
        result = self.seed()
        self.assertEqual(result.status, "not-seeded", result)
        self.assertIn("running", result.reason)
        self.assertFalse(self.wt_build().exists())

    def test_the_worktree_must_not_be_the_main_checkout(self):
        result = self.tool.seed(self.main, self.main, self.PRESET)
        self.assertEqual(result.status, "not-seeded", result)


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
        lengths = (1, 2, 3, 5, 8, 9, 12, 16, 17, 24, 32, 33, 48, 49, 50, 96, 97, 98, 145, 500)
        d = self.tmp / "lengths"
        d.mkdir()
        manifest = []
        commands = {}
        for n in lengths:
            if n <= 3:
                command = {1: ":", 2: ":;", 3: ":;:"}[n]
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
