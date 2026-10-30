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

    def test_smoke_scenario_loads_with_the_three_checks(self):
        scenario = run_scenario.load_scenario(REPO / "tools/scenario/smoke.yaml")
        self.assertEqual(scenario["name"], "smoke")
        self.assertEqual([s["kind"] for s in scenario["steps"]], ["wait_log", "wait_log", "fire", "expect_log"])
        self.assertIn("to in game", scenario["steps"][0]["wait_log"]["pattern"])
        # A cold load of the lobby level takes 177-185 s on a quiet machine, so 300 s left under
        # 50 s of headroom and a loaded machine failed the step (#792).
        self.assertEqual(scenario["steps"][0]["wait_log"]["timeout"], 600)
        self.assertIn("control listening", scenario["steps"][1]["wait_log"]["pattern"])
        self.assertEqual(scenario["steps"][2]["fire"]["action"], "refresh_friends")
        self.assertIn("slot=45 name=RefreshFriends", scenario["steps"][3]["expect_log"]["pattern"])

    def test_a_step_with_two_kinds_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "bad.yaml"
            path.write_text("name: bad\nsteps:\n  - name: two\n    wait_log: {pattern: a}\n    fire: {action: x}\n")
            with self.assertRaises(ValueError):
                run_scenario.load_scenario(path)

    def test_local_server_keys_are_rejected(self):
        for key in ("server: local", "peers: [1]", "friends: [1]", "met: [1]"):
            with self.subTest(key=key), tempfile.TemporaryDirectory() as tmp:
                path = pathlib.Path(tmp) / "local.yaml"
                path.write_text(f"name: x\n{key}\nsteps:\n  - name: a\n    wait_log: {{pattern: a}}\n")
                with self.assertRaisesRegex(ValueError, "not supported"):
                    run_scenario.load_scenario(path)


class ControlPatienceTest(unittest.TestCase):
    """A fire's reply waits for the game's next frame, and one frame can last many seconds while the
    client loads the social lobby. The old 10 s socket timeout cut a reply the endpoint would still
    have sent (#674)."""

    def test_a_reply_slower_than_ten_seconds_is_still_read(self):
        import socket
        import threading
        import time
        import control

        server = socket.socket()
        server.bind(("127.0.0.1", 0))
        server.listen(1)
        port = server.getsockname()[1]

        def serve():
            conn, _ = server.accept()
            with conn:
                conn.recv(4096)
                time.sleep(11)
                conn.sendall(b'{"ok": true}\n')
            server.close()

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        try:
            self.assertEqual(control.call(port, {"op": "state"}), {"ok": True})
        finally:
            thread.join(5)

    def test_the_runner_s_control_socket_outlasts_a_long_frame(self):
        import socket
        import threading
        import time

        server = socket.socket()
        server.bind(("127.0.0.1", 0))
        server.listen(1)
        port = server.getsockname()[1]

        def serve():
            conn, _ = server.accept()
            with conn:
                conn.recv(4096)
                time.sleep(11)
                conn.sendall(b'{"ok": true}\n')
            server.close()

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        client = run_scenario.Control(port)
        try:
            self.assertEqual(client.call({"op": "state"}), {"ok": True})
        finally:
            client.close()
            thread.join(5)


class TeardownRaceTest(unittest.TestCase):
    """A client that outlives its Wine server makes the next launch refuse to start (launch-client.sh
    exit 4), which cost the invite scenario in suite 20261018T061805 (#674)."""

    def fake_bin(self, tmp, pgrep_body: str):
        import os
        import pathlib
        bin_dir = pathlib.Path(tmp) / "bin"
        bin_dir.mkdir()
        (bin_dir / "pgrep").write_text("#!/bin/bash\n" + pgrep_body)
        kill_log = pathlib.Path(tmp) / "killed"
        (bin_dir / "kill").write_text(f'#!/bin/bash\necho "$@" >> "{kill_log}"\n')
        (bin_dir / "wineserver").write_text("#!/bin/bash\nexit 0\n")
        for f in bin_dir.iterdir():
            f.chmod(0o755)
        old = os.environ["PATH"]
        os.environ["PATH"] = f"{bin_dir}:{old}"
        self.addCleanup(lambda: os.environ.__setitem__("PATH", old))
        return kill_log

    def standin(self) -> int:
        """A live process whose argv[0] is ./echovr.exe: game_pids counts it on its command line (#1490)."""
        import pathlib
        import sys
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
        from test_game_pids import start
        proc = start(["bash", "-c", "exec -a ./echovr.exe sleep 600"], b"./echovr.exe\0")
        self.addCleanup(lambda: (proc.kill(), proc.wait()))
        return proc.pid

    def test_waits_for_a_client_that_goes_away_and_never_kills_it(self):
        import tempfile
        import time
        pid = self.standin()
        with tempfile.TemporaryDirectory() as tmp:
            state = f"{tmp}/count"
            kill_log = self.fake_bin(
                tmp, f'n=$(cat "{state}" 2>/dev/null || echo 0); echo $((n+1)) > "{state}"\n'
                     f'if [ "$n" -lt 1 ]; then echo {pid}; fi\n')
            started = time.monotonic()
            self.assertTrue(run_scenario.wait_for_game_exit(timeout=10))
            self.assertLess(time.monotonic() - started, 5)
            self.assertFalse(kill_log.exists())

    def test_teardown_waits_for_the_client_after_the_wine_server(self):
        import os
        import pathlib
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            state = f"{tmp}/count"
            self.fake_bin(tmp, f'n=$(cat "{state}" 2>/dev/null || echo 0); echo $((n+1)) > "{state}"\n'
                              'if [ "$n" -eq 0 ]; then echo 4242; fi\n')
            root = pathlib.Path(tmp) / "game"
            (root / "echovr/bin/win10").mkdir(parents=True)
            old = {k: os.environ.get(k) for k in ("NEVR_GAME_ROOT",)}
            os.environ["NEVR_GAME_ROOT"] = str(root)
            run_scenario.wineprefix.cache_clear()
            try:
                run = run_scenario.Run({"name": "teardown"}, pathlib.Path("/nonexistent.dll"), pathlib.Path(tmp))
                run.teardown()
            finally:
                for k, v in old.items():
                    if v is None:
                        os.environ.pop(k, None)
                    else:
                        os.environ[k] = v
                run_scenario.wineprefix.cache_clear()
            # pgrep ran during teardown, i.e. it waited for the client after the Wine server.
            self.assertTrue(pathlib.Path(state).exists(), "teardown never looked for a live client")
            self.assertGreaterEqual(int(pathlib.Path(state).read_text().strip()), 1)

    def test_kills_a_client_that_never_goes_away(self):
        import tempfile
        pid = self.standin()
        with tempfile.TemporaryDirectory() as tmp:
            kill_log = self.fake_bin(tmp, f"echo {pid}\n")
            self.assertFalse(run_scenario.wait_for_game_exit(timeout=0.1))
            self.assertIn(str(pid), kill_log.read_text())


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

    def test_a_client_that_lost_its_lobby_ends_the_wait(self):
        """Suite 20261018T065159 hung 38 minutes on this line: the client was dropped by its lobby
        host and the first step waited without a timeout (#674)."""
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "console.log"
            path.write_text("2026-10-18T12:16:27.680Z error [EVR] [NETGAME] Disconnected from server "
                            "due to timeout\n")
            log = run_scenario.ConsoleLog(path)
            # A bounded wait: without the pattern this test would hang forever, not fail.
            with self.assertRaises(run_scenario.StepFailed) as caught:
                log.wait("to in game", 0.2)
            self.assertIn("Disconnected from server due to timeout", str(caught.exception))

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


CONSOLE = (
    "\x1b[90m2026-10-22T09:21:01.746Z\x1b[0m \x1b[0minfo\x1b[0m [EVR] [NETGAME] "
    "NetGame switching state (from logged out, to loading root)\n"
    "noise line without a state\n"
    "\x1b[90m2026-10-22T09:21:05.782Z\x1b[0m \x1b[0minfo\x1b[0m [EVR] [NETGAME] "
    "NetGame switching state (from loading root, to logged in)\n"
    "\x1b[90m2026-10-22T09:25:15.532Z\x1b[0m \x1b[0minfo\x1b[0m [EVR] [NETGAME] "
    "NetGame switching state (from ready for game, to in game)\n"
)


class StateTimelineTest(unittest.TestCase):
    def test_every_state_change_is_listed_with_its_timestamp(self):
        self.assertEqual(run_scenario.state_timeline(CONSOLE), [
            {"from": "logged out", "to": "loading root", "ts": "2026-10-22T09:21:01.746Z"},
            {"from": "loading root", "to": "logged in", "ts": "2026-10-22T09:21:05.782Z"},
            {"from": "ready for game", "to": "in game", "ts": "2026-10-22T09:25:15.532Z"},
        ])

    def test_a_console_without_states_gives_an_empty_list(self):
        self.assertEqual(run_scenario.state_timeline("nothing here\n"), [])

    def test_a_state_line_without_a_timestamp_keeps_its_states_and_a_null_ts(self):
        self.assertEqual(run_scenario.state_timeline("NetGame switching state (from a, to b)\n"),
                         [{"from": "a", "to": "b", "ts": None}])


class BuildReportTest(unittest.TestCase):
    def test_the_report_carries_the_load_at_start_and_the_state_timeline(self):
        report = run_scenario.build_report("smoke", pathlib.Path("/x/BugSplat64.dll"), True, True,
                                           "1.00 2.00 3.00", CONSOLE, [{"step": "s", "result": "PASS"}])
        self.assertEqual(report["loadavg_start"], "1.00 2.00 3.00")
        self.assertEqual([s["to"] for s in report["states"]], ["loading root", "logged in", "in game"])
        self.assertEqual((report["scenario"], report["passed"], report["dll_restored"]), ("smoke", True, True))
        self.assertEqual(report["steps"], [{"step": "s", "result": "PASS"}])


class LoadavgTest(unittest.TestCase):
    def test_reads_the_three_averages_from_the_given_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "loadavg"
            path.write_text("11.20 63.69 143.84 3/4779 777090\n")
            self.assertEqual(run_scenario.read_loadavg(path), "11.20 63.69 143.84")

    def test_an_unreadable_file_gives_none_not_an_exception(self):
        self.assertIsNone(run_scenario.read_loadavg(pathlib.Path("/nonexistent/loadavg")))
