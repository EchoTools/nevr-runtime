"""The scenario runner's pure parts: loading, variable substitution, state checks."""
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "scenario"))
import run_scenario  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[2]


class LoadScenarioTest(unittest.TestCase):
    def test_invite_scenario_loads_with_typed_variables(self):
        scenario = run_scenario.load_scenario(REPO / "tools/scenario/scenarios/invite.yaml")
        self.assertEqual(scenario["name"], "invite")
        inject = next(s for s in scenario["steps"] if s["kind"] == "inject")
        self.assertEqual(inject["inject"]["id"], 1000000000000000001)  # stays an integer
        fire = next(s for s in scenario["steps"] if s["kind"] == "fire")
        self.assertEqual(fire["fire"]["user"], "OVR-ORG-1000000000000000001")
        last = scenario["steps"][-1]
        self.assertIn("target=1000000000000000001", last["expect_log"]["pattern"])

    def test_a_step_with_two_kinds_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "bad.yaml"
            path.write_text("name: bad\nsteps:\n  - name: two\n    wait_log: {pattern: a}\n    fire: {action: x}\n")
            with self.assertRaises(ValueError):
                run_scenario.load_scenario(path)


class StateMatchesTest(unittest.TestCase):
    def test_friend_invitable(self):
        state = {"friends": [{"id": 7, "online": True, "invitable": 1}]}
        ok, _ = run_scenario.state_matches(state, {"state_until": {"friend_invitable": 7}})
        self.assertTrue(ok)
        ok, detail = run_scenario.state_matches(state, {"state_until": {"friend_invitable": 8}})
        self.assertFalse(ok)
        self.assertIn("not in the roster", detail)

    def test_party_joinable(self):
        state = {"party": {"id": 0, "joinable": False, "members": [1]}}
        ok, detail = run_scenario.state_matches(state, {"state_until": {"party_joinable": True}})
        self.assertFalse(ok)
        self.assertIn("joinable=False", detail)


class ConsoleLogTest(unittest.TestCase):
    def test_wait_strips_ansi_and_honours_the_start_offset(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "console.log"
            path.write_text("\x1b[0mfirst slot=8 name=SendInvite\nsecond\n")
            log = run_scenario.ConsoleLog(path)
            self.assertIsNotNone(log.wait("slot=8 name=SendInvite", 0))
            self.assertIsNone(log.wait("slot=8 name=SendInvite", 0, start=len(log.text())))


if __name__ == "__main__":
    unittest.main()
