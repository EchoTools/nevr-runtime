"""#1857: the PLUGINS card's component id in patch/plugins_card.h is the id the override set gives the card. The override
entry (the card's CR15UIPage2ElementCR in the mnu_master override) is the source of truth; the hook reads the constant, and
the local check below fails when the two differ, so a changed id cannot drift unnoticed.

What is in the repository: the header's two ids (source, like the addresses in hook/addresses.h), and
tools/tests/fixtures/plugins_card_override_ids.json, which holds the override file's name and the two byte offsets of the ids
inside it (layout facts), and no id and no byte of the game's data. The override set itself is not in the repository
(RELEASING.md "Game-derived packages" keeps game bytes out).

What CI runs: the header defines two distinct ids, and the check itself, on a stand-in file built from the header's own ids at
the fixture's offsets (it passes on a matching file and fails on a drifted, a changed or a shared id, and on swapped offsets).
What CI does not run: the comparison of the header with the real override file. That test reads both ids from the real file at
the fixture's offsets and runs only when NEVR_PLUGINS_CARD_OVERRIDE_DIR names a directory holding the override set; without it
the test skips, so that comparison is NOT VERIFIED in CI.
"""
from __future__ import annotations

import json
import os
import re
import struct
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent  # <repo>/tools/tests
ROOT = TESTS_DIR.parent.parent  # the repo root
HEADER = ROOT / "src/runtime/patch/plugins_card.h"
FIXTURE = ROOT / "tools/tests/fixtures/plugins_card_override_ids.json"
ENV = "NEVR_PLUGINS_CARD_OVERRIDE_DIR"
HEX_BASE = 16
# Bytes of slack past the last id in the stand-in override file the tests below build.
FAKE_OVERRIDE_SLACK = 64
ID_BYTES = 8


def load_fixture() -> dict:
    return json.loads(FIXTURE.read_text())


def constant(header: str, name: str) -> int | None:
    match = re.search(rf"\b{name}\s*=\s*(0x[0-9a-fA-F]+)", header)
    return int(match.group(1), HEX_BASE) if match else None


def header_ids(header: str) -> tuple[int, int]:
    """(tutorials, plugins) as the header defines them."""
    tutorials = constant(header, "kTutorialsCardComponent")
    plugins = constant(header, "kPluginsCardComponent")
    assert tutorials is not None and plugins is not None, "plugins_card.h does not define both ids"
    return tutorials, plugins


def ids_in(blob: bytes, fixture: dict) -> tuple[int, int]:
    return (
        struct.unpack_from("<Q", blob, fixture["tutorials"]["offset"])[0],
        struct.unpack_from("<Q", blob, fixture["plugins"]["offset"])[0],
    )


def problems(header: str, blob: bytes, fixture: dict) -> list[str]:
    plugins = constant(header, "kPluginsCardComponent")
    tutorials = constant(header, "kTutorialsCardComponent")
    if plugins is None or tutorials is None:
        return ["plugins_card.h does not define kPluginsCardComponent and kTutorialsCardComponent"]
    file_tutorials, file_plugins = ids_in(blob, fixture)
    out = []
    if file_plugins != plugins:
        out.append(f"the override's PLUGINS id is 0x{file_plugins:016x}, the header's is 0x{plugins:016x}")
    if file_tutorials != tutorials:
        out.append(f"the override's TUTORIALS id is 0x{file_tutorials:016x}, the header's is 0x{tutorials:016x}")
    if file_plugins == file_tutorials:
        out.append("the PLUGINS card shares the TUTORIALS id, so a press on it starts the tutorial")
    return out


def blob_with(fixture: dict, tutorials: int, plugins: int) -> bytes:
    size = max(fixture["tutorials"]["offset"], fixture["plugins"]["offset"]) + ID_BYTES + FAKE_OVERRIDE_SLACK
    blob = bytearray(size)
    struct.pack_into("<Q", blob, fixture["tutorials"]["offset"], tutorials)
    struct.pack_into("<Q", blob, fixture["plugins"]["offset"], plugins)
    return bytes(blob)


class PluginsCardIdTest(unittest.TestCase):
    def test_the_header_defines_two_distinct_ids(self):
        header = HEADER.read_text()
        plugins = constant(header, "kPluginsCardComponent")
        tutorials = constant(header, "kTutorialsCardComponent")
        self.assertIsNotNone(plugins)
        self.assertIsNotNone(tutorials)
        self.assertNotEqual(tutorials, plugins, "the clone must not share the TUTORIALS id: a press would start the tutorial")

    def test_the_fixture_holds_the_file_name_and_two_distinct_offsets_and_no_id(self):
        fixture = load_fixture()
        self.assertEqual(set(fixture), {"override_file", "tutorials", "plugins"})
        self.assertEqual(fixture["override_file"], "0xa691ec467180d00c.0xac360e41e4ede056")
        self.assertEqual(set(fixture["tutorials"]), {"offset"}, "no id may be stored in the repository")
        self.assertEqual(set(fixture["plugins"]), {"offset"}, "no id may be stored in the repository")
        self.assertNotEqual(fixture["tutorials"]["offset"], fixture["plugins"]["offset"])

    def test_the_check_passes_on_a_file_built_from_the_header(self):
        fixture = load_fixture()
        header = HEADER.read_text()
        tutorials, plugins = header_ids(header)
        self.assertEqual(problems(header, blob_with(fixture, tutorials, plugins), fixture), [])

    def test_the_check_fails_when_the_header_id_drifts_from_the_file(self):
        fixture = load_fixture()
        header = HEADER.read_text()
        tutorials, plugins = header_ids(header)
        blob = blob_with(fixture, tutorials, plugins)
        drifted = header.replace(f"0x{plugins:016x}", f"0x{plugins + 1:016x}")
        self.assertNotEqual(drifted, header)
        self.assertNotEqual(problems(drifted, blob, fixture), [])

    def test_the_check_fails_when_the_override_changed_the_id(self):
        fixture = load_fixture()
        header = HEADER.read_text()
        tutorials, plugins = header_ids(header)
        self.assertNotEqual(problems(header, blob_with(fixture, tutorials, plugins + 1), fixture), [])

    def test_the_check_names_a_changed_tutorials_id(self):
        fixture = load_fixture()
        header = HEADER.read_text()
        tutorials, plugins = header_ids(header)
        found = problems(header, blob_with(fixture, tutorials + 1, plugins), fixture)
        self.assertEqual(len(found), 1)
        self.assertIn("TUTORIALS", found[0])

    def test_the_check_names_a_shared_id_even_when_the_header_and_file_agree(self):
        fixture = load_fixture()
        header = HEADER.read_text()
        tutorials, plugins = header_ids(header)
        shared = header.replace(f"0x{plugins:016x}", f"0x{tutorials:016x}")
        found = problems(shared, blob_with(fixture, tutorials, tutorials), fixture)
        self.assertEqual(len(found), 1)
        self.assertIn("shares", found[0])

    def test_the_check_fails_when_the_clone_shares_the_tutorial_id(self):
        fixture = load_fixture()
        header = HEADER.read_text()
        tutorials, _ = header_ids(header)
        self.assertNotEqual(problems(header, blob_with(fixture, tutorials, tutorials), fixture), [])

    def test_the_check_fails_when_the_offsets_are_swapped(self):
        fixture = load_fixture()
        header = HEADER.read_text()
        tutorials, plugins = header_ids(header)
        blob = blob_with(fixture, tutorials, plugins)
        swapped = {**fixture, "tutorials": fixture["plugins"], "plugins": fixture["tutorials"]}
        self.assertNotEqual(problems(header, blob, swapped), [])

    def test_the_header_matches_the_real_override_file(self):
        directory = os.environ.get(ENV)
        if not directory:
            # CI never sets ENV (the override set is not in the repository), so this test always skips in CI: the comparison of
            # the header with the real override file runs only locally, and is NOT VERIFIED in CI.
            self.skipTest(f"{ENV} is not set: the override set is not in the repository, so the header's ids were not compared with the real file (NOT VERIFIED in CI)")
        fixture = load_fixture()
        path = Path(directory) / fixture["override_file"]
        self.assertTrue(path.is_file(), f"{ENV}={directory} has no {fixture['override_file']}")
        blob = path.read_bytes()
        self.assertEqual(ids_in(blob, fixture), header_ids(HEADER.read_text()))
        self.assertEqual(problems(HEADER.read_text(), blob, fixture), [])


if __name__ == "__main__":
    unittest.main()
