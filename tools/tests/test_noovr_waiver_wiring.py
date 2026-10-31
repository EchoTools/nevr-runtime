"""P27 (#1774): the -noovr argument-lookup waiver is wired at boot and the byte patch it replaces is gone (rank 3).

Source-order sensors: the byte logic is covered by src/runtime/tests/test_cmdline_waiver.cpp, and the live run is the
contrast run named in the PR. Each assertion names the text it looks for, so a failure says what moved.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def _code_lines(rel: str) -> list[str]:
    """Lines without // comments, so prose that explains a rule cannot satisfy a check."""
    out = []
    for line in _read(rel).splitlines():
        stripped = line.lstrip()
        if stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*"):
            continue
        out.append(line.split("//", 1)[0])
    return out


def test_install_is_called_from_initialize_before_preprocess_runs():
    lines = _code_lines("src/runtime/lifecycle/initialize.cpp")
    install = [i for i, line in enumerate(lines) if "nevr_noovr_arglookup::Install(" in line]
    assert len(install) == 1, "initialize.cpp must call nevr_noovr_arglookup::Install exactly once"
    text = "\n".join(lines)
    assert "PatchNoOvrRequiresSpectatorStream" not in text, "the byte patch's call site is still in initialize.cpp"


def test_the_byte_patch_and_its_constants_are_gone():
    for rel in ("src/runtime/patch/mode_patches.cpp", "src/runtime/patch/mode_patches.h", "src/runtime/hook/addresses.h"):
        text = "\n".join(_code_lines(rel))
        assert "PatchNoOvrRequiresSpectatorStream" not in text, f"{rel} still names the byte patch"
        assert "NOOVR_SPECTATOR" not in text, f"{rel} still names NOOVR_SPECTATOR"
        assert "0x11690D" not in text, f"{rel} still pins the 0x11690D byte patch"


def test_the_hook_is_built_into_the_runtime_and_pins_the_callee_and_site():
    cmake = _read("src/runtime/CMakeLists.txt")
    assert '"patch/noovr_arglookup.cpp"' in cmake, "noovr_arglookup.cpp is not in the runtime source list"
    addresses = "\n".join(_code_lines("src/runtime/hook/addresses.h"))
    assert "CMAINARGS_FIND_ARG_INDEX_BY_SYMBOL = 0x1400D56A0" in addresses
    assert "NOOVR_ARG_LOOKUP_CALL = 0x140116904" in addresses


def test_the_hook_uses_the_repo_idioms():
    text = "\n".join(_code_lines("src/runtime/patch/noovr_arglookup.cpp"))
    assert "PatchDetour(" in text and '"CMainArgs::FindArgIndexBySymbol"' in text
    assert "nevr::ValidatePrologue(" in text, "the callee and the call site must be prologue-validated"
    assert "__builtin_return_address(0)" in text
    assert "__attribute__((noinline))" in text, "the return address is only the game's call site when not inlined"
    assert "catch (" not in text, "no catch in the hook"
