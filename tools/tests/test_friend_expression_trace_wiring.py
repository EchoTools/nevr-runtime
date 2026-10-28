"""#775: the PC client traces every evaluation of the tablet's R15NetIsFriendExpression.

The tablet shows ADD FRIEND only when the expression's output is false, and nothing else on that path logs, so a
missing button could not be told from a hidden one. Source-shape sensors for the trace in
patch/party_invite_gate.cpp (no test target links it):

  - the hook is installed through the prologue-checked installer on the function's address;
  - the hook calls the original first and reads its output block only when the original had an input getter;
  - the trace carries the verdict, the roster index it matched and the roster's size.
"""

import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]


def gate() -> str:
    return (ROOT / "src/runtime/patch/party_invite_gate.cpp").read_text()


class IsFriendExpressionTrace(unittest.TestCase):
    def test_install_hooks_the_expression_with_a_checked_prologue(self):
        body = extract_braced_function(gate(), "void Install(std::uintptr_t gameBase)")
        self.assertIn(
            "InstallChecked(gameBase, kIsFriendExpressionVA, kIsFriendExpressionPrologue, g_originalIsFriendExpression",
            " ".join(body.split()),
        )
        self.assertIn("0x140D98950", strip_comments(gate()))

    def test_the_hook_runs_the_original_before_it_reads_the_output(self):
        body = extract_braced_function(gate(), "void IsFriendExpressionHook(")
        self.assertLess(
            body.index("g_originalIsFriendExpression(node, out, thread, getter)"), body.index("out[1]")
        )
        self.assertIn("getter == nullptr", body)

    def test_the_trace_reports_the_verdict_the_matched_index_and_the_roster_size(self):
        body = extract_braced_function(gate(), "void IsFriendExpressionHook(")
        for needle in ("friend=%d", "index=%d", "roster_count=%u", "matched_account=%llu", "[NEVR.PARTY]"):
            self.assertIn(needle, body)


if __name__ == "__main__":
    unittest.main()
