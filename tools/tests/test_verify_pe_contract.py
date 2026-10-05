#!/usr/bin/env python3
"""Regression tests for tools/verify_pe_contract.py (BugSplat64.dll PE surface)."""

import importlib.util
import pathlib
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
_SPEC = importlib.util.spec_from_file_location(
    "verify_pe_contract", REPO / "tools" / "verify_pe_contract.py")
contract = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(contract)


def host_dump(exports=None, ordinal_one=contract.ORDINAL_ONE, import_dlls=("KERNEL32.dll",)):
    """Render objdump -p text shaped like binutils 2.47 output for a DLL."""
    names = sorted(contract.EXPECTED_EXPORTS if exports is None else exports)
    rows = []
    next_ordinal = 2
    for hint, name in enumerate(names):
        if name == ordinal_one:
            ordinal = 1
        else:
            ordinal = next_ordinal
            next_ordinal += 1
        rows.append(f"\t[{ordinal - 1:4d}] +base[{ordinal:4d}]  {hint:04x} {name}")
    imports = []
    for dll in import_dlls:
        imports += [f"\tDLL Name: {dll}",
                    "\tvma:     Hint/Ord Member-Name Bound-To",
                    "\t1f5e000   1234  CreateFileA",
                    ""]
    return "\n".join(
        ["The Import Tables (interpreted .idata section contents)", ""] + imports +
        ["The Export Tables (interpreted .edata section contents)", "",
         "[Ordinal/Name Pointer] Table -- Ordinal Base 1",
         "\t          Ordinal   Hint Name"] + rows + [""])


def game_dump(rows=None):
    if rows is None:
        rows = [f"\t016c30{index:02x}  <none>  00{index:02x}  {name}"
                for index, name in enumerate(sorted(contract.GAME_IMPORTS))]
    return "\n".join(["\tDLL Name: BugSplat64.dll",
                      "\tvma:     Ordinal  Hint  Member-Name  Bound-To"] + rows + [""])


class HostContractTest(unittest.TestCase):
    def test_current_surface_passes(self):
        exports = contract.check_host(host_dump())
        self.assertEqual(len(exports), 26)
        self.assertEqual(exports[contract.ORDINAL_ONE], 1)

    def test_missing_export_fails(self):
        surface = set(contract.EXPECTED_EXPORTS) - {"NEVR_SetGameModule"}
        with self.assertRaisesRegex(contract.ContractError, "missing=.*NEVR_SetGameModule"):
            contract.check_host(host_dump(exports=surface))

    def test_extra_export_fails(self):
        surface = set(contract.EXPECTED_EXPORTS) | {"MH_CreateHook"}
        with self.assertRaisesRegex(contract.ContractError, "extra=.*MH_CreateHook"):
            contract.check_host(host_dump(exports=surface))

    def test_placeholder_off_ordinal_one_fails(self):
        with self.assertRaisesRegex(contract.ContractError,
                                    "DetoursExportPlaceholder is ordinal [0-9]+, expected 1"):
            contract.check_host(host_dump(ordinal_one="MiniDumpWriteDump"))

    def test_eager_runtime_import_fails(self):
        with self.assertRaisesRegex(contract.ContractError, "statically imports nevr.dll"):
            contract.check_host(host_dump(import_dlls=("KERNEL32.dll", "NEVR.DLL")))

    def test_missing_export_table_fails_closed(self):
        with self.assertRaisesRegex(contract.ContractError, "no \\[Ordinal/Name Pointer\\]"):
            contract.check_host("The Import Tables\n\tDLL Name: KERNEL32.dll\n")

    def test_empty_export_table_fails_closed(self):
        text = "[Ordinal/Name Pointer] Table -- Ordinal Base 1\n\t   Ordinal   Hint Name\n\n"
        with self.assertRaisesRegex(contract.ContractError, "zero rows"):
            contract.check_host(text)

    def test_unknown_export_row_format_fails_closed(self):
        text = host_dump().replace("+base[", "+BASE[", 1)
        with self.assertRaisesRegex(contract.ContractError, "unparseable export row"):
            contract.check_host(text)

    def test_missing_import_table_fails_closed(self):
        with self.assertRaisesRegex(contract.ContractError, "zero DLLs"):
            contract.check_host(host_dump(import_dlls=()))


class GameContractTest(unittest.TestCase):
    def setUp(self):
        self.exports = contract.check_host(host_dump())

    def test_pinned_named_imports_pass(self):
        self.assertEqual(contract.check_game(game_dump(), self.exports), contract.GAME_IMPORTS)

    def test_ordinal_import_fails(self):
        rows = game_dump().splitlines()[2:-1] + ["\t016c30c8     12  <none>"]
        with self.assertRaisesRegex(contract.ContractError, "non-by-name"):
            contract.check_game(game_dump(rows=rows), self.exports)

    def test_import_not_exported_fails(self):
        exports = dict(self.exports)
        del exports["?setFlags@MiniDmpSender@@QEAA_NK@Z"]
        with self.assertRaisesRegex(contract.ContractError, "not exported by host"):
            contract.check_game(game_dump(), exports)

    def test_game_import_drift_fails(self):
        rows = game_dump().splitlines()[2:-1] + ["\t016c30c8  <none>  0030  ?newCall@MiniDmpSender@@QEAAXXZ"]
        with self.assertRaisesRegex(contract.ContractError, "drifted"):
            contract.check_game(game_dump(rows=rows), self.exports)

    def test_no_host_imports_fails(self):
        with self.assertRaisesRegex(contract.ContractError, "imports nothing"):
            contract.check_game("\tDLL Name: KERNEL32.dll\n\t1f5e000  <none>  0001  ExitProcess\n",
                                self.exports)


if __name__ == "__main__":
    unittest.main()
