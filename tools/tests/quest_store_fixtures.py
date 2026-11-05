"""A scratch repository tree for the store-build tool tests (tools/quest_store_build.py, tools/quest_store_install.py).

It holds the files the steps leave and the tools read: the pin file, a finished stock-data directory, the sentinel library, the
build info, the libr15 patcher and a repacked APK. Every file is a few bytes of made-up content; nothing here is game data.
"""
from __future__ import annotations

import hashlib
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import quest_store_build as qsb  # noqa: E402

ANDROID = "_data/5932408047/rad15/android/"
DATA = {
    ANDROID + "manifests/48037dc70b0ecab2": b"manifest-bytes-" * 40,
    ANDROID + "packages/48037dc70b0ecab2_0": bytes(range(256)) * 50,
    ANDROID + "manifests/2b47aab238f60515": b"second manifest",
    ANDROID + "packages/2b47aab238f60515_0": b"second package" * 7,
}
OBB_SHA = hashlib.sha256(b"a made-up store obb").hexdigest()
RECIPE = "android-repack-apk apk shim:\n    tools/quest_keystore.sh with apksigner\n    python3 -I tools/quest_repack_libs.py stage\n"
SIGNER = "ab" * 32


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def pins_text(data: dict = DATA) -> str:
    lines = [f"obb_sha256 {OBB_SHA}", "obb_size 12345"]
    for path, content in data.items():
        lines.append(f"file {sha(content)} {len(content)} {path}")
    return "\n".join(lines) + "\n"


def write(path: pathlib.Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def make_root(base: pathlib.Path, data: dict = DATA) -> tuple[qsb.Layout, pathlib.Path]:
    """Returns the layout and the store APK's path (outside the tree: a per-machine input)."""
    root = base / "repo"
    layout = qsb.Layout(root)
    write(layout.pins, pins_text(data).encode())
    stock = layout.stock_root / OBB_SHA
    for path, content in data.items():
        write(stock / path, content)
    write(stock / "stock-data.json", b'{"schema": 1}\n')
    write(layout.shim, b"sentinel shim v1")
    write(layout.build_info, b'constexpr char kVersion[] = "6.17.0";\n')
    write(layout.patcher, b"patcher v1")
    write(layout.repacked_apk, b"signed apk made by the repack")
    write(root / "tools" / "quest_keystore.sh", b"keystore script v1")
    write(root / "tools" / "quest_repack_libs.py", b"repack libs v1")
    store_apk = base / "store.apk"
    store_apk.write_bytes(b"the store apk")
    return layout, store_apk
