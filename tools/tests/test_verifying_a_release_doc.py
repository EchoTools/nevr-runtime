"""#925: docs/reference/verifying-a-release.md and its fixtures stay true.

The page tells an installer maintainer which public keys to pin, prints their key ids in two spellings, and ships
scripts and signed test vectors. Prose cannot be wrong in a way anything notices, so these checks read the page:

- the key ids it prints are the ids the printed keys decode to (a wrong id makes a pin or a revocation never match);
- the fixtures verify with the test key it names, using an Ed25519 verifier written here from RFC 8032 and
  hashlib (no minisign needed), and a flipped byte does not;
- the fixtures differ from each other in the way the page says;
- the code blocks the page tells the reader to run exist;
- when minisign and jq are installed, the page's own scripts (extracted from it) give the verdicts its vector table
  promises, and the cases the review of #935 found: the verified bytes are the parsed bytes, step 8 uses the stored
  manifest, the path is checked before any request, the compiled minimum, the clock, the byte caps, the zip's entry
  types, the APK's certificate set, and recovery from a compromised key;
- the page names no host but the manifest's, and no private repository or local path.
"""

import hashlib
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DOC = ROOT / "docs/reference/verifying-a-release.md"
VECTORS = ROOT / "docs/reference/release-verification"

BLOCK = re.compile(r"^```(\w+) (\S+)\n(.*?)^```$", re.S | re.M)

EXPECTED_BLOCKS = {
    "pinned-keys.txt",
    "test-pinned-keys.txt",
    "fetch-capped.sh",
    "verify-manifest.sh",
    "stored-entry.sh",
    "check-artifact.sh",
    "check-zip.py",
    "check-apk.sh",
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

    def test_the_fixtures_are_said_to_be_test_fixtures_near_the_top_and_beside_them(self):
        def flat(text):
            return re.sub(r"\s+", " ", text.replace(">", " "))

        top = flat("\n".join(doc_text().splitlines()[:14]))
        readme = flat((VECTORS / "README.md").read_text())
        comment = (VECTORS / "test-key.pub").read_text().split("\n")[0]
        for where, text in (("the top of the page", top), ("the fixtures' README", readme)):
            for needle in ("not one of the pinned keys", "never sign anything but real releases"):
                self.assertIn(needle, text, f"{where} lacks {needle!r}")
        self.assertIn("TEST key", comment)
        self.assertIn("not a release key", comment)

    def test_the_status_of_the_host_names_what_is_not_built(self):
        text = doc_text()
        for needle in ("**Built**", "**Not built", "docs/adr/0012-static-install-root.md", "docs/adr/0010-signed-plugins-only.md"):
            self.assertTrue(names(text, needle), f"the host section lacks {needle!r}")


NEEDED_TOOLS = ("minisign", "jq", "sha256sum", "base64", "curl", "dd", "date", "od", "sed")


def have_tools():
    return all(shutil.which(tool) for tool in NEEDED_TOOLS)


class Signer:
    """A throwaway minisign key. The page's fixtures are signed with a test key whose secret half is not kept, so a
    manifest that has to say something else (a hostile path, a revocation, a bad time) is signed with one of these."""

    def __init__(self, root: Path, name: str):
        self.name = name
        self.pub, self.secret = root / (name + ".pub"), root / (name + ".key")
        subprocess.run(["minisign", "-G", "-W", "-f", "-p", str(self.pub), "-s", str(self.secret)], capture_output=True, check=True)
        self.key = self.pub.read_text().split("\n")[1]
        self.id = decode_pub(self.key)[0].hex()

    def sign(self, directory: Path, manifest: bytes, comment: str = "test") -> Path:
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "manifest.json").write_bytes(manifest)
        subprocess.run(
            ["minisign", "-S", "-s", str(self.secret), "-m", str(directory / "manifest.json"),
             "-x", str(directory / "manifest.json.sig"), "-t", comment],
            capture_output=True, check=True)
        return directory


def good_manifest() -> dict:
    return json.loads((VECTORS / "good/manifest.json").read_text())


def dump(manifest: dict) -> bytes:
    return (json.dumps(manifest, indent=2) + "\n").encode()


def small_release(artifacts: dict, version: str = "5.5.1") -> dict:
    """The fixture manifest with its artifacts replaced by small real ones: {kind: (file name, bytes, extra members)}."""
    manifest = good_manifest()
    entries = []
    for kind, (name, data, extra) in artifacts.items():
        entry = {"name": "nevr-runtime-v%s-%s" % (version, name), "kind": kind, "platform": "x",
                 "path": "v%s/nevr-runtime-v%s-%s" % (version, version, name),
                 "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        entry.update(extra)
        entries.append(entry)
    manifest["releases"] = {version: dict(manifest["releases"]["5.5.1"], version=version, artifacts=entries)}
    manifest["channels"] = {"beta": version}
    return manifest


class _Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"  # the body ends when the connection closes, so no length is announced unless asked for

    def do_GET(self):  # noqa: N802 (the name http.server calls)
        self.server.requests.append(self.path)
        body = self.server.routes.get(self.path)
        if body is None:
            self.send_error(404)
            return
        self.send_response(200)
        if self.server.announce:
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *args):
        pass


class Server:
    def __init__(self, routes: dict, announce: bool = False):
        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
        self.httpd.routes, self.httpd.requests, self.httpd.announce = routes, [], announce
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:%d/nevr-runtime" % self.httpd.server_address[1]

    @property
    def requests(self):
        return self.httpd.requests

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join()


@unittest.skipUnless(have_tools(), "needs " + ", ".join(NEEDED_TOOLS) + ": the Ed25519 checks above run without them")
class ScriptCase(unittest.TestCase):
    """Base: the page's own scripts extracted next to each other, a throwaway signer, a pinned-keys file for it."""

    NOW = "2026-10-26T12:00:00Z"

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        cls.scripts = cls.root / "scripts"
        cls.scripts.mkdir()
        for name, body in blocks().items():
            if "/" not in name:
                (cls.scripts / name).write_text(body)
        cls.signer = Signer(cls.root, "signer")
        cls.other = Signer(cls.root, "other")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.t = Path(tempfile.mkdtemp(dir=self.root))
        self.pinned = self.pin(self.signer)

    def fresh(self, label="state") -> Path:
        """A state directory path that does not exist yet and has not been used by this test."""
        self.counter = getattr(self, "counter", 0) + 1
        return self.t / ("%s-%d" % (label, self.counter))

    def pin(self, *signers) -> Path:
        path = self.t / ("pinned-%d.txt" % len(list(self.t.glob("pinned-*.txt"))))
        path.write_text("".join("%s %s\n" % (s.name.upper(), s.key) for s in signers))
        return path

    def run_script(self, name, *args, scripts=None, env=None):
        return subprocess.run(["sh", str((scripts or self.scripts) / name), *map(str, args)],
                              capture_output=True, text=True, check=False, env=env)

    def signed(self, manifest, signer=None, name="m") -> Path:
        data = manifest if isinstance(manifest, bytes) else dump(manifest)
        return (signer or self.signer).sign(self.t / name, data)

    def verify(self, directory, state_dir, now=None, channel="beta", pinned=None, scripts=None):
        return self.run_script("verify-manifest.sh", directory, pinned or self.pinned, state_dir,
                               self.NOW if now is None else now, channel, scripts=scripts)

    def verified(self, manifest, name="state") -> Path:
        """Run steps 2 to 7 on a manifest the throwaway signer signed; the state directory they wrote."""
        state_dir = self.fresh(name)
        result = self.verify(self.signed(manifest), state_dir)
        self.assertEqual((result.returncode, result.stderr), (0, ""), result.stdout)
        return state_dir

    def scripts_with_minimum(self, minimum: int) -> Path:
        directory = self.t / ("scripts-min-%d" % minimum)
        shutil.copytree(self.scripts, directory)
        script = directory / "verify-manifest.sh"
        text = script.read_text()
        self.assertEqual(text.count("\nMIN_MANIFEST_VERSION=1\n"), 1)
        script.write_text(text.replace("\nMIN_MANIFEST_VERSION=1\n", "\nMIN_MANIFEST_VERSION=%d\n" % minimum))
        return directory

    def assert_fails(self, result, step, message):
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        # The last line is the verdict; curl and cp may print their own diagnostic before it.
        self.assertEqual(result.stderr.strip().splitlines()[-1], "FAIL step %d: %s" % (step, message))


class ManifestScript(ScriptCase):
    """verify-manifest.sh, extracted from the page and run on the fixtures."""

    def test_good_passes_with_the_test_key_and_stores_the_verified_bytes_and_the_state(self):
        state_dir = self.t / "state"
        pinned = self.t / "test-pinned.txt"
        shutil.copy(self.scripts / "test-pinned-keys.txt", pinned)
        result = self.verify(VECTORS / "good", state_dir, pinned=pinned)
        self.assertEqual((result.returncode, result.stderr), (0, ""))
        self.assertIn("OK signed by key id c7b61c00e82fdcb1; manifest_version 1; channel beta -> 5.5.1", result.stdout)
        state = json.loads((state_dir / "state.json").read_text())
        good = (VECTORS / "good/manifest.json").read_bytes()
        self.assertEqual(state["highest_manifest_version"], 1)
        self.assertEqual(state["last_manifest_sha256"], hashlib.sha256(good).hexdigest())
        self.assertEqual((state_dir / "manifest.json").read_bytes(), good)
        self.assertEqual((state_dir / "manifest.json.sig").read_bytes(), (VECTORS / "good/manifest.json.sig").read_bytes())
        self.assertEqual(sorted(p.name for p in state_dir.iterdir()), ["manifest.json", "manifest.json.sig", "state.json"])

    def test_good_is_refused_with_the_real_pinned_keys(self):
        result = self.verify(VECTORS / "good", self.t / "state", pinned=self.scripts / "pinned-keys.txt")
        self.assert_fails(result, 2, "signing key c7b61c00e82fdcb1 is not pinned")
        self.assertFalse((self.t / "state").exists(), "a refused manifest leaves no state")

    def test_legacy_ed_is_refused_although_minisign_accepts_it(self):
        pinned = self.t / "test-pinned.txt"
        shutil.copy(self.scripts / "test-pinned-keys.txt", pinned)
        self.assert_fails(self.verify(VECTORS / "legacy-ed", self.t / "state", pinned=pinned), 2,
                          "signature algorithm is not ED (hashed)")
        accepted = subprocess.run(
            ["minisign", "-V", "-q", "-m", str(VECTORS / "legacy-ed/manifest.json"), "-x", str(VECTORS / "legacy-ed/manifest.json.sig"),
             "-p", str(VECTORS / "test-key.pub")], capture_output=True, check=False)
        self.assertEqual(accepted.returncode, 0, "the page says the minisign command alone accepts a legacy signature")

    def test_unpinned_key_is_refused(self):
        result = self.verify(self.signed(good_manifest(), self.other), self.t / "state")
        self.assert_fails(result, 2, "signing key %s is not pinned" % self.other.id)

    def test_same_version_other_bytes_is_refused_at_step_4(self):
        first = self.verified(good_manifest())
        changed = good_manifest()
        changed["generated"] = "2026-10-25T00:00:00Z"
        self.assert_fails(self.verify(self.signed(changed, name="m2"), first), 4, "same manifest_version, different bytes")

    def test_the_same_bytes_again_are_accepted(self):
        state_dir = self.verified(good_manifest())
        self.assertEqual(self.verify(self.t / "m", state_dir).returncode, 0)

    def test_a_lower_version_is_refused_at_step_4(self):
        state_dir = self.t / "state"
        state_dir.mkdir()
        (state_dir / "state.json").write_text(json.dumps({"highest_manifest_version": 2}))
        self.assert_fails(self.verify(self.signed(good_manifest()), state_dir), 4, "manifest version went down: got 1, have 2")

    def test_an_expired_manifest_is_refused_at_step_5_and_not_stored(self):
        result = self.verify(self.signed(good_manifest()), self.t / "state", now="2026-11-08T00:00:00Z")
        self.assert_fails(result, 5, "update information is stale since %s; check the clock or try later" % FIXTURE_EXPIRES)
        self.assertFalse((self.t / "state").exists())

    def test_a_revoked_signing_key_is_refused_at_step_2(self):
        state_dir = self.t / "state"
        state_dir.mkdir()
        (state_dir / "state.json").write_text(json.dumps({"revoked_keys": [self.signer.id]}))
        self.assert_fails(self.verify(self.signed(good_manifest()), state_dir), 2, "signing key %s is revoked" % self.signer.id)

    def test_a_flipped_byte_is_refused_at_step_2(self):
        directory = self.signed(good_manifest())
        data = bytearray((directory / "manifest.json").read_bytes())
        data[100] ^= 1
        (directory / "manifest.json").write_bytes(bytes(data))
        self.assert_fails(self.verify(directory, self.t / "state"), 2, "signature does not verify")

    def test_a_channel_with_no_release_is_refused_at_step_7(self):
        self.assert_fails(self.verify(self.signed(good_manifest()), self.t / "state", channel="stable"), 7,
                          "channel stable has no release")

    # ---- 1. the bytes that were verified are the bytes that are parsed ------------------------------------------------

    def test_a_manifest_rewritten_after_the_signature_check_is_not_the_one_that_is_parsed(self):
        directory = self.signed(good_manifest())
        swapped = good_manifest()
        swapped["channels"] = {"beta": "9.9.9"}
        swapped["releases"] = {"9.9.9": dict(swapped["releases"]["5.5.1"], version="9.9.9")}
        (self.t / "swap.json").write_bytes(dump(swapped))
        shim = self.t / "shim"
        shim.mkdir()
        (shim / "minisign").write_text(
            '#!/bin/sh\n"%s" "$@" || exit $?\ncp "%s" "%s"\n'
            % (shutil.which("minisign"), self.t / "swap.json", directory / "manifest.json"))
        (shim / "minisign").chmod(0o755)
        env = dict(os.environ, PATH="%s:%s" % (shim, os.environ["PATH"]))
        original = (directory / "manifest.json").read_bytes()
        result = self.run_script("verify-manifest.sh", directory, self.pinned, self.t / "state", self.NOW, "beta", env=env)
        self.assertNotEqual((directory / "manifest.json").read_bytes(), original, "the shim did not rewrite the manifest: this test proves nothing")
        self.assertEqual((result.returncode, result.stderr), (0, ""), result.stdout)
        self.assertIn("channel beta -> 5.5.1", result.stdout)
        self.assertEqual((self.t / "state/manifest.json").read_bytes(), original)
        self.assertEqual(json.loads((self.t / "state/state.json").read_text())["last_manifest_sha256"], hashlib.sha256(original).hexdigest())

    def test_a_manifest_over_1_mib_or_a_signature_over_1_kib_is_refused_at_step_1(self):
        directory = self.signed(good_manifest())
        (directory / "manifest.json").write_bytes(b" " * 1048577)
        self.assert_fails(self.verify(directory, self.fresh()), 1, "manifest.json is over 1 MiB")
        directory = self.signed(good_manifest(), name="m2")
        (directory / "manifest.json.sig").write_bytes(b"x" * 1025)
        self.assert_fails(self.verify(directory, self.fresh()), 1, "manifest.json.sig is over 1 KiB")

    # ---- 5. the compiled minimum --------------------------------------------------------------------------------------

    def test_a_fresh_install_refuses_a_manifest_older_than_its_own_build(self):
        scripts = self.scripts_with_minimum(2)
        result = self.verify(self.signed(good_manifest()), self.t / "state", scripts=scripts)
        self.assert_fails(result, 4, "manifest version went down: got 1, have 2")
        self.assertFalse((self.t / "state").exists())

    def test_losing_the_state_does_not_lower_the_minimum(self):
        scripts = self.scripts_with_minimum(2)
        newer = good_manifest()
        newer["manifest_version"] = 2
        self.assertEqual(self.verify(self.signed(newer), self.t / "state", scripts=scripts).returncode, 0)
        shutil.rmtree(self.t / "state")
        self.assert_fails(self.verify(self.signed(good_manifest(), name="old"), self.t / "state", scripts=scripts), 4,
                          "manifest version went down: got 1, have 2")

    def test_the_stored_highest_wins_over_a_lower_minimum(self):
        newer = good_manifest()
        newer["manifest_version"] = 5
        state_dir = self.verified(newer)
        self.assert_fails(self.verify(self.signed(good_manifest(), name="old"), state_dir), 4,
                          "manifest version went down: got 1, have 5")

    def test_a_manifest_equal_to_the_minimum_is_accepted_on_a_first_run(self):
        self.assertEqual(self.verify(self.signed(good_manifest()), self.t / "state").returncode, 0)

    # ---- 6. the current time ------------------------------------------------------------------------------------------

    def test_a_current_time_that_is_not_a_utc_time_fails_closed(self):
        directory = self.signed(good_manifest())
        for bad in ("", "yesterday", "2026-10-26T12:00:00", "2026-10-26 12:00:00Z", "2026-13-45T25:61:61Z",
                    "2026-02-30T00:00:00Z", self.NOW + "\n", "\n" + self.NOW, "2026-10-26T12:00:00+00:00", "0"):
            with self.subTest(now=bad):
                state_dir = self.t / "state"
                result = self.verify(directory, state_dir, now=bad)
                self.assert_fails(result, 5, "the current time is not UTC written as YYYY-MM-DDTHH:MM:SSZ")
                self.assertFalse(state_dir.exists())

    def test_an_expires_that_is_not_a_utc_time_fails_closed(self):
        for bad in ("2999", "2999-01-01", "9999-99-99T99:99:99Z", FIXTURE_EXPIRES + "\n", 5, None):
            with self.subTest(expires=bad):
                manifest = good_manifest()
                manifest["expires"] = bad
                self.assertEqual(self.verify(self.signed(manifest, name="e"), self.t / "state").returncode, 1)
                self.assertFalse((self.t / "state").exists())

    # ---- state -------------------------------------------------------------------------------------------------------

    def test_a_state_file_that_is_not_a_state_file_fails_at_step_6(self):
        directory = self.signed(good_manifest())
        for label, content in (("not json", "{"), ("a list", "[]"), ("string version", '{"highest_manifest_version": "2"}'),
                               ("negative version", '{"highest_manifest_version": -1}'), ("fraction", '{"highest_manifest_version": 1.5}'),
                               ("revoked not a list", '{"revoked_keys": "abc"}'), ("revoked holds a number", '{"revoked_keys": [1]}'),
                               ("sha not a string", '{"last_manifest_sha256": 7}')):
            with self.subTest(label):
                state_dir = self.t / "state"
                shutil.rmtree(state_dir, ignore_errors=True)
                state_dir.mkdir()
                (state_dir / "state.json").write_text(content)
                result = self.verify(directory, state_dir)
                self.assertEqual(result.returncode, 1)
                self.assertRegex(result.stderr, r"^FAIL step 6: .*state\.json is not a state file")

    def test_a_state_file_that_cannot_be_read_does_not_become_an_empty_state(self):
        directory = self.signed(good_manifest())
        state_dir = self.t / "state"
        (state_dir / "state.json").mkdir(parents=True)  # exists, and reading it is an error
        result = self.verify(directory, state_dir)
        self.assertEqual(result.returncode, 1)
        self.assertRegex(result.stderr.strip().splitlines()[-1], r"^FAIL step 6: cannot read .*state\.json")
        notdir = self.t / "afile"
        notdir.write_text("x")
        result = self.verify(directory, notdir)
        self.assertEqual(result.returncode, 1)
        self.assertRegex(result.stderr.strip().splitlines()[-1], r"^FAIL step 6: ")

    def test_a_missing_state_is_a_first_run(self):
        self.assertEqual(self.verify(self.signed(good_manifest()), self.t / "nothing" / "here").returncode, 0)

    def test_no_temporary_file_is_left_in_the_state_directory(self):
        state_dir = self.verified(good_manifest())
        self.assertEqual(sorted(p.name for p in state_dir.iterdir()), ["manifest.json", "manifest.json.sig", "state.json"])

    # ---- the manifest's own values -----------------------------------------------------------------------------------

    def test_a_manifest_version_that_is_not_a_whole_number_is_refused_at_step_3(self):
        for bad in (1.5, "1", None, -1):
            with self.subTest(manifest_version=bad):
                manifest = good_manifest()
                manifest["manifest_version"] = bad
                result = self.verify(self.signed(manifest, name="v"), self.t / "state")
                self.assert_fails(result, 3, "manifest_version is not a whole number")

    def test_a_channel_that_names_something_that_is_not_a_version_is_refused_at_step_7(self):
        for bad in ("../x", "5.5.1\n", "5.5", "5.5.1 ", 5):
            with self.subTest(version=bad):
                manifest = good_manifest()
                manifest["channels"] = {"beta": bad}
                result = self.verify(self.signed(manifest, name="c"), self.fresh())
                self.assert_fails(result, 7, "channel beta names a version that is not X.Y.Z")

    def test_a_revocation_list_that_is_not_a_list_of_strings_is_refused_at_step_6(self):
        for bad in ("abc", [1], {"a": 1}):
            with self.subTest(revoked_keys=bad):
                manifest = good_manifest()
                manifest["revoked_keys"] = bad
                result = self.verify(self.signed(manifest, name="r"), self.fresh())
                self.assert_fails(result, 6, "revoked_keys in the manifest is not a list of strings")

    # ---- a key compromise --------------------------------------------------------------------------------------------

    def test_a_manifest_that_revokes_the_signing_key_is_accepted_and_that_key_is_refused_afterwards(self):
        pinned = self.pin(self.signer, self.other)  # the way K1 and KR are both pinned
        state_dir = self.verified(good_manifest())
        recovery = good_manifest()
        recovery["manifest_version"] = 2
        recovery["revoked_keys"] = [self.signer.id]
        accepted = self.verify(self.signed(recovery, self.other, "recovery"), state_dir, pinned=pinned)
        self.assertEqual((accepted.returncode, accepted.stderr), (0, ""))
        self.assertIn("signed by key id " + self.other.id, accepted.stdout)
        self.assertEqual(json.loads((state_dir / "state.json").read_text())["revoked_keys"], [self.signer.id])
        later = good_manifest()
        later["manifest_version"] = 3
        self.assert_fails(self.verify(self.signed(later, self.signer, "later"), state_dir, pinned=pinned), 2,
                          "signing key %s is revoked" % self.signer.id)

    def test_a_recovery_manifest_below_a_version_already_accepted_is_refused_as_a_rollback(self):
        pinned = self.pin(self.signer, self.other)
        high = good_manifest()
        high["manifest_version"] = 9
        state_dir = self.verified(high)
        recovery = good_manifest()
        recovery["manifest_version"] = 2
        recovery["revoked_keys"] = [self.signer.id]
        self.assert_fails(self.verify(self.signed(recovery, self.other, "recovery"), state_dir, pinned=pinned), 4,
                          "manifest version went down: got 2, have 9")


class ArtifactScripts(ScriptCase):
    """check-artifact.sh, stored-entry.sh, fetch-capped.sh, check-zip.py and check-apk.sh, extracted from the page."""

    PATH = "/nevr-runtime/v5.5.1/nevr-runtime-v5.5.1-windows.zip"

    def setUp(self):
        super().setUp()
        self.data = b"windows zip bytes" * 100
        self.servers = []

    def tearDown(self):
        for server in self.servers:
            server.close()

    def serve(self, routes, announce=False):
        server = Server(routes, announce)
        self.servers.append(server)
        return server

    def state_for(self, artifacts=None):
        return self.verified(small_release(artifacts or {"windows-zip": ("windows.zip", self.data, {})}))

    def artifact(self, state_dir, base, out=None, version="5.5.1", kind="windows-zip"):
        out = out or self.t / "out.bin"
        return self.run_script("check-artifact.sh", state_dir, version, kind, base, out), out

    def test_a_download_that_matches_passes(self):
        state_dir = self.state_for()
        server = self.serve({self.PATH: self.data})
        result, out = self.artifact(state_dir, server.base)
        self.assertEqual((result.returncode, result.stderr), (0, ""))
        self.assertIn("OK step 8", result.stdout)
        self.assertEqual(out.read_bytes(), self.data)
        self.assertEqual(server.requests, [self.PATH])

    def test_the_same_with_an_announced_length_passes_too(self):
        state_dir = self.state_for()
        result, out = self.artifact(state_dir, self.serve({self.PATH: self.data}, announce=True).base)
        self.assertEqual((result.returncode, result.stderr), (0, ""))

    def test_a_changed_byte_is_refused_and_the_download_is_deleted(self):
        state_dir = self.state_for()
        changed = bytearray(self.data)
        changed[10] ^= 1
        result, out = self.artifact(state_dir, self.serve({self.PATH: bytes(changed)}).base)
        self.assert_fails(result, 8, "sha256 differs from the manifest")
        self.assertFalse(out.exists())

    def test_a_short_download_is_refused_and_deleted(self):
        state_dir = self.state_for()
        result, out = self.artifact(state_dir, self.serve({self.PATH: self.data[:-1]}).base)
        self.assert_fails(result, 8, "size is %d, the manifest says %d" % (len(self.data) - 1, len(self.data)))
        self.assertFalse(out.exists())

    def test_a_server_that_sends_more_than_size_is_cut_off_and_refused(self):
        state_dir = self.state_for()
        server = self.serve({self.PATH: self.data + b"\0" * 5_000_000})  # no Content-Length: nothing announces the excess
        result, out = self.artifact(state_dir, server.base)
        self.assert_fails(result, 8, "the server sent more than %d bytes" % len(self.data))
        self.assertFalse(out.exists())

    def test_a_server_that_announces_the_excess_is_capped_too(self):
        state_dir = self.state_for()
        server = self.serve({self.PATH: self.data + b"\0" * 5_000_000}, announce=True)
        result, out = self.artifact(state_dir, server.base)
        self.assertEqual(result.returncode, 1)
        self.assertRegex(result.stderr.strip().splitlines()[-1], r"^FAIL step 8: ")
        self.assertFalse(out.exists())

    def test_a_missing_file_is_a_failure(self):
        state_dir = self.state_for()
        result, out = self.artifact(state_dir, self.serve({}).base)
        self.assert_fails(result, 8, "could not fetch %s%s" % (self.servers[-1].base, self.PATH[len("/nevr-runtime"):]))
        self.assertFalse(out.exists())

    # ---- 3 and 4. the path is checked before any request, and checked whole --------------------------------------------

    def hostile(self, path, version="5.5.1"):
        manifest = small_release({"windows-zip": ("windows.zip", self.data, {})}, version)
        manifest["releases"][version]["artifacts"][0]["path"] = path
        return self.verified(manifest, name="hostile")

    def test_a_path_that_is_not_one_version_directory_and_one_file_name_is_refused_before_any_request(self):
        for bad in ("v5.5.1/nevr-runtime-v5.5.1-windows.zip\n", "v5.5.1/nevr-runtime-v5.5.1-windows.zip/", "../v5.5.1/nevr-runtime-v5.5.1-windows.zip",
                    "v5.5.1/../nevr-runtime-v5.5.1-windows.zip", "/v5.5.1/nevr-runtime-v5.5.1-windows.zip",
                    "v5.5.1/sub/nevr-runtime-v5.5.1-windows.zip", "v5.5.1/nevr-runtime-v5.5.1-", "v5.5.1/nevr-runtime-v5.5.1-Windows.zip",
                    "v5.5.1/nevr-runtime-v5.5.1-a b", "v5.5.1/nevr-runtime-v5.5.1-a?b=c", "v5.5.1/nevr-runtime-v5.5.1-a#b",
                    "v5.5.1/nevr-runtime-v5.5.1-a\\b", "v5.5.1/nevr-runtime-v5.5.2-windows.zip", "v9.9.9/nevr-runtime-v9.9.9-windows.zip",
                    "http://evil.example/x", "v5.5.1/nevr-runtime-v5.5.1-windows.zip\nx"):
            with self.subTest(path=bad):
                state_dir = self.hostile(bad)
                server = self.serve({})
                result, out = self.artifact(state_dir, server.base)
                self.assert_fails(result, 8, "path in the manifest is not v<version>/nevr-runtime-v<version>-<name>")
                self.assertEqual(server.requests, [], "a request was made for a path that should have been refused")

    def test_a_version_that_is_not_x_y_z_is_refused_before_any_request(self):
        state_dir = self.state_for()
        server = self.serve({})
        for bad in ("5.5.1\n", "5.5", "../5.5.1", "05.5.1", ""):
            with self.subTest(version=bad):
                result, _out = self.artifact(state_dir, server.base, version=bad)
                self.assertEqual(result.returncode, 1)
                self.assertRegex(result.stderr, r"^FAIL step 8: ")
        self.assertEqual(server.requests, [])

    def test_size_and_sha256_that_are_not_what_they_should_be_are_refused_before_any_request(self):
        for field, bad in (("size", -1), ("size", 1.5), ("size", "9"), ("sha256", "AB" * 32), ("sha256", "ab" * 31), ("sha256", ("ab" * 32) + "\n")):
            with self.subTest(field=field, value=bad):
                manifest = small_release({"windows-zip": ("windows.zip", self.data, {})})
                manifest["releases"]["5.5.1"]["artifacts"][0][field] = bad
                state_dir = self.verified(manifest, name="malformed")
                server = self.serve({})
                result, _out = self.artifact(state_dir, server.base)
                self.assert_fails(result, 8, "size or sha256 in the manifest is malformed")
                self.assertEqual(server.requests, [])

    def test_a_kind_the_release_lacks_or_lists_twice_is_refused(self):
        state_dir = self.state_for()
        result, _out = self.artifact(state_dir, self.serve({}).base, kind="plugin")
        self.assert_fails(result, 8, "release 5.5.1 has no plugin artifact, or lists it twice")
        manifest = small_release({"windows-zip": ("windows.zip", self.data, {})})
        manifest["releases"]["5.5.1"]["artifacts"] *= 2
        twice = self.verified(manifest, name="twice")
        result, _out = self.artifact(twice, self.serve({}).base)
        self.assert_fails(result, 8, "release 5.5.1 has no windows-zip artifact, or lists it twice")

    # ---- 2. step 8 uses the manifest that step 2 verified -------------------------------------------------------------

    def test_a_stored_manifest_edited_after_step_6_is_refused_before_any_request(self):
        state_dir = self.state_for()
        stored = state_dir / "manifest.json"
        edited = json.loads(stored.read_text())
        edited["releases"]["5.5.1"]["artifacts"][0]["path"] = "v5.5.1/nevr-runtime-v5.5.1-evil.zip"
        stored.write_bytes(dump(edited))
        server = self.serve({})
        result, _out = self.artifact(state_dir, server.base)
        self.assert_fails(result, 8, "the stored manifest is not the one that passed step 2")
        self.assertEqual(server.requests, [])

    def test_a_state_directory_with_no_verified_manifest_is_refused(self):
        server = self.serve({})
        empty = self.t / "empty"
        empty.mkdir()
        result, _out = self.artifact(empty, server.base)
        self.assert_fails(result, 8, "no verified manifest in %s; run verify-manifest.sh first" % empty)
        self.assertEqual(server.requests, [])

    def test_a_manifest_path_is_not_a_state_directory(self):
        # The old interface took the manifest's path. Handing one over now finds no verified manifest.
        result, _out = self.artifact(self.t / "m2" / "manifest.json", self.serve({}).base)
        self.assertEqual(result.returncode, 1)
        self.assertRegex(result.stderr, r"^FAIL step 8: no verified manifest in ")

    def test_a_state_json_that_does_not_name_the_stored_manifest_is_refused(self):
        state_dir = self.state_for()
        (state_dir / "state.json").write_text(json.dumps({"highest_manifest_version": 1}))
        result, _out = self.artifact(state_dir, self.serve({}).base)
        self.assert_fails(result, 8, "the stored manifest is not the one that passed step 2")

    # ---- 10. every download is capped, the manifest too ---------------------------------------------------------------

    def fetch(self, url, out, max_bytes, step=1):
        return self.run_script("fetch-capped.sh", step, url, out, max_bytes)

    def test_the_manifest_fetch_stops_at_the_limit(self):
        server = self.serve({"/nevr-runtime/manifest.json": b"{" * 3_000_000})
        out = self.t / "manifest.json"
        result = self.fetch(server.base + "/manifest.json", out, 1048576)
        self.assert_fails(result, 1, "the server sent more than 1048576 bytes")
        self.assertFalse(out.exists())

    def test_a_fetch_within_the_limit_keeps_exactly_what_was_sent(self):
        server = self.serve({"/nevr-runtime/manifest.json.sig": b"abc"})
        out = self.t / "sig"
        result = self.fetch(server.base + "/manifest.json.sig", out, 1024)
        self.assertEqual((result.returncode, result.stderr, out.read_bytes()), (0, "", b"abc"))

    def test_a_fetch_of_exactly_the_limit_is_accepted_and_one_byte_more_is_not(self):
        server = self.serve({"/nevr-runtime/a": b"x" * 1024, "/nevr-runtime/b": b"x" * 1025})
        self.assertEqual(self.fetch(server.base + "/a", self.t / "a", 1024).returncode, 0)
        self.assert_fails(self.fetch(server.base + "/b", self.t / "b", 1024), 1, "the server sent more than 1024 bytes")

    def test_the_reader_stops_consuming_at_the_limit_so_an_endless_body_is_not_read(self):
        shim = self.t / "curlshim"
        shim.mkdir()
        marker = self.t / "curl-finished"
        (shim / "curl").write_text(
            "#!%s\nimport os, sys\nchunk = b'x' * 65536\ntry:\n    for _ in range(800):\n        sys.stdout.buffer.write(chunk)\n"
            "        sys.stdout.buffer.flush()\nexcept BrokenPipeError:\n    sys.exit(23)\nopen(%r, 'w').close()\n" % (sys.executable, str(marker)))
        (shim / "curl").chmod(0o755)
        env = dict(os.environ, PATH="%s:%s" % (shim, os.environ["PATH"]))
        out = self.t / "endless"
        result = self.run_script("fetch-capped.sh", 1, "http://example.invalid/x", out, 1024, env=env)
        self.assert_fails(result, 1, "the server sent more than 1024 bytes")
        self.assertFalse(marker.exists(), "the reader consumed the whole 50 MB body: the cap does not bound what is read")

    def test_a_limit_that_is_not_a_number_is_refused(self):
        self.assertEqual(self.fetch("http://127.0.0.1:1/x", self.t / "x", "1; echo hi").returncode, 1)

    # ---- 7. the zip ---------------------------------------------------------------------------------------------------

    def zip_state(self, members, contents=None):
        """A verified state directory whose windows-zip lists `contents` (default: the regular files in `members`)."""
        archive = self.t / "release.zip"
        with zipfile.ZipFile(archive, "w") as z:
            for item in members:
                if isinstance(item, tuple):
                    z.writestr(item[0], item[1])
                else:
                    z.writestr(item, b"")
        listed = contents if contents is not None else [
            {"path": m[0], "size": len(m[1]), "sha256": hashlib.sha256(m[1]).hexdigest()}
            for m in members if isinstance(m, tuple) and not m[0].endswith("/") and not isinstance(m[1], zipfile.ZipInfo)]
        manifest = small_release({"windows-zip": ("windows.zip", archive.read_bytes(), {"contents": listed})})
        return self.verified(manifest, name="zipstate"), archive

    def check_zip(self, state_dir, archive, version="5.5.1"):
        return subprocess.run([sys.executable, "-I", str(self.scripts / "check-zip.py"), str(state_dir), version, str(archive)],
                              capture_output=True, text=True, check=False)

    def test_a_zip_whose_entries_are_all_listed_passes(self):
        state_dir, archive = self.zip_state([("a.txt", b"one"), ("b.dll", b"two")])
        result = self.check_zip(state_dir, archive)
        self.assertEqual((result.returncode, result.stderr), (0, ""), result.stdout)
        self.assertIn("all 2 entries", result.stdout)

    def test_an_entry_the_manifest_does_not_list_is_refused(self):
        state_dir, archive = self.zip_state([("a.txt", b"one")], contents=[])
        self.assertIn("the zip holds a.txt, which the manifest does not list", self.check_zip(state_dir, archive).stderr)

    def test_a_directory_entry_is_refused(self):
        state_dir, archive = self.zip_state([("a.txt", b"one"), "sub/"],
                                            contents=[{"path": "a.txt", "size": 3, "sha256": hashlib.sha256(b"one").hexdigest()}])
        result = self.check_zip(state_dir, archive)
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL step 8: the zip holds sub/, which the manifest does not list", result.stderr)

    def test_a_listed_name_that_is_a_directory_is_still_refused(self):
        state_dir, archive = self.zip_state(["sub/"], contents=[{"path": "sub/", "size": 0, "sha256": hashlib.sha256(b"").hexdigest()}])
        self.assertIn("sub/ is not a regular file", self.check_zip(state_dir, archive).stderr)

    def test_a_directory_entry_with_no_unix_mode_is_refused_even_when_it_is_listed(self):
        archive = self.t / "dos.zip"
        info = zipfile.ZipInfo("sub/")
        info.external_attr = 0x10  # the DOS directory flag and no mode bits: only is_dir() sees it
        with zipfile.ZipFile(archive, "w") as z:
            z.writestr(info, b"")
        listed = [{"path": "sub/", "size": 0, "sha256": hashlib.sha256(b"").hexdigest()}]
        state_dir = self.verified(small_release({"windows-zip": ("windows.zip", archive.read_bytes(), {"contents": listed})}), name="dosstate")
        self.assertIn("sub/ is not a regular file", self.check_zip(state_dir, archive).stderr)

    def test_a_symbolic_link_is_refused_even_when_it_is_listed(self):
        archive = self.t / "link.zip"
        info = zipfile.ZipInfo("link")
        info.create_system = 3
        info.external_attr = 0o120777 << 16
        with zipfile.ZipFile(archive, "w") as z:
            z.writestr(info, "/etc/passwd")
        listed = [{"path": "link", "size": 11, "sha256": hashlib.sha256(b"/etc/passwd").hexdigest()}]
        state_dir = self.verified(small_release({"windows-zip": ("windows.zip", archive.read_bytes(), {"contents": listed})}), name="linkstate")
        self.assertIn("link is not a regular file", self.check_zip(state_dir, archive).stderr)

    def test_a_regular_file_entry_with_unix_permissions_is_accepted(self):
        archive = self.t / "perm.zip"
        info = zipfile.ZipInfo("run.sh")
        info.create_system = 3
        info.external_attr = 0o100755 << 16
        with zipfile.ZipFile(archive, "w") as z:
            z.writestr(info, "echo")
        listed = [{"path": "run.sh", "size": 4, "sha256": hashlib.sha256(b"echo").hexdigest()}]
        state_dir = self.verified(small_release({"windows-zip": ("windows.zip", archive.read_bytes(), {"contents": listed})}), name="permstate")
        self.assertEqual(self.check_zip(state_dir, archive).returncode, 0)

    def test_a_traversal_entry_a_changed_member_and_a_missing_member_are_refused(self):
        state_dir, archive = self.zip_state([("a.txt", b"one")])
        with zipfile.ZipFile(archive, "a") as z:
            z.writestr("../evil.dll", "x")
        self.assertIn("../evil.dll, which the manifest does not list", self.check_zip(state_dir, archive).stderr)
        changed = self.t / "changed.zip"
        with zipfile.ZipFile(changed, "w") as z:
            z.writestr("a.txt", "ONE")
        self.assertIn("a.txt has a different sha256", self.check_zip(state_dir, changed).stderr)
        empty = self.t / "empty.zip"
        with zipfile.ZipFile(empty, "w"):
            pass
        self.assertIn("the manifest lists a.txt, which the zip does not hold", self.check_zip(state_dir, empty).stderr)

    def test_a_stored_manifest_that_was_edited_is_refused_by_the_zip_check_too(self):
        state_dir, archive = self.zip_state([("a.txt", b"one")])
        (state_dir / "manifest.json").write_bytes(dump(small_release({"windows-zip": ("windows.zip", b"x", {"contents": []})})))
        self.assertIn("the stored manifest is not the one that passed step 2", self.check_zip(state_dir, archive).stderr)

    def test_the_zip_check_takes_its_manifest_from_the_state_directory_only(self):
        state_dir, archive = self.zip_state([("a.txt", b"one")])
        result = subprocess.run([sys.executable, "-I", str(self.scripts / "check-zip.py"), str(state_dir / "manifest.json"), "5.5.1", str(archive)],
                                capture_output=True, text=True, check=False)
        self.assertIn("no verified manifest in", result.stderr)


class ApkScript(ScriptCase):
    """check-apk.sh with an aapt2 and an apksigner that print what the test says."""

    CODE = 4987566
    CERT = "df58594fd774584f9bde5b9df33228ea3e5d8ee87efc32f2c24e84368ff0af58"
    OTHER = "0" * 63 + "1"

    def state_for(self, android):
        manifest = small_release({"quest-apk": ("quest.apk", b"apk bytes", {"android": android})})
        return self.verified(manifest)

    def run_apk(self, state_dir, package="com.readyatdawn.r15", code=CODE, digests=(CERT,), signer_status=0):
        shim = self.t / "apkshim"
        shim.mkdir(exist_ok=True)
        (shim / "aapt2").write_text("#!/bin/sh\necho \"package: name='%s' versionCode='%d' versionName='x' platformBuildVersionName='y'\"\n" % (package, code))
        lines = "".join("echo 'Signer #%d certificate SHA-256 digest: %s'\n" % (i + 1, d) for i, d in enumerate(digests))
        (shim / "apksigner").write_text("#!/bin/sh\n%sexit %d\n" % (lines, signer_status))
        for name in ("aapt2", "apksigner"):
            (shim / name).chmod(0o755)
        env = dict(os.environ, PATH="%s:%s" % (shim, os.environ["PATH"]))
        apk = self.t / "app.apk"
        apk.write_bytes(b"apk bytes")
        return self.run_script("check-apk.sh", state_dir, "5.5.1", apk, env=env)

    def test_the_package_the_version_code_and_the_certificate_that_match_pass(self):
        state_dir = self.state_for({"package": "com.readyatdawn.r15", "version_code": self.CODE, "signer_cert_sha256": self.CERT})
        result = self.run_apk(state_dir)
        self.assertEqual((result.returncode, result.stderr), (0, ""))
        self.assertIn(self.CERT, result.stdout)

    def test_a_second_certificate_the_manifest_does_not_list_fails(self):
        state_dir = self.state_for({"package": "com.readyatdawn.r15", "version_code": self.CODE, "signer_cert_sha256": self.CERT})
        result = self.run_apk(state_dir, digests=(self.CERT, self.OTHER))
        self.assert_fails(result, 8, "the APK is signed by %s, which the manifest does not list" % self.OTHER)

    def test_a_different_certificate_fails(self):
        state_dir = self.state_for({"package": "com.readyatdawn.r15", "version_code": self.CODE, "signer_cert_sha256": self.CERT})
        self.assert_fails(self.run_apk(state_dir, digests=(self.OTHER,)), 8,
                          "the APK is signed by %s, which the manifest does not list" % self.OTHER)

    def test_several_digests_all_listed_pass(self):
        state_dir = self.state_for({"package": "com.readyatdawn.r15", "version_code": self.CODE, "signer_cert_sha256": self.CERT})
        self.assertEqual(self.run_apk(state_dir, digests=(self.CERT, self.CERT)).returncode, 0)

    def test_a_manifest_with_no_certificate_compares_only_the_package_and_the_version_code(self):
        state_dir = self.state_for({"package": "com.readyatdawn.r15", "version_code": self.CODE})
        self.assertEqual(self.run_apk(state_dir, digests=(self.OTHER,)).returncode, 0)

    def test_a_wrong_package_or_version_code_fails(self):
        state_dir = self.state_for({"package": "com.readyatdawn.r15", "version_code": self.CODE})
        self.assert_fails(self.run_apk(state_dir, package="com.example.x"), 8,
                          "the APK package is 'com.example.x', the manifest says 'com.readyatdawn.r15'")
        self.assert_fails(self.run_apk(state_dir, code=7), 8, "the APK versionCode is '7', the manifest says '%d'" % self.CODE)

    def test_an_apksigner_that_refuses_the_file_fails_whatever_it_printed(self):
        state_dir = self.state_for({"package": "com.readyatdawn.r15", "version_code": self.CODE, "signer_cert_sha256": self.CERT})
        self.assert_fails(self.run_apk(state_dir, signer_status=1), 8, "apksigner does not accept %s" % (self.t / "app.apk"))

    def test_an_apksigner_that_prints_no_certificate_fails(self):
        state_dir = self.state_for({"package": "com.readyatdawn.r15", "version_code": self.CODE})
        self.assert_fails(self.run_apk(state_dir, digests=()), 8, "apksigner printed no signing certificate")


IMPLS_ENV = "NEVR_VERIFYING_DOC_BUILD"


def _impl_tools():
    return bool(os.environ.get(IMPLS_ENV) == "1" and shutil.which("cargo") and shutil.which("dotnet"))


def _run_build(argv, cwd):
    result = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, check=False, timeout=1800)
    if result.returncode != 0:
        raise AssertionError("%s failed:\n%s\n%s" % (" ".join(argv), result.stdout[-3000:], result.stderr[-3000:]))


class ImplCases:
    """The same cases for the page's Rust program and its C# harness. They are built from the page's own blocks, so the
    cases run against exactly what the page prints. Set NEVR_VERIFYING_DOC_BUILD=1 with cargo and dotnet installed (the
    crates and packages come from their usual caches or the network)."""

    MINIMUM = 1

    def run_impl(self, *args, minimum=None):
        argv = self.command(minimum or self.MINIMUM) + [str(a) for a in args]
        return subprocess.run(argv, capture_output=True, text=True, check=False, timeout=120)

    def manifest_cmd(self, directory, state_dir, now=None, channel="beta", pinned=None, minimum=None):
        return self.run_impl("manifest", directory, pinned or self.pinned, state_dir, self.NOW if now is None else now, channel, minimum=minimum)

    @staticmethod
    def verdict_line(result):
        """The verdict: the last line that starts FAIL step (a message may hold a newline that was in the input)."""
        lines = [l for l in result.stderr.splitlines() if l.startswith("FAIL step ")]
        return lines[-1] if lines else ""

    def verdict(self, result, step, contains=""):
        line = self.verdict_line(result)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertTrue(line.startswith("FAIL step %d: " % step), "verdict: %r; stderr: %r" % (line, result.stderr))
        self.assertIn(contains, line)
        for crash in ("panicked", "Unhandled", "   at "):
            self.assertNotIn(crash, result.stderr)

    def failed_closed(self, result):
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertRegex(self.verdict_line(result), r"^FAIL step \d: ", result.stderr)
        for crash in ("panicked", "Unhandled", "   at "):
            self.assertNotIn(crash, result.stderr)

    def ok_state(self, manifest, name="state"):
        state_dir = self.fresh(name)
        result = self.manifest_cmd(self.signed(manifest, name=name + "-m"), state_dir)
        self.assertEqual((result.returncode, result.stderr), (0, ""), result.stdout)
        return state_dir

    # ---- steps 2 to 7 on the fixtures ---------------------------------------------------------------------------------

    def test_good_passes_with_the_test_key_and_stores_the_verified_bytes(self):
        pinned = self.t / "test-pinned.txt"
        shutil.copy(self.scripts / "test-pinned-keys.txt", pinned)
        state_dir = self.fresh()
        result = self.manifest_cmd(VECTORS / "good", state_dir, pinned=pinned)
        self.assertEqual((result.returncode, result.stderr), (0, ""), result.stdout)
        self.assertIn("channel beta -> 5.5.1", result.stdout)
        good = (VECTORS / "good/manifest.json").read_bytes()
        self.assertEqual((state_dir / "manifest.json").read_bytes(), good)
        state = json.loads((state_dir / "state.json").read_text())
        self.assertEqual((state["highest_manifest_version"], state["last_manifest_sha256"]), (1, hashlib.sha256(good).hexdigest()))
        self.assertEqual(sorted(p.name for p in state_dir.iterdir()), ["manifest.json", "manifest.json.sig", "state.json"])

    def test_fixtures_are_refused_the_way_the_page_says(self):
        test_pinned = self.t / "test-pinned.txt"
        shutil.copy(self.scripts / "test-pinned-keys.txt", test_pinned)
        self.verdict(self.manifest_cmd(VECTORS / "good", self.fresh(), pinned=self.scripts / "pinned-keys.txt"), 2, "not pinned")
        self.verdict(self.manifest_cmd(VECTORS / "legacy-ed", self.fresh(), pinned=test_pinned), 2, "not ED")
        self.verdict(self.manifest_cmd(VECTORS / "unpinned-key", self.fresh(), pinned=test_pinned), 2, "not pinned")
        first = self.fresh()
        self.assertEqual(self.manifest_cmd(VECTORS / "good", first, pinned=test_pinned).returncode, 0)
        self.verdict(self.manifest_cmd(VECTORS / "same-version-other-bytes", first, pinned=test_pinned), 4, "different bytes")
        self.verdict(self.manifest_cmd(VECTORS / "good", self.fresh(), now="2026-11-08T00:00:00Z", pinned=test_pinned), 5, "stale")

    def test_a_flipped_byte_is_refused_at_step_2(self):
        directory = self.signed(good_manifest())
        data = bytearray((directory / "manifest.json").read_bytes())
        data[100] ^= 1
        (directory / "manifest.json").write_bytes(bytes(data))
        self.verdict(self.manifest_cmd(directory, self.fresh()), 2)

    def test_a_lower_version_and_a_revoked_key_come_from_the_state(self):
        state_dir = self.fresh()
        state_dir.mkdir()
        (state_dir / "state.json").write_text(json.dumps({"highest_manifest_version": 2}))
        self.verdict(self.manifest_cmd(self.signed(good_manifest()), state_dir), 4, "went down: got 1, have 2")
        revoked = self.fresh()
        revoked.mkdir()
        (revoked / "state.json").write_text(json.dumps({"revoked_keys": [self.signer.id]}))
        self.verdict(self.manifest_cmd(self.signed(good_manifest(), name="m2"), revoked), 2, "revoked")

    def test_no_temporary_file_is_left_in_the_state_directory(self):
        state_dir = self.ok_state(good_manifest())
        self.assertEqual(sorted(p.name for p in state_dir.iterdir()), ["manifest.json", "manifest.json.sig", "state.json"])

    # ---- 5. the compiled minimum --------------------------------------------------------------------------------------

    def test_a_fresh_install_refuses_a_manifest_older_than_its_own_build_and_losing_the_state_does_not_lower_it(self):
        directory = self.signed(good_manifest())
        self.verdict(self.manifest_cmd(directory, self.fresh(), minimum=2), 4, "went down: got 1, have 2")
        newer = good_manifest()
        newer["manifest_version"] = 2
        state_dir = self.fresh()
        self.assertEqual(self.manifest_cmd(self.signed(newer, name="n"), state_dir, minimum=2).returncode, 0)
        shutil.rmtree(state_dir)
        self.verdict(self.manifest_cmd(directory, state_dir, minimum=2), 4, "went down: got 1, have 2")

    def test_a_manifest_at_the_minimum_is_accepted_on_a_first_run(self):
        self.assertEqual(self.manifest_cmd(self.signed(good_manifest()), self.fresh()).returncode, 0)

    # ---- 6. the current time -------------------------------------------------------------------------------------------

    def test_a_current_time_that_is_not_a_utc_time_fails_closed(self):
        directory = self.signed(good_manifest())
        for bad in ("", "yesterday", "2026-10-26T12:00:00", "2026-10-26 12:00:00Z", "2026-13-45T25:61:61Z", "2026-02-30T00:00:00Z",
                    self.NOW + "\n", "\n" + self.NOW, "2026-10-26T12:00:00+00:00", "0", "2026-10-26T12:00:00Z "):
            with self.subTest(now=bad):
                state_dir = self.fresh()
                self.verdict(self.manifest_cmd(directory, state_dir, now=bad), 5, "current time")
                self.assertFalse(state_dir.exists())

    def test_an_expires_that_is_not_a_utc_time_fails_closed(self):
        for bad in ("2999", "2999-01-01", "9999-99-99T99:99:99Z", FIXTURE_EXPIRES + "\n", 5, None):
            with self.subTest(expires=bad):
                manifest = good_manifest()
                manifest["expires"] = bad
                state_dir = self.fresh()
                self.verdict(self.manifest_cmd(self.signed(manifest, name="e"), state_dir), 5)
                self.assertFalse(state_dir.exists())

    # ---- 8. any parse failure is a refusal, never a crash --------------------------------------------------------------

    def test_a_field_of_the_wrong_type_is_a_refusal_and_not_a_crash(self):
        cases = [("schema_version", "1"), ("schema_version", None), ("product", 5), ("product", ["nevr-runtime"]),
                 ("manifest_version", "1"), ("manifest_version", 1.5), ("manifest_version", -1), ("manifest_version", None),
                 ("expires", 5), ("channels", []), ("channels", "x"), ("channels", {"beta": 5}), ("channels", {"beta": None}),
                 ("releases", "x"), ("releases", []), ("revoked_keys", "abc"), ("revoked_keys", [1]), ("revoked_keys", {"a": 1})]
        for key, value in cases:
            with self.subTest(**{key: value}):
                manifest = good_manifest()
                manifest[key] = value
                self.failed_closed(self.manifest_cmd(self.signed(manifest, name="w"), self.fresh()))

    def test_a_stored_artifact_entry_of_the_wrong_type_is_a_refusal_and_not_a_crash(self):
        data = b"zip bytes"
        for key, value in (("kind", 5), ("path", 5), ("path", None), ("size", "9"), ("size", None), ("size", -1), ("sha256", 5), ("sha256", None)):
            with self.subTest(**{key: value}):
                manifest = small_release({"windows-zip": ("windows.zip", data, {})})
                manifest["releases"]["5.5.1"]["artifacts"][0][key] = value
                state_dir = self.ok_state(manifest, "w")
                server = self.serve({})
                self.failed_closed(self.artifact_cmd(state_dir, server.base))
                self.assertEqual(server.requests, [])
        manifest = small_release({"windows-zip": ("windows.zip", data, {})})
        manifest["releases"]["5.5.1"]["artifacts"] = "x"
        self.failed_closed(self.artifact_cmd(self.ok_state(manifest, "w2"), self.serve({}).base))

    # ---- state ---------------------------------------------------------------------------------------------------------

    def test_a_state_file_that_is_not_a_state_file_fails_at_step_6(self):
        directory = self.signed(good_manifest())
        for label, content in (("not json", "{"), ("a list", "[]"), ("string version", '{"highest_manifest_version": "2"}'),
                               ("negative version", '{"highest_manifest_version": -1}'), ("fraction", '{"highest_manifest_version": 1.5}'),
                               ("revoked not a list", '{"revoked_keys": "abc"}'), ("revoked holds a number", '{"revoked_keys": [1]}'),
                               ("sha not a string", '{"last_manifest_sha256": 7}')):
            with self.subTest(label):
                state_dir = self.fresh()
                state_dir.mkdir()
                (state_dir / "state.json").write_text(content)
                self.verdict(self.manifest_cmd(directory, state_dir), 6)

    def test_a_state_file_that_cannot_be_read_does_not_become_an_empty_state(self):
        directory = self.signed(good_manifest())
        state_dir = self.fresh()
        (state_dir / "state.json").mkdir(parents=True)
        self.verdict(self.manifest_cmd(directory, state_dir), 6)
        notdir = self.t / "afile"
        notdir.write_text("x")
        self.verdict(self.manifest_cmd(directory, notdir), 6)

    def test_a_missing_state_is_a_first_run(self):
        self.assertEqual(self.manifest_cmd(self.signed(good_manifest()), self.t / "nothing" / "here").returncode, 0)

    # ---- key compromise ------------------------------------------------------------------------------------------------

    def test_a_manifest_that_revokes_the_signing_key_is_accepted_and_that_key_is_refused_afterwards(self):
        pinned = self.pin(self.signer, self.other)
        state_dir = self.ok_state(good_manifest())
        recovery = good_manifest()
        recovery["manifest_version"] = 2
        recovery["revoked_keys"] = [self.signer.id]
        accepted = self.manifest_cmd(self.signed(recovery, self.other, "recovery"), state_dir, pinned=pinned)
        self.assertEqual((accepted.returncode, accepted.stderr), (0, ""), accepted.stdout)
        self.assertEqual(json.loads((state_dir / "state.json").read_text())["revoked_keys"], [self.signer.id])
        later = good_manifest()
        later["manifest_version"] = 3
        self.verdict(self.manifest_cmd(self.signed(later, self.signer, "later"), state_dir, pinned=pinned), 2, "revoked")

    # ---- step 1 and step 8 over HTTP ----------------------------------------------------------------------------------

    PATH = ArtifactScripts.PATH
    DATA = b"windows zip bytes" * 100

    def serve(self, routes, announce=False):
        server = Server(routes, announce)
        self.addCleanup(server.close)
        return server

    def artifact_cmd(self, state_dir, base, out=None, version="5.5.1", kind="windows-zip"):
        self.last_out = out or self.t / ("out-%d.bin" % len(list(self.t.glob("out-*.bin"))))
        return self.run_impl("artifact", state_dir, version, kind, base, self.last_out)

    def state_for(self, data=None):
        return self.ok_state(small_release({"windows-zip": ("windows.zip", data or self.DATA, {})}), "art")

    def test_a_download_that_matches_passes_and_is_kept(self):
        server = self.serve({self.PATH: self.DATA})
        result = self.artifact_cmd(self.state_for(), server.base)
        self.assertEqual((result.returncode, result.stderr), (0, ""), result.stdout)
        self.assertEqual(self.last_out.read_bytes(), self.DATA)
        self.assertEqual(server.requests, [self.PATH])

    def test_a_changed_byte_a_short_file_and_a_missing_file_are_refused_and_nothing_is_kept(self):
        changed = bytearray(self.DATA)
        changed[3] ^= 1
        for label, body, step_text in (("changed", bytes(changed), "sha256"), ("short", self.DATA[:-1], "size is"), ("missing", None, "")):
            with self.subTest(label):
                server = self.serve({} if body is None else {self.PATH: body})
                self.verdict(self.artifact_cmd(self.state_for(), server.base), 8, step_text)
                self.assertFalse(self.last_out.exists())

    def test_a_server_that_sends_more_than_size_is_cut_off_and_refused_announced_or_not(self):
        for announce in (False, True):
            with self.subTest(announce=announce):
                server = self.serve({self.PATH: self.DATA + b"\0" * 5_000_000}, announce=announce)
                self.verdict(self.artifact_cmd(self.state_for(), server.base), 8, "more than %d bytes" % len(self.DATA))
                self.assertFalse(self.last_out.exists())

    def test_a_path_that_is_not_one_version_directory_and_one_file_name_is_refused_before_any_request(self):
        for bad in ("v5.5.1/nevr-runtime-v5.5.1-windows.zip\n", "v5.5.1/nevr-runtime-v5.5.1-windows.zip/", "../v5.5.1/nevr-runtime-v5.5.1-windows.zip",
                    "v5.5.1/../nevr-runtime-v5.5.1-windows.zip", "/v5.5.1/nevr-runtime-v5.5.1-windows.zip",
                    "v5.5.1/sub/nevr-runtime-v5.5.1-windows.zip", "v5.5.1/nevr-runtime-v5.5.1-", "v5.5.1/nevr-runtime-v5.5.1-Windows.zip",
                    "v5.5.1/nevr-runtime-v5.5.1-a b", "v5.5.1/nevr-runtime-v5.5.1-a?b=c", "v5.5.1/nevr-runtime-v5.5.1-a#b",
                    "v5.5.1/nevr-runtime-v5.5.1-a\\b", "v5.5.1/nevr-runtime-v5.5.2-windows.zip", "v9.9.9/nevr-runtime-v9.9.9-windows.zip",
                    "http://evil.example/x", "v5.5.1/nevr-runtime-v5.5.1-windows.zip\nx"):
            with self.subTest(path=bad):
                manifest = small_release({"windows-zip": ("windows.zip", self.DATA, {})})
                manifest["releases"]["5.5.1"]["artifacts"][0]["path"] = bad
                server = self.serve({})
                self.verdict(self.artifact_cmd(self.ok_state(manifest, "h"), server.base), 8, "path in the manifest")
                self.assertEqual(server.requests, [], "a request was made for a path that should have been refused")

    def test_a_version_that_is_not_x_y_z_and_a_kind_that_is_missing_or_twice_are_refused_before_any_request(self):
        state_dir = self.state_for()
        server = self.serve({})
        for bad in ("5.5.1\n", "5.5", "../5.5.1", "05.5.1", ""):
            with self.subTest(version=bad):
                self.failed_closed(self.artifact_cmd(state_dir, server.base, version=bad))
        self.verdict(self.artifact_cmd(state_dir, server.base, kind="plugin"), 8, "no plugin artifact")
        manifest = small_release({"windows-zip": ("windows.zip", self.DATA, {})})
        manifest["releases"]["5.5.1"]["artifacts"] *= 2
        self.verdict(self.artifact_cmd(self.ok_state(manifest, "twice"), server.base), 8, "twice")
        self.assertEqual(server.requests, [])

    def test_step_8_uses_the_stored_manifest_and_no_other(self):
        server = self.serve({self.PATH: self.DATA})
        state_dir = self.state_for()
        edited = json.loads((state_dir / "manifest.json").read_text())
        edited["releases"]["5.5.1"]["artifacts"][0]["path"] = "v5.5.1/nevr-runtime-v5.5.1-evil.zip"
        (state_dir / "manifest.json").write_bytes(dump(edited))
        self.verdict(self.artifact_cmd(state_dir, server.base), 8, "not the one that passed step 2")
        empty = self.t / "empty"
        empty.mkdir()
        self.verdict(self.artifact_cmd(empty, server.base), 8, "no verified manifest")
        self.verdict(self.artifact_cmd(self.t / "no-such-dir" / "manifest.json", server.base), 8, "no verified manifest")
        unnamed = self.state_for()
        (unnamed / "state.json").write_text(json.dumps({"highest_manifest_version": 1}))
        self.verdict(self.artifact_cmd(unnamed, server.base), 8, "not the one that passed step 2")
        self.assertEqual(server.requests, [])

    def test_the_manifest_fetch_stops_at_the_limit_and_a_good_pair_is_kept(self):
        too_big = self.serve({"/nevr-runtime/manifest.json": b"{" * 3_000_000})
        directory = self.t / "fetched-big"
        directory.mkdir()
        self.verdict(self.run_impl("fetch", too_big.base, directory), 1, "more than 1048576")
        self.assertFalse((directory / "manifest.json").exists())
        pair = self.signed(good_manifest(), name="pair")
        served = self.serve({"/nevr-runtime/manifest.json": (pair / "manifest.json").read_bytes(),
                             "/nevr-runtime/manifest.json.sig": (pair / "manifest.json.sig").read_bytes()})
        directory = self.t / "fetched-ok"
        directory.mkdir()
        self.assertEqual(self.run_impl("fetch", served.base, directory).returncode, 0)
        self.assertEqual((directory / "manifest.json").read_bytes(), (pair / "manifest.json").read_bytes())
        long_sig = self.serve({"/nevr-runtime/manifest.json": b"{}", "/nevr-runtime/manifest.json.sig": b"x" * 5000})
        self.verdict(self.run_impl("fetch", long_sig.base, directory), 1, "more than 1024")


@unittest.skipUnless(_impl_tools(), "set %s=1 with cargo and dotnet installed to build and run the page's Rust and C#" % IMPLS_ENV)
class RustProgram(ImplCases, ScriptCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        base = cls.root / "rust"
        for minimum in (1, 2):
            directory = base if minimum == 1 else cls.root / "rust2"
            if minimum == 2:
                shutil.copytree(base, directory)
            (directory / "src").mkdir(parents=True, exist_ok=True)
            (directory / "Cargo.toml").write_text(blocks()["Cargo.toml"])
            source = blocks()["src/main.rs"]
            if minimum == 2:
                self_check = "const MIN_MANIFEST_VERSION: u64 = 1;"
                assert source.count(self_check) == 1
                source = source.replace(self_check, "const MIN_MANIFEST_VERSION: u64 = 2;")
            (directory / "src/main.rs").write_text(source)
            _run_build(["cargo", "build", "--release", "--quiet"], directory)
        cls.binaries = {1: str(base / "target/release/manifestcheck"), 2: str(cls.root / "rust2/target/release/manifestcheck")}

    def command(self, minimum):
        return [self.binaries[minimum]]


@unittest.skipUnless(_impl_tools(), "set %s=1 with cargo and dotnet installed to build and run the page's Rust and C#" % IMPLS_ENV)
class CSharpHarness(ImplCases, ScriptCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.dlls = {}
        for minimum in (1, 2):
            directory = cls.root / ("cs%d" % minimum)
            for name in ("NevrManifest/NevrManifest.csproj", "NevrManifest/ManifestCheck.cs", "Harness/Harness.csproj", "Harness/Program.cs"):
                (directory / name).parent.mkdir(parents=True, exist_ok=True)
                text = blocks()[name]
                if name == "Harness/Program.cs" and minimum == 2:
                    assert text.count("const long MinimumManifestVersion = 1;") == 1
                    text = text.replace("const long MinimumManifestVersion = 1;", "const long MinimumManifestVersion = 2;")
                (directory / name).write_text(text)
            _run_build(["dotnet", "build", "Harness", "-c", "Release", "-o", str(directory / "out"), "--nologo", "-v", "q"], directory)
            cls.dlls[minimum] = str(directory / "out/Harness.dll")

    def command(self, minimum):
        return ["dotnet", self.dlls[minimum]]



class Pins(unittest.TestCase):
    """9. nothing the page builds resolves a newer dependency than the one it was reviewed with."""

    def test_every_rust_dependency_is_pinned_to_one_version(self):
        lines = [l for l in blocks()["Cargo.toml"].split("[dependencies]")[1].splitlines() if l.strip()]
        self.assertGreaterEqual(len(lines), 5)
        for line in lines:
            self.assertRegex(line, r'version\s*=\s*"=\d+\.\d+\.\d+"|=\s*"=\d+\.\d+\.\d+"', line)

    def test_every_nuget_package_is_pinned_to_one_version_and_a_lock_file_is_written(self):
        for name in ("NevrManifest/NevrManifest.csproj", "Harness/Harness.csproj"):
            text = blocks()[name]
            self.assertIn("<RestorePackagesWithLockFile>true</RestorePackagesWithLockFile>", text, name)
            for version in re.findall(r'<PackageReference [^>]*Version="([^"]+)"', text):
                self.assertRegex(version, r"^\[\d+\.\d+\.\d+\]$", name)
        self.assertIn("BouncyCastle.Cryptography", blocks()["NevrManifest/NevrManifest.csproj"])
        self.assertIn('Version="[2.7.0]"', blocks()["NevrManifest/NevrManifest.csproj"])

    def test_the_page_says_to_commit_the_lock_files_and_build_locked(self):
        text = doc_text()
        for needle in ("cargo build --locked --release", "dotnet restore --locked-mode", "Cargo.lock", "packages.lock.json"):
            self.assertTrue(names(text, needle), needle)


class Constants(unittest.TestCase):
    """5. all three implementations carry the integrator's minimum manifest_version and use it."""

    def test_each_implementation_has_the_constant_and_takes_the_larger_of_it_and_the_stored_value(self):
        self.assertRegex(blocks()["verify-manifest.sh"], r"\nMIN_MANIFEST_VERSION=1\n")
        self.assertIn('[ "$MIN_MANIFEST_VERSION" -gt "$have" ]', blocks()["verify-manifest.sh"])
        self.assertRegex(blocks()["src/main.rs"], r"const MIN_MANIFEST_VERSION: u64 = 1;")
        self.assertIn("stored.max(MIN_MANIFEST_VERSION)", blocks()["src/main.rs"])
        self.assertRegex(blocks()["Harness/Program.cs"], r"const long MinimumManifestVersion = 1;")
        self.assertIn("Math.Max(state.Highest, minimumVersion)", blocks()["NevrManifest/ManifestCheck.cs"])

    def test_every_implementation_checks_the_path_before_it_fetches(self):
        rust = blocks()["src/main.rs"]
        self.assertLess(rust.index("if !path_ok(version, path)"), rust.index("fetch_capped(8,"))
        csharp = blocks()["Harness/Program.cs"]
        self.assertLess(csharp.index("FindArtifact("), csharp.index("ManifestCheck.Fetch(http, args[4]"))
        script = blocks()["check-artifact.sh"]
        self.assertLess(script.index("stored-entry.sh"), script.index("fetch-capped.sh"))


if __name__ == "__main__":
    unittest.main()
