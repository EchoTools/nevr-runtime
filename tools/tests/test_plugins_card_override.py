"""#1857: the PLUGINS card's component id in patch/plugins_card.h is the id the override set gives the card. The override
entry (the card's CR15UIPage2ElementCR in the mnu_master override) is the source of truth; the hook reads the constant, and
this check fails when the two differ, so a changed id cannot drift unnoticed.

The override set is not in the repository (it is made from game files, and RELEASING.md "Game-derived packages" keeps game
bytes out). What is in the repository is tools/tests/fixtures/plugins_card_override_ids.json: the two ids and the two byte
offsets, nothing else of the resource, and no byte of the game's own data. Every run checks the header against that fixture,
and checks the check itself on a stand-in file built from it. Point NEVR_PLUGINS_CARD_OVERRIDE_DIR at the directory that holds
the real override set and the fixture is checked against the real file as well; without it that one test skips and says so.
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


def fixture_ids(fixture: dict) -> tuple[int, int]:
    return int(fixture["tutorials"]["component"], HEX_BASE), int(fixture["plugins"]["component"], HEX_BASE)


def constant(header: str, name: str) -> int | None:
    match = re.search(rf"\b{name}\s*=\s*(0x[0-9a-fA-F]+)", header)
    return int(match.group(1), HEX_BASE) if match else None


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
    def test_the_header_defines_both_ids(self):
        header = HEADER.read_text()
        self.assertIsNotNone(constant(header, "kPluginsCardComponent"))
        self.assertIsNotNone(constant(header, "kTutorialsCardComponent"))

    def test_the_header_ids_are_the_fixtures(self):
        fixture = load_fixture()
        tutorials, plugins = fixture_ids(fixture)
        header = HEADER.read_text()
        self.assertEqual(constant(header, "kPluginsCardComponent"), plugins)
        self.assertEqual(constant(header, "kTutorialsCardComponent"), tutorials)

    def test_the_fixture_names_the_override_file_and_two_distinct_ids(self):
        fixture = load_fixture()
        tutorials, plugins = fixture_ids(fixture)
        self.assertEqual(fixture["override_file"], "0xa691ec467180d00c.0xac360e41e4ede056")
        self.assertNotEqual(tutorials, plugins, "the clone must not share the TUTORIALS id: a press would start the tutorial")
        self.assertNotEqual(fixture["tutorials"]["offset"], fixture["plugins"]["offset"])

    def test_the_check_passes_on_a_file_built_from_the_fixture(self):
        fixture = load_fixture()
        tutorials, plugins = fixture_ids(fixture)
        self.assertEqual(problems(HEADER.read_text(), blob_with(fixture, tutorials, plugins), fixture), [])

    def test_the_check_fails_when_the_header_id_drifts_from_the_fixture(self):
        fixture = load_fixture()
        tutorials, plugins = fixture_ids(fixture)
        header = HEADER.read_text().replace(f"0x{plugins:016x}", f"0x{plugins + 1:016x}")
        self.assertNotEqual(problems(header, blob_with(fixture, tutorials, plugins), fixture), [])

    def test_the_check_fails_when_the_override_changed_the_id(self):
        fixture = load_fixture()
        tutorials, plugins = fixture_ids(fixture)
        self.assertNotEqual(problems(HEADER.read_text(), blob_with(fixture, tutorials, plugins + 1), fixture), [])

    def test_the_check_fails_when_the_clone_shares_the_tutorial_id(self):
        fixture = load_fixture()
        tutorials, _ = fixture_ids(fixture)
        self.assertNotEqual(problems(HEADER.read_text(), blob_with(fixture, tutorials, tutorials), fixture), [])

    def test_the_fixture_matches_the_real_override_set(self):
        directory = os.environ.get(ENV)
        if not directory:
            # CI never sets ENV (the override set is not in the repository), so this test always skips in CI: the check of the
            # fixture against the real override file runs only locally, and is NOT VERIFIED in CI.
            self.skipTest(f"{ENV} is not set: the override set is not in the repository, so the fixture was not compared with it (not run in CI)")
        fixture = load_fixture()
        path = Path(directory) / fixture["override_file"]
        self.assertTrue(path.is_file(), f"{ENV}={directory} has no {fixture['override_file']}")
        self.assertEqual(ids_in(path.read_bytes(), fixture), fixture_ids(fixture))
        self.assertEqual(problems(HEADER.read_text(), path.read_bytes(), fixture), [])


if __name__ == "__main__":
    unittest.main()
