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


def host_dump(exports=None, import_dlls=("KERNEL32.dll",)):
    """Render objdump -p text shaped like binutils output for a DLL.

    `exports` is a {name: ordinal} map; the default is the pinned surface."""
    table = dict(contract.EXPECTED_EXPORTS if exports is None else exports)
    rows = [f"\t[{ordinal - 1:4d}] +base[{ordinal:4d}]  {hint:04x} {name}"
            for hint, (name, ordinal) in enumerate(sorted(table.items()))]
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
        self.assertEqual(len(exports), 28)
        self.assertEqual(exports[contract.ORDINAL_ONE], 1)
        self.assertEqual(exports, contract.EXPECTED_EXPORTS)

    def test_every_name_in_the_hosts_exports_def_is_pinned_and_every_pin_is_declared(self):
        # The host's exports.def names all 28 exports: a name added without a pin is surface nobody reviewed (#884
        # added two and the pin stayed at 26), and a pin without a declaration is a name the DLL would not export.
        text = (REPO / "src" / "runtime" / "host" / "exports.def").read_text(encoding="utf-8")
        lines = [line.split(";", 1)[0].strip() for line in text.splitlines()]
        self.assertIn("EXPORTS", lines, "exports.def has no EXPORTS line")
        declared = set()
        for line in lines[lines.index("EXPORTS") + 1:]:
            if line:
                declared.add(line.split("=", 1)[0].split()[0])
        self.assertEqual(len(declared), 28, "exports.def parsed to other than 28 names")
        self.assertEqual(sorted(declared - set(contract.EXPECTED_EXPORTS)), [])
        self.assertEqual(sorted(set(contract.EXPECTED_EXPORTS) - declared), [])

    def test_pinned_ordinals_follow_the_linker_name_order(self):
        # Every ordinal but the placeholder's is the linker's: 2.. in byte order of the name.
        others = sorted(n for n in contract.EXPECTED_EXPORTS if n != contract.ORDINAL_ONE)
        self.assertEqual({n: i + 2 for i, n in enumerate(others)},
                         {n: o for n, o in contract.EXPECTED_EXPORTS.items() if n != contract.ORDINAL_ONE})

    def test_missing_export_fails(self):
        surface = {k: v for k, v in contract.EXPECTED_EXPORTS.items() if k != "NEVR_SetGameModule"}
        with self.assertRaisesRegex(contract.ContractError, "missing=.*NEVR_SetGameModule"):
            contract.check_host(host_dump(exports=surface))

    def test_extra_export_fails(self):
        surface = dict(contract.EXPECTED_EXPORTS)
        surface["MH_CreateHook"] = 29
        with self.assertRaisesRegex(contract.ContractError, "extra=.*MH_CreateHook"):
            contract.check_host(host_dump(exports=surface))

    def test_placeholder_off_ordinal_one_fails(self):
        surface = dict(contract.EXPECTED_EXPORTS)
        surface[contract.ORDINAL_ONE], surface["MiniDumpWriteDump"] = 11, 1
        with self.assertRaisesRegex(contract.ContractError,
                                    "DetoursExportPlaceholder is ordinal [0-9]+, expected 1"):
            contract.check_host(host_dump(exports=surface))

    def test_renumbered_export_fails(self):
        surface = dict(contract.EXPECTED_EXPORTS)
        surface["NEVR_GetUPnPConfig"], surface["NEVR_RegisterResourceOverride"] = 16, 14
        with self.assertRaisesRegex(contract.ContractError,
                                    "ordinals drifted.*NEVR_GetUPnPConfig: expected 14, found 16"):
            contract.check_host(host_dump(exports=surface))

    def test_eager_runtime_import_fails(self):
        with self.assertRaisesRegex(contract.ContractError, "statically imports nevr-runtime.dll"):
            contract.check_host(host_dump(import_dlls=("KERNEL32.dll", "NEVR-RUNTIME.DLL")))

    def test_the_runtime_dll_is_named_nevr_runtime_dll(self):
        self.assertEqual(contract.RUNTIME_DLL, "nevr-runtime.dll")

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



HOST_BASE = 0x180000000
RUNTIME_BASE = 0x181000000
COMMIT = "a" * 40


def identity(version="5.5.0-dev.4+abc1234", commit=COMMIT):
    return b"\x00NEVR-BUILD " + version.encode() + b" " + commit.encode() + b"\x00"


def pe_dump(exports, base, size, import_dlls=("KERNEL32.dll",)):
    """objdump -p text for a DLL with the given {name: ordinal} exports, ImageBase and SizeOfImage."""
    text = host_dump(exports=exports, import_dlls=import_dlls)
    header = f"ImageBase\t\t{base:016x}\nSizeOfImage\t\t{size:08x}\n"
    return header + text


def runtime_dump(exports=None, base=RUNTIME_BASE, size=0x1800000, import_dlls=("KERNEL32.dll", "WS2_32.dll")):
    return pe_dump({contract.RUNTIME_ENTRY: 1} if exports is None else exports, base, size, import_dlls)


def thin_host_dump(**kwargs):
    kwargs.setdefault("size", 0x400000)
    base = kwargs.pop("base", HOST_BASE)
    return pe_dump(contract.EXPECTED_EXPORTS, base, kwargs.pop("size"), **kwargs)


class RuntimeContractTest(unittest.TestCase):
    """ADR 0012 step 2: nevr-runtime.dll carries the rendezvous entry and the identity, apart from the host."""

    def test_current_surface_passes(self):
        info = contract.check_runtime(runtime_dump(), b"MZ" + identity())
        self.assertEqual(info["base"], RUNTIME_BASE)

    def test_the_rendezvous_entry_is_required(self):
        with self.assertRaisesRegex(contract.ContractError, "does not export NEVR_GetRuntimeApi"):
            contract.check_runtime(runtime_dump(exports={"SomethingElse": 1}), b"MZ" + identity())

    def test_the_runtime_does_not_export_what_the_game_imports(self):
        surface = {contract.RUNTIME_ENTRY: 1, "?createReport@MiniDmpSender@@QEAAXPEAU_EXCEPTION_POINTERS@@@Z": 2}
        with self.assertRaisesRegex(contract.ContractError, "exports names the game imports from the host"):
            contract.check_runtime(runtime_dump(exports=surface), b"MZ" + identity())

    def test_the_runtime_exports_nothing_the_host_owns(self):
        for name in ("NEVR_ScheduleReturnToLobby", "NEVR_RegisterAsset", "NEVR_SetGameModule", "DllMain"):
            with self.subTest(name=name):
                with self.assertRaisesRegex(contract.ContractError, "exports names it may not"):
                    contract.check_runtime(runtime_dump(exports={contract.RUNTIME_ENTRY: 1, name: 2}),
                                           b"MZ" + identity())

    def test_the_runtime_may_export_the_static_modules_entry_points(self):
        names = {contract.RUNTIME_ENTRY: 1, "token_auth_Init": 2, "platform_compat_Init": 3, "TokenAuth_GetToken": 4}
        contract.check_runtime(runtime_dump(exports=names), b"MZ" + identity())

    def test_exactly_one_identity_literal_is_required(self):
        with self.assertRaisesRegex(contract.ContractError, "expected exactly one NEVR-BUILD literal, found 0"):
            contract.check_runtime(runtime_dump(), b"MZ")
        with self.assertRaisesRegex(contract.ContractError, "expected exactly one NEVR-BUILD literal, found 2"):
            contract.check_runtime(runtime_dump(), b"MZ" + identity() + identity())

    def test_the_runtime_does_not_sit_on_the_hosts_base(self):
        with self.assertRaisesRegex(contract.ContractError, "image base"):
            contract.check_runtime(runtime_dump(base=HOST_BASE), b"MZ" + identity())

    def test_the_runtime_is_not_linked_at_the_unpinned_default(self):
        with self.assertRaisesRegex(contract.ContractError, "image base"):
            contract.check_runtime(runtime_dump(base=0x180400000), b"MZ" + identity())

    def test_the_host_must_import_nothing_from_the_runtime_by_name(self):
        # the runtime is not a dependency of the host in either direction
        with self.assertRaisesRegex(contract.ContractError, "imports BugSplat64.dll"):
            contract.check_runtime(runtime_dump(import_dlls=("KERNEL32.dll", "BugSplat64.dll")), b"MZ" + identity())


class ThinHostTest(unittest.TestCase):
    """The host has the exports and no runtime code."""

    def test_a_thin_host_passes(self):
        contract.check_host_is_thin(thin_host_dump(), b"MZ" + identity())

    def test_a_network_import_is_runtime_code(self):
        with self.assertRaisesRegex(contract.ContractError, "imports ws2_32.dll"):
            contract.check_host_is_thin(thin_host_dump(import_dlls=("KERNEL32.dll", "WS2_32.dll")), b"MZ" + identity())

    def test_a_runtime_string_is_runtime_code(self):
        for marker in contract.RUNTIME_ONLY_MARKERS:
            with self.subTest(marker=marker):
                with self.assertRaisesRegex(contract.ContractError, "carries runtime-only text"):
                    contract.check_host_is_thin(thin_host_dump(), b"MZ" + identity() + b"\x00" + marker + b"\x00")

    def test_a_host_the_size_of_the_runtime_is_not_thin(self):
        with self.assertRaisesRegex(contract.ContractError, "SizeOfImage"):
            contract.check_host_is_thin(thin_host_dump(size=0x1730000), b"MZ" + identity())

    def test_the_host_is_linked_at_the_image_base_the_game_imports_it_at(self):
        # N146: the DLL the game imports stays at the stock image base, so the game's graphics DLLs load.
        for base in (RUNTIME_BASE, 0x140000000, 0x10000000):
            with self.subTest(base=hex(base)):
                with self.assertRaisesRegex(contract.ContractError, "host image base is .*expected 0x180000000"):
                    contract.check_host_is_thin(thin_host_dump(base=base), b"MZ" + identity())

    def test_the_host_carries_its_own_identity_literal(self):
        with self.assertRaisesRegex(contract.ContractError, "expected exactly one NEVR-BUILD literal, found 0"):
            contract.check_host_is_thin(thin_host_dump(), b"MZ")


class PairContractTest(unittest.TestCase):
    """The two files of one build state one version and one commit."""

    def test_a_matching_pair_passes(self):
        contract.check_pair(b"MZ" + identity(), b"MZ" + identity())

    def test_a_different_version_fails(self):
        with self.assertRaisesRegex(contract.ContractError, "different version"):
            contract.check_pair(b"MZ" + identity(version="5.5.0"), b"MZ" + identity(version="5.5.1"))

    def test_a_different_commit_fails(self):
        with self.assertRaisesRegex(contract.ContractError, "different commit"):
            contract.check_pair(b"MZ" + identity(), b"MZ" + identity(commit="b" * 40))

    def test_the_ranges_must_not_overlap(self):
        with self.assertRaisesRegex(contract.ContractError, "overlap"):
            contract.check_ranges({"base": HOST_BASE, "size": 0x2000000}, {"base": RUNTIME_BASE, "size": 0x100000})


class MainTest(unittest.TestCase):
    def run_main(self, host, runtime):
        import tempfile
        outputs = {"host": host, "runtime": runtime}
        calls = []

        def fake_objdump(path):
            calls.append(path.name)
            return outputs[path.stem]

        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            (tmp / "host.dll").write_bytes(b"MZ" + identity())
            (tmp / "runtime.dll").write_bytes(b"MZ" + identity())
            real = contract.run_objdump
            contract.run_objdump = fake_objdump
            try:
                return contract.main(["--dll", str(tmp / "host.dll"), "--runtime-dll", str(tmp / "runtime.dll")]), calls
            finally:
                contract.run_objdump = real

    def test_a_good_pair_exits_zero_and_reads_both_files(self):
        code, calls = self.run_main(thin_host_dump(), runtime_dump())
        self.assertEqual(code, 0)
        self.assertEqual(calls, ["host.dll", "runtime.dll"])

    def test_a_bad_runtime_exits_one(self):
        code, _ = self.run_main(thin_host_dump(), runtime_dump(exports={"x": 1}))
        self.assertEqual(code, 1)

    def test_a_runtime_dll_is_required(self):
        with self.assertRaises(SystemExit):
            contract.main(["--dll", "x.dll"])

if __name__ == "__main__":
    unittest.main()
