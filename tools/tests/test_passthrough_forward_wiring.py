"""#548: both passthrough sites in builtin_filter.cpp ask the one forward predicate.

The predicate itself is tested on the host (runtime/tests/test_passthrough_forward.cpp). This checks that the filter
calls it at both sites, and that no site still guards on a bare `varargs != nullptr`, which dropped every line whose
format has no conversion.
"""

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
FILTER = ROOT / "src" / "runtime" / "log" / "builtin_filter.cpp"
CALL = "nevr_passthrough_forward::CanForward(fmt, varargs)"
BARE_GUARD = "varargs != nullptr"


class PassthroughForwardWiringTest(unittest.TestCase):
    def test_both_passthrough_sites_call_the_predicate(self):
        src = FILTER.read_text(encoding="utf-8")
        self.assertEqual(
            src.count(CALL),
            2,
            "builtin_filter.cpp must ask CanForward at both passthrough sites (#548)",
        )

    def test_no_site_guards_on_a_bare_null_varargs_check(self):
        src = FILTER.read_text(encoding="utf-8")
        self.assertNotIn(
            BARE_GUARD,
            src,
            "a bare varargs != nullptr guard drops lines with no conversion (#548)",
        )


if __name__ == "__main__":
    unittest.main()
