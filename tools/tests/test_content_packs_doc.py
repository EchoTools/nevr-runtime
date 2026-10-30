"""#1424: docs/reference/content-packs.md stays true of the tree.

The page tells a plugin author how a plugin serves a content pack: the runtime serves none itself, so the plugin
answers the game's package opens and its script loads, from the early plugin pass. Prose cannot be wrong
in a way anything notices, so these checks read the sources the page relies on:

- nothing this tree loads into the game process hooks CreateFileW or CreateFileA or touches the game's script loader,
  so a pack plugin may (when that changes, the page is wrong and this fails);
- the runtime owns the four LoadLibrary hooks and records each with HookGuard, and a plugin that re-hooks a recorded
  address is reported through ServerFatal after its init, so the page's "never hook LoadLibrary*" holds;
- the page and the SDK give the script loader the same address and the same four arguments;
- plugins load from <root>\\plugins, so a plugin finds the root as its own directory's parent;
- the early pass the page asks for exists and is documented under the key the page names.
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


class TheRuntimeLeavesThePackSeamsToAPlugin(unittest.TestCase):
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
        self.assertEqual(hits, [], f"the runtime now takes CreateFileW/A's address; update {DOC} (a pack plugin may "
                                   "no longer hook it)")

    def test_no_runtime_source_names_the_script_loader(self):
        spelling = re.compile(r"\b(?:0x0*1400eb0f0|0x0*eb0f0|VA_LOAD_SCRIPT_DLL)\b", re.I)
        hits = []
        for path in runtime_sources():
            for number, line in enumerate(code(path.read_text(encoding="utf-8", errors="replace")).splitlines(), 1):
                if spelling.search(line):
                    hits.append(f"{path.relative_to(REPO)}:{number}: {line.strip()}")
        self.assertEqual(hits, [], f"the runtime now names the script loader; update {DOC} (a pack plugin may no "
                                   "longer hook it)")

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


class WhereThePluginFindsTheRoot(unittest.TestCase):
    def test_plugins_load_from_the_roots_plugins_directory(self):
        root = code(read("src/core/nevr_root.h"))
        self.assertRegex(root, r'kPlugins\s*=\s*L"plugins"\s*;')
        self.assertIn("nevr_root::PluginsDir(nevr_root::RootA())", code(read("src/runtime/ext/plugin_loader.cpp")))
        self.assertIn("Plugins load from `<root>\\plugins\\`", read(DOC))


class TheEarlyPassThePageAsksFor(unittest.TestCase):
    def test_the_early_pass_runs_before_the_games_first_command_line_pass(self):
        boot = code(read("src/runtime/lifecycle/boot.cpp"))
        early = boot.index("LoadPlugins(PluginPhase::Early);")
        original = boot.index("EchoVR::PreprocessCommandLine(pGame);", early)
        self.assertLess(early, original)

    def test_the_key_the_page_names_is_the_documented_one(self):
        self.assertRegex(read("docs/reference/example-config.yaml"), r"(?m)^#\s+early\s+— if true, the plugin loads")
        self.assertIn("`early: true`", read(DOC))


if __name__ == "__main__":
    unittest.main()
