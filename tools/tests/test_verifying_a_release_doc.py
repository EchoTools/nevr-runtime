"""#925: docs/reference/verifying-a-release.md and its fixtures stay true.

The page tells an installer maintainer which public keys to pin, prints their key ids in two spellings, and ships
scripts and signed test vectors. Prose cannot be wrong in a way anything notices, so these checks read the page:

- the key ids it prints are the ids the printed keys decode to (a wrong id makes a pin or a revocation never match);
- the fixtures verify with the test key it names, using an Ed25519 verifier written here from RFC 8032 and
  hashlib (no minisign needed), and a flipped byte does not;
- the fixtures differ from each other in the way the page says;
- the code blocks the page tells the reader to run exist;
- when minisign and jq are installed, the page's own shell script gives the verdicts its vector table promises;
- the page names no host but the manifest's, and no private repository or local path.
"""

import hashlib
import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DOC = ROOT / "docs/reference/verifying-a-release.md"
VECTORS = ROOT / "docs/reference/release-verification"

BLOCK = re.compile(r"^```(\w+) (\S+)\n(.*?)^```$", re.S | re.M)

EXPECTED_BLOCKS = {
    "pinned-keys.txt",
    "test-pinned-keys.txt",
    "verify-manifest.sh",
    "check-artifact.sh",
    "check-zip.py",
    "Cargo.toml",
    "src/main.rs",
    "NevrManifest/NevrManifest.csproj",
    "NevrManifest/ManifestCheck.cs",
    "Harness/Harness.csproj",
    "Harness/Program.cs",
}

# The fixture's expiry: the page's expired-manifest case uses a time after it.
FIXTURE_EXPIRES = "2026-11-07T18:00:00Z"


# --- Ed25519 verification, RFC 8032 section 6 (the reference code, reduced to verification) ---

_P = 2**255 - 19
_Q = 2**252 + 27742317777372353535851937790883648493
_D = -121665 * pow(121666, _P - 2, _P) % _P
_SQRT_M1 = pow(2, (_P - 1) // 4, _P)


def _add(a, b):
    A, B = (a[1] - a[0]) * (b[1] - b[0]) % _P, (a[1] + a[0]) * (b[1] + b[0]) % _P
    C, D = 2 * a[3] * b[3] * _D % _P, 2 * a[2] * b[2] % _P
    E, F, G, H = B - A, D - C, D + C, B + A
    return (E * F % _P, G * H % _P, F * G % _P, E * H % _P)


def _mul(s, point):
    result = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            result = _add(result, point)
        point = _add(point, point)
        s >>= 1
    return result


def _equal(a, b):
    return (a[0] * b[2] - b[0] * a[2]) % _P == 0 and (a[1] * b[2] - b[1] * a[2]) % _P == 0


def _recover_x(y, sign):
    if y >= _P:
        return None
    x2 = (y * y - 1) * pow(_D * y * y + 1, _P - 2, _P) % _P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P != 0:
        x = x * _SQRT_M1 % _P
    if (x * x - x2) % _P != 0:
        return None
    if (x & 1) != sign:
        x = _P - x
    return x


def _decompress(s):
    y = int.from_bytes(s, "little")
    sign, y = y >> 255, y & ((1 << 255) - 1)
    x = _recover_x(y, sign)
    return None if x is None else (x, y, 1, x * y % _P)


_GY = 4 * pow(5, _P - 2, _P) % _P
_GX = _recover_x(_GY, 0)
_G = (_GX, _GY, 1, _GX * _GY % _P)


def ed25519_verify(public: bytes, message: bytes, signature: bytes) -> bool:
    if len(public) != 32 or len(signature) != 64:
        return False
    a, r = _decompress(public), _decompress(signature[:32])
    s = int.from_bytes(signature[32:], "little")
    if a is None or r is None or s >= _Q:
        return False
    h = int.from_bytes(hashlib.sha512(signature[:32] + public + message).digest(), "little") % _Q
    return _equal(_mul(s, _G), _add(r, _mul(h, a)))


# --- minisign files ---


def decode_pub(base64_key: str):
    import base64

    blob = base64.b64decode(base64_key, validate=True)
    assert len(blob) == 42 and blob[:2] == b"Ed", "not a minisign public key"
    return blob[2:10], blob[10:]


def read_sig(path: Path):
    """(alg, key id in file order, signature, trusted comment) of a minisign .sig file."""
    import base64

    lines = path.read_text().split("\n")
    blob = base64.b64decode(lines[1], validate=True)
    assert len(blob) == 74
    assert lines[2].startswith("trusted comment: ")
    return blob[:2], blob[2:10], blob[10:], lines[2][len("trusted comment: "):], base64.b64decode(lines[3])


def minisign_verify(manifest: bytes, sig, public: bytes) -> bool:
    alg, _key_id, signature, comment, global_signature = sig
    if alg != b"ED":
        return False
    digest = hashlib.blake2b(manifest, digest_size=64).digest()
    return ed25519_verify(public, digest, signature) and ed25519_verify(
        public, signature + comment.encode(), global_signature
    )


# --- the page ---


def doc_text() -> str:
    return DOC.read_text()


def names(text: str, needle: str) -> bool:
    """`needle in text` for the page, so a failure prints the needle and not the whole page."""
    return needle in text


def blocks() -> dict:
    return {name: body for _lang, name, body in BLOCK.findall(doc_text())}


def key_lines(block: str):
    return [line.split() for line in block.splitlines() if line.strip() and not line.startswith("#")]


# The three pinned keys exactly as the page must print them. The keys' source is a file outside this repository,
# so a change to a key is a deliberate edit of the page AND of this line.
PINNED = {
    "K1": "RWTA7pyFRwI9NPbeZVbRJxtiA0YsG3jolqhV8mxuCEP0AUb24092wkuy",
    "K2": "RWQ+cq9F86dyVc7yFNVU+YAVnSl5pJFun+A68lujJ1p3GsMb2v3vrK0F",
    "KR": "RWRBDKzhAOWXf+lhvvVo22yLwhwmABcky2IkqcvmLWtpG8Pm9/ux8dYx",
}


class PinnedKeys(unittest.TestCase):
    def test_the_printed_keys_are_the_pinned_keys(self):
        self.assertEqual({label: key for label, key in key_lines(blocks()["pinned-keys.txt"])}, PINNED)

    def test_the_doc_prints_exactly_k1_k2_and_kr(self):
        self.assertEqual([label for label, _key in key_lines(blocks()["pinned-keys.txt"])], ["K1", "K2", "KR"])

    def test_each_printed_key_id_is_the_id_the_key_decodes_to_in_both_spellings(self):
        block = blocks()["pinned-keys.txt"]
        comments = [line for line in block.splitlines() if line.startswith("# K")]
        self.assertEqual(len(comments), 3, "one comment line per pinned key")
        for comment, (label, key) in zip(comments, key_lines(block)):
            key_id, _public = decode_pub(key)
            file_order = key_id.hex()
            displayed = key_id[::-1].hex().upper()
            self.assertEqual(
                comment,
                f"# {label}: key id {displayed} (file order {file_order})",
                f"{label}: the printed ids are not the ones the key decodes to",
            )

    def test_the_test_key_block_is_the_test_key_file(self):
        (label, key), = key_lines(blocks()["test-pinned-keys.txt"])
        self.assertEqual(label, "TEST")
        self.assertEqual(key, (VECTORS / "test-key.pub").read_text().split("\n")[1])

    def test_the_doc_names_the_test_key_id_in_both_spellings(self):
        key_id, _public = decode_pub(key_lines(blocks()["test-pinned-keys.txt"])[0][1])
        self.assertTrue(names(doc_text(), key_id[::-1].hex().upper()), "the displayed id of the test key")
        self.assertTrue(names(doc_text(), key_id.hex()), "the file-order id of the test key")

    def test_the_test_key_is_not_a_pinned_key(self):
        pinned = {decode_pub(key)[0] for _label, key in key_lines(blocks()["pinned-keys.txt"])}
        test_id, _public = decode_pub(key_lines(blocks()["test-pinned-keys.txt"])[0][1])
        self.assertNotIn(test_id, pinned)


class Fixtures(unittest.TestCase):
    def setUp(self):
        _id, self.test_public = decode_pub(key_lines(blocks()["test-pinned-keys.txt"])[0][1])
        self.test_id = _id

    def manifest(self, name):
        return (VECTORS / name / "manifest.json").read_bytes()

    def sig(self, name):
        return read_sig(VECTORS / name / "manifest.json.sig")

    def test_good_verifies_with_the_test_key(self):
        self.assertTrue(minisign_verify(self.manifest("good"), self.sig("good"), self.test_public))

    def test_good_signature_is_hashed_mode_by_the_test_key(self):
        alg, key_id, _signature, comment, _global = self.sig("good")
        self.assertEqual((alg, key_id), (b"ED", self.test_id))
        self.assertEqual(comment, f"nevr-runtime manifest_version=1 expires={FIXTURE_EXPIRES}")

    def test_one_flipped_byte_of_the_manifest_does_not_verify(self):
        data = bytearray(self.manifest("good"))
        data[100] ^= 1
        self.assertFalse(minisign_verify(bytes(data), self.sig("good"), self.test_public))

    def test_a_changed_trusted_comment_does_not_verify(self):
        alg, key_id, signature, comment, global_signature = self.sig("good")
        changed = (alg, key_id, signature, comment.replace("manifest_version=1", "manifest_version=2"), global_signature)
        self.assertFalse(minisign_verify(self.manifest("good"), changed, self.test_public))

    def test_legacy_ed_fixture_is_legacy_mode_by_the_test_key(self):
        alg, key_id, _signature, _comment, _global = self.sig("legacy-ed")
        self.assertEqual((alg, key_id), (b"Ed", self.test_id))

    def test_unpinned_key_fixture_verifies_with_its_own_key_only(self):
        alg, key_id, _signature, _comment, _global = self.sig("unpinned-key")
        pinned = {decode_pub(key)[0] for _label, key in key_lines(blocks()["pinned-keys.txt"])}
        self.assertEqual(alg, b"ED")
        self.assertNotIn(key_id, pinned | {self.test_id})
        self.assertFalse(minisign_verify(self.manifest("unpinned-key"), self.sig("unpinned-key"), self.test_public))

    def test_same_version_fixture_has_the_same_version_other_bytes_and_a_good_signature(self):
        good, other = self.manifest("good"), self.manifest("same-version-other-bytes")
        self.assertNotEqual(good, other)
        self.assertEqual(json.loads(good)["manifest_version"], json.loads(other)["manifest_version"])
        self.assertTrue(minisign_verify(other, self.sig("same-version-other-bytes"), self.test_public))

    def test_every_fixture_manifest_is_the_example_the_doc_describes(self):
        for name in ("good", "legacy-ed", "unpinned-key", "same-version-other-bytes"):
            m = json.loads(self.manifest(name))
            self.assertEqual((m["schema_version"], m["product"], m["manifest_version"]), (1, "nevr-runtime", 1), name)
            self.assertEqual(m["expires"], FIXTURE_EXPIRES, name)
            self.assertEqual(m["channels"], {"beta": "5.5.1"}, name)
            self.assertNotIn("stable", m["channels"], name)
        self.assertEqual(self.manifest("good"), self.manifest("legacy-ed"))
        self.assertEqual(self.manifest("good"), self.manifest("unpinned-key"))


class PageContent(unittest.TestCase):
    def test_every_code_block_the_page_tells_the_reader_to_use_exists(self):
        self.assertEqual(set(blocks()), EXPECTED_BLOCKS)

    def test_the_page_names_every_vector_directory_that_exists(self):
        directories = {p.name for p in VECTORS.iterdir() if p.is_dir()}
        self.assertEqual(directories, {"good", "legacy-ed", "unpinned-key", "same-version-other-bytes"})
        for name in directories:
            self.assertTrue(names(doc_text(), f"`{name}/`"), f"the page does not name the vector directory {name}")

    def test_the_only_host_the_page_names_is_the_manifest_host(self):
        hosts = set(re.findall(r"https?://([^/\s)`'\"]+)", doc_text()))
        self.assertLessEqual(hosts, {"evr.echo.taxi", "127.0.0.1"}, f"hosts named: {sorted(hosts)}")
        self.assertIn("evr.echo.taxi", hosts)

    def test_the_page_names_no_private_repository_and_no_local_path(self):
        text = doc_text()
        for forbidden in ("release-signer", "/home/", "/var/tmp", "/mnt/", "/tmp/", "C:\\Users", "~/"):
            self.assertFalse(names(text, forbidden), f"the page names {forbidden!r}")

    def test_the_status_of_the_host_names_what_is_not_built(self):
        text = doc_text()
        for needle in ("**Built**", "**Not built", "docs/adr/0012-static-install-root.md", "docs/adr/0010-signed-plugins-only.md"):
            self.assertTrue(names(text, needle), f"the host section lacks {needle!r}")


@unittest.skipUnless(
    shutil.which("minisign") and shutil.which("jq") and shutil.which("sha256sum") and shutil.which("base64"),
    "needs minisign, jq, sha256sum and base64: the Ed25519 checks above run without them",
)
class ShellScriptVerdicts(unittest.TestCase):
    """The page's verify-manifest.sh, extracted and run, gives the verdict its vector table promises."""

    NOW = "2026-10-26T12:00:00Z"

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        for name, body in blocks().items():
            if "/" not in name:
                (cls.dir / name).write_text(body)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_script(self, vector: Path, pinned: str, state: dict, now: str = NOW, channel: str = "beta"):
        state_path = self.dir / "state.json"
        state_path.write_text(json.dumps(state))
        result = subprocess.run(
            ["sh", str(self.dir / "verify-manifest.sh"), str(vector), str(self.dir / pinned), str(state_path), now, channel],
            capture_output=True,
            text=True,
            check=False,
        )
        return result.returncode, result.stdout, result.stderr, json.loads(state_path.read_text())

    def test_good_passes_with_the_test_key_and_stores_the_state(self):
        code, out, err, state = self.run_script(VECTORS / "good", "test-pinned-keys.txt", {})
        self.assertEqual((code, err), (0, ""))
        self.assertIn("OK signed by key id c7b61c00e82fdcb1; manifest_version 1; channel beta -> 5.5.1", out)
        self.assertEqual(state["highest_manifest_version"], 1)
        self.assertEqual(state["last_manifest_sha256"], hashlib.sha256((VECTORS / "good/manifest.json").read_bytes()).hexdigest())

    def test_good_is_refused_with_the_real_pinned_keys(self):
        code, _out, err, _state = self.run_script(VECTORS / "good", "pinned-keys.txt", {})
        self.assertEqual((code, err.strip()), (1, "FAIL step 2: signing key c7b61c00e82fdcb1 is not pinned"))

    def test_legacy_ed_is_refused_although_minisign_accepts_it(self):
        code, _out, err, _state = self.run_script(VECTORS / "legacy-ed", "test-pinned-keys.txt", {})
        self.assertEqual((code, err.strip()), (1, "FAIL step 2: signature algorithm is not ED (hashed)"))
        accepted = subprocess.run(
            ["minisign", "-V", "-q", "-m", str(VECTORS / "legacy-ed/manifest.json"), "-x", str(VECTORS / "legacy-ed/manifest.json.sig"),
             "-p", str(VECTORS / "test-key.pub")],
            capture_output=True,
            check=False,
        )
        self.assertEqual(accepted.returncode, 0, "the page says the minisign command alone accepts a legacy signature")

    def test_unpinned_key_is_refused(self):
        code, _out, err, _state = self.run_script(VECTORS / "unpinned-key", "test-pinned-keys.txt", {})
        self.assertEqual(code, 1)
        self.assertRegex(err, r"^FAIL step 2: signing key [0-9a-f]{16} is not pinned")

    def test_same_version_other_bytes_is_refused_at_step_4(self):
        good = hashlib.sha256((VECTORS / "good/manifest.json").read_bytes()).hexdigest()
        code, _out, err, _state = self.run_script(
            VECTORS / "same-version-other-bytes",
            "test-pinned-keys.txt",
            {"highest_manifest_version": 1, "last_manifest_sha256": good},
        )
        self.assertEqual((code, err.strip()), (1, "FAIL step 4: same manifest_version, different bytes"))

    def test_a_lower_version_is_refused_at_step_4(self):
        code, _out, err, _state = self.run_script(VECTORS / "good", "test-pinned-keys.txt", {"highest_manifest_version": 2})
        self.assertEqual((code, err.strip()), (1, "FAIL step 4: manifest version went down: got 1, have 2"))

    def test_an_expired_manifest_is_refused_at_step_5(self):
        code, _out, err, state = self.run_script(VECTORS / "good", "test-pinned-keys.txt", {}, now="2026-11-08T00:00:00Z")
        self.assertEqual(code, 1)
        self.assertIn(f"FAIL step 5: update information is stale since {FIXTURE_EXPIRES}", err)
        self.assertEqual(state, {}, "an expired manifest is not stored")

    def test_a_revoked_signing_key_is_refused_at_step_2(self):
        code, _out, err, _state = self.run_script(VECTORS / "good", "test-pinned-keys.txt", {"revoked_keys": ["c7b61c00e82fdcb1"]})
        self.assertEqual((code, err.strip()), (1, "FAIL step 2: signing key c7b61c00e82fdcb1 is revoked"))

    def test_a_flipped_byte_is_refused_at_step_2(self):
        tampered = self.dir / "tampered"
        shutil.copytree(VECTORS / "good", tampered, dirs_exist_ok=True)
        data = bytearray((tampered / "manifest.json").read_bytes())
        data[100] ^= 1
        (tampered / "manifest.json").write_bytes(bytes(data))
        code, _out, err, _state = self.run_script(tampered, "test-pinned-keys.txt", {})
        self.assertEqual((code, err.strip()), (1, "FAIL step 2: signature does not verify"))

    def test_a_channel_with_no_release_is_refused_at_step_7(self):
        code, _out, err, _state = self.run_script(VECTORS / "good", "test-pinned-keys.txt", {}, channel="stable")
        self.assertEqual((code, err.strip()), (1, "FAIL step 7: channel stable has no release"))


if __name__ == "__main__":
    unittest.main()
