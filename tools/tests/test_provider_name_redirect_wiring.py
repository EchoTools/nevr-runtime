"""P34/P35 (#1774): the provider-name redirect replaces the two .rdata string rewrites (rank 3).

Source-order sensors: the argument swap is driven through the real detour by src/runtime/tests/test_provider_name_redirect.cpp
(in test_behavioral); the live run is the contrast run named in the PR. Each assertion names the text it looks for.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def _code(rel: str) -> str:
    """The file without // and block-comment lines, so prose that explains a rule cannot satisfy a check."""
    out = []
    for line in _read(rel).splitlines():
        stripped = line.lstrip()
        if stripped.startswith(("//", "*", "/*")):
            continue
        out.append(line.split("//", 1)[0])
    return "\n".join(out)


def test_init_installs_the_redirect_and_a_failure_is_the_mode_gated_server_fatal():
    text = _code("src/runtime/patch/pnsrad_enabler.cpp")
    assert text.count("nevr_provider_name_redirect::Install(base_addr)") == 1
    assert "ServerFatal(" in text, "a server that cannot install the redirect must stop (client: a Warning)"
    assert "g_bootHookFailed" not in text, "the failure policy is ServerFatal at Init, not the boot flag"


def test_the_string_rewrites_are_gone():
    text = _code("src/runtime/patch/pnsrad_enabler.cpp")
    for gone in ("STR_PNSOVR", "STR_PNSDEMO", "STR_SIZE", "0x16d35c4", "0x16d35e8"):
        assert gone not in text, f"pnsrad_enabler.cpp still names {gone}"


def test_the_hook_pins_the_loader_and_uses_the_repo_idioms():
    addresses = _code("src/runtime/hook/addresses.h")
    assert "CNSPROVIDER_LOAD_BY_NAME = 0x606690" in addresses
    text = _code("src/runtime/patch/provider_name_redirect.cpp")
    assert "nevr::ValidatePrologue(" in text, "the loader's prologue must be validated before the detour"
    assert 'PatchDetour(&g_original, reinterpret_cast<PVOID>(&LoadByNameHook), "CNSProvider::LoadByName")' in text
    assert '"pnsovr"' in text and '"pnsdemo"' in text and '"pnsrad"' in text
    assert "catch (" not in text


def test_the_source_and_its_test_are_built():
    cmake = _read("src/runtime/CMakeLists.txt")
    assert '"patch/provider_name_redirect.cpp"' in cmake, "provider_name_redirect.cpp is not in the runtime source list"
    assert "tests/test_provider_name_redirect.cpp" in cmake, "test_provider_name_redirect.cpp is not in test_behavioral"
