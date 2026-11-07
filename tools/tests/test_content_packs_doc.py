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

#2312, the Quest twin: the page's "On Quest" table names the hook's function, slot, stub, call sites and resource-manager
pointer that src/quest/sentinel/pinned_targets.h pins, the folder content_folder.h reads, the step order data_wiring.cpp runs,
and the Quest's refusal tokens; and the Quest compiles the runtime's shared decision instead of a copy of it (#1598). The
page says which libr15 serves a folder pack (the patched one: neither the hook's slot nor the permission-wait hook that
reaches the data step is a service-side target), keys no other spelling of the media root, and says the rename safety is
shown on the host only; ADR 0029 carries the #2312 note as an amendment.
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
CORE = "src/runtime/content/content_core.cpp"
QUEST_FOLDER = "src/quest/data/content_folder.cpp"


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
        sources = read(SERVE) + read(INSTALL) + read(JSON) + read(CORE) + read(QUEST_FOLDER)
        for reason in sorted(reasons):
            self.assertIn(f'"{reason}"', sources, f"{DOC} lists reason {reason}, which no source writes")


PINNED = "src/quest/sentinel/pinned_targets.h"
QUEST_HOOK = "src/quest/sentinel/content_pack_hook.cpp"
WIRING = "src/quest/sentinel/data_wiring.cpp"


def quest_section() -> str:
    doc = read(DOC)
    return doc[doc.index("## On Quest"):doc.index("## What the log shows")]


class TheQuestHookThePageNames(unittest.TestCase):
    def test_the_slot_and_function_are_the_pinned_ones_and_a_got_slot(self):
        pinned = code(read(PINNED))
        self.assertRegex(pinned, r"LibR15CSysFileOpen\(\)\s*\{\s*return \{kLibR15, kCSysFileOpenSymbol, RelocKind::kJumpSlot, "
                                 r"LibR15PatchedBuildId\(\), 0x36f6450ULL\};")
        self.assertRegex(pinned, r"kCSysFileOpenVaddr\s*=\s*0xf85538\s*;")
        self.assertRegex(pinned, r"kCSysFileOpenStub\s*=\s*0xf3dad0\s*;")
        section = quest_section()
        for spelling in ("libr15 `0xf85538`", "GOT slot `0x36f6450`", "PLT stub `0xf3dad0`", "N151"):
            self.assertIn(spelling, section)
        self.assertIn("InstallThunk<OpenThunk>(g_hook, sentinel::pinned::LibR15CSysFileOpen())", code(read(QUEST_HOOK)))

    def test_the_call_sites_on_the_page_are_the_pinned_ones(self):
        pinned = code(read(PINNED))
        block = pinned[pinned.index("kCSysFileOpenContentSites[] = {"):]
        block = block[:block.index("};")]
        sites = re.findall(r"\{(0x[0-9a-f]+), (true|false), ", block)
        self.assertEqual(len(sites), 3, block)
        section = quest_section()
        calls = [f"`{at}`" for at, tail in sites if tail == "false"]
        tails = [f"`{at}`" for at, tail in sites if tail == "true"]
        self.assertIn("(`bl` at " + " and ".join(calls) + ")", section)
        self.assertIn("(`b` at " + " and ".join(tails) + ")", section)

    def test_the_resource_manager_pointer_is_the_pinned_one(self):
        self.assertRegex(code(read(PINNED)), r"kResourceManagerPtrVaddr\s*=\s*0x376ed60\s*;")
        self.assertIn("kResourceManagerPtrVaddr", code(read(QUEST_HOOK)))
        self.assertIn("libr15 `0x376ed60`", quest_section())


class TheQuestFolderAndOrder(unittest.TestCase):
    def test_packs_are_read_from_the_media_roots_nevr_content(self):
        self.assertRegex(code(read("src/quest/data/content_folder.h")), r'kContentSubdir\s*=\s*"nevr/content"\s*;')
        self.assertIn("content::ContentDir(data_root::kMediaRoot)", code(read(QUEST_HOOK)))
        media = re.search(r'kMediaRoot\s*=\s*"([^"]+)"\s*;', code(read("src/quest/sentinel/data_root.h"))).group(1)
        section = quest_section()
        row = re.search(r"^\| Where a pack lives \|.*$", section, re.M)
        self.assertIsNotNone(row, "the On Quest table has no 'Where a pack lives' row")
        self.assertIn("| `<media root>/nevr/content/<id>/`; the media root is " + f"`{media}`", row.group(0))
        self.assertIn("`_data/5932408047/rad15/android`", section)

    def test_the_folder_pack_is_decided_after_the_bundled_pack_step(self):
        wiring = code(read(WIRING))
        after = wiring[wiring.index("void AfterStorage("):]
        self.assertRegex(after, r"else if \(RunPack\(engineRoot\)\) \{\s*RunContent\(engineRoot\);")
        self.assertIn("after the bundled pack step", quest_section())


class TheQuestSharesTheDecision(unittest.TestCase):
    def test_the_quest_compiles_the_runtimes_core_and_turns_on_its_own_refusals(self):
        cmake = read("src/quest/CMakeLists.txt")
        for source in ("../runtime/content/content_core.cpp", "../runtime/content/content_json.cpp"):
            self.assertIn(source, cmake, f"the Quest no longer compiles {source}; a Quest-only copy breaks #1598")
        folder = code(read(QUEST_FOLDER))
        self.assertIn("in.scripts_supported = false;", folder)
        self.assertIn("in.refuse_placed_bundle = true;", folder)
        self.assertNotIn("nlohmann", folder, "the Quest parses content.json itself again; it is content_json.cpp's job")

    def test_the_pc_leaves_the_quests_refusals_off(self):
        header = code(read("src/runtime/content/content_core.h"))
        self.assertRegex(header, r"bool scripts_supported = true;")
        self.assertRegex(header, r"bool refuse_placed_bundle = false;")
        serve = code(read(SERVE))
        self.assertNotIn("scripts_supported", serve)
        self.assertNotIn("refuse_placed_bundle", serve)

    def test_the_quests_own_tokens_are_on_the_page(self):
        doc = read(DOC)
        table = doc[doc.index("| `reason` |"):doc.index("Each file that passes")]
        for token in ("scripts_unsupported", "bundled_pack_placed"):
            self.assertIn(f"| `{token}` | Quest only:", table)



ADR_0029 = "docs/adr/0029-quest-bundled-content-pack.md"


class WhichQuestBuildServesAPack(unittest.TestCase):
    def test_the_slot_and_the_data_steps_hook_are_not_service_side_and_the_page_says_so(self):
        pinned = code(read(PINNED))
        for target in ("LibR15CSysFileOpen", "LibR15RequestPermissions", "LibR15CheckPermission"):
            body = re.search(target + r"\(\)\s*\{(.*?)\n\}", pinned, re.S)
            self.assertIsNotNone(body, target)
            self.assertNotIn("ServiceSide()", body.group(1),
                             f"{target} is service-side now: the page's 'Which builds' row and content_folder.h must change")
        self.assertIn("sentinel::pinned::LibR15RequestPermissions(), sentinel::pinned::LibR15CheckPermission()",
                      code(read("src/quest/sentinel/permission_wait_hook.cpp")))
        row = re.search(r"^\| Which builds \|.*$", quest_section(), re.M)
        self.assertIsNotNone(row, "the On Quest table has no 'Which builds' row")
        self.assertIn("the patched libr15 only", row.group(0))
        self.assertIn("no folder pack is served there and no `[NEVR.CONTENT]` line is written", row.group(0))

    def test_no_other_spelling_of_the_media_root_is_keyed(self):
        self.assertNotIn("com.readyatdawn.afk", read(DOC))
        folder = code(read(QUEST_FOLDER))
        self.assertNotIn("path_rewrite", folder)
        self.assertRegex(folder, r"std::string PosixPackFiles::Key\(const std::string& path\) \{ return path; \}")

    def test_the_keys_root_is_the_engine_root_the_data_step_names(self):
        wiring = code(read(WIRING))
        body = wiring[wiring.index("EngineRootChoice EngineRoot() {"):]
        body = body[:body.index("\n}")]
        for root in ("obb_skip::CurrentRoot()", "obb_skip::DataRootRedirect()", "data_root::kLegacyRoot"):
            self.assertIn(root, body, f"EngineRoot() no longer picks {root}: the page's 'A path's spelling' row must change")
        row = re.search(r"^\| A path's spelling \|.*$", quest_section(), re.M)
        self.assertIsNotNone(row, "the On Quest table has no 'A path's spelling' row")
        self.assertIn("the root the data step names (`engine_root`: the media root, the OBB-skip root or the legacy root)",
                      row.group(0))
        self.assertIn("the pack's own folder stays under the media root", row.group(0))

    def test_the_rename_safety_is_said_to_be_shown_on_the_host_only(self):
        self.assertIn("(shown on the host's file systems; not yet on the headset's shared storage)", quest_section())

    def test_adr_0029_carries_the_folder_pack_as_an_amendment(self):
        adr = read(ADR_0029)
        self.assertIn("\n## Amendment (#2312): a pack installed as a folder\n", adr)
        amendment = adr[adr.index("## Amendment (#2312)"):]
        self.assertIn("runtime step (section 3)", amendment)
        self.assertNotIn("item 5", amendment)


if __name__ == "__main__":
    unittest.main()
