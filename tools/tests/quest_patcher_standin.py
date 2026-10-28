"""A stand-in for the libr15 patcher (src/quest/patch/quest_patch_libr15) for the tests of the Quest repack.

The real tool refuses any file but the store's 58 MB libr15.so, so the tests that run `tools/quest_repack_libs.py
verify-signed` or `just android-repack-apk` over a fabricated APK give them this executable instead. It takes the real
tool's command line (`--stock F --out F --report F [--extra-rows F]`), records the arguments it was given in
`<script>.argv` beside itself, and by mode:

  ok       writes --out as the --stock bytes followed by MARKER (so its output differs from its input and depends on it)
           and a --report file, exits 0
  fail     writes a line to stderr and exits 3, writing nothing
  nowrite  exits 0 without writing --out
  empty    exits 0 after writing an empty --out
  same     writes --out as the --stock bytes unchanged and exits 0 (a patcher that patched nothing)
"""
from __future__ import annotations

import pathlib
import stat
import sys

MARKER = b"\n#NEVR-STANDIN-PATCHED\n"

_SCRIPT = '''#!{python}
import pathlib
import sys

argv = sys.argv[1:]
me = pathlib.Path(__file__)
me.with_name(me.name + ".argv").write_text("\\n".join(argv) + "\\n")
flags = dict(zip(argv[0::2], argv[1::2]))
mode = {mode!r}
if mode == "fail":
    sys.stderr.write("standin patcher: refused (mode fail)\\n")
    sys.exit(3)
if mode == "nowrite":
    sys.exit(0)
out = pathlib.Path(flags["--out"])
if mode == "empty":
    out.write_bytes(b"")
    sys.exit(0)
stock = pathlib.Path(flags["--stock"]).read_bytes()
out.write_bytes(stock if mode == "same" else stock + {marker!r})
pathlib.Path(flags["--report"]).write_text("{{}}\\n")
'''


def make_standin(directory: pathlib.Path, mode: str = "ok", name: str = "quest_patch_libr15") -> pathlib.Path:
    """Write the stand-in executable `directory/name` and return its path."""
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / name
    path.write_text(_SCRIPT.format(python=sys.executable, mode=mode, marker=MARKER))
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


def recorded_argv(path: pathlib.Path) -> list[str]:
    """The arguments the stand-in was last run with."""
    return path.with_name(path.name + ".argv").read_text().splitlines()
