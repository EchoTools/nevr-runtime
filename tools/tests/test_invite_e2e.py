"""tools/invite_e2e.py: the parts that read a client's log and judge the run (the run itself needs two live clients).

The log lines are the shapes the runtime writes: `[NEVR.PATCH] service redirect ...` (src/runtime/lifecycle/config.cpp),
`[NEVR.INVITE] ...` (src/runtime/patch/invite_*.h) and `[NEVR.WS] lobby session ...` (src/runtime/compat/lobby_frame.h)."""
from __future__ import annotations

import os
import pathlib
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import invite_e2e as e2e  # noqa: E402

LOG = "\n".join([
    "MESA-EGL: warning: DRI3 error",  # a line with no stamp
    "\x1b[90m2026-10-30T02:10:02.554Z\x1b[0m \x1b[0minfo\x1b[0m [NEVR.PATCH] service redirect key=config_host "
    "from=wss://config.readyatdawn.com/rad/rad15_live to=ws://127.0.0.1:49162/config",
    "2026-10-30T02:10:02.629Z info [NEVR.PATCH] HTTP(S) connection redirected: https://api.readyatdawn.com/ → https://g.example.com:7350/",
    "2026-10-30T02:10:03.000Z info [NEVR.PATCH] service redirect key=login_host from=wss://login.readyatdawn.com/x to=wss://login.readyatdawn.com/x",
    "2026-10-30T02:10:04.000Z info something fetched https://cdn.readyatdawn.com/asset.bin and went on",
    "2026-10-30T02:10:05.000Z info [NEVR.INVITE] answer op=resolve status=200 latency_ms=140 result=ok",
])


class ParseTest(unittest.TestCase):
    def test_a_line_loses_its_escapes_and_its_stamp_and_keeps_its_number(self):
        lines = e2e.parse_lines(LOG)
        self.assertEqual(lines[0].ts, None)
        self.assertEqual(lines[0].n, 1)
        self.assertEqual(lines[1].ts, "2026-10-30T02:10:02.554Z")
        self.assertTrue(lines[1].text.startswith("info [NEVR.PATCH] service redirect"))
        self.assertNotIn("\x1b", lines[1].text)

    def test_first_reads_only_after_the_mark(self):
        lines = e2e.parse_lines(LOG)
        self.assertEqual(e2e.first(lines, r"result=(\w+)")[1][1], "ok")
        self.assertIsNone(e2e.first(lines, r"result=(\w+)", after=lines[-1].n))

    def test_first_bad_line_finds_a_refused_answer_and_ignores_an_ok_one(self):
        ok = e2e.parse_lines(LOG)
        self.assertIsNone(e2e.first_bad_line(ok))
        bad = e2e.parse_lines(LOG + "\n2026-10-30T02:10:06.000Z info [NEVR.INVITE] answer op=resolve status=200 latency_ms=1 result=closed")
        self.assertEqual(e2e.first_bad_line(bad).n, ok[-1].n + 1)
        fatal = e2e.parse_lines(LOG + "\n2026-10-30T02:10:07.000Z error Lost connection to the login service")
        self.assertEqual(e2e.first_bad_line(fatal).n, ok[-1].n + 1)


class RadUrlTest(unittest.TestCase):
    def setUp(self):
        self.hits = e2e.find_rad_urls(e2e.parse_lines(LOG))

    def test_each_redirect_kind_is_classified_with_its_destination(self):
        service, http, *_ = self.hits
        self.assertEqual((service.kind, service.url, service.final, service.redirected),
                         ("service_redirect", "wss://config.readyatdawn.com/rad/rad15_live", "ws://127.0.0.1:49162/config", True))
        self.assertEqual((http.kind, http.url, http.redirected), ("http_redirect", "https://api.readyatdawn.com/", True))

    def test_a_redirect_to_a_readyatdawn_host_is_not_a_redirect(self):
        slipped = self.hits[2]
        self.assertEqual((slipped.kind, slipped.redirected), ("service_redirect", False))

    def test_a_line_that_only_names_the_host_is_not_redirected_and_names_its_call_site(self):
        other = self.hits[3]
        self.assertEqual((other.kind, other.url, other.redirected), ("other", "https://cdn.readyatdawn.com/asset.bin", False))
        self.assertIn("no redirect line", other.call_site)
        self.assertIn("config.cpp:629", self.hits[0].call_site)

    def test_a_line_without_the_domain_is_not_a_hit(self):
        self.assertEqual(len(self.hits), 4)

    def test_only_a_slip_after_the_first_invite_request_is_on_the_invite_path(self):
        rad = {"A": self.hits}
        self.assertEqual([h.url for _, h in e2e.slips_on_invite_path(rad, {"A": "2026-10-30T02:10:03.500Z"})],
                         ["https://cdn.readyatdawn.com/asset.bin"])
        self.assertEqual(e2e.slips_on_invite_path(rad, {"A": "2026-10-30T02:10:04.500Z"}), [])
        self.assertEqual(e2e.slips_on_invite_path(rad, {"A": None}), [])


class RenderTest(unittest.TestCase):
    def test_a_failed_gating_step_makes_the_verdict_fail_and_a_soft_one_does_not(self):
        line = e2e.parse_lines("2026-10-30T02:10:05.000Z info [NEVR.INVITE] answer op=create status=200 latency_ms=1 result=ok")[0]
        ok = e2e.Step("A creates", True, [("A", line, "")])
        soft = e2e.Step("profile", False, [("A", None, "no line")], gating=False)
        text = e2e.render([ok, soft], {}, {}, ["head"])
        self.assertIn("1. PASS  A creates", text)
        self.assertIn("A:console.log:1: 2026-10-30T02:10:05.000Z info [NEVR.INVITE] answer op=create", text)
        self.assertIn("2. info  profile", text)
        self.assertIn("none named in either client's log", text)
        self.assertTrue(text.endswith("VERDICT: PASS"))
        self.assertTrue(e2e.render([e2e.Step("x", False)], {}, {}, []).endswith("VERDICT: FAIL"))

    def test_a_url_that_slipped_is_listed_with_its_log_line_and_call_site(self):
        hits = e2e.find_rad_urls(e2e.parse_lines(LOG))
        text = e2e.render([], {"A": hits}, {"A": "2026-10-30T02:10:03.500Z"}, [])
        self.assertIn("A NOT REDIRECTED (invite path): https://cdn.readyatdawn.com/asset.bin", text)
        self.assertIn("A redirected: wss://config.readyatdawn.com/rad/rad15_live -> ws://127.0.0.1:49162/config", text)
        self.assertIn("[A:console.log:5;", text)


class AccountsTest(unittest.TestCase):
    def test_an_account_the_readme_bars_is_not_used(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            for name in ("1111111111", "2222222222", "3333333333"):
                (root / name).mkdir()
                (root / name / ".credentials.json").write_text("{}")
            (root / "4444444444").mkdir()  # no sign-in in it
            (root / "README.md").write_text("- 2222222222/: someone's OWN account. Do NOT use it for test runs.\n- 1111111111/: fine\n")
            self.assertEqual(e2e.stored_accounts(root), ["1111111111", "3333333333"])


class MakeSafeTest(unittest.TestCase):
    def test_a_copy_loses_config_plugins_sign_ins_and_links_into_the_install(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            src = tmp / "install"
            (src / "bin/win10").mkdir(parents=True)
            copy = tmp / "copy"
            echovr = copy / "echovr"
            nevr = echovr / ".wineprefix/drive_c/users/u/AppData/Local/nevr/echovr"
            for d in (echovr / "_local", echovr / "bin/win10/plugins", nevr / "plugins"):
                d.mkdir(parents=True)
            (echovr / "_local/config.yaml").write_text("x")
            (echovr / "_local/config.json").write_text("x")
            (echovr / "_local/.credentials.json").write_text("secret")
            (nevr / ".credentials.json.bak").write_text("secret")
            (echovr / "bin/win10/plugins/p.dll").write_text("x")
            (nevr / "plugins/q.dll").write_text("x")
            (echovr / "bin/win10/keep.dll").write_text("keep")
            os.symlink(src / "bin", echovr / "into-install")
            os.symlink("/", echovr / "to-root")
            e2e.make_safe(copy, src)
            self.assertFalse(list(echovr.rglob(".credentials.json*")))
            self.assertFalse((echovr / "_local/config.yaml").exists() or (echovr / "_local/config.json").exists())
            self.assertEqual(list((echovr / "bin/win10/plugins").iterdir()), [])
            self.assertEqual(list((nevr / "plugins").iterdir()), [])
            self.assertFalse((echovr / "into-install").is_symlink())
            self.assertTrue((echovr / "to-root").is_symlink(), "a link that leaves the install elsewhere stays (AGENTS.md lists it)")
            self.assertEqual((echovr / "bin/win10/keep.dll").read_text(), "keep")

    def test_a_copy_is_removed_only_when_it_is_one_this_run_made(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = pathlib.Path(tmp)
            (run / "copy-A").mkdir()
            (run / "evidence").mkdir()
            e2e.remove_copy(run / "copy-A", run)
            self.assertFalse((run / "copy-A").exists())
            with self.assertRaises(SystemExit):
                e2e.remove_copy(run / "evidence", run)
            self.assertTrue((run / "evidence").exists())
            with self.assertRaises(SystemExit):
                e2e.remove_copy(run.parent, run)


if __name__ == "__main__":
    unittest.main()
