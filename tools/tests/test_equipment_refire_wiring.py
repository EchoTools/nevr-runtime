"""The PC equipment re-fire (#2121, src/runtime/patch/equipment_refire.cpp): the parts a host test cannot reach without the game.

The decisions are tested in src/runtime/tests/test_equipment_refire.cpp. What stays here is the glue's own tables and its wiring, read as
source: each setter row pins the whole 100-byte body of the setter it hooks, and the row's slot, record field and handler must be what
those bytes say (a row whose bytes belong to another setter would hook the wrong function or re-fire the wrong handler); the runtime
installs the hooks once at boot and runs the due re-fires on the game loop's thread only.

Python because every test in tools/tests is Python and the repository has no Go tools module yet; the file reads source text and runs
nothing."""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import strip_comments

ROOT = Path(__file__).resolve().parents[2]
GLUE = ROOT / "src/runtime/patch/equipment_refire.cpp"
RULES = ROOT / "src/runtime/patch/equipment_refire_rules.h"

# slot enumerator -> the record field the setter compares and writes (the rules header's kRecordValueOffset)
FIELD = {"kWeapon": 0x18, "kTacMod": 0x20, "kOrdnance": 0x28}
SETTER_BODY_SIZE = 100
HANDLER_PROLOGUE_SIZE = 32


def rows():
    """The kRows initializers of the glue: (slot, name, setter va, handler va, setter body, handler prologue)."""
    text = strip_comments(GLUE.read_text())
    table = text[text.index("constexpr SetterRow kRows") : text.index("constexpr std::uint64_t kNetGameVa")]
    pattern = re.compile(
        r'\{Slot::(\w+),\s*"([^"]+)",\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*\{([^}]*)\},\s*\{([^}]*)\}\}', re.S
    )
    out = []
    for m in pattern.finditer(table):
        body = bytes(int(b, 16) for b in re.findall(r"0x[0-9a-fA-F]{2}", m.group(5)))
        handler = bytes(int(b, 16) for b in re.findall(r"0x[0-9a-fA-F]{2}", m.group(6)))
        out.append((m.group(1), m.group(2), int(m.group(3), 16), int(m.group(4), 16), body, handler))
    return out


class SetterRows(unittest.TestCase):
    def test_there_is_one_row_per_slot_in_the_rules_headers_slot_order(self):
        self.assertEqual([r[0] for r in rows()], ["kWeapon", "kTacMod", "kOrdnance"])

    def test_the_arrays_have_the_sizes_the_glue_declares(self):
        for slot, _name, _setter, _handler, body, handler in rows():
            self.assertEqual(len(body), SETTER_BODY_SIZE, slot)
            self.assertEqual(len(handler), HANDLER_PROLOGUE_SIZE, slot)

    def test_each_setter_compares_and_writes_the_field_of_its_slot(self):
        for slot, name, _setter, _handler, body, _handler_bytes in rows():
            want = FIELD[slot]
            # CMP [RDI+disp8],R8 = 4c 39 47 dd; MOV [RDI+disp8],R8 = 4c 89 47 dd
            cmp_at = body.find(bytes([0x4C, 0x39, 0x47]))
            mov_at = body.find(bytes([0x4C, 0x89, 0x47]))
            self.assertGreaterEqual(cmp_at, 0, name)
            self.assertGreaterEqual(mov_at, 0, name)
            self.assertEqual(body[cmp_at + 3], want, f"{name}: the compared field")
            self.assertEqual(body[mov_at + 3], want, f"{name}: the written field")

    def test_each_setter_calls_the_handler_the_row_names(self):
        for slot, name, setter, handler, body, _handler_bytes in rows():
            call_at = body.find(b"\xe8")
            self.assertGreaterEqual(call_at, 0, name)
            rel = int.from_bytes(body[call_at + 1 : call_at + 5], "little", signed=True)
            self.assertEqual(setter + call_at + 5 + rel, handler, f"{name}: the first call is the change handler")

    def test_the_setter_addresses_are_distinct_and_the_handlers_too(self):
        r = rows()
        self.assertEqual(len({x[2] for x in r}), 3)
        self.assertEqual(len({x[3] for x in r}), 3)

    def test_the_fields_the_rules_header_reads_are_the_same_three(self):
        text = strip_comments(RULES.read_text())
        m = re.search(r"kRecordValueOffset\[3\]\s*=\s*\{([^}]*)\}", text)
        self.assertIsNotNone(m)
        self.assertEqual([int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]+", m.group(1))], [FIELD["kWeapon"], FIELD["kTacMod"], FIELD["kOrdnance"]])


class Wiring(unittest.TestCase):
    def test_boot_installs_the_hooks_once_after_the_hooking_engine_is_up(self):
        text = strip_comments((ROOT / "src/runtime/lifecycle/initialize.cpp").read_text())
        calls = re.findall(r"nevr_equipment_refire::Install\(", text)
        self.assertEqual(len(calls), 1)
        self.assertLess(text.index("minhook initialized"), text.index("nevr_equipment_refire::Install("))

    def test_the_due_refires_run_on_the_game_loops_thread_only(self):
        text = strip_comments((ROOT / "src/runtime/frame/tick.cpp").read_text())
        m = re.search(r"if \(loop != 0 && loop == GetCurrentThreadId\(\)\) nevr_equipment_refire::OnFrame\(\);", text)
        self.assertIsNotNone(m, "OnFrame must be called only where loop == GetCurrentThreadId()")
        self.assertEqual(len(re.findall(r"nevr_equipment_refire::OnFrame\(", text)), 1)

    def test_the_source_is_in_the_runtime_dll(self):
        text = strip_comments((ROOT / "src/runtime/CMakeLists.txt").read_text())
        self.assertIn('"patch/equipment_refire.cpp"', text)

    def test_a_server_never_tracks_and_the_hook_reads_the_mode_at_call_time(self):
        # g_isServer is not set when initialize.cpp installs (N131), so the glue reads it in the hook, not in Install.
        text = strip_comments(GLUE.read_text())
        hook = text[text.index("void SetterHook") : text.index("const char* StaleReason")]
        self.assertIn("g_isServer != FALSE", hook)
        install = text[text.index("void Install(") : text.index("void OnFrame()")]
        self.assertNotIn("g_isServer", install)

    def test_the_net_game_getters_argument_is_checked_before_the_game_reads_it(self):
        text = strip_comments(GLUE.read_text())
        disp = re.search(r"kNetGameFieldOffset = (0x[0-9a-fA-F]+);", text)
        body = re.search(r"kNetGamePrologue = \{([^}]*)\}", text)
        self.assertIsNotNone(disp)
        self.assertIsNotNone(body)
        raw = bytes(int(b, 16) for b in re.findall(r"0x[0-9a-fA-F]{2}", body.group(1)))
        # MOV RAX,[RCX+disp32]; RET
        self.assertEqual(raw[:3], bytes([0x48, 0x8B, 0x81]))
        self.assertEqual(int.from_bytes(raw[3:7], "little"), int(disp.group(1), 16))
        local = text[text.index("bool IsLocalRecord") : text.index("void ValueText")]
        self.assertLess(local.index("kNetGameFieldOffset"), local.index("g_netGame("))

    def test_the_handler_is_called_for_the_slot_that_was_due(self):
        text = strip_comments(GLUE.read_text())
        self.assertIn("g_handler[slotIndex](reinterpret_cast<void*>(r.owner), r.handle);", text)

    def test_the_original_runs_before_the_change_is_judged(self):
        text = strip_comments(GLUE.read_text())
        hook = text[text.index("void SetterHook") : text.index("const char* StaleReason")]
        self.assertLess(hook.index("RecordValue("), hook.index("g_original[I]("))
        self.assertLess(hook.index("g_original[I]("), hook.index("IsLocalRecord("))
        self.assertLess(hook.index("IsLocalRecord("), hook.index("g_debouncer.Changed("))


if __name__ == "__main__":
    unittest.main()
