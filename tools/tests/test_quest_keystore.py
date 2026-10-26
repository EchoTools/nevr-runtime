"""tools/quest_keystore.sh against a fake `pass`: the Quest APK signing key is read from the pass store into a private
temp file for one command and removed on every exit path; it is never generated, printed or put on a command line.
The fixture key is made here with keytool for the test; the real key is never touched."""
from __future__ import annotations

import base64
import hashlib
import os
import pathlib
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
ALIAS = "nevrdebug"
KS_ENTRY = "services/echovrce/quest-signing-keystore"
PASS_ENTRY = "services/echovrce/quest-signing-keystore-pass"

FAKE_PASS = r'''#!/bin/bash
# A fake `pass`: entries are files under $FAKE_PASS_DIR (the entry path with "/" turned into "__").
dir="$FAKE_PASS_DIR"
name() { printf '%s' "${1//\//__}"; }
case "$1" in
  show) [[ -f "$dir/$(name "$2")" ]] || { echo "Error: $2 is not in the password store." >&2; exit 1; }
        cat "$dir/$(name "$2")" ;;
  insert) shift; [[ "$1" == "-m" ]] && shift
          [[ -f "$dir/$(name "$1")" ]] && { echo "fake pass: $1 exists" >&2; exit 1; }
          cat > "$dir/$(name "$1")" ;;
  *) exit 64 ;;
esac
'''


FAKE_SHRED = r'''#!/bin/bash
# A fake `shred`: record the full argument list where the test can see it, then do the real work.
printf '%s\n' "$*" >> "$FAKE_SHRED_LOG"
exec /usr/bin/shred "$@"
'''


FAKE_KEYTOOL = r'''#!/bin/bash
# A fake `keytool`: record the full argument list where the test can see it, then do the real work.
printf '%s\n' "$*" >> "$FAKE_KEYTOOL_LOG"
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


def fingerprint(keystore: pathlib.Path) -> str:
    out = subprocess.run(
        ["keytool", "-list", "-keystore", str(keystore), "-alias", ALIAS, "-storepass", PASSPHRASE],
        capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        if "SHA-256" in line or "SHA256" in line:
            return line.split(":", 1)[1].strip().replace(":", "").lower()
    raise AssertionError(out)


class QuestKeystoreTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="quest-keystore-test-", dir="/var/tmp"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        (self.bin / "pass").write_text(FAKE_PASS)
        (self.bin / "pass").chmod(0o755)
        (self.bin / "shred").write_text(FAKE_SHRED)
        (self.bin / "shred").chmod(0o755)
        (self.bin / "keytool").write_text(FAKE_KEYTOOL)
        (self.bin / "keytool").chmod(0o755)
        self.keytool_log = self.tmp / "keytool.log"
        self.shred_log = self.tmp / "shred.log"
        self.store = self.tmp / "store"
        self.store.mkdir()
        self.run_dir = self.tmp / "run"
        self.run_dir.mkdir(mode=0o700)
        self.ks = self.tmp / "fixture.jks"
        subprocess.run(
            ["keytool", "-genkeypair", "-keystore", str(self.ks), "-alias", ALIAS, "-keyalg", "RSA", "-keysize", "2048",
             "-validity", "30", "-storepass", PASSPHRASE, "-keypass", PASSPHRASE,
             "-dname", "CN=Quest Keystore Test, O=test"], capture_output=True, check=True)
        self.fp = fingerprint(self.ks)
        self.env = dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}", FAKE_PASS_DIR=str(self.store),
                        FAKE_SHRED_LOG=str(self.shred_log), FAKE_KEYTOOL_LOG=str(self.keytool_log),
                        FAKE_KEYTOOL_REAL=shutil.which("keytool"),
                        XDG_RUNTIME_DIR=str(self.run_dir), NEVR_QUEST_KS_IMPORT_PASS=PASSPHRASE)

    def entry(self, name: str) -> pathlib.Path:
        return self.store / name.replace("/", "__")

    def seed(self) -> None:
        self.entry(KS_ENTRY).write_text(base64.b64encode(self.ks.read_bytes()).decode() + "\n")
        self.entry(PASS_ENTRY).write_text(PASSPHRASE + "\n")

    def run_script(self, *args, **kw):
        return subprocess.run([str(SCRIPT), *args], capture_output=True, text=True, env=self.env, timeout=60, **kw)

    def left_behind(self):
        return [p for p in self.run_dir.rglob("*")]

    # ---- with ------------------------------------------------------------------------------------------------

    def test_with_gives_the_command_a_private_copy_of_the_stored_key(self):
        self.seed()
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
        self.seed()
        self.assertEqual(self.run_script("with", "true").returncode, 0)
        self.assertEqual(self.left_behind(), [])

    def test_the_copy_is_gone_after_the_command_fails_and_its_status_is_kept(self):
        self.seed()
        result = self.run_script("with", "bash", "-c", "exit 7")
        self.assertEqual(result.returncode, 7)
        self.assertEqual(self.left_behind(), [])

    def test_the_copy_is_gone_after_sigterm(self):
        self.seed()
        proc = subprocess.Popen([str(SCRIPT), "with", "sleep", "30"], env=self.env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        deadline = time.time() + 20
        while time.time() < deadline and not [p for p in self.run_dir.rglob("*") if p.is_file()]:
            time.sleep(0.05)
        self.assertTrue([p for p in self.run_dir.rglob("*") if p.is_file()], "the keystore was never written")
        proc.send_signal(signal.SIGTERM)
        proc.communicate(timeout=20)
        self.assertEqual(self.left_behind(), [])

    def passphrase_holders(self, script=SCRIPT):
        """Command lines that carry the passphrase and belong to the tool under test: the tool itself, everything it
        starts while the wrapped command runs, and every `keytool` call it makes (the fake `keytool` records those,
        because they are over before the wrapped command starts). Processes that are not the tool's are not looked at:
        other runs of this file, on the same host, put the passphrase on the command line of their own `keytool`."""
        proc = subprocess.Popen([str(script), "with", "sleep", "30"], env=self.env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
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
            found += [" ".join(argv) for _, argv in tree if PASSPHRASE in " ".join(argv)]
        finally:
            # The tool first, so its trap shreds the key; then whatever it left running, which would hold the pipes
            # open for `communicate`.
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=20)
            finally:
                for pid, _ in tree:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                proc.communicate(timeout=20)
        calls = self.keytool_log.read_text().splitlines() if self.keytool_log.exists() else []
        self.assertTrue(calls, "the tool never called keytool, so there was nothing to check")
        found += [f"keytool {line}" for line in calls if PASSPHRASE in line]
        return found

    def test_neither_the_passphrase_nor_the_key_is_printed_or_on_a_command_line(self):
        self.seed()
        self.assertEqual(self.passphrase_holders(), [], "the passphrase was on a command line of the tool")
        result = self.run_script("with", "true")
        self.assertEqual(result.returncode, 0, result.stderr)
        blob = base64.b64encode(self.ks.read_bytes()).decode()
        for text in (result.stdout, result.stderr):
            self.assertNotIn(PASSPHRASE, text)
            self.assertNotIn(blob[:40], text)

    def test_a_process_that_is_not_the_tools_and_names_the_passphrase_does_not_fail_the_check(self):
        self.seed()
        # `bash -c <script> <name>`: the name is argument 0 of the script and stays on the command line. The script is
        # two commands because bash replaces itself with a script that is one command, and that drops the name.
        other = subprocess.Popen(["bash", "-c", "sleep 30; :", PASSPHRASE])
        self.addCleanup(lambda: (other.kill(), other.wait()))
        everyone = [argv for _, argv in all_processes()]
        self.assertTrue(any(PASSPHRASE in " ".join(argv) for argv in everyone),
                        "the unrelated process is not in the process table, so this test proves nothing")
        self.assertEqual(self.passphrase_holders(), [])

    def leaking_copy(self, old: str, new: str) -> pathlib.Path:
        text = SCRIPT.read_text()
        self.assertEqual(text.count(old), 1, f"{old!r} is not in the script exactly once")
        copy = self.tmp / "leaky_quest_keystore.sh"
        copy.write_text(text.replace(old, new))
        copy.chmod(0o755)
        return copy

    def test_the_check_fails_when_the_tool_puts_the_passphrase_on_the_command_line_of_keytool(self):
        self.seed()
        leaky = self.leaking_copy('-alias "$ALIAS" -storepass:env NEVR_QUEST_KS_PASS >/dev/null 2>&1; then\n        err "the stored keystore',
                                  '-alias "$ALIAS" -storepass "$passphrase" >/dev/null 2>&1; then\n        err "the stored keystore')
        found = self.passphrase_holders(leaky)
        self.assertTrue(any(line.startswith("keytool ") for line in found), found)

    def test_the_check_fails_when_a_process_the_tool_starts_holds_the_passphrase(self):
        self.seed()
        leaky = self.leaking_copy('    log "keystore ready entry=$KS_ENTRY alias=$ALIAS"',
                                  '    bash -c "sleep 30; :" "$passphrase" &\n    log "keystore ready entry=$KS_ENTRY alias=$ALIAS"')
        found = self.passphrase_holders(leaky)
        self.assertTrue(any(line.startswith("bash -c sleep 30; :") for line in found), found)

    def test_a_missing_key_is_an_error_that_names_the_import_and_the_command_never_runs(self):
        marker = self.tmp / "ran"
        result = self.run_script("with", "bash", "-c", f"touch {marker}")
        self.assertEqual(result.returncode, 1)
        self.assertIn("ERROR", result.stderr)
        self.assertIn("import-quest-keystore", result.stderr)
        self.assertFalse(marker.exists())
        self.assertEqual(self.left_behind(), [])

    def test_a_missing_passphrase_is_an_error_too(self):
        self.seed()
        self.entry(PASS_ENTRY).unlink()
        marker = self.tmp / "ran"
        result = self.run_script("with", "bash", "-c", f"touch {marker}")
        self.assertEqual(result.returncode, 1)
        self.assertFalse(marker.exists())
        self.assertEqual(self.left_behind(), [])

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
        self.seed()
        seen = self.tmp / "seen-key-success"
        result = self.run_script("with", "bash", "-c", f'printf "%s" "$NEVR_QUEST_KS" > {seen}')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_shredded(seen.read_text())
        self.assertEqual(self.left_behind(), [])

    def test_shred_is_called_on_the_key_after_the_command_fails(self):
        self.seed()
        seen = self.tmp / "seen-key-failure"
        result = self.run_script("with", "bash", "-c", f'printf "%s" "$NEVR_QUEST_KS" > {seen}; exit 7')
        self.assertEqual(result.returncode, 7)
        self.assert_shredded(seen.read_text())
        self.assertEqual(self.left_behind(), [])

    def assert_shredded_after_signal(self, sig, parent_ignores_sigint=False):
        self.seed()
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
        self.assertTrue(keys, "the keystore was never written")
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

    # ---- fingerprint ----------------------------------------------------------------------------------------

    def test_fingerprint_prints_only_the_signer_sha256(self):
        self.seed()
        result = self.run_script("fingerprint")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, self.fp + "\n")
        self.assertEqual(self.left_behind(), [])

    # ---- import ---------------------------------------------------------------------------------------------

    def test_import_stores_the_key_and_its_passphrase_and_prints_only_the_fingerprint(self):
        result = self.run_script("import", str(self.ks))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, f"imported: signer sha256 {self.fp}\n")
        self.assertEqual(base64.b64decode(self.entry(KS_ENTRY).read_text()), self.ks.read_bytes())
        self.assertEqual(self.entry(PASS_ENTRY).read_text().splitlines()[0], PASSPHRASE)
        self.assertNotIn(PASSPHRASE, result.stdout + result.stderr)

    def test_import_refuses_when_a_key_is_already_stored_and_changes_nothing(self):
        self.seed()
        other = self.tmp / "other.jks"
        subprocess.run(["keytool", "-genkeypair", "-keystore", str(other), "-alias", ALIAS, "-keyalg", "RSA",
                        "-keysize", "2048", "-validity", "30", "-storepass", PASSPHRASE, "-keypass", PASSPHRASE,
                        "-dname", "CN=Another Key"], capture_output=True, check=True)
        before = self.entry(KS_ENTRY).read_text()
        result = self.run_script("import", str(other))
        self.assertEqual(result.returncode, 1)
        self.assertIn("already", result.stderr)
        self.assertEqual(self.entry(KS_ENTRY).read_text(), before)

    def test_import_refuses_a_file_that_is_not_there(self):
        result = self.run_script("import", str(self.tmp / "nope.jks"))
        self.assertEqual(result.returncode, 1)
        self.assertFalse(self.entry(KS_ENTRY).exists())

    # ---- the recipe ----------------------------------------------------------------------------------------

    def recipe(self, name: str) -> str:
        text = JUSTFILE.read_text()
        start = text.index(f"\n{name} ")
        end = text.index("\n\n", start)
        return text[start:end]

    def test_the_repack_never_generates_a_key_and_signs_through_the_pass_store(self):
        body = self.recipe("android-repack-apk")
        self.assertNotIn("keytool -genkeypair", body)
        self.assertNotIn("genkeypair", JUSTFILE.read_text())
        self.assertIn("tools/quest_keystore.sh", body)
        self.assertIn("--ks-pass env:", body)

    def test_the_recipes_to_import_and_to_read_the_fingerprint_exist(self):
        text = JUSTFILE.read_text()
        self.assertIn("import-quest-keystore file", text)
        self.assertIn("quest-keystore-fingerprint", text)


if __name__ == "__main__":
    unittest.main()
