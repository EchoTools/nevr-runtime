#!/usr/bin/env python3
"""Keep the S7d sample-config sensor independent of local setup state."""

import pathlib
import unittest


REPO = pathlib.Path(__file__).resolve().parents[2]


class SampleConfigSensorTest(unittest.TestCase):
    def test_sample_config_sensor_uses_tracked_fixture_not_ignored_local_state(self):
        justfile = (REPO / "justfile").read_text(encoding="utf-8")
        sensor = justfile.split("# S7d — the tracked sample config", 1)[1].split("# N112", 1)[0]
        executable = "\n".join(
            line for line in sensor.splitlines() if not line.lstrip().startswith("#")
        )

        self.assertIn("docs/reference/example-config.yaml", executable)
        self.assertNotIn("echovr/_local/config.yaml", executable)

        sample = (REPO / "docs/reference/example-config.yaml").read_text(encoding="utf-8")
        for section in ("auth:", "services:", "identity:", "version:"):
            with self.subTest(section=section):
                self.assertIn(section, sample)


if __name__ == "__main__":
    unittest.main()
