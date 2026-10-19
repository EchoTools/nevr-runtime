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

    def test_neither_the_passphrase_nor_the_key_is_printed_or_on_a_command_line(self):
        self.seed()
        probe = 'ps -eo args | grep -c "[f]ixture-pass-7f3a91" || true'
        result = self.run_script("with", "bash", "-c", probe)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "0", "the passphrase was on a command line")
        blob = base64.b64encode(self.ks.read_bytes()).decode()
        for text in (result.stdout, result.stderr):
            self.assertNotIn(PASSPHRASE, text)
            self.assertNotIn(blob[:40], text)

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
