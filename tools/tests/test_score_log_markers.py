#!/usr/bin/env python3
"""Regression tests for current-format smoke-log markers."""

import os
import pathlib
import subprocess
import tempfile
import unittest


REPO = pathlib.Path(__file__).resolve().parents[2]
SCORE_LOG = REPO / "tests" / "smoke" / "score-log.sh"
SCRATCH_ROOT = pathlib.Path("/var/tmp/work-nevr-runtime")
SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)


def score(log: str, group: str = "server") -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory(prefix="score-log-markers-", dir=SCRATCH_ROOT) as temp_dir:
        temp = pathlib.Path(temp_dir)
        log_path = temp / "captured.log"
        log_path.write_text(log, encoding="utf-8")
        environment = os.environ.copy()
        environment["TMPDIR"] = str(temp)
        return subprocess.run(
            [str(SCORE_LOG), str(log_path), group],
            cwd=REPO,
            env=environment,
            capture_output=True,
            check=False,
            text=True,
        )


def row(output: str, row_id: str) -> str:
    return next(line for line in output.splitlines() if line.split()[:1] == [row_id])


class CurrentSmokeMarkerTest(unittest.TestCase):
    def test_current_boot_redirect_and_login_success_markers_score(self):
        log = (
            "[NEVR.PATCH] boot hooks installed ok=TrUe\n"
            "[NEVR.PATCH] service redirect key=auth from=source to=target\n"
            "[NEVR.WS] LOGIN SUCCESS\n"
            "[NEVR.HEADLESS] engine flags 0x1 -> 0x0 (bit0_render=CLEAR(HEADLESS))\n"
            "[NEVR.PATCH] hook FAILED name=LoadLibraryW reason=MH_ERROR_ALREADY_CREATED\n"
        )
        result = score(log, group="all")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS", row(result.stdout, "B03"))
        self.assertIn("PASS", row(result.stdout, "S15"))
        self.assertIn("PASS", row(result.stdout, "S20"))
        self.assertIn("PASS", row(result.stdout, "S44"))

    def test_false_boot_marker_and_login_failure_override_success(self):
        log = (
            "[NEVR.PATCH] boot hooks installed ok=true\n"
            "[NEVR.PATCH] boot hooks installed ok=FALSE\n"
            "[NEVR.WS] LOGIN SUCCESS\n"
            "[NEVR.WS] login failed status=400\n"
            "[NEVR.HEADLESS] engine flags 0x1 -> 0x0 (bit0_render=CLEAR(HEADLESS))\n"
        )
        result = score(log, group="all")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("FAIL", row(result.stdout, "B03"))
        self.assertIn("FAIL", row(result.stdout, "S20"))

    def test_missing_login_marker_remains_absent(self):
        result = score("[NEVR.PATCH] boot hooks installed ok=true\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("ABSENT", row(result.stdout, "S20"))

    def test_client_auth_and_http_success_markers_still_score(self):
        log = (
            "[NEVR.AUTH] Configured: url=https://service.example/auth\n"
            "[NEVR.AUTH] Token refreshed successfully expires_in=3600s\n"
            "[NEVR.HTTP] Response: 200 (32 bytes)\n"
        )
        result = score(log, group="client")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS", row(result.stdout, "C03"))
        self.assertIn("PASS", row(result.stdout, "C08"))
        self.assertIn("PASS", row(result.stdout, "C11"))

    def test_explicit_auth_http_failures_override_success_markers(self):
        log = (
            "[NEVR.AUTH] Configured: url=https://service.example/auth\n"
            "[NEVR.AUTH] Missing nevr_http_uri or nevr_http_key\n"
            "[NEVR.AUTH] Token refreshed successfully expires_in=3600s\n"
            "[NEVR.AUTH] token refresh failed (1 consecutive attempt) — will retry in 60s\n"
            "[NEVR.HTTP] Response: 200 (32 bytes)\n"
            "[NEVR.HTTP] curl failed: url=https://service.example curl_code=28\n"
        )
        result = score(log, group="client")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("FAIL", row(result.stdout, "C03"))
        self.assertIn("FAIL", row(result.stdout, "C08"))
        self.assertIn("FAIL", row(result.stdout, "C11"))

    def test_telemetry_transport_failure_marker_is_retained(self):
        log = (
            "[NEVR.TELEMETRY] Connected to telemetry server reconnect_count=0\n"
            "[NEVR.TELEMETRY] Connection error: http_status=502 retries=1 reconnect_count=1\n"
        )
        result = score(log, group="server")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("FAIL", row(result.stdout, "S30"))

    def test_status_form_real_hook_failure_overrides_boot_success(self):
        log = (
            "[NEVR.PATCH] boot hooks installed ok=true\n"
            "[NEVR.PATCH] hook failed name=NewHook status=MH_ERROR_UNSUPPORTED_FUNCTION\n"
            "[NEVR.HEADLESS] engine flags 0x1 -> 0x0 (bit0_render=CLEAR(HEADLESS))\n"
        )
        result = score(log, group="all")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("FAIL", row(result.stdout, "S44"))

    def test_msxml6_hook_row_passes_when_installed_and_fails_when_not(self):
        installed = ("[NEVR.MODULE] platform_compat initialized: 3/3 hooks installed "
                     "(tls=ok createdir=ok msxml6=ok)\n")
        result = score(installed, group="all")
        self.assertIn("PASS", row(result.stdout, "M04"))
        missing = ("[NEVR.MODULE] platform_compat initialized: 2/3 hooks installed "
                   "(tls=ok createdir=ok msxml6=FAILED)\n"
                   "[NEVR.MODULE] MSXML6 pass-through hook NOT installed \u2014 requests still reach the system "
                   "XMLHTTP object, but the pass-through line will not be logged\n")
        result = score(missing, group="all")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("FAIL", row(result.stdout, "M04"))


if __name__ == "__main__":
    unittest.main()
