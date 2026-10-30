import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function, strip_comments


ROOT = Path(__file__).resolve().parents[2]

# The social facade, the party invite gate and the websocket bridge: the sites #1292 names. Each Info record in
# them is one line at the default level, so a pointer value there is noise (docs/standards/logging.md, "You SHALL
# NOT": pointer values at INFO). The repo-wide sweep is not this check's job; it covers these files only.
FILES = (
    "src/runtime/patch/social_facade_object.cpp",
    "src/runtime/patch/social_facade.cpp",
    "src/runtime/patch/party_invite_gate.cpp",
    "src/runtime/compat/ws_bridge.cpp",
)

CALL = re.compile(r"(?<![A-Za-z0-9_])Log\(")
ADJACENT_LITERALS = re.compile(r'\s*(?:"(?:[^"\\]|\\.)*"\s*)+')
LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')
POINTER = re.compile(r"%[-+ #0-9.]*p(?![A-Za-z])")


def info_formats(source: str):
    """Yield (line, format) for each Log() call whose level may be Info, from comment-stripped source."""
    for match in CALL.finditer(source):
        comma = source.find(",", match.end())
        level = source[match.end():comma]
        if "Info" not in level:
            continue
        literals = ADJACENT_LITERALS.match(source, comma + 1)
        line = source.count("\n", 0, match.start()) + 1
        if literals is None:
            yield line, None
            continue
        yield line, "".join(LITERAL.findall(literals.group(0)))


class InfoRecordsCarryNoPointers(unittest.TestCase):
    def test_no_info_format_in_the_named_files_takes_a_pointer(self):
        offenders = []
        for rel in FILES:
            source = strip_comments((ROOT / rel).read_text(errors="replace"))
            found = list(info_formats(source))
            self.assertTrue(found, f"{rel}: no Info Log() call found; the check is watching nothing")
            for line, fmt in found:
                if fmt is None:
                    offenders.append(f"{rel}:{line} Info Log() without a literal format; the check cannot read it")
                elif POINTER.search(fmt):
                    offenders.append(f"{rel}:{line} Info format takes a pointer: {fmt[:80]!r}")
        self.assertEqual(offenders, [], "pointer values belong at Debug (#1292)")

    def test_the_line_a_pointer_check_must_catch_is_caught(self):
        # The check's own regex, on a planted Info line: it must find the pointer, and must not find one in a Debug line.
        planted = 'Log(EchoVR::LogLevel::Info, "x ws=%p", p);\nLog(EchoVR::LogLevel::Debug, "x ws=%p", p);'
        hits = [fmt for _, fmt in info_formats(planted) if fmt and POINTER.search(fmt)]
        self.assertEqual(hits, ["x ws=%p"])

    def test_log_frame_messages_is_debug_on_a_server_too(self):
        # #1292: the per-message lines are Debug in both run modes; the server-only Info branch and its comment are gone.
        source = strip_comments((ROOT / "src/runtime/compat/ws_bridge.cpp").read_text(errors="replace"))
        body = extract_braced_function(source, "static int LogFrameMessages(const char* direction")
        self.assertNotIn("LogLevel::Info", body)
        self.assertNotIn("g_isServer", body)


if __name__ == "__main__":
    unittest.main()
