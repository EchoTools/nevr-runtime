"""#989, #1135: a release's Quest APK has a recipe, and the loader in it is the release's own asset.

`just release-quest-apk vX.Y.Z` repacks the store game with the loader CI built for the tag and attached to the
release (tools/quest_release_loader.py), instead of compiling a second one: the asset and the APK's loader are one
file, and the recipe refuses unless their sha256 agree. This test reads the justfile text (the recipe cannot run
here: it needs the signing key, the store APK and a release).
"""

import hashlib
import os
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
JUSTFILE = (ROOT / "justfile").read_text()


def recipe(name):
    m = re.search(rf"^{re.escape(name)}\b[^\n]*:[^\n]*\n((?:[ \t]+[^\n]*\n|\n)*)", JUSTFILE, re.M)
    if not m:
        raise AssertionError(f"justfile has no recipe {name}")
    return m.group(0)


class ReleaseQuestApk(unittest.TestCase):
    """The body of `release-quest-apk` is the private recipe `_release-quest-apk`, which `release-quest-variant-apk` runs too."""

    def test_the_release_recipe_exists_and_refuses_a_non_release_tag(self):
        self.assertIn("release-quest-apk tag", recipe("release-quest-apk").splitlines()[0])
        body = recipe("_release-quest-apk")
        self.assertIn(r"^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$", body)

    def test_it_compiles_nothing(self):
        body = recipe("_release-quest-apk")
        for forbidden in ("_quest-apk", "cmake", "GITHUB_ACTIONS", "build-android", "android-dev"):
            self.assertNotIn(forbidden, body)

    def test_it_packages_the_release_asset_and_refuses_unless_the_apk_carries_it(self):
        body = recipe("_release-quest-apk")
        fetch = body.index("tools/quest_release_loader.py fetch --tag")
        identity = body.index("tools/package_release.py quest-library")
        repack = body.index("just android-repack-apk")
        verify = body.index("tools/quest_release_loader.py verify-apk")
        copy = body.index('cp -f "$out/repack/r15_nevr-sentinel_signed.apk"')
        self.assertLess(fetch, identity)
        self.assertLess(identity, repack)
        self.assertLess(repack, verify)
        self.assertLess(verify, copy)
        self.assertIn('--version "$ver" --commit "$tag_commit"', body)
        self.assertIn('"$out/loader/libovrplatformloader.so"', body[repack:verify])
        self.assertIn('"$ver"', body[repack:verify])  # the versionCode comes from the tag, not from a build tree
        self.assertIn('--sha256 "$loader_sha"', body[verify:copy])

    def test_the_shared_recipe_builds_a_development_package_only(self):
        body = recipe("_quest-apk")
        self.assertNotIn("--fresh", body)
        self.assertNotIn("mode", body.splitlines()[0])

    def test_the_repack_takes_a_version_where_there_is_no_build_info(self):
        body = recipe("android-repack-apk")
        self.assertIn('version=""', body.splitlines()[0])
        self.assertIn('--version "$version"', body)

    def test_package_dev_and_the_release_share_one_quest_build(self):
        self.assertIn('just _quest-apk "{{ store_apk }}"', recipe("package-dev"))
        self.assertNotIn("just android-repack-apk", recipe("package-dev"))


SHA = lambda data: hashlib.sha256(data).hexdigest()  # noqa: E731

# Stand-ins for the tools the release recipes call, which need the signing key, the store APK and a release. Each records
# its arguments in $CALLS.
STUB_PREFLIGHT = """#!/usr/bin/env python3
import os, sys
open(os.environ["CALLS"], "a").write("preflight " + " ".join(sys.argv[1:]) + "\\n")
"""
STUB_KEYSTORE = """#!/bin/sh
echo "keystore $*" >> "$CALLS"
"""
STUB_LOADER = """import hashlib, os, pathlib, sys
a = sys.argv[1:]
open(os.environ["CALLS"], "a").write("loader " + " ".join(a) + "\\n")
if a[0] == "fetch":
    out = pathlib.Path(a[a.index("--out") + 1]); out.mkdir(parents=True, exist_ok=True)
    data = b"loader asset of " + a[a.index("--tag") + 1].encode()
    (out / "libovrplatformloader.so").write_bytes(data)
    print(hashlib.sha256(data).hexdigest())
elif a[0] == "verify-apk":
    sys.exit(0 if a[a.index("--sha256") + 1] == hashlib.sha256(b"loader asset of v1.2.3").hexdigest() else 1)
"""
STUB_PACKAGE = """import os, sys
open(os.environ["CALLS"], "a").write("package " + " ".join(sys.argv[1:]) + "\\n")
"""
# The repack recipe with the same parameters as the real one, writing a "signed APK" that shows what it was given.
STUB_REPACK = """android-repack-apk apk shim="s" workdir="w" plugin="" version="":
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p "{{ workdir }}"
    echo "repack apk={{ apk }} shim={{ shim }} workdir={{ workdir }} plugin={{ plugin }} version={{ version }}" >> "$CALLS"
    printf 'signed plugin=[%s] version=%s' "{{ plugin }}" "{{ version }}" > "{{ workdir }}/r15_nevr-sentinel_signed.apk"
"""
# The same recipe with the parameters the real one has since the bundled pack (docs/adr/0029-quest-bundled-content-pack.md).
STUB_REPACK_PACK = """android-repack-apk apk shim="s" workdir="w" plugin="" version="" extra_rows="" patcher="" json_include="" plugin_early="" plugin_off="" pack="" pack_meta="" extra_libs="":
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p "{{ workdir }}"
    echo "repack apk={{ apk }} plugin={{ plugin }} version={{ version }} others=[{{ extra_rows }}|{{ patcher }}|{{ json_include }}|{{ plugin_early }}|{{ plugin_off }}] pack={{ pack }} pack_meta={{ pack_meta }}" >> "$CALLS"
    printf 'signed' > "{{ workdir }}/r15_nevr-sentinel_signed.apk"
"""
PLUGINS = "/priv/libq15t.so:/priv/libq15t_early.so"
# The repack with the script-library parameter (M5): it records the library directory it was given.
STUB_REPACK_LIBS = """android-repack-apk apk shim="s" workdir="w" plugin="" version="" extra_rows="" patcher="" json_include="" plugin_early="" plugin_off="" pack="" pack_meta="" extra_libs="":
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p "{{ workdir }}"
    echo "repack apk={{ apk }} plugin={{ plugin }} version={{ version }} pack={{ pack }} pack_meta={{ pack_meta }} libs={{ extra_libs }}" >> "$CALLS"
    printf 'signed' > "{{ workdir }}/r15_nevr-sentinel_signed.apk"
"""


class RecipesRun(unittest.TestCase):
    """The recipe texts as they are in the justfile, run by `just` in a throwaway repository with stand-ins for the tools
    that need the signing key and a release."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = pathlib.Path(self.tmp.name) / "repo"
        (self.dir / "tools").mkdir(parents=True)
        self.calls = pathlib.Path(self.tmp.name) / "calls.log"
        self.store = pathlib.Path(self.tmp.name) / "store.apk"
        self.store.write_bytes(b"store")
        texts = [recipe(name) for name in ("release-quest-apk", "release-quest-variant-apk", "release-quest-combat-apk", "_release-quest-apk")]
        (self.dir / "justfile").write_text("\n".join(texts) + "\n" + STUB_REPACK, encoding="utf-8")
        for name, text in (("release_preflight.py", STUB_PREFLIGHT), ("quest_release_loader.py", STUB_LOADER),
                           ("package_release.py", STUB_PACKAGE)):
            (self.dir / "tools" / name).write_text(text)
        keystore = self.dir / "tools" / "quest_keystore.sh"
        keystore.write_text(STUB_KEYSTORE)
        for executable in (keystore, self.dir / "tools" / "release_preflight.py"):  # the recipe runs these two directly
            executable.chmod(0o755)
        git = ["git", "-C", str(self.dir), "-c", "user.name=t", "-c", "user.email=t@example.invalid",
               "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false"]
        for argv in (["init", "-q", "-b", "main"], ["commit", "-q", "--allow-empty", "-m", "x"], ["tag", "v1.2.3"]):
            subprocess.run([*git, *argv], check=True, capture_output=True)
        self.head = subprocess.run([*git, "rev-parse", "HEAD"], check=True, capture_output=True, text=True).stdout.strip()

    def just(self, *args):
        env = {**os.environ, "CALLS": str(self.calls)}
        return subprocess.run(["just", *args], cwd=self.dir, capture_output=True, text=True, env=env)

    def calls_made(self):
        return self.calls.read_text().splitlines() if self.calls.exists() else []

    def release(self):
        return self.just("release-quest-apk", "v1.2.3", str(self.store))

    def variant(self, tag="v1.2.3", plugins=PLUGINS):
        return self.just("release-quest-variant-apk", tag, plugins, str(self.store))

    def test_the_release_recipe_writes_the_same_files_and_says_the_same_as_before(self):
        result = self.release()
        self.assertEqual(result.returncode, 0, result.stderr)
        out = self.dir / "build" / "quest-release" / "v1.2.3"
        name = "nevr-runtime-v1.2.3-quest.apk"
        apk = (out / name).read_bytes()
        self.assertEqual(apk, b"signed plugin=[] version=1.2.3", "the release APK carries no plugin")
        self.assertEqual((out / f"{name}.sha256").read_text(), f"{SHA(apk)}  {name}\n")
        self.assertEqual(result.stdout, f"{SHA(apk)}  {name}\n")
        loader_sha = SHA(b"loader asset of v1.2.3")
        self.assertEqual(result.stderr, (
            f"release-quest-apk: packaging the v1.2.3 loader asset at {self.head[:7]}\n"
            f"release-quest-apk: build/quest-release/v1.2.3/{name} is v1.2.3 at {self.head[:7]}, with the release's loader "
            f"(sha256 {loader_sha}). It is NOT a release asset: publish it outside GitHub (RELEASING.md).\n"))
        self.assertEqual(self.calls_made(), [
            "preflight --tag v1.2.3", "keystore check",
            "loader fetch --tag v1.2.3 --out build/quest-release/v1.2.3/loader",
            "package quest-library --lib build/quest-release/v1.2.3/loader/libovrplatformloader.so --version 1.2.3 "
            f"--commit {self.head}",
            f"repack apk={self.store} shim=build/quest-release/v1.2.3/loader/libovrplatformloader.so "
            "workdir=build/quest-release/v1.2.3/repack plugin= version=1.2.3",
            f"loader verify-apk --apk build/quest-release/v1.2.3/repack/r15_nevr-sentinel_signed.apk --sha256 {loader_sha}"])

    def test_the_release_recipe_still_refuses_a_non_release_tag_in_its_own_name(self):
        result = self.just("release-quest-apk", "v1.2", str(self.store))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release-quest-apk: not a release tag (vX.Y.Z): v1.2\n", result.stderr)
        self.assertEqual(self.calls_made(), [])

    def test_the_variant_recipe_writes_its_apk_and_sidecar_under_the_release_build_directory(self):
        result = self.variant()
        self.assertEqual(result.returncode, 0, result.stderr)
        out = self.dir / "build" / "quest-release" / "v1.2.3" / "variant"
        name = "nevr-runtime-v1.2.3-q15t-quest.apk"
        apk = (out / name).read_bytes()
        self.assertEqual((out / f"{name}.sha256").read_text(), f"{SHA(apk)}  {name}\n", "sha256sum format, of that name")
        self.assertEqual(result.stdout, f"{SHA(apk)}  {name}\n")
        self.assertEqual(sorted(p.name for p in out.iterdir()), sorted([name, f"{name}.sha256", "loader", "repack"]))
        self.assertFalse((self.dir / "build" / "quest-release" / "v1.2.3" / "nevr-runtime-v1.2.3-quest.apk").exists())

    def test_a_rerun_starts_from_an_empty_output_directory(self):
        stale = self.dir / "build/quest-release/v1.2.3/variant/stale.txt"
        stale.parent.mkdir(parents=True)
        stale.write_text("left by an earlier run")
        self.assertEqual(self.variant().returncode, 0)
        self.assertFalse(stale.exists())

    def test_the_variant_recipe_passes_the_libraries_colon_separated_to_the_repack_and_the_release_version(self):
        self.assertEqual(self.variant().returncode, 0)
        repack = [c for c in self.calls_made() if c.startswith("repack ")]
        self.assertEqual(repack, [f"repack apk={self.store} shim=build/quest-release/v1.2.3/variant/loader/"
                                  "libovrplatformloader.so workdir=build/quest-release/v1.2.3/variant/repack "
                                  f"plugin={PLUGINS} version=1.2.3"])
        apk = (self.dir / "build/quest-release/v1.2.3/variant/nevr-runtime-v1.2.3-q15t-quest.apk").read_bytes()
        self.assertEqual(apk, f"signed plugin=[{PLUGINS}] version=1.2.3".encode())

    def test_the_variants_loader_is_the_releases_asset_and_the_apk_is_checked_against_it(self):
        self.assertEqual(self.variant().returncode, 0)
        calls = self.calls_made()
        loader_sha = SHA(b"loader asset of v1.2.3")
        self.assertIn("preflight --tag v1.2.3", calls)
        self.assertIn("keystore check", calls)
        self.assertIn("loader fetch --tag v1.2.3 --out build/quest-release/v1.2.3/variant/loader", calls)
        self.assertIn("loader verify-apk --apk build/quest-release/v1.2.3/variant/repack/r15_nevr-sentinel_signed.apk "
                      f"--sha256 {loader_sha}", calls)
        self.assertLess([i for i, c in enumerate(calls) if c.startswith("loader fetch")][0],
                        [i for i, c in enumerate(calls) if c.startswith("repack")][0])
        self.assertLess([i for i, c in enumerate(calls) if c.startswith("repack")][0],
                        [i for i, c in enumerate(calls) if c.startswith("loader verify-apk")][0])

    def test_the_variant_recipe_leaves_the_release_apk_where_it_is(self):
        self.assertEqual(self.release().returncode, 0)
        main = self.dir / "build/quest-release/v1.2.3/nevr-runtime-v1.2.3-quest.apk"
        before = main.read_bytes()
        self.assertEqual(self.variant().returncode, 0)
        self.assertEqual(main.read_bytes(), before)
        self.assertTrue((main.parent / f"{main.name}.sha256").exists())

    def test_the_variant_recipe_refuses_a_non_release_tag_and_missing_libraries_in_its_own_name(self):
        for tag, plugins, want in (("v1.2", PLUGINS, "release-quest-variant-apk: not a release tag (vX.Y.Z): v1.2"),
                                   ("v1.2.3", "", "release-quest-variant-apk: ")):
            with self.subTest(tag=tag, plugins=plugins):
                result = self.variant(tag, plugins)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(want, result.stderr)
                self.assertNotIn("repack ", "\n".join(self.calls_made()))

    def test_the_release_apk_never_carries_a_plugin(self):
        result = self.just("_release-quest-apk", "release-quest-apk", "v1.2.3", str(self.store), PLUGINS, "", "", "")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release-quest-apk: a release APK carries no plugin library", result.stderr)
        self.assertNotIn("repack ", "\n".join(self.calls_made()))

    def test_the_combat_recipe_writes_the_release_name_under_the_combat_directory(self):
        self.use_pack_stub()
        result = self.just("release-quest-combat-apk", "v1.2.3", PLUGINS, str(self.store))
        self.assertEqual(result.returncode, 0, result.stderr)
        out = self.dir / "build" / "quest-release" / "v1.2.3" / "combat"
        name = "nevr-runtime-v1.2.3-quest.apk"
        apk = (out / name).read_bytes()
        self.assertEqual(apk, b"signed", "the stand-in repack's output, copied under the release name")
        self.assertEqual((out / f"{name}.sha256").read_text(), f"{SHA(apk)}  {name}\n", "sha256sum format, of the release name")
        self.assertEqual(result.stdout, f"{SHA(apk)}  {name}\n")
        self.assertEqual(sorted(p.name for p in out.iterdir()), sorted([name, f"{name}.sha256", "loader", "repack"]))
        self.assertFalse((self.dir / "build" / "quest-release" / "v1.2.3" / "variant").exists())
        self.assertFalse((self.dir / "build" / "quest-release" / "v1.2.3" / name).exists())

    def test_the_combat_recipe_names_libq15t_early_as_the_early_plugin_and_nothing_else_does(self):
        self.use_pack_stub()
        self.assertEqual(self.just("release-quest-combat-apk", "v1.2.3", PLUGINS, str(self.store)).returncode, 0)
        self.assertEqual(self.variant().returncode, 0)
        self.assertEqual(self.release().returncode, 0)
        repack = [c for c in self.calls_made() if c.startswith("repack ")]
        self.assertEqual(repack[0], f"repack apk={self.store} plugin={PLUGINS} version=1.2.3 "
                         "others=[|||libq15t_early.so|] pack= pack_meta=")
        self.assertEqual(repack[1], f"repack apk={self.store} plugin={PLUGINS} version=1.2.3 others=[||||] pack= pack_meta=")
        self.assertEqual(repack[2], f"repack apk={self.store} plugin= version=1.2.3 others=[||||] pack= pack_meta=")

    def test_the_combat_recipe_with_a_pack_passes_the_early_plugin_with_the_pack(self):
        self.use_pack_stub()
        result = self.just("release-quest-combat-apk", "v1.2.3", PLUGINS, str(self.store), "/p/pack.zip", "/p/meta.json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"repack apk={self.store} plugin={PLUGINS} version=1.2.3 others=[|||libq15t_early.so|] "
                      "pack=/p/pack.zip pack_meta=/p/meta.json", self.calls_made())

    def test_the_combat_recipe_refuses_a_non_release_tag_and_a_missing_plugin_in_its_own_name(self):
        for tag, plugins, want in (
                ("v1.2", PLUGINS, "release-quest-combat-apk: not a release tag (vX.Y.Z): v1.2"),
                ("v1.2.3", "", "release-quest-combat-apk: the variant APK carries plugin libraries, and none is named")):
            with self.subTest(tag=tag, plugins=plugins):
                result = self.just("release-quest-combat-apk", tag, plugins, str(self.store))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(want, result.stderr)
                self.assertNotIn("repack ", "\n".join(self.calls_made()))

    # ---- the bundled game-data pack (docs/adr/0029-quest-bundled-content-pack.md) ----

    def use_pack_stub(self):
        texts = [recipe(name) for name in ("release-quest-apk", "release-quest-variant-apk", "release-quest-combat-apk", "_release-quest-apk")]
        (self.dir / "justfile").write_text("\n".join(texts) + "\n" + STUB_REPACK_PACK, encoding="utf-8")

    def test_the_variant_recipe_passes_the_pack_and_its_description_to_the_repack(self):
        self.use_pack_stub()
        result = self.just("release-quest-variant-apk", "v1.2.3", PLUGINS, str(self.store), "/p/pack.zip", "/p/meta.json")
        self.assertEqual(result.returncode, 0, result.stderr)
        repack = [c for c in self.calls_made() if c.startswith("repack ")]
        self.assertEqual(repack, [f"repack apk={self.store} plugin={PLUGINS} version=1.2.3 others=[||||] "
                                  "pack=/p/pack.zip pack_meta=/p/meta.json"])

    def test_without_a_pack_the_repack_is_given_none_and_the_call_is_the_one_it_was_before(self):
        self.use_pack_stub()
        self.assertEqual(self.variant().returncode, 0)
        self.assertEqual(self.release().returncode, 0)
        repack = [c for c in self.calls_made() if c.startswith("repack ")]
        self.assertEqual(len(repack), 2)
        for call in repack:
            self.assertTrue(call.endswith("others=[||||] pack= pack_meta="), call)

    def test_the_release_apk_never_carries_a_pack(self):
        result = self.just("_release-quest-apk", "release-quest-apk", "v1.2.3", str(self.store), "", "", "/p/pack.zip", "/p/meta.json")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release-quest-apk: a release APK carries no game-data pack", result.stderr)
        self.assertNotIn("repack ", "\n".join(self.calls_made()))

    def test_the_combat_recipe_passes_the_script_library_directory_to_the_repack(self):
        texts = [recipe(name) for name in ("release-quest-apk", "release-quest-variant-apk", "release-quest-combat-apk",
                                           "_release-quest-apk")]
        (self.dir / "justfile").write_text("\n".join(texts) + "\n" + STUB_REPACK_LIBS, encoding="utf-8")
        result = self.just("release-quest-combat-apk", "v1.2.3", PLUGINS, str(self.store), "", "", "/p/libs")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"repack apk={self.store} plugin={PLUGINS} version=1.2.3 pack= pack_meta= libs=/p/libs",
                      self.calls_made())

    def test_a_release_apk_or_a_variant_never_takes_script_libraries(self):
        result = self.just("_release-quest-apk", "release-quest-apk", "v1.2.3", str(self.store), "", "", "", "", "/p/libs")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release-quest-apk: a release APK carries no script library", result.stderr)
        self.assertNotIn("repack ", "\n".join(self.calls_made()))

    def test_a_description_with_no_pack_is_refused_before_the_repack(self):
        self.use_pack_stub()
        result = self.just("release-quest-variant-apk", "v1.2.3", PLUGINS, str(self.store), "", "/p/meta.json")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release-quest-variant-apk: pack_meta describes a pack, and no pack is given", result.stderr)
        self.assertNotIn("repack ", "\n".join(self.calls_made()))


class RecipeText(unittest.TestCase):
    def test_the_variant_recipe_sits_beside_the_release_recipe_and_takes_a_tag_and_the_plugins(self):
        self.assertIn("release-quest-variant-apk tag plugins", recipe("release-quest-variant-apk").splitlines()[0])
        self.assertLess(JUSTFILE.index("\nrelease-quest-apk "), JUSTFILE.index("\nrelease-quest-variant-apk "))

    def test_the_variant_recipe_takes_the_store_apk_the_release_recipe_takes(self):
        def default(name):
            return re.search(r'store_apk=(env\("[^"]+"\)|"[^"]+")', recipe(name).splitlines()[0]).group(1)

        self.assertEqual(default("release-quest-variant-apk"), default("release-quest-apk"))

    def test_only_the_variant_recipe_takes_a_pack(self):
        self.assertIn('pack="" pack_meta=""', recipe("release-quest-variant-apk").splitlines()[0])
        self.assertNotIn("pack", recipe("release-quest-apk").splitlines()[0])

    def test_the_combat_recipe_takes_plugins_and_a_pack_and_runs_the_shared_body_as_combat(self):
        line = recipe("release-quest-combat-apk").splitlines()[0]
        self.assertIn('release-quest-combat-apk tag plugins store_apk=env("NEVR_STORE_APK") pack="" pack_meta="" libs=""', line)
        self.assertIn('_release-quest-apk "release-quest-combat-apk" tag store_apk plugins "combat" pack pack_meta libs',
                      recipe("release-quest-combat-apk"))

    def test_both_release_recipes_run_the_one_shared_body(self):
        self.assertIn('_release-quest-apk "release-quest-apk" tag store_apk "" ""', recipe("release-quest-apk"))
        self.assertIn('_release-quest-apk "release-quest-variant-apk" tag store_apk plugins "q15t"',
                      recipe("release-quest-variant-apk"))
        self.assertNotIn("android-repack-apk", recipe("release-quest-variant-apk"))


if __name__ == "__main__":
    unittest.main()
