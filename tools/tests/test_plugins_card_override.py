"""#1857: the PLUGINS card's component id in patch/plugins_card.h is the id the override set gives the card. The override
entry (the card's CR15UIPage2ElementCR in the mnu_master override) is the source of truth; the hook reads the constant, and
this check fails when the two differ, so a changed id cannot drift unnoticed.

The override set is not in the repository (it is made from game files). Point NEVR_PLUGINS_CARD_OVERRIDE_DIR at the
directory that holds it; without it the file checks skip and say so, and the source checks below still run.
"""
from __future__ import annotations

import os
import re
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "src/runtime/patch/plugins_card.h"
ENV = "NEVR_PLUGINS_CARD_OVERRIDE_DIR"
# The page-element resource of the mnu_master level (type 0xa691ec467180d00c, level 0xac360e41e4ede056).
OVERRIDE_FILE = "0xa691ec467180d00c.0xac360e41e4ede056"
# Byte offsets in that override file of the two card entries' component ids: the tutorials card, and the clone made from
# it (the PLUGINS card). Measured on the override set of #1857, not a layout the game documents.
TUTORIALS_ID_OFFSET = 4080
PLUGINS_ID_OFFSET = 5616
HEX_BASE = 16
# Size of the stand-in override file the tests below build; it only has to cover both offsets plus an 8-byte id.
FAKE_OVERRIDE_BYTES = 6000


def constant(header: str, name: str) -> int | None:
    match = re.search(rf"\b{name}\s*=\s*(0x[0-9a-fA-F]+)", header)
    return int(match.group(1), HEX_BASE) if match else None


def ids_in(blob: bytes) -> tuple[int, int]:
    return (
        struct.unpack_from("<Q", blob, TUTORIALS_ID_OFFSET)[0],
        struct.unpack_from("<Q", blob, PLUGINS_ID_OFFSET)[0],
    )


def problems(header: str, blob: bytes) -> list[str]:
    plugins = constant(header, "kPluginsCardComponent")
    tutorials = constant(header, "kTutorialsCardComponent")
    if plugins is None or tutorials is None:
        return ["plugins_card.h does not define kPluginsCardComponent and kTutorialsCardComponent"]
    file_tutorials, file_plugins = ids_in(blob)
    out = []
    if file_plugins != plugins:
        out.append(f"the override's PLUGINS id is 0x{file_plugins:016x}, the header's is 0x{plugins:016x}")
    if file_tutorials != tutorials:
        out.append(f"the override's TUTORIALS id is 0x{file_tutorials:016x}, the header's is 0x{tutorials:016x}")
    if file_plugins == file_tutorials:
        out.append("the PLUGINS card shares the TUTORIALS id, so a press on it starts the tutorial")
    return out


def blob_with(tutorials: int, plugins: int) -> bytes:
    blob = bytearray(FAKE_OVERRIDE_BYTES)
    struct.pack_into("<Q", blob, TUTORIALS_ID_OFFSET, tutorials)
    struct.pack_into("<Q", blob, PLUGINS_ID_OFFSET, plugins)
    return bytes(blob)


class PluginsCardIdTest(unittest.TestCase):
    def test_the_header_defines_both_ids(self):
        header = HEADER.read_text()
        self.assertIsNotNone(constant(header, "kPluginsCardComponent"))
        self.assertIsNotNone(constant(header, "kTutorialsCardComponent"))

    def test_the_check_passes_on_matching_ids(self):
        header = HEADER.read_text()
        blob = blob_with(constant(header, "kTutorialsCardComponent"), constant(header, "kPluginsCardComponent"))
        self.assertEqual(problems(header, blob), [])

    def test_the_check_fails_when_the_override_changed_the_id(self):
        header = HEADER.read_text()
        blob = blob_with(constant(header, "kTutorialsCardComponent"), constant(header, "kPluginsCardComponent") + 1)
        self.assertNotEqual(problems(header, blob), [])

    def test_the_check_fails_when_the_clone_shares_the_tutorial_id(self):
        header = HEADER.read_text()
        tutorials = constant(header, "kTutorialsCardComponent")
        self.assertNotEqual(problems(header, blob_with(tutorials, tutorials)), [])

    def test_the_header_matches_the_override_set(self):
        directory = os.environ.get(ENV)
        if not directory:
            self.skipTest(f"{ENV} is not set: the override set is not in the repository, so the header was not compared with it")
        path = Path(directory) / OVERRIDE_FILE
        self.assertTrue(path.is_file(), f"{ENV}={directory} has no {OVERRIDE_FILE}")
        self.assertEqual(problems(HEADER.read_text(), path.read_bytes()), [])


if __name__ == "__main__":
    unittest.main()
