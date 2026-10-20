"""tools/asset_cache.py: puts a file into the asset cache in the layout src/runtime/patch/asset_cache.h defines.

Run for real, as a subprocess, against a temp directory. The C++ side is tested by src/runtime/tests/test_asset_cache.cpp;
the last class pins that the two agree on the file names and the index format."""
import hashlib
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "asset_cache.py"
TYPE, NAME = "0xbeac1969cb7b8861", "0x1f72c5e0fc9fc8c0"


def run(*args):
    return subprocess.run([sys.executable, "-I", str(TOOL), *args], capture_output=True, text=True)


class AssetCacheToolTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="asset-cache-tool-")
        self.root = Path(self._tmp.name)
        self.cache = self.root / "nevr" / "echovr" / "cache"
        self.src = self.root / "src.dds"
        self.src.write_bytes(bytes(range(256)) * 20)

    def tearDown(self):
        self._tmp.cleanup()

    def put(self, **kw):
        args = ["put", "--dir", str(self.cache), "--type", kw.get("type", TYPE), "--name", kw.get("name", NAME)]
        if "label" in kw:
            args += ["--label", kw["label"]]
        return run(*args, str(kw.get("file", self.src)))

    def index(self):
        return json.loads((self.cache / "index.json").read_text())

    def test_put_names_the_blob_by_its_sha256_and_indexes_it(self):
        result = self.put(label="cache_splash")
        self.assertEqual(result.returncode, 0, result.stderr)
        sha = hashlib.sha256(self.src.read_bytes()).hexdigest()
        self.assertEqual((self.cache / f"{sha}.blob").read_bytes(), self.src.read_bytes())
        self.assertEqual(self.index(), {"version": 1, "overrides": [
            {"type": TYPE, "name": NAME, "sha256": sha, "label": "cache_splash"}]})
        self.assertIn(f"sha256={sha}", result.stdout)

    def test_no_temp_file_is_left_behind(self):
        self.assertEqual(self.put().returncode, 0)
        self.assertEqual(list(self.cache.glob("*.tmp")), [])

    def test_a_second_put_for_the_same_type_and_name_replaces_the_entry(self):
        self.assertEqual(self.put(label="a").returncode, 0)
        other = self.root / "other.dds"
        other.write_bytes(b"other bytes")
        self.assertEqual(self.put(file=other, label="b").returncode, 0)
        self.assertEqual(self.put(name="0x42", label="c").returncode, 0)
        entries = self.index()["overrides"]
        self.assertEqual([(e["name"], e["label"]) for e in entries], [(NAME, "b"), ("0x0000000000000042", "c")])
        self.assertEqual(entries[0]["sha256"], hashlib.sha256(b"other bytes").hexdigest())

    def test_verify_passes_a_good_cache_and_fails_a_flipped_byte(self):
        self.assertEqual(self.put().returncode, 0)
        self.assertEqual(run("verify", "--dir", str(self.cache)).returncode, 0)
        blob = next(self.cache.glob("*.blob"))
        data = bytearray(blob.read_bytes())
        data[10] ^= 1
        blob.write_bytes(bytes(data))
        result = run("verify", "--dir", str(self.cache))
        self.assertEqual(result.returncode, 1)
        self.assertIn("does not hash to its name", result.stderr)

    def test_verify_fails_an_indexed_blob_that_is_gone(self):
        self.assertEqual(self.put().returncode, 0)
        next(self.cache.glob("*.blob")).unlink()
        result = run("verify", "--dir", str(self.cache))
        self.assertEqual(result.returncode, 1)
        self.assertIn("blob missing", result.stderr)

    def test_a_damaged_index_stops_the_put_and_is_left_as_found(self):
        self.cache.mkdir(parents=True)
        (self.cache / "index.json").write_text("{ not json")
        result = self.put()
        self.assertEqual(result.returncode, 1)
        self.assertIn("not overwriting", result.stderr)
        self.assertEqual((self.cache / "index.json").read_text(), "{ not json")
        self.assertEqual(list(self.cache.glob("*.blob")), [], "no blob is written when the index cannot be updated")

    def test_bad_arguments_fail_loudly(self):
        for bad in ({"type": "beac"}, {"name": "0x"}, {"name": "0x0"}, {"name": "0x" + "f" * 17}):
            result = self.put(**bad)
            self.assertEqual(result.returncode, 1, bad)
            self.assertTrue(result.stderr.startswith("FAIL:"), result.stderr)
        self.assertEqual(self.put(file=self.root / "missing.dds").returncode, 1)


class AgreesWithTheCppCache(unittest.TestCase):
    def test_file_names_and_index_format_match_asset_cache_cpp(self):
        cpp = (REPO / "src/runtime/patch/asset_cache.cpp").read_text()
        header = (REPO / "src/runtime/patch/asset_cache.h").read_text()
        tool = TOOL.read_text()
        self.assertIn('kBlobSuffix[] = L".blob"', cpp)
        self.assertIn('BLOB_SUFFIX = ".blob"', tool)
        self.assertIn('kIndexName[] = L"index.json"', header)
        self.assertIn('INDEX_NAME = "index.json"', tool)
        self.assertIn('kTempSuffix[] = L".tmp"', cpp)
        self.assertIn('.tmp"', tool)
        self.assertIn('{"version", 1}, {"overrides", overrides}', cpp)
        self.assertIn('{"version": 1, "overrides": kept}', tool)
        self.assertIn('"0x%016llx"', cpp)
        self.assertIn('f"0x{type_hash:016x}"', tool)
        cap = re.search(r"kMaxBlobBytes = (\d+)ull \* 1024 \* 1024", header)
        self.assertEqual(int(cap.group(1)) * 1024 * 1024, 64 * 1024 * 1024)
        self.assertIn("MAX_BLOB_BYTES = 64 * 1024 * 1024", tool)


if __name__ == "__main__":
    unittest.main()
