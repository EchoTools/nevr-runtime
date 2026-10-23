"""tools/fetch_real_signed_dll.py (#745): it hands the test a file only when both digests match."""
import contextlib
import hashlib
import importlib.util
import io
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

TOOL = Path(__file__).resolve().parents[1] / "fetch_real_signed_dll.py"
spec = importlib.util.spec_from_file_location("fetch_real_signed_dll", TOOL)
fetch_tool = importlib.util.module_from_spec(spec)
sys.modules["fetch_real_signed_dll"] = fetch_tool
spec.loader.exec_module(fetch_tool)


def make_zip(member: str, payload: bytes) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as archive:
        archive.writestr(member, payload)
    return buf.getvalue()


class FetchRealSignedDll(unittest.TestCase):
    """unittest, not pytest: `just verify` discovers with unittest and the builder image has no pytest."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.cache = self.tmp / "cache"

    def serve(self, member="BugSplat64.dll", payload=b"MZ pretend signed dll", pin_dll=True):
        data = make_zip(member, payload)
        source = self.tmp / "release.zip"
        source.write_bytes(data)
        patches = [mock.patch.object(fetch_tool, "ZIP_SHA256", hashlib.sha256(data).hexdigest())]
        if pin_dll:
            patches.append(mock.patch.object(fetch_tool, "DLL_SHA256", hashlib.sha256(payload).hexdigest()))
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)
        return source, payload

    def test_a_matching_zip_is_extracted_into_the_cache(self):
        source, payload = self.serve()
        path = fetch_tool.fetch(self.cache, source.as_uri())
        self.assertEqual(path.read_bytes(), payload)
        self.assertEqual(list(self.cache.glob("*.part")), [])

    def test_a_cached_file_with_the_right_digest_is_reused_without_a_download(self):
        source, _ = self.serve()
        first = fetch_tool.fetch(self.cache, source.as_uri())
        source.unlink()
        self.assertEqual(fetch_tool.fetch(self.cache, source.as_uri()), first)

    def test_a_zip_with_another_digest_is_refused_and_leaves_no_file(self):
        source, _ = self.serve()
        with mock.patch.object(fetch_tool, "ZIP_SHA256", "0" * 64):
            with self.assertRaisesRegex(SystemExit, "expected " + "0" * 64):
                fetch_tool.fetch(self.cache, source.as_uri())
        self.assertFalse(self.cache.exists())

    def test_a_dll_with_another_digest_is_refused_and_leaves_no_file(self):
        source, _ = self.serve()
        with mock.patch.object(fetch_tool, "DLL_SHA256", "1" * 64):
            with self.assertRaisesRegex(SystemExit, "expected " + "1" * 64):
                fetch_tool.fetch(self.cache, source.as_uri())
        self.assertFalse(self.cache.exists())

    def test_a_zip_without_the_member_is_refused(self):
        source, _ = self.serve(member="other.dll", payload=b"x", pin_dll=False)
        with self.assertRaisesRegex(SystemExit, "is not in the zip"):
            fetch_tool.fetch(self.cache, source.as_uri())

    def test_a_failed_download_exits_1_with_the_reason(self):
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            rc = fetch_tool.main([str(self.cache), "--url", (self.tmp / "missing.zip").as_uri()])
        self.assertEqual(rc, 1)
        self.assertIn("fetch_real_signed_dll:", err.getvalue())

    def test_a_response_over_the_size_limit_is_refused(self):
        source, _ = self.serve()
        with mock.patch.object(fetch_tool, "MAX_ZIP_BYTES", 16):
            with self.assertRaisesRegex(SystemExit, "larger than 16 bytes"):
                fetch_tool.fetch(self.cache, source.as_uri())
        self.assertFalse(self.cache.exists())


if __name__ == "__main__":
    unittest.main()
