"""tools/quest_keystore.sh: the Quest APK signing key is the keystore file NEVR_QUEST_KEYSTORE names, opened with the passphrase
in NEVR_QUEST_KEYSTORE_PASS. The script copies it into a private temp file for one command and removes the copy on every
exit path; it is never generated, printed or put on a command line. The fixture key is made here with keytool for the
test; the real key is never touched."""
from __future__ import annotations

import base64
import hashlib
import os
import pathlib
import re
import shutil
import signal
import subprocess
import tempfile
import time
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools/quest_keystore.sh"
JUSTFILE = REPO / "justfile"
PASSPHRASE = "fixture-pass-7f3a91"
ALIAS = "nevr"

FAKE_SHRED = r'''#!/bin/bash
# A fake `shred`: record the full argument list where the test can see it, then do the real work.
printf '%s\n' "$*" >> "$FAKE_SHRED_LOG"
exec /usr/bin/shred "$@"
'''


FAKE_KEYTOOL = r'''#!/bin/bash
# A fake `keytool`: record its pid, its parent's pid and the full argument list where the test can see them, then do
# the real work.
printf '%s %s keytool %s\n' "$$" "$PPID" "$*" >> "$FAKE_KEYTOOL_LOG"
exec "$FAKE_KEYTOOL_REAL" "$@"
'''


def process_table() -> dict:
    """pid -> (ppid, argv) for every process this user can read, from /proc."""
    table = {}
    for entry in pathlib.Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text()
            raw = (entry / "cmdline").read_bytes()
        except OSError:
            continue  # the process ended while the table was being read
        ppid = int(stat[stat.rindex(")") + 2:].split()[1])
        argv = [part.decode(errors="replace") for part in raw.split(b"\0") if part]
        table[int(entry.name)] = (ppid, argv)
    return table


def all_processes() -> list:
    return [(pid, argv) for pid, (_, argv) in process_table().items()]


def process_tree(root: int) -> list:
    """(pid, argv) of `root` and of everything that descends from it right now."""
    table = process_table()
    members, queue = [], [root]
    while queue:
        pid = queue.pop()
        if pid in table:
            members.append((pid, table[pid][1]))
        queue += [child for child, (ppid, _) in table.items() if ppid == pid]
    return members


def alive(pid: int) -> bool:
    """True while `pid` is a process that is running or stopped; a zombie, or a pid that is gone, is not alive."""
    try:
        stat = pathlib.Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return False
    return stat[stat.rindex(")") + 2] not in "ZX"


def fingerprint(keystore: pathlib.Path) -> str:
    out = subprocess.run(
        ["keytool", "-list", "-keystore", str(keystore), "-alias", ALIAS, "-storepass", PASSPHRASE],
        capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        if "SHA-256" in line or "SHA256" in line:
            return line.split(":", 1)[1].strip().replace(":", "").lower()
    raise AssertionError(out)


def stop_tool(proc: subprocess.Popen, tool_log: pathlib.Path, bound_s: float) -> None:
    """Stop the tool under test, and everything it started, by its process group: passphrase_holders starts the tool
    in a session of its own, so the group id is the tool's pid. SIGTERM goes to the tool alone, so its trap shreds the
    key. Its exit is waited for without reaping it, so its pid still names the group when the group is signalled. A
    tool still running after `bound_s` seconds is stopped the same way, and the check fails with a message that names
    the wait and carries the tool's log."""
    os.kill(proc.pid, signal.SIGTERM)
    deadline = time.time() + bound_s
    expired = False
    while os.waitid(os.P_PID, proc.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT) is None:
        if time.time() > deadline:
            expired = True
            break
        time.sleep(0.05)
    os.killpg(proc.pid, signal.SIGKILL)
    proc.wait(timeout=20)
    if expired:
        raise AssertionError(f"the tool (pid {proc.pid}) did not exit within {bound_s} s of SIGTERM; "
                             f"its log: {tool_log.read_text()!r}")


class QuestKeystoreTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="quest-keystore-test-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        (self.bin / "shred").write_text(FAKE_SHRED)
        (self.bin / "shred").chmod(0o755)
        (self.bin / "keytool").write_text(FAKE_KEYTOOL)
        (self.bin / "keytool").chmod(0o755)
        self.keytool_log = self.tmp / "keytool.log"
        self.shred_log = self.tmp / "shred.log"
        self.run_dir = self.tmp / "run"
        self.run_dir.mkdir(mode=0o700)
        self.ks = self.tmp / "fixture.jks"
        subprocess.run(
            ["keytool", "-genkeypair", "-keystore", str(self.ks), "-alias", ALIAS, "-keyalg", "RSA", "-keysize", "2048",
             "-validity", "30", "-storepass", PASSPHRASE, "-keypass", PASSPHRASE,
             "-dname", "CN=Quest Keystore Test, O=test"], capture_output=True, check=True)
        self.fp = fingerprint(self.ks)
        self.env = dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}",
                        FAKE_SHRED_LOG=str(self.shred_log), FAKE_KEYTOOL_LOG=str(self.keytool_log),
                        FAKE_KEYTOOL_REAL=shutil.which("keytool"),
                        XDG_RUNTIME_DIR=str(self.run_dir),
                        NEVR_QUEST_KEYSTORE=str(self.ks), NEVR_QUEST_KEYSTORE_PASS=PASSPHRASE)

    def run_script(self, *args, env=None, **kw):
        return subprocess.run([str(SCRIPT), *args], capture_output=True, text=True,
                              env=self.env if env is None else env, timeout=60, **kw)

    def without(self, *names: str) -> dict:
        return {k: v for k, v in self.env.items() if k not in names}

    def left_behind(self):
        return [p for p in self.run_dir.rglob("*")]

    # ---- with ------------------------------------------------------------------------------------------------

    def test_with_gives_the_command_a_private_copy_of_the_keystore(self):
        out = self.tmp / "seen.txt"
        cmd = ('f="$NEVR_QUEST_KS"; printf "%s %s %s %s" "$(stat -c %a "$f")" "$(stat -c %a "$(dirname "$f")")" '
               f'"$(sha256sum < "$f" | cut -d" " -f1)" "$NEVR_QUEST_KS_ALIAS" > {out}')
        result = self.run_script("with", "bash", "-c", cmd)
        self.assertEqual(result.returncode, 0, result.stderr)
        mode, dir_mode, digest, alias = out.read_text().split()
        self.assertEqual((mode, dir_mode), ("600", "700"))
        self.assertEqual(digest, hashlib.sha256(self.ks.read_bytes()).hexdigest())
        self.assertEqual(alias, ALIAS)

    def test_the_copy_is_gone_after_the_command_succeeds(self):
        self.assertEqual(self.run_script("with", "true").returncode, 0)
        self.assertEqual(self.left_behind(), [])

    def test_the_copy_is_gone_after_the_command_fails_and_its_status_is_kept(self):
        result = self.run_script("with", "bash", "-c", "exit 7")
        self.assertEqual(result.returncode, 7)
        self.assertEqual(self.left_behind(), [])

    def test_the_copy_is_gone_after_sigterm(self):
        proc = subprocess.Popen([str(SCRIPT), "with", "sleep", "30"], env=self.env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        deadline = time.time() + 20
        while time.time() < deadline and not [p for p in self.run_dir.rglob("*") if p.is_file()]:
            time.sleep(0.05)
        self.assertTrue([p for p in self.run_dir.rglob("*") if p.is_file()], "the keystore was never copied")
        proc.send_signal(signal.SIGTERM)
        proc.communicate(timeout=20)
        self.assertEqual(self.left_behind(), [])

    def passphrase_holders(self, script=SCRIPT, stop_bound_s=20):
        """Lines `<pid> <ppid> <command line>` of the processes that carry the passphrase and belong to the tool under
        test: the tool itself, everything it starts while the wrapped command runs, and every `keytool` call it makes
        (the fake `keytool` records those, because they are over before the wrapped command starts). A failure
        therefore names its holder. Processes that are not the tool's are not looked at: other runs of this file, on
        the same host, put the passphrase on the command line of their own `keytool`.

        Its waits are bounded: the wrapped command must start within 30 s, and the tool must exit within stop_bound_s of
        SIGTERM, or the check fails and names that wait. The tool runs in a session of its own, so everything it starts
        is in its process group, and stop_tool stops the group, including a child started after the look. The tool's
        output goes to a file, so no wait depends on a pipe that a process the tool started may hold open."""
        tool_log = self.tmp / "holders-tool.log"
        with tool_log.open("w") as sink:
            proc = subprocess.Popen([str(script), "with", "sleep", "30"], env=self.env, stdout=sink,
                                    stderr=subprocess.STDOUT, start_new_session=True)
        self.addCleanup(lambda: proc.poll() is None and proc.kill())
        found = []
        tree = []
        try:
            deadline = time.time() + 30
            while time.time() < deadline and not any(argv[:1] == ["sleep"] for _, argv in tree):
                time.sleep(0.05)
                tree = process_tree(proc.pid)
            self.assertTrue(any(argv[:1] == ["sleep"] for _, argv in tree),
                            f"the wrapped command never started; tree {tree}")
            self.assertGreaterEqual(len(tree), 2, f"the scan saw {tree}")
            table = process_table()
            found += [f"{pid} {table[pid][0] if pid in table else '?'} {' '.join(argv)}"
                      for pid, argv in tree if PASSPHRASE in " ".join(argv)]
        finally:
            self.tool_pid = proc.pid
            stop_tool(proc, tool_log, stop_bound_s)
        calls = self.keytool_log.read_text().splitlines() if self.keytool_log.exists() else []
        self.assertTrue(calls, "the tool never called keytool, so there was nothing to check")
        found += [line for line in calls if PASSPHRASE in line]
        return found

    def test_neither_the_passphrase_nor_the_key_is_printed_or_on_a_command_line(self):
        self.assertEqual(self.passphrase_holders(), [], "the passphrase was on a command line of the tool")
        result = self.run_script("with", "true")
        self.assertEqual(result.returncode, 0, result.stderr)
        blob = base64.b64encode(self.ks.read_bytes()).decode()
        for text in (result.stdout, result.stderr):
            self.assertNotIn(PASSPHRASE, text)
            self.assertNotIn(blob[:40], text)

    def test_a_process_that_is_not_the_tools_and_names_the_passphrase_does_not_fail_the_check(self):
        # `bash -c <script> <name>`: the name is argument 0 of the script and stays on the command line. The script is
        # two commands because bash replaces itself with a script that is one command, and that drops the name.
        other = subprocess.Popen(["bash", "-c", "sleep 30; :", PASSPHRASE])
        self.addCleanup(lambda: (other.kill(), other.wait()))
        everyone = [argv for _, argv in all_processes()]
        self.assertTrue(any(PASSPHRASE in " ".join(argv) for argv in everyone),
                        "the unrelated process is not in the process table, so this test proves nothing")
        self.assertEqual(self.passphrase_holders(), [])

    def leaking_copy(self, *edits: str) -> pathlib.Path:
        """A copy of the tool with each (old, new) pair of `edits` replaced once: the leaks the checks must catch."""
        text = SCRIPT.read_text()
        for old, new in zip(edits[::2], edits[1::2]):
            self.assertEqual(text.count(old), 1, f"{old!r} is not in the script exactly once")
            text = text.replace(old, new)
        copy = self.tmp / "leaky_quest_keystore.sh"
        copy.write_text(text)
        copy.chmod(0o755)
        return copy

    def test_the_check_fails_when_the_tool_puts_the_passphrase_on_the_command_line_of_keytool(self):
        leaky = self.leaking_copy('-alias "$ALIAS" -storepass:env NEVR_QUEST_KS_PASS >/dev/null 2>&1; then\n        err "$KS_VAR cannot',
                                  '-alias "$ALIAS" -storepass "$NEVR_QUEST_KEYSTORE_PASS" >/dev/null 2>&1; then\n        err "$KS_VAR cannot')
        found = self.passphrase_holders(leaky)
        self.assertTrue(any(re.match(r"^[0-9]+ [0-9]+ keytool ", line) for line in found), found)

    def test_the_check_fails_when_a_process_the_tool_starts_holds_the_passphrase(self):
        leaky = self.leaking_copy('    log "keystore ready alias=$ALIAS"',
                                  '    bash -c "sleep 30; :" "$NEVR_QUEST_KEYSTORE_PASS" &\n    log "keystore ready alias=$ALIAS"')
        found = self.passphrase_holders(leaky)
        self.assertTrue(any(re.match(r"^[0-9]+ [0-9]+ bash -c sleep 30; : ", line) for line in found), found)

    def test_a_process_the_tool_starts_after_the_check_looked_is_stopped_before_the_check_returns(self):
        ready = self.tmp / "late-ready"
        pidfile = self.tmp / "late-child.pid"
        # The holder waits for the tool's key (the flag), then starts a child a second later that holds the tool's
        # output. The check has taken its look by then, and the tool sleeps 3 s in its cleanup, so the child starts
        # before the check stops the tool. A stop that signals only what the look saw, or a wait on the output, waits
        # out the child's 30 s.
        holder = (f"    bash -c 'until [ -e {ready} ]; do sleep 0.05; done; sleep 1; sleep 30 & echo $! > {pidfile}; "
                  "wait' \"$NEVR_QUEST_KEYSTORE_PASS\" &\n"
                  f"    : > {ready}\n"
                  '    log "keystore ready alias=$ALIAS"')
        leaky = self.leaking_copy('    log "keystore ready alias=$ALIAS"', holder,
                                  "    local rc=$?", "    local rc=$?\n    sleep 3")
        self.passphrase_holders(leaky)
        self.assertTrue(pidfile.exists(), "the holder never started its child")
        late = int(pidfile.read_text())
        self.assertFalse(alive(late), f"the child started after the check looked (pid {late}) outlived the check")
        self.assertFalse(pathlib.Path(f"/proc/{self.tool_pid}").exists(),
                         f"the tool (pid {self.tool_pid}) was not reaped before the check returned")

    def test_alive_is_false_for_a_zombie_and_true_for_a_running_process(self):
        zombie = subprocess.Popen(["true"])
        os.waitid(os.P_PID, zombie.pid, os.WEXITED | os.WNOWAIT)  # exited, not reaped: its /proc entry says Z
        self.assertFalse(alive(zombie.pid), "a zombie counted as alive")
        zombie.wait()
        runner = subprocess.Popen(["sleep", "30"])
        self.addCleanup(lambda: (runner.kill(), runner.wait()))
        self.assertTrue(alive(runner.pid), "a running process counted as dead")

    def test_a_tool_that_ignores_sigterm_fails_the_check_and_names_the_wait(self):
        leaky = self.leaking_copy("trap 'handle_signal 143' TERM", "trap '' TERM")
        with self.assertRaises(AssertionError) as caught:
            self.passphrase_holders(leaky, stop_bound_s=1)
        message = str(caught.exception)
        self.assertIn("did not exit within 1 s of SIGTERM", message)
        self.assertIn("keystore ready", message)  # the tool's own log is part of the message

    # ---- inputs: a missing or wrong value stops the command before it runs ---------------------------------

    def assert_stopped_before_the_command(self, result, marker, *names: str):
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("ERROR", result.stderr)
        for name in names:
            self.assertIn(name, result.stderr)
        self.assertFalse(marker.exists(), "the command ran")
        self.assertEqual(self.left_behind(), [])

    def test_a_missing_keystore_variable_is_an_error_that_names_it_and_the_command_never_runs(self):
        marker = self.tmp / "ran"
        result = self.run_script("with", "bash", "-c", f"touch {marker}", env=self.without("NEVR_QUEST_KEYSTORE"))
        self.assert_stopped_before_the_command(result, marker, "NEVR_QUEST_KEYSTORE is not set")

    def test_a_keystore_path_that_is_not_a_readable_file_is_an_error_that_names_it(self):
        marker = self.tmp / "ran"
        missing = self.tmp / "nope.jks"
        env = dict(self.env, NEVR_QUEST_KEYSTORE=str(missing))
        result = self.run_script("with", "bash", "-c", f"touch {marker}", env=env)
        self.assert_stopped_before_the_command(result, marker, "NEVR_QUEST_KEYSTORE", str(missing))

    def test_a_missing_passphrase_variable_is_an_error_that_names_it(self):
        marker = self.tmp / "ran"
        result = self.run_script("with", "bash", "-c", f"touch {marker}", env=self.without("NEVR_QUEST_KEYSTORE_PASS"))
        self.assert_stopped_before_the_command(result, marker, "NEVR_QUEST_KEYSTORE_PASS is not set")

    def test_a_wrong_passphrase_stops_before_the_command_and_does_not_print_it(self):
        marker = self.tmp / "ran"
        wrong = "not-the-passphrase-41c9"
        env = dict(self.env, NEVR_QUEST_KEYSTORE_PASS=wrong)
        result = self.run_script("with", "bash", "-c", f"touch {marker}", env=env)
        self.assert_stopped_before_the_command(result, marker, "cannot be opened with NEVR_QUEST_KEYSTORE_PASS")
        self.assertNotIn(wrong, result.stdout + result.stderr)

    # ---- shred ----------------------------------------------------------------------------------------------

    def shred_lines(self):
        if not self.shred_log.exists():
            return []
        return [line for line in self.shred_log.read_text().splitlines() if line.strip()]

    def assert_shredded(self, key_path):
        lines = self.shred_lines()
        self.assertTrue(lines, "shred was never called")
        self.assertTrue(any("-u" in line.split() and str(key_path) in line for line in lines),
                        f"no 'shred -u {key_path}' in {lines}")

    def test_shred_is_called_on_the_key_after_the_command_succeeds(self):
        seen = self.tmp / "seen-key-success"
        result = self.run_script("with", "bash", "-c", f'printf "%s" "$NEVR_QUEST_KS" > {seen}')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_shredded(seen.read_text())
        self.assertEqual(self.left_behind(), [])

    def test_shred_is_called_on_the_key_after_the_command_fails(self):
        seen = self.tmp / "seen-key-failure"
        result = self.run_script("with", "bash", "-c", f'printf "%s" "$NEVR_QUEST_KS" > {seen}; exit 7')
        self.assertEqual(result.returncode, 7)
        self.assert_shredded(seen.read_text())
        self.assertEqual(self.left_behind(), [])

    def assert_shredded_after_signal(self, sig, parent_ignores_sigint=False):
        # A shell starts a background job with SIGINT ignored and the child inherits it, and a non-interactive bash
        # cannot trap a signal that was ignored on entry. The script is started with the default disposition so the
        # result does not depend on how the test run was started.
        if parent_ignores_sigint:
            previous = signal.signal(signal.SIGINT, signal.SIG_IGN)
            self.addCleanup(signal.signal, signal.SIGINT, previous)
        proc = subprocess.Popen([str(SCRIPT), "with", "sleep", "30"], env=self.env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                preexec_fn=lambda: signal.signal(signal.SIGINT, signal.SIG_DFL))
        self.addCleanup(lambda: proc.poll() is None and proc.kill())
        deadline = time.time() + 20
        while time.time() < deadline and not [p for p in self.run_dir.rglob("*") if p.is_file()]:
            time.sleep(0.05)
        keys = [p for p in self.run_dir.rglob("*") if p.is_file()]
        self.assertTrue(keys, "the keystore was never copied")
        key = keys[0]
        proc.send_signal(sig)
        proc.communicate(timeout=20)
        self.assert_shredded(key)
        self.assertEqual(self.left_behind(), [])

    def test_shred_is_called_on_sigterm(self):
        self.assert_shredded_after_signal(signal.SIGTERM)

    def test_shred_is_called_on_sigint(self):
        self.assert_shredded_after_signal(signal.SIGINT)

    def test_shred_is_called_on_sigint_when_the_test_process_ignores_sigint(self):
        self.assert_shredded_after_signal(signal.SIGINT, parent_ignores_sigint=True)

    def test_shred_is_called_on_sighup(self):
        self.assert_shredded_after_signal(signal.SIGHUP)

    def test_a_failed_temporary_directory_stops_the_tool_before_the_command(self):
        stub_dir = self.tmp / "no-mktemp"
        stub_dir.mkdir()
        (stub_dir / "mktemp").write_text("#!/bin/bash\nexit 1\n")
        (stub_dir / "mktemp").chmod(0o755)
        env = dict(self.env, PATH=f"{stub_dir}:{self.env['PATH']}")
        marker = self.tmp / "ran"
        result = self.run_script("with", "bash", "-c", f"touch {marker}", env=env)
        self.assertEqual(result.returncode, 1)
        self.assertIn("cannot create a private temporary directory", result.stderr)
        self.assertFalse(marker.exists())

    # ---- fingerprint ----------------------------------------------------------------------------------------

    def test_fingerprint_prints_only_the_signer_sha256(self):
        result = self.run_script("fingerprint")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, self.fp + "\n")
        self.assertEqual(self.left_behind(), [])

    # ---- check ----------------------------------------------------------------------------------------------

    def test_check_succeeds_silently_when_the_keystore_opens(self):
        result = self.run_script("check")
        self.assertEqual((result.returncode, result.stdout), (0, ""), result.stderr)
        self.assertEqual(self.left_behind(), [])

    def test_check_names_the_variable_that_is_missing(self):
        result = self.run_script("check", env=self.without("NEVR_QUEST_KEYSTORE_PASS"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("NEVR_QUEST_KEYSTORE_PASS is not set", result.stderr)

    def test_check_fails_on_a_wrong_passphrase(self):
        result = self.run_script("check", env=dict(self.env, NEVR_QUEST_KEYSTORE_PASS="wrong-7e2d"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("cannot be opened", result.stderr)
        self.assertNotIn("wrong-7e2d", result.stdout + result.stderr)

    # ---- the script's own shape ---------------------------------------------------------------------------

    def test_the_script_reads_the_store_only_from_the_two_variables_and_never_from_pass(self):
        text = SCRIPT.read_text()
        self.assertNotRegex(text, r"\bpass (show|insert)\b")
        self.assertNotIn("import-quest-keystore", text)
        self.assertNotIn("nevrdebug", text)
        self.assertIn('readonly ALIAS="nevr"', text)
        self.assertIn('readonly KS_VAR="NEVR_QUEST_KEYSTORE"', text)
        self.assertIn('readonly PASS_VAR="NEVR_QUEST_KEYSTORE_PASS"', text)

    def test_import_is_no_longer_a_mode_of_the_script(self):
        result = self.run_script("import", str(self.ks))
        self.assertEqual(result.returncode, 2)
        self.assertIn("usage", result.stderr)

    def test_the_package_recipes_ask_the_script_for_the_key_instead_of_naming_its_entry(self):
        text = JUSTFILE.read_text()
        self.assertNotIn("pass show services/echovrce", text)
        for name in ("package-dev", "_release-quest-apk"):  # the body of release-quest-apk and release-quest-variant-apk
            self.assertIn("tools/quest_keystore.sh check", self.recipe(name), name)

    # ---- the recipe ----------------------------------------------------------------------------------------

    def recipe(self, name: str) -> str:
        text = JUSTFILE.read_text()
        start = text.index(f"\n{name} ")
        end = text.index("\n\n", start)
        return text[start:end]

    def test_the_repack_never_generates_a_key_and_signs_through_the_keystore_script(self):
        body = self.recipe("android-repack-apk")
        self.assertNotIn("keytool -genkeypair", body)
        self.assertNotIn("genkeypair", JUSTFILE.read_text())
        self.assertIn("tools/quest_keystore.sh", body)
        self.assertIn("--ks-pass env:", body)

    def test_the_recipes_to_read_the_fingerprint_exist_and_import_is_gone(self):
        text = JUSTFILE.read_text()
        self.assertIn("quest-keystore-fingerprint", text)
        self.assertNotIn("import-quest-keystore", text)


if __name__ == "__main__":
    unittest.main()
