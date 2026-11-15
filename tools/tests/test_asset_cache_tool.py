"""tools/asset_cache.py: puts a file into the asset cache in the layout src/runtime/patch/asset_cache.h defines.

Run for real, as a subprocess, against a temp directory. The C++ side is tested by src/runtime/tests/test_asset_cache.cpp;
the last class pins that the two agree on the file names and the index format."""
import hashlib
import importlib.util
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


def load_tool():
    spec = importlib.util.spec_from_file_location("asset_cache_tool", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# The name grammar of src/runtime/patch/asset_cache_index.h (IsBlobLeaf, IsTempLeaf), written out again here as the test's own
# reading of it. The vectors below are checked against the tool's grammar and against this one.
CPP_GRAMMAR = re.compile(r"(?:[0-9a-f]{64}\.blob)|(?:(?:[0-9a-f]{64}|index)\.[0-9]{1,20}\.[0-9]{1,20}\.[0-9]{1,20}\.tmp)")
OWNED_VALID = ["0" * 64 + ".blob", "f" * 64 + ".1.2.3.tmp", "index.4.5.6.tmp", "a" * 64 + ".4294967295.18446744073709551615.7.tmp"]
OWNED_INVALID = ["../" + "0" * 64 + ".blob", "a" * 63 + ".blob", "A" * 64 + ".blob", "g" * 64 + ".blob", "a" * 64 + ".1.2.tmp",
                 "notes.1.2.3.tmp", "owned.txt", "index.json", "a" * 64 + ".1.2.3.tmp.blob", "a" * 64 + ".x.2.3.tmp",
                 "a" * 64 + ".123456789012345678901.2.3.tmp"]


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

    def owned_lines(self):
        return (self.cache / "owned.txt").read_text().splitlines()

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

    def test_put_takes_a_payload_the_size_of_the_largest_poster(self):
        # #1421: a lobby poster's payload (buffer 1) is 78,294,084 bytes; synthetic bytes of that length.
        big = self.root / "poster.dds"
        big.write_bytes(bytes(78294084))
        result = self.put(type="0xbeac1969cb7b8861", name="0xa240a2bc051b2f23", file=big)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(next(self.cache.glob("*.blob")).stat().st_size, 78294084)

    def test_bad_arguments_fail_loudly(self):
        for bad in ({"type": "beac"}, {"name": "0x"}, {"name": "0x0"}, {"name": "0x" + "f" * 17}):
            result = self.put(**bad)
            self.assertEqual(result.returncode, 1, bad)
            self.assertTrue(result.stderr.startswith("FAIL:"), result.stderr)
        self.assertEqual(self.put(file=self.root / "missing.dds").returncode, 1)


class OwnedNamesTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="asset-cache-owned-")
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

    def owned_lines(self):
        return (self.cache / "owned.txt").read_text().splitlines()

    def test_every_line_the_tool_writes_is_a_name_of_the_cpp_grammar(self):
        self.assertEqual(self.put(label="a").returncode, 0)
        sha = hashlib.sha256(self.src.read_bytes()).hexdigest()
        lines = self.owned_lines()
        self.assertIn(sha + ".blob", lines)
        self.assertTrue(any(line.startswith("index.") for line in lines), lines)
        for line in lines:
            self.assertIsNotNone(CPP_GRAMMAR.fullmatch(line), line)

    def test_a_blob_put_twice_is_listed_once(self):
        self.assertEqual(self.put(label="a").returncode, 0)
        self.assertEqual(self.put(label="b").returncode, 0)
        sha = hashlib.sha256(self.src.read_bytes()).hexdigest()
        lines = self.owned_lines()
        self.assertEqual(lines.count(sha + ".blob"), 1)
        blob_temps = [line for line in lines if line.startswith(sha + ".") and line.endswith(".tmp")]
        self.assertEqual(len(blob_temps), 1, "one blob temp: the second put wrote no blob")
        self.assertEqual(len(lines), len(set(lines)), "no name is listed twice")

    def test_each_name_is_listed_before_its_file_is_created(self):
        tool = load_tool()
        seen = []
        real = tool.write_file_synced

        def checked(directory, temp, final, data):
            if final != "owned.txt":  # the list's own rewrites are not blobs or the index
                listed = (self.cache / "owned.txt").read_text().splitlines() if (self.cache / "owned.txt").exists() else []
                seen.append((temp, temp in listed, final))
            return real(directory, temp, final, data)

        tool.write_file_synced = checked
        try:
            tool.put(str(self.cache), TYPE, NAME, "", str(self.src))
        finally:
            tool.write_file_synced = real
        self.assertEqual(len(seen), 2, "a blob temp and the index temp")
        for temp, listed, _final in seen:
            self.assertTrue(listed, f"{temp} was created before it was listed")

    def test_a_legacy_cache_adopts_only_blobs_whose_content_hashes_to_their_names(self):
        self.cache.mkdir(parents=True)
        good = b"legacy bytes"
        good_name = hashlib.sha256(good).hexdigest() + ".blob"
        (self.cache / good_name).write_bytes(good)
        (self.cache / ("c" * 64 + ".blob")).write_bytes(b"not what its name says")
        (self.cache / ("a" * 64 + ".1.2.3.tmp")).write_bytes(b"a legacy temp")
        self.assertEqual(self.put().returncode, 0)
        lines = self.owned_lines()
        self.assertIn(good_name, lines)
        self.assertNotIn("c" * 64 + ".blob", lines)
        self.assertNotIn("a" * 64 + ".1.2.3.tmp", lines)

    def test_a_damaged_owned_list_drops_the_bad_lines_and_is_rewritten_clean(self):
        self.cache.mkdir(parents=True)
        keep = "e" * 64 + ".blob"
        (self.cache / "owned.txt").write_bytes(b"../escape.blob\n" + keep.encode() + b"\r\n" + b"\x00junk\n" + (b"f" * 64) + b".blo")
        result = self.put()
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = self.owned_lines()
        self.assertIn(keep, lines, "a CRLF name survives")
        self.assertFalse(any("escape" in line or "junk" in line or line.endswith(".blo") for line in lines), lines)
        for line in lines:
            self.assertIsNotNone(CPP_GRAMMAR.fullmatch(line), line)

    def test_a_torn_tail_then_a_put_gives_a_parseable_list_with_the_new_names(self):
        self.cache.mkdir(parents=True)
        listed = "d" * 64 + ".blob"
        (self.cache / "owned.txt").write_text(listed + "\n" + "c" * 64 + ".bl")  # the last append was cut off
        self.assertEqual(self.put().returncode, 0)
        sha = hashlib.sha256(self.src.read_bytes()).hexdigest()
        lines = self.owned_lines()
        self.assertIn(listed, lines)
        self.assertIn(sha + ".blob", lines)
        for line in lines:
            self.assertIsNotNone(CPP_GRAMMAR.fullmatch(line), line)

    def test_the_grammar_vectors_agree_between_the_tool_and_this_test(self):
        tool = load_tool()
        for name in OWNED_VALID:
            self.assertTrue(tool.is_owned_leaf(name), name)
            self.assertIsNotNone(CPP_GRAMMAR.fullmatch(name), name)
        for name in OWNED_INVALID:
            self.assertFalse(tool.is_owned_leaf(name), name)
            self.assertIsNone(CPP_GRAMMAR.fullmatch(name), name)
        self.assertTrue(tool.is_owned_leaf(tool.temp_leaf("index")))
        self.assertTrue(tool.is_owned_leaf(tool.temp_leaf("a" * 64)))


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
        self.assertEqual(int(cap.group(1)) * 1024 * 1024, 128 * 1024 * 1024)
        self.assertIn("MAX_BLOB_BYTES = 128 * 1024 * 1024", tool)
        owned_header = (REPO / "src/runtime/patch/asset_cache_index.h").read_text()
        self.assertIn('kOwnedIndexName[] = "owned.txt"', owned_header)
        self.assertIn('OWNED_NAME = "owned.txt"', tool)
        self.assertIn('OWNED_TEMP_NAME = "owned.txt.tmp"', tool)
        self.assertIn('inline bool IsTempLeaf', owned_header)
        self.assertIn("detail::IsDigits(numbers.substr(0, second), 20)", owned_header)  # numbers up to 20 digits
        self.assertIn("[0-9]{1,20}", tool)


if __name__ == "__main__":
    unittest.main()
