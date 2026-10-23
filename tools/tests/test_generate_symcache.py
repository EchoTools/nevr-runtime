"""tools/generate-symcache.sh writes the same bytes under every locale (#792): `just build` runs it,
and a locale-dependent order rewrote the tracked symcache_data.cpp under systemd's C.UTF-8."""
import os
import pathlib
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools/generate-symcache.sh"


def fixture_go_file(path: pathlib.Path) -> None:
    lines = ["// fixture"] * 32
    # Hashes that differ only in the case of their hex digits tie under `sort -n` and are ordered by
    # the locale's collation: C puts upper case first, en_US puts lower case first.
    for i in range(1200):
        lines.append(f'\t0x{0x9000000000000000 + i * 977:016x}: "name{i}",')
    lines.append('\t0xA1B9CAE1F8588968: "UpperCaseHash",')
    lines.append('\t0xa1b9cae1f8588968: "LowerCaseHash",')
    lines.append('\t0xB1B9CAE1F8588968: "UpperB",')
    lines.append('\t0xb1b9cae1f8588968: "LowerB",')
    lines += ["// end"] * 10
    path.write_text("\n".join(lines) + "\n")


class GenerateSymcacheTest(unittest.TestCase):
    def generate(self, locale: str) -> str:
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            go_file, out = tmp / "core_hash_lookup.go", tmp / "symcache_data.cpp"
            fixture_go_file(go_file)
            env = {k: v for k, v in os.environ.items() if not k.startswith("LC_") and k != "LANG"}
            env.update(LANG=locale, NEVR_SYMCACHE_GO_FILE=str(go_file), NEVR_SYMCACHE_OUT=str(out))
            result = subprocess.run(["bash", str(SCRIPT)], env=env, capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            return out.read_text()

    def test_the_output_is_the_same_under_c_utf8_and_en_us_utf8(self):
        self.assertEqual(self.generate("C.UTF-8"), self.generate("en_US.UTF-8"))

    def test_ties_come_out_in_byte_order(self):
        text = self.generate("en_US.UTF-8")
        self.assertLess(text.index("UpperCaseHash"), text.index("LowerCaseHash"))
        self.assertLess(text.index("UpperB"), text.index("LowerB"))


if __name__ == "__main__":
    unittest.main()
