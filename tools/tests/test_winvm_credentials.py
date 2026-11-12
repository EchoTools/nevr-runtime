"""systest refuses a rig that holds a sign-in file before it sets the rig up, deploys to it or starts the game."""

from __future__ import annotations

import argparse
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "winvm"))
import systest  # noqa: E402

SIGN_IN_LISTING = systest.RIG_LOCALAPPDATA + "\\nevr\\echovr\\.credentials.json\r\n"


class FakeGuest:
    """Answers the rig's credential search with a canned listing; records every script it is asked to run."""

    def __init__(self, listing: str = ""):
        self.listing = listing
        self.scripts: list[str] = []

    def ps(self, script: str) -> str:
        self.scripts.append(script)
        return self.listing

    def put(self, local, remote_dir, name):
        raise AssertionError("nothing is deployed to a refused rig")


class RefuseCopyCredentialsTest(unittest.TestCase):
    def test_a_rig_holding_a_sign_in_file_is_refused_with_its_path(self):
        with self.assertRaises(systest.EnvError) as refused:
            systest.refuse_copy_credentials(FakeGuest(SIGN_IN_LISTING))
        self.assertIn(SIGN_IN_LISTING.strip(), str(refused.exception))

    def test_a_clean_rig_is_not_refused(self):
        systest.refuse_copy_credentials(FakeGuest(""))

    def test_the_search_covers_the_rig_echovr_folder_and_its_localappdata_by_the_shell_pattern(self):
        guest = FakeGuest("")
        systest.refuse_copy_credentials(guest)
        (script,) = guest.scripts
        self.assertIn(systest.ROOT + "\\echovr", script)
        self.assertIn(systest.RIG_LOCALAPPDATA, script)
        self.assertIn(systest.CREDENTIAL_PATTERN, script)
        self.assertEqual(systest.CREDENTIAL_PATTERN, ".credentials.json*")

    def test_the_boot_scenario_refuses_before_the_rig_is_set_up_deployed_to_or_launched(self):
        guest = FakeGuest(SIGN_IN_LISTING)
        args = argparse.Namespace(with_legacy_dbgcore=False, game_args="", wait=1)
        with tempfile.TemporaryDirectory() as t, \
                mock.patch.object(systest, "setup_rig") as setup, \
                mock.patch.object(systest, "deploy") as deploy, \
                mock.patch.object(systest, "launch") as launch:
            with self.assertRaises(systest.EnvError):
                systest.scenario_boot(guest, pathlib.Path(t) / "BugSplat64.dll", pathlib.Path(t), args)
        setup.assert_not_called()
        deploy.assert_not_called()
        launch.assert_not_called()


if __name__ == "__main__":
    unittest.main()
