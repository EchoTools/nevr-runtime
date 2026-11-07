"""#1424: docs/reference/content-packs.md stays true of the tree.

The page says the runtime serves an installed content pack itself: one hook on the game's CFile::Open for manifests and
packages, a load rewriter on the runtime's own LoadLibrary hooks for scripts, installed after the early plugin pass and
before the game's first command-line pass reads its data. Prose cannot be wrong in a way anything notices, so these
checks read the sources the page relies on:

- the hook's address and the page's agree, and the hook is on CFile::Open, not on kernel32's CreateFileW/A (no source the
  game process loads takes their address) and not on the game's script loader (no source names it);
- the script rewriter is registered on the runtime's LoadLibrary hooks, which the runtime owns and records with HookGuard,
  and a plugin that re-hooks a recorded address is reported through ServerFatal, so the page's "never hook LoadLibrary*"
  holds;
- the page and the SDK give the script loader the same address and the same four arguments;
- packs are read from <root>\\content, and the install runs after the early pass and before the original call;
- every refusal reason the page lists is a token the code writes.
"""

import pathlib
import re
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
DOC = "docs/reference/content-packs.md"
SDK = "sdk/include/nevr/pc/address_registry.h"

# The game's module loader: CScriptResource's deserializer (0x1412ebb50) loads each script DLL through it.
LOADER_VA = "0x1400EB0F0"
LOADER_SIGNATURE = "HMODULE __fastcall (const char* path, void* arg, int call_main, int reuse)"


def read(rel: str) -> str:
    return (REPO / rel).read_text(encoding="utf-8")


# A raw string, a comment, a string literal, or a character literal (a `'` after a digit is a digit separator).
TOKEN = re.compile(r"""(?<![A-Za-z0-9_])(?:u8|u|U|L)?R"(?P<d>[^ ()\\\t\n]{0,16})\(.*?\)(?P=d)"|//[^\n]*|/\*.*?\*/"""
                   r"""|"(?:\\.|[^"\\\n])*"|(?<![A-Za-z0-9_])'(?:\\.|[^'\\\n])*'""", re.S)


def code(text: str) -> str:
    """Comments removed, so prose can neither satisfy nor trip a check. Literals are kept whole, so a `//` inside one
    (a URL) does not end the line's code; a block comment keeps its line breaks, so line numbers stay true."""
    return TOKEN.sub(lambda m: "\n" * m.group().count("\n") if m.group().startswith("/") else m.group(), text)


def without_strings(line: str) -> str:
    """String literals emptied, so a log message that names a function is not a use of it."""
    return re.sub(r'"(?:\\.|[^"\\])*"', '""', line)


# What the game process loads from this tree: the host, the runtime and what links into them (core, abi, modules,
# extension, scripting) and the LibOVRPlatform stub. Not the Quest build and not the launcher, a separate process.
NOT_IN_THE_GAME_PROCESS = ("quest", "launcher")


def runtime_sources():
    """Every C++ source under src/ that the game process loads, outside tests."""
    root = REPO / "src"
    for path in sorted(root.rglob("*")):
        if path.suffix not in (".cpp", ".h", ".hpp", ".inl", ".c"):
            continue
        parts = path.relative_to(root).parts
        if parts[0] in NOT_IN_THE_GAME_PROCESS or "tests" in parts:
            continue
        yield path


CREATE_FILE_BY_NAME = re.compile(r'"CreateFile[AW]"')
CREATE_FILE_AS_VALUE = re.compile(r"\bCreateFile[AW]?\b(?!\s*\()")
ADJACENT_LITERALS = re.compile(r'"\s*"')


def takes_create_file(line: str) -> bool:
    """A line (comments already removed) that takes CreateFileW/A's address, as a hook must: its name as a string
    (GetProcAddress), or the name used as a value (`&CreateFileW`, or `CreateFileW` passed without a call). A call
    opens a file and is not a hook; a message that names the function is not either."""
    joined = ADJACENT_LITERALS.sub("", line)  # "CreateFile" "W" is one literal to the compiler
    return bool(CREATE_FILE_BY_NAME.search(joined) or CREATE_FILE_AS_VALUE.search(without_strings(line)))


class ThePackIsServedThroughTheGamesOwnOpen(unittest.TestCase):
    def test_the_matcher_tells_a_hook_from_a_call(self):
        hooks = ('auto p = GetProcAddress(k32, "CreateFileW");', "Hooking::Attach(&CreateFileW, Hook);",
                 "auto p = reinterpret_cast<void*>(CreateFileA);", "MH_CreateHook(::CreateFileW, Hook, &orig);",
                 "auto p = reinterpret_cast<void*>(&CreateFile);", 'GetProcAddress(k32, "CreateFile" "W");',
                 'const char* u = "ws://x"; void* p = reinterpret_cast<void*>(CreateFileW);',
                 'auto s = R"(a " // b)"; void* p = reinterpret_cast<void*>(CreateFileW);')
        not_hooks = ("HANDLE h = CreateFileW(path, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);",
                     'Log(Warning, "[NEVR.PATCH] CreateFileW failed error=%lu", GetLastError());',
                     "file = CreateFileA (path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);",
                     "h = Open(path); // a hook would take &CreateFileW here",
                     "h = Open(path); /* GetProcAddress(k32, \"CreateFileW\") */",
                     "int n = 1'000; h = Open(path); // a hook would take &CreateFileW, as 'x' says",
                     """if (c == '"') h = Open(path); // "quoted" &CreateFileW""")
        for line in hooks:
            self.assertTrue(takes_create_file(code(line)), line)
        for line in not_hooks:
            self.assertFalse(takes_create_file(code(line)), line)

    def test_no_runtime_source_hooks_create_file(self):
        hits = []
        for path in runtime_sources():
            for number, line in enumerate(code(path.read_text(encoding="utf-8", errors="replace")).splitlines(), 1):
                if takes_create_file(line):
                    hits.append(f"{path.relative_to(REPO)}:{number}: {line.strip()}")
        self.assertEqual(hits, [], f"the runtime now takes CreateFileW/A's address; packs are served through CFile::Open "
                                   f"alone ({DOC}), so a kernel32 hook is a second path")

    def test_no_runtime_source_names_the_script_loader(self):
        spelling = re.compile(r"\b(?:0x0*1400eb0f0|0x0*eb0f0|VA_LOAD_SCRIPT_DLL)\b", re.I)
        hits = []
        for path in runtime_sources():
            for number, line in enumerate(code(path.read_text(encoding="utf-8", errors="replace")).splitlines(), 1):
                if spelling.search(line):
                    hits.append(f"{path.relative_to(REPO)}:{number}: {line.strip()}")
        self.assertEqual(hits, [], f"the runtime now names the script loader; {DOC} says scripts are served by the "
                                   "content-pack load rewriter, not a hook on the loader")

    def test_the_scan_sees_the_game_process_sources(self):
        # A scope that reads nothing passes every check above.
        seen = {str(p.relative_to(REPO)) for p in runtime_sources()}
        for expected in ("src/runtime/hook/dll_load_hook.cpp", "src/core/logging.cpp",
                         "src/modules/platform-compat/src/platform_compat.cpp"):
            self.assertIn(expected, seen)


class TheRuntimeOwnsLoadLibrary(unittest.TestCase):
    def test_the_four_load_library_hooks_are_recorded_with_hook_guard(self):
        hook = code(read("src/runtime/hook/dll_load_hook.cpp"))
        for name in ("LoadLibraryA", "LoadLibraryW", "LoadLibraryExA", "LoadLibraryExW"):
            self.assertIn(f'{{"{name}",', hook, f"{name} is no longer in the hook table; update {DOC}")
        self.assertIn("nevr_hook_guard::Record(s.target, s.name)", hook,
                      f"the LoadLibrary hooks are no longer recorded with HookGuard; update {DOC}")

    def test_a_plugin_that_re_hooks_a_recorded_address_is_reported_through_server_fatal(self):
        loader = code(read("src/runtime/ext/plugin_loader.cpp"))
        verify = loader.index("nevr_hook_guard::VerifyAll(")
        fatal = loader.index("ServerFatal(", verify)
        self.assertLess(fatal - verify, 400, f"VerifyAll's collisions no longer reach ServerFatal; update {DOC}")
        self.assertIn("re-hooked %d address(es) this runtime already owns", loader[fatal:fatal + 200])


class TheScriptLoaderIsDescribedOnce(unittest.TestCase):
    def test_the_sdk_gives_the_loaders_address_and_four_arguments(self):
        sdk = read(SDK)
        self.assertRegex(code(sdk), rf"VA_LOAD_SCRIPT_DLL\s*=\s*{LOADER_VA}\s*;")
        block = sdk[:sdk.index("VA_LOAD_SCRIPT_DLL")]
        block = block[block.rindex("// ---"):]
        self.assertIn(f"Signature: {LOADER_SIGNATURE}", block,
                      "the SDK's comment above VA_LOAD_SCRIPT_DLL must give the loader's four arguments")

    def test_the_page_gives_the_same_address_and_signature(self):
        doc = read(DOC)
        self.assertIn(f"`VA_LOAD_SCRIPT_DLL`, `{LOADER_VA}`", doc)
        self.assertIn(f"`{LOADER_SIGNATURE}`", doc)
        self.assertIn(f"`{SDK}`", doc)


INSTALL = "src/runtime/content/content_pack_install.cpp"
SERVE = "src/runtime/content/content_pack.cpp"
JSON = "src/runtime/content/content_json.cpp"


class TheHookThePageNames(unittest.TestCase):
    def test_the_hook_is_on_cfile_open_at_the_address_the_page_gives(self):
        install = code(read(INSTALL))
        self.assertRegex(install, r"kCFileOpenVa\s*=\s*0x1400e15e0\s*;")
        self.assertIn("PatchDetour(&g_cfileOpen,", install)
        self.assertIn("`CFile::Open` (echovr.exe `0x1400e15e0`", read(DOC))

    def test_scripts_go_through_the_runtimes_own_load_rewriter(self):
        install = code(read(INSTALL))
        self.assertIn('nevr_dll_load_hook::AddLoadRewriter("content-pack", &RewriteScript)', install)
        self.assertIn("registered as `content-pack`", read(DOC))


class WhereAndWhen(unittest.TestCase):
    def test_packs_are_read_from_the_roots_content_folder(self):
        self.assertRegex(code(read("src/core/nevr_root.h")), r'kContent\s*=\s*L"content"\s*;')
        self.assertIn("nevr_root::ContentDir(root)", code(read(INSTALL)))
        self.assertIn("`<root>\\content\\<id>\\`", read(DOC))

    def test_the_install_runs_after_the_early_pass_and_before_the_games_first_command_line_pass(self):
        boot = code(read("src/runtime/lifecycle/boot.cpp"))
        early = boot.index("LoadPlugins(PluginPhase::Early);")
        install = boot.index("nevr_content::Install(", early)
        original = boot.index("EchoVR::PreprocessCommandLine(pGame);", install)
        self.assertLess(early, install)
        self.assertLess(install, original)


class TheRefusalsThePageLists(unittest.TestCase):
    def test_every_reason_in_the_table_is_a_token_the_code_writes(self):
        doc = read(DOC)
        table = doc[doc.index("| `reason` |"):doc.index("Each file that passes")]
        reasons = set(re.findall(r"`([a-z_]+)`", table)) - {"reason", "content", "json"}
        self.assertGreaterEqual(len(reasons), 12, reasons)
        sources = read(SERVE) + read(INSTALL) + read(JSON)
        for reason in sorted(reasons):
            self.assertIn(f'"{reason}"', sources, f"{DOC} lists reason {reason}, which no source writes")


if __name__ == "__main__":
    unittest.main()
