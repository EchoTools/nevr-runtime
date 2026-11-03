"""#807: the first-run data step is wired into the game's start in the order the plan needs.

The hooks reach libr15's GOT, which no host test can. The host tests (data_wiring_test, permission_wait_hook_test)
drive the real handler and the step through the thunk entry; what they cannot see is that the production entry arms
the step, and the order inside the step that the game's start depends on. That is pinned here by source shape.
"""

import re
import unittest
from pathlib import Path

from tools.tests.test_runtime_lifecycle_invariants import extract_braced_function

ROOT = Path(__file__).resolve().parents[2]
SENTINEL = ROOT / "src/quest/sentinel"


def read(name):
    return (SENTINEL / name).read_text()


class TheProductionEntryArmsTheStep(unittest.TestCase):
    def test_the_data_step_is_armed_before_the_permission_wait_hook_is_installed(self):
        body = extract_braced_function(read("entry.cpp"), "bool InstallPermissionWaitHook() noexcept {")
        self.assertIn("data_wiring::Install(options)", body)
        self.assertIn("options.show = &nevr_quest::toast::Show", body)
        self.assertLess(body.index("data_wiring::Install(options)"), body.index("permission_wait::Install()"))


class TheHandlerRunsTheStepAfterTheWait(unittest.TestCase):
    def test_after_storage_runs_after_the_wait_and_only_for_the_boot_request(self):
        body = extract_braced_function(read("permission_wait_hook.cpp"), "void RequestHandler(")
        self.assertIn("g_spent.exchange(true", body)
        self.assertLess(body.index("g_spent.exchange(true"), body.index("wait(names, count)"))
        self.assertLess(body.index("wait(names, count)"), body.index("g_after.load"))
        self.assertLess(body.index("g_after.load"), body.index("after(outcome)"))

    def test_the_handler_reaches_the_step_only_through_a_pointer(self):
        body = extract_braced_function(read("permission_wait_hook.cpp"), "void RequestHandler(")
        self.assertNotIn("data_wiring", body)
        self.assertNotIn("AfterStorage(", body)


class TheAppIsKeptBeforeTheGameRuns(unittest.TestCase):
    def test_process_end_stores_the_app_before_the_original_android_main(self):
        body = extract_braced_function(read("process_end_hook.cpp"), "void MainHandler(")
        self.assertIn("g_app.store(app", body)
        self.assertLess(body.index("g_app.store(app"), body.index("original(app)"))


class TheStepOrder(unittest.TestCase):
    def body(self):
        return extract_braced_function(
            read("data_wiring.cpp"), "void AfterStorage(permission_wait::StorageOutcome outcome) noexcept {"
        )

    def test_the_step_looks_at_the_root_the_engine_reads_and_never_resolves_it_again(self):
        body = self.body()
        self.assertIn("const EngineRootChoice choice = EngineRoot();", body)
        self.assertIn("const std::string& engineRoot = choice.root;", body)
        self.assertIn("config.engine_root = engineRoot;", body)
        self.assertNotIn("ResolveRoot", body)
        root = extract_braced_function(read("data_wiring.cpp"), "EngineRootChoice EngineRoot() {")
        self.assertIn("obb_skip::CurrentCounts().mountSkipped > 0", root)
        self.assertIn("obb_skip::CurrentRoot()", root)
        self.assertIn("obb_skip::DataRootRedirect()", root)
        self.assertIn("data_root::kLegacyRoot", root)

    def test_a_release_build_never_fetches_and_a_missing_install_ends_the_process_with_its_reason(self):
        body = self.body()
        self.assertIn("config.allow_fetch = Opts().allow_fetch;", body)
        self.assertRegex(body, r"if \(missing\) \{\s*EndFailed\(\"game_data_missing\"\);")
        self.assertLess(body.index("RunDataStep"), body.index('EndFailed("game_data_missing")'))
        header = read("data_wiring.h")
        self.assertRegex(header, r"bool allow_fetch = false;")
        entry = extract_braced_function(read("entry.cpp"), "bool InstallPermissionWaitHook() noexcept {")
        self.assertIn("options.allow_fetch = nevr_build::kIsDev;", entry)

    def test_no_storage_grant_with_the_legacy_root_writes_nothing_and_ends_the_process(self):
        body = self.body()
        self.assertRegex(body, r"if \(!granted && engineRoot == data_root::kLegacyRoot\) \{")
        gate = body.index("!granted && engineRoot == data_root::kLegacyRoot")
        self.assertLess(gate, body.index("RunDataStep"))
        self.assertIn('EndFailed("no_storage_grant")', body[gate:body.index("RunDataStep")])

    def test_a_failed_step_ends_nonzero_through_the_end_with_its_reason(self):
        body = self.body()
        self.assertRegex(body, r'if \(failed\) EndFailed\("data_step_failed"\)')
        self.assertLess(body.index("RunDataStep"), body.index("if (failed) EndFailed("))
        wiring = extract_braced_function(read("data_wiring.cpp"), "void EndFailed(const char* reason) noexcept {")
        self.assertIn("process_end::EndWithExitCode(process_end::kFailureExitCode, reason)", wiring)
        self.assertNotIn("EndProcess", re.sub(r"//[^\n]*", "", read("data_wiring.cpp")))
        end = extract_braced_function(
            read("process_end_hook.cpp"), "void EndWithExitCode(int exitCode, const char* reason) noexcept {"
        )
        self.assertNotIn("_exit(0)", end)

    def test_no_grant_skips_the_legacy_root_and_the_probe_but_not_the_data_step(self):
        body = self.body()
        self.assertIn("const bool granted = permission_wait::StorageGranted(outcome);", body)
        self.assertIn("if (granted && Opts().probe_enabled) RunProbe();", body)
        self.assertIn("config.legacy_root_readable = granted;", body)
        self.assertLess(body.index("config.legacy_root_readable = granted;"), body.index("RunDataStep"))
        # the how-to-allow-Storage message only when the media root could not be written and there is no grant
        self.assertRegex(body, r"failed && !granted && result\.status == data::FetchStatus::kBadRoot")
        gate = body.index("failed && !granted")
        self.assertIn("kNeedsStoragePermission", body[gate:])
        self.assertIn('EndFailed("no_storage_grant")', body[gate:])
        self.assertNotIn("StopWithoutGrant", re.sub(r"//[^\n]*", "", read("data_wiring.cpp")))

    def test_the_production_exists_check_accepts_the_manifest_file(self):
        body = self.body()
        self.assertIn("Opts().exists != nullptr ? Opts().exists : &obb_skip::PathExists;", body)
        path = extract_braced_function(read("obb_skip_hook.cpp"), "bool PathExists(const char* path) noexcept {")
        self.assertIn("S_ISREG(st.st_mode)", path)
        self.assertIn("st.st_size > 0", path)
        self.assertIn("S_ISDIR(st.st_mode)", path)

    def test_the_probe_runs_only_in_a_development_build_and_after_the_gate(self):
        body = self.body()
        self.assertRegex(body, r"if \(granted && Opts\(\)\.probe_enabled\) RunProbe\(\);")
        entry = extract_braced_function(read("entry.cpp"), "bool InstallPermissionWaitHook() noexcept {")
        self.assertIn("options.probe_enabled = nevr_build::kIsDev;", entry)

    def test_there_is_no_cancel_while_the_step_blocks_android_main(self):
        code = re.sub(r"//[^\n]*", "", read("data_wiring.cpp"))
        self.assertNotIn("keep_going", code)
        self.assertNotIn("NotDestroyed", code)
        self.assertNotIn("kAppDestroyRequested", code)
        self.assertNotIn("config.fetch.backoff", code)  # the fetch's default backoff applies

    def test_the_download_bounds(self):
        header = read("data_wiring.h")
        self.assertRegex(header, r"kStallSeconds\s*=\s*120;")
        self.assertRegex(header, r"kMinBytesPerSecond\s*=\s*16 \* 1024;")
        wiring = extract_braced_function(
            read("data_wiring.cpp"), "std::unique_ptr<data::CurlTransport> MakeProductionTransport() {"
        )
        self.assertIn("kStallSeconds", wiring)
        self.assertIn("kMinBytesPerSecond", wiring)

    def test_the_message_is_held_ten_seconds(self):
        self.assertRegex(read("data_wiring.h"), r"unsigned hold_ms = 5000;")
        hold = extract_braced_function(read("data_wiring.cpp"), "void HoldMessage(const std::string& message) noexcept {")
        self.assertEqual(hold.count("Sleep(Opts().hold_ms)"), 2)

    def test_a_release_build_holds_nothing_while_no_presenter_is_known_to_draw(self):
        hold = extract_braced_function(read("data_wiring.cpp"), "void HoldMessage(const std::string& message) noexcept {")
        self.assertLess(hold.index("if (Opts().hold_ms == 0) return;"), hold.index("Sleep(Opts().hold_ms)"))
        self.assertLess(hold.index("if (Opts().hold_ms == 0) return;"), hold.index("Present("))
        entry = extract_braced_function(read("entry.cpp"), "bool InstallPermissionWaitHook() noexcept {")
        # Exactly one mention of hold_ms in the body, and it is the guarded assignment: a second assignment (always
        # zero, or a later non-zero value that undoes the guard) or an unguarded one leaves the release build wrong.
        self.assertEqual(len(re.findall(r"\bhold_ms\b", entry)), 1, entry)
        self.assertEqual(len(re.findall(r"if \(!nevr_build::kIsDev\) options\.hold_ms = 0;", entry)), 1, entry)

    def test_each_kisdev_guarded_option_is_assigned_once_and_before_install(self):
        # allow_fetch and probe_enabled carry the same count-based pin as hold_ms: an extra `options.allow_fetch = true;`
        # after the guarded line makes a release build download, and an extra `options.probe_enabled = true;` makes it
        # read the probe file. Each guarded line must also come before Install(options), which copies the options
        # (`Opts() = options;`): a line moved after it sets a local the step never sees, and the release build then
        # keeps the Options defaults (hold_ms 5000: a release build that holds ten seconds).
        entry = re.sub(
            r"//[^\n]*", "",
            extract_braced_function(read("entry.cpp"), "bool InstallPermissionWaitHook() noexcept {"),
        )
        install = entry.index("nevr_quest::data_wiring::Install(options);")
        for name, line in (
            ("allow_fetch", "options.allow_fetch = nevr_build::kIsDev;"),
            ("probe_enabled", "options.probe_enabled = nevr_build::kIsDev;"),
            ("hold_ms", "if (!nevr_build::kIsDev) options.hold_ms = 0;"),
        ):
            self.assertEqual(len(re.findall(r"\b%s\b" % name, entry)), 1, (name, entry))
            self.assertEqual(entry.count(line), 1, (name, entry))
            self.assertLess(entry.index(line), install, (name, entry))

    def test_the_log_never_says_shown_for_a_toast_that_was_only_requested(self):
        text = re.sub(r"//[^\n]*", "", read("data_wiring.cpp") + read("toast_presenter.cpp"))
        self.assertIn('"requested"', text)
        self.assertNotIn('"shown"', text)

    def test_the_probe_is_consumed_before_it_acts(self):
        body = extract_braced_function(read("data_wiring.cpp"), "void RunProbe() noexcept {")
        # Every kind acts after the rename: the parse and the three presenters/waits are all below it.
        for acts in ("ParseProbeCommand(", "Present(", "ProbeShow(", "Sleep("):
            self.assertLess(body.index("std::rename"), body.index(acts), acts)
        self.assertIn("kMaxProbeBlockSeconds", body)

    def test_the_probe_kinds_are_the_five_whole_words_and_each_has_its_presenter(self):
        header = read("probe_command.h")
        for word in ('"toast "', '"ui-toast "', '"ui-post "', '"dialog "', '"block "'):
            self.assertIn(word, header)
        body = extract_braced_function(read("data_wiring.cpp"), "void RunProbe() noexcept {")
        self.assertIn('ProbeShow("ui-toast", Opts().show_ui, command.arg)', body)
        self.assertIn('ProbeShow("ui-post", Opts().show_post, command.arg)', body)
        self.assertIn('ProbeShow("dialog", Opts().show_dialog, command.arg)', body)
        self.assertIn('Present(command.arg, "probe")', body)

    def test_the_main_looper_toast_is_reachable_from_the_probe_only_the_floor_message_keeps_its_presenter(self):
        entry = extract_braced_function(read("entry.cpp"), "bool InstallPermissionWaitHook() noexcept {")
        self.assertIn("options.show = &nevr_quest::toast::ShowPreferUiThread;", entry)
        self.assertIn("options.show_ui = &nevr_quest::toast::ShowOnMainLooper;", entry)
        self.assertIn("options.show_post = &nevr_quest::toast::ShowOnUiThread;", entry)
        # The floor message goes through Opts().show; nothing but the probe reads show_ui or show_dialog.
        text = re.sub(r"//[^\n]*", "", read("data_wiring.cpp"))
        self.assertEqual(text.count("Opts().show_ui"), 1)
        self.assertEqual(text.count("Opts().show_dialog"), 1)
        self.assertEqual(text.count("Opts().show_post"), 1)
        present = extract_braced_function(read("data_wiring.cpp"), "void Present(")
        self.assertIn("Opts().show(", present)

    def test_the_ui_looper_is_taken_in_the_constructor_in_every_build(self):
        # #829: the floor message's presenter needs the UI looper in a release build too, so the capture is unconditional.
        ctor = extract_braced_function(read("entry.cpp"), "static void nevr_sentinel_ctor() {")
        self.assertIn("nevr_quest::toast::CaptureUiLooper();", ctor)
        self.assertNotIn("kIsDev) nevr_quest::toast::CaptureUiLooper", ctor)

    def test_the_looper_callback_makes_the_toast_on_the_calling_thread_and_never_the_main_looper_overload(self):
        text = re.sub(r"//[^\n]*", "", read("toast_presenter.cpp"))
        cb = extract_braced_function(text, "int UiCallback(int fd, int events, void* /*data*/) noexcept {")
        self.assertIn("ShowWhere(posted->app, posted->text, &prepared, Where::kThisThread)", cb)
        self.assertIn("delete posted;", cb)


class TheLeftoversArePutRightBeforeTheDecision(unittest.TestCase):
    def test_recover_runs_before_clean_and_before_resolve_on_every_root(self):
        data = ROOT / "src/quest/data/data_step.cpp"
        text = data.read_text()
        put = extract_braced_function(text, "void PutLeftoversRight(const StepConfig& config) {")
        self.assertLess(put.index("RecoverReplaced("), put.index("CleanLeftovers("))
        self.assertIn("RecoveryRootsFor(config)", put)
        roots = extract_braced_function(text, "std::vector<std::string> RecoveryRootsFor(const StepConfig& config) {")
        self.assertIn("data_root::kCandidates", roots)
        self.assertIn("data_root::kLegacyRoot", roots)  # dropped without the grant
        run = extract_braced_function(text, "StepResult RunDataStep(const StepConfig& config, Transport& transport) {")
        self.assertLess(run.index("PutLeftoversRight(config)"), run.index("data_root::Resolve("))

    def test_the_notice_comes_from_the_fetch_after_its_space_check(self):
        fetch = (ROOT / "src/quest/data/fetch.cpp").read_text()
        self.assertIn("c.before_download", fetch)
        self.assertLess(fetch.index("kNoSpace"), fetch.index("c.before_download"))


class TheToastClearsEveryException(unittest.TestCase):
    def test_pending_is_checked_before_the_result_after_every_call(self):
        text = re.sub(r"//[^\n]*", "", read("toast_presenter.cpp"))
        self.assertNotRegex(text, r"if \([a-zA-Z]+ == nullptr \|\| Pending\(env\)\)")
        self.assertIn("ExceptionClear", text)


class TheConstructorBeginNamesTheBuild(unittest.TestCase):
    # #1391: the begin record carries this build's version and commit. The record is written by LogConstructorBegin
    # (src/quest/integration/ctor_sequence.cpp, pinned on the host by integration_sequence_test); this pins that the
    # constructor hands it the build's own identity from the generated header, not a literal.
    def test_the_constructor_passes_the_generated_build_identity(self):
        text = re.sub(r"//[^\n]*", "", read("entry.cpp"))
        ctor = extract_braced_function(text, "static void nevr_sentinel_ctor() {")
        self.assertIn("nevr_quest::integration::LogConstructorBegin(nevr_build::kVersion, nevr_build::kCommit);", ctor)


class NoLoadTimeInitializer(unittest.TestCase):
    def test_the_wiring_holds_no_global_with_a_constructor(self):
        text = re.sub(r"//[^\n]*", "", read("data_wiring.cpp"))
        self.assertNotRegex(text, r"^(?:static\s+)?Options\s+\w+\s*;", "a global Options is a dynamic initializer")
        self.assertIn("static Options options;", text)  # the function-local one the step uses


class NoUrlInTheLogAndNoCatchAll(unittest.TestCase):
    FILES = ["data_wiring.cpp", "toast_presenter.cpp"]

    def test_no_url_literal_in_the_wiring(self):
        for name in self.FILES:
            text = re.sub(r"//[^\n]*", "", read(name))
            self.assertNotIn("https://", text, name)
            self.assertNotIn("kZipUrl", text, name)

    def test_no_catch_all(self):
        for name in self.FILES + ["android_glue_layout_check.cpp"]:
            self.assertIsNone(re.search(r"catch\s*\(\s*\.\.\.\s*\)", read(name)), name)


class TheGlueOffsetsAreAssertedAgainstTheNdkHeaders(unittest.TestCase):
    CONSTANTS = {
        "kAppActivity": "0x18",
        "kAppDestroyRequested": "0x64",
        "kActivityVm": "0x8",
        "kActivityEnv": "0x10",
        "kActivityClazz": "0x18",
    }

    def test_every_constant_has_its_value_and_a_static_assert(self):
        layout = read("android_glue_layout.h")
        check = read("android_glue_layout_check.cpp")
        for name, value in self.CONSTANTS.items():
            self.assertRegex(layout, rf"{name}\s*=\s*{value};", name)
            self.assertIn(f"sentinel::glue::{name}", check, name)
        self.assertEqual(len(re.findall(r"^static_assert\(", check, re.M)), len(self.CONSTANTS) + 1)  # + the JavaVM slot

    def test_the_javavm_slot_the_game_calls_is_asserted_by_name(self):
        check = read("android_glue_layout_check.cpp")
        self.assertIn("offsetof(JNIInvokeInterface, DetachCurrentThread) == 0x28", check)


if __name__ == "__main__":
    unittest.main()


class TheBundledPackRunsAfterTheDataStep(unittest.TestCase):
    """ADR 0029: the pack step is the data step's last act, reached only when the data is in place, in every build."""

    def body(self):
        return extract_braced_function(
            read("data_wiring.cpp"), "void AfterStorage(permission_wait::StorageOutcome outcome) noexcept {"
        )

    def test_the_pack_runs_after_every_way_the_data_step_ends_the_process(self):
        body = self.body()
        self.assertEqual(body.count("RunPack("), 1)
        run = body.index("RunPack(engineRoot)")
        for ender in ('EndFailed("no_storage_grant")', 'EndFailed("game_data_missing")', 'EndFailed("data_step_failed")'):
            self.assertLess(body.rindex(ender), run, ender)
        self.assertLess(body.index("data::RunDataStep(config, *transport)"), run)
        # A failed data step takes the other branch: the pack step never runs over data that is not there.
        self.assertRegex(body, r'if \(failed\) EndFailed\("data_step_failed"\);\s*else RunPack\(engineRoot\);')

    def test_the_pack_step_is_not_gated_on_a_development_build(self):
        # The variant APK that carries a pack is a release build (allow_fetch false): the pack step must not read it.
        text = re.sub(r"//[^\n]*", "", read("data_wiring.cpp"))
        run_pack = extract_braced_function(text, "void RunPack(const std::string& engineRoot) noexcept {")
        self.assertNotIn("allow_fetch", run_pack)
        self.assertNotIn("kIsDev", run_pack)

    def test_a_failed_placement_ends_the_process_with_its_own_reason(self):
        text = re.sub(r"//[^\n]*", "", read("data_wiring.cpp"))
        run_pack = extract_braced_function(text, "void RunPack(const std::string& engineRoot) noexcept {")
        self.assertIn('EndFailed("pack_failed")', run_pack)
        self.assertIn("PackOutcome::kFailed", run_pack)

    def test_the_data_steps_cleanup_routine_covers_the_pack_and_the_pack_step_runs_it_before_any_write(self):
        staging = (ROOT / "src/quest/data/staging.cpp").read_text()
        clean = extract_braced_function(staging, "int CleanLeftovers(const std::string& root, int* failed) noexcept {")
        self.assertIn("CleanPackLeftovers(root", clean)
        pack = (ROOT / "src/quest/data/pack.cpp").read_text()
        run = extract_braced_function(pack, "PackResult RunPackStep(const PackConfig& config) {")
        self.assertLess(run.index("CleanPackLeftovers(config.root"), run.index("WriteTemporary("))
        self.assertLess(run.index("CleanPackLeftovers(config.root"), run.index("CheckSpace("))

    def test_the_package_is_written_before_the_manifest(self):
        pack = (ROOT / "src/quest/data/pack.cpp").read_text()
        run = extract_braced_function(pack, "PackResult RunPackStep(const PackConfig& config) {")
        first = run.index("if (pack.files[i].path != pack.manifest_path) order.push_back(i);")
        second = run.index("if (pack.files[i].path == pack.manifest_path) order.push_back(i);")
        self.assertLess(first, second)

    def test_the_apk_is_found_from_where_the_sentinel_was_mapped_when_no_path_is_given(self):
        # No host test maps the sentinel from an APK, so the production source of the path is pinned by shape (ADR 0029).
        text = re.sub(r"//[^\n]*", "", read("data_wiring.cpp"))
        run_pack = extract_braced_function(text, "void RunPack(const std::string& engineRoot) noexcept {")
        self.assertIn("pack.apk_path = !Opts().apk_path.empty() ? Opts().apk_path : OwnApkPath();", run_pack)
        own = extract_braced_function(text, "std::string OwnApkPath() {")
        self.assertIn("dladdr(reinterpret_cast<const void*>(&AfterStorage), &info)", own)
        self.assertIn("data::ApkPathForLibrary(info.dli_fname,", own)
