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

    def test_local_server_keys_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "local.yaml"
            path.write_text("name: x\nserver: local\nsteps:\n  - name: a\n    wait_log: {pattern: a}\n")
            with self.assertRaisesRegex(ValueError, "server not supported"):
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


class InviteStateTest(unittest.TestCase):
    def test_invite_count_and_sender(self):
        state = {"invites": [{"party": 77, "sender": 4242}]}
        ok, _ = run_scenario.state_matches(state, {"state_until": {"invite_count": 1, "invite_sender": 4242}})
        self.assertTrue(ok)
        ok, _ = run_scenario.state_matches(state, {"state_until": {"invite_count": 1, "invite_sender": 1}})
        self.assertFalse(ok)
        ok, _ = run_scenario.state_matches({"invites": []}, {"state_until": {"invite_count": 0}})
        self.assertTrue(ok)

    def test_path_equals(self):
        state = {"game": {"self_muted": True, "social_features": 5}, "party": {"locked": False}}
        self.assertTrue(run_scenario.state_matches(state, {"state_until": {"path": "game.self_muted", "equals": True}})[0])
        self.assertFalse(run_scenario.state_matches(state, {"state_until": {"path": "party.locked", "equals": True}})[0])
        self.assertTrue(run_scenario.state_matches(state, {"state_until": {"path": "game.social_features", "equals": "5"}})[0])
        self.assertFalse(run_scenario.state_matches(state, {"state_until": {"path": "game.missing", "equals": 0}})[0])
        self.assertTrue(run_scenario.state_matches(state, {"state_until": {"path": "game.social_features", "bits_set": 4, "bits_clear": 2}})[0])
        self.assertFalse(run_scenario.state_matches(state, {"state_until": {"path": "game.social_features", "bits_set": 2}})[0])

    def test_party_members(self):
        state = {"party": {"id": 7, "members": [1, 2]}}
        self.assertTrue(run_scenario.state_matches(state, {"state_until": {"party_members": 2}})[0])
        self.assertFalse(run_scenario.state_matches(state, {"state_until": {"party_members": 1}})[0])

    def test_party_joining_and_room(self):
        state = {"party": {"id": 0, "room": 77, "joining": True}}
        ok, _ = run_scenario.state_matches(state, {"state_until": {"party_joining": True, "party_room": "77"}})
        self.assertTrue(ok)
        ok, _ = run_scenario.state_matches(state, {"state_until": {"party_joining": True, "party_room": 5}})
        self.assertFalse(ok)
        ok, _ = run_scenario.state_matches(state, {"state_until": {"party_joining": False}})
        self.assertFalse(ok)


class ConsoleLogTest(unittest.TestCase):
    def test_wait_strips_ansi_and_honours_the_start_offset(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "console.log"
            path.write_text("\x1b[0mfirst slot=8 name=SendInvite\nsecond\n")
            log = run_scenario.ConsoleLog(path)
            self.assertIsNotNone(log.wait("slot=8 name=SendInvite", 0))
            self.assertIsNone(log.wait("slot=8 name=SendInvite", 0, start=len(log.text())))


class WaitStopsOnFailureTest(unittest.TestCase):
    """A wait ends on the game's own failure, not on a clock."""

    def test_a_fatal_line_ends_the_wait_and_is_quoted(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "console.log"
            path.write_text("0494:err:winediag:nodrv_CreateWindow Application tried to create a window, "
                            "but no driver could be loaded.\n")
            log = run_scenario.ConsoleLog(path)
            with self.assertRaises(run_scenario.StepFailed) as caught:
                log.wait("to in game", None)
            self.assertIn("no driver could be loaded", str(caught.exception))

    def test_a_game_that_exited_ends_the_wait(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "console.log"
            path.write_text("2026-10-01T19:23:08.656Z info [EVR] [NETGAME] NetGame switching state "
                            "(from logged out, to loading root)\n")
            log = run_scenario.ConsoleLog(path)
            with self.assertRaises(run_scenario.StepFailed) as caught:
                log.wait("to in game", None, alive=lambda: False)
            self.assertIn("exited", str(caught.exception))
            self.assertIn("to loading root", str(caught.exception))

    def test_a_fatal_line_before_the_start_offset_is_history_not_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "console.log"
            path.write_text("[NEVR.FATAL] old\nslot=37 name=OpenFriendRequestUI call=1\n")
            log = run_scenario.ConsoleLog(path)
            self.assertIsNotNone(log.wait("slot=37", 0, start=len("[NEVR.FATAL] old\n")))


if __name__ == "__main__":
    unittest.main()


class SuiteTableTest(unittest.TestCase):
    def test_one_row_per_scenario(self):
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scenario"))
        import run_all
        md = run_all.suite_table([{"scenario": "a", "result": "PASS", "seconds": 3.2, "folder": "/x/a"},
                                  {"scenario": "b", "result": "FAIL", "seconds": 9.9, "folder": "/x/b"}])
        self.assertIn("| 1 | a | PASS | 3 | /x/a |", md)
        self.assertIn("| 2 | b | FAIL | 10 | /x/b |", md)


class GpuWaitTest(unittest.TestCase):
    def test_waits_until_enough_memory_is_free(self):
        readings = iter([(1000, "pid 7, 6000 MiB"), (1000, "pid 7, 6000 MiB"), (5000, "none")])
        calls = []
        original_free, original_poll = run_scenario.gpu_free_mib, run_scenario.GPU_POLL_SECONDS
        run_scenario.gpu_free_mib = lambda: (calls.append(1), next(readings))[1]
        run_scenario.GPU_POLL_SECONDS = 0
        try:
            run_scenario.wait_for_gpu_memory(4096)
        finally:
            run_scenario.gpu_free_mib, run_scenario.GPU_POLL_SECONDS = original_free, original_poll
        self.assertEqual(len(calls), 3)


class LateVariableTest(unittest.TestCase):
    def test_unknown_names_survive_load_and_expand_later(self):
        step = run_scenario.substitute({"pattern": "for '${who}' in ${room}"}, {"room": 7})
        self.assertEqual(step["pattern"], "for '${who}' in 7")
        self.assertEqual(run_scenario.substitute(step, {"who": "OVR-ORG-5"})["pattern"], "for 'OVR-ORG-5' in 7")


class ListStateTest(unittest.TestCase):
    def test_contains_and_lacks(self):
        state = {"game": {"muted_users": [5, 77]}}
        self.assertTrue(run_scenario.state_matches(state, {"state_until": {"path": "game.muted_users", "contains": "77"}})[0])
        self.assertFalse(run_scenario.state_matches(state, {"state_until": {"path": "game.muted_users", "lacks": 77}})[0])
        self.assertTrue(run_scenario.state_matches(state, {"state_until": {"path": "game.muted_users", "lacks": 9}})[0])
