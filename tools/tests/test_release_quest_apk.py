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
import shlex
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

    def test_the_repack_names_its_variant_as_its_last_parameter(self):
        self.assertTrue(recipe("android-repack-apk").splitlines()[0].split(":")[0].rstrip().endswith('variant=""'))

    def test_every_recipe_that_repacks_an_apk_passes_a_variant_word(self):
        """#2170: the repack has 13 parameters before `variant` and every call in the justfile gives all 14, so none leaves the word
        to a default it did not choose."""
        calls = []
        for number, line in enumerate(JUSTFILE.splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith("just android-repack-apk "):
                calls.append((number, shlex.split(stripped)[2:]))
        self.assertGreaterEqual(len(calls), 4, "package-dev's _quest-apk, the autopilot APK, the store variant and the release body")
        for number, args in calls:
            self.assertEqual(len(args), 14, f"justfile:{number} passes {len(args)} of the repack's 14 arguments: {args}")
            self.assertIn(args[13], ("", "$variant"), f"justfile:{number} passes the variant word {args[13]!r}")

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
# tools/quest_release_base.py (#1994): `prepare` writes the base it was asked for; either command fails when $FAIL_BASE names it.
STUB_BASE = """import os, pathlib, sys
a = sys.argv[1:]
open(os.environ["CALLS"], "a").write("base " + " ".join(a) + "\\n")
if os.environ.get("FAIL_BASE") == a[0]:
    sys.exit(1)
if a[0] == "prepare":
    out = pathlib.Path(a[a.index("--out") + 1]); out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(b"reference base")
"""
# The repack recipe with the same parameters as the real one, writing a "signed APK" that shows what it was given.
STUB_REPACK = """android-repack-apk apk shim="s" workdir="w" plugin="" version="" extra_rows="" patcher="" json_include="" plugin_early="" plugin_off="" pack="" pack_meta="" extra_libs="" variant="":
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p "{{ workdir }}"
    echo "{{ variant }}" >> "$VARIANTS"
    echo "repack apk={{ apk }} shim={{ shim }} workdir={{ workdir }} plugin={{ plugin }} version={{ version }}" >> "$CALLS"
    printf 'signed plugin=[%s] version=%s' "{{ plugin }}" "{{ version }}" > "{{ workdir }}/r15_nevr-sentinel_signed.apk"
"""
# The same recipe with the parameters the real one has since the bundled pack (docs/adr/0029-quest-bundled-content-pack.md).
STUB_REPACK_PACK = """android-repack-apk apk shim="s" workdir="w" plugin="" version="" extra_rows="" patcher="" json_include="" plugin_early="" plugin_off="" pack="" pack_meta="" extra_libs="" variant="":
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p "{{ workdir }}"
    echo "{{ variant }}" >> "$VARIANTS"
    echo "repack apk={{ apk }} plugin={{ plugin }} version={{ version }} others=[{{ extra_rows }}|{{ patcher }}|{{ json_include }}|{{ plugin_early }}|{{ plugin_off }}] pack={{ pack }} pack_meta={{ pack_meta }}" >> "$CALLS"
    printf 'signed' > "{{ workdir }}/r15_nevr-sentinel_signed.apk"
"""
# A trap: no justfile has a q15t library build, so a release recipe that runs one is recorded here ("q15t-build" in the calls).
STUB_Q15T_TRAP = """build-quest-q15t:
    #!/usr/bin/env bash
    set -euo pipefail
    echo "q15t-build" >> "$CALLS"
    mkdir -p build/quest-q15t
    printf 'libq15t built from the tag' > build/quest-q15t/libq15t.so
"""
# The old call shape, `<tag> <plugins> <store>`: a plugin library passed in.
PLUGINS = "/priv/libq15t.so:/priv/other.so"
# The repack with the script-library parameter (M5): it records the library directory it was given.
STUB_REPACK_LIBS = """android-repack-apk apk shim="s" workdir="w" plugin="" version="" extra_rows="" patcher="" json_include="" plugin_early="" plugin_off="" pack="" pack_meta="" extra_libs="" variant="":
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p "{{ workdir }}"
    echo "{{ variant }}" >> "$VARIANTS"
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
        self.variants = pathlib.Path(self.tmp.name) / "variants.log"  # the variant word each repack was given, one line each
        self.store = pathlib.Path(self.tmp.name) / "store.apk"
        self.store.write_bytes(b"store")
        texts = [recipe(name) for name in ("release-quest-apk", "release-quest-reference-apk", "release-quest-variant-apk",
                                           "release-quest-combat-apk", "release-quest-arena-apk", "_release-quest-apk")]
        (self.dir / "justfile").write_text("\n".join(texts) + "\n" + STUB_REPACK + "\n" + STUB_Q15T_TRAP, encoding="utf-8")
        for name, text in (("release_preflight.py", STUB_PREFLIGHT), ("quest_release_loader.py", STUB_LOADER),
                           ("package_release.py", STUB_PACKAGE), ("quest_release_base.py", STUB_BASE)):
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

    def just(self, *args, **extra_env):
        env = {**os.environ, "CALLS": str(self.calls), "VARIANTS": str(self.variants), **extra_env}
        return subprocess.run(["just", *args], cwd=self.dir, capture_output=True, text=True, env=env)

    def variants_given(self):
        return self.variants.read_text().splitlines() if self.variants.exists() else []

    def calls_made(self):
        return self.calls.read_text().splitlines() if self.calls.exists() else []

    def release(self):
        """The reference repack (#1994), asked for by name (docs/adr/0047): the APK the manifest lists as quest-apk."""
        return self.just("release-quest-reference-apk", "v1.2.3", str(self.store))

    def default(self, tag="v1.2.3", *more, **env):
        """`just release-quest-apk <tag>` with no variant argument: the default build (docs/adr/0047)."""
        return self.just("release-quest-apk", tag, str(self.store), *more, **env)

    def variant(self, tag="v1.2.3", *more, **env):
        return self.just("release-quest-variant-apk", tag, str(self.store), *more, **env)

    def arena(self, tag="v1.2.3", *more, **env):
        return self.just("release-quest-arena-apk", tag, str(self.store), *more, **env)

    def combat(self, tag="v1.2.3", *more, **env):
        return self.just("release-quest-combat-apk", tag, str(self.store), *more, **env)

    def test_the_default_recipe_builds_arena_from_the_store_apk_and_no_reference_build(self):
        """docs/adr/0047: `release-quest-apk <tag>` with no variant argument repacks the store APK it is given, never the reference
        build: no base is prepared or checked, the variant word is `arena`, check-arena refuses any reference entry (its libvrapi.so
        runtime included) before the APK is written, and the output is the arena APK. No combat APK is written."""
        result = self.default()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.variants_given(), ["arena"])
        calls = self.calls_made()
        self.assertFalse([c for c in calls if c.startswith("base prepare") or c.startswith("base check-release")])
        repack = [c for c in calls if c.startswith("repack ")]
        self.assertEqual(len(repack), 1)
        self.assertEqual([c for c in calls if c.endswith("-build")], [], "no plugin library is built")
        self.assertIn(f"apk={self.store} ", repack[0])
        self.assertEqual(repack[0].split(" plugin=")[1].split(" ")[0], "")
        self.assertIn("base check-arena --apk build/quest-release/v1.2.3/arena/repack/r15_nevr-sentinel_signed.apk", calls)
        release = self.dir / "build" / "quest-release" / "v1.2.3"
        name = "nevr-runtime-v1.2.3-arena-quest.apk"
        apk = (release / "arena" / name).read_bytes()
        self.assertEqual(result.stdout, f"{SHA(apk)}  {name}\n")
        for combat in ("nevr-runtime-v1.2.3-quest.apk", "combat", "variant", "base"):
            self.assertFalse((release / combat).exists(), f"{combat} is written only by a recipe asked for by name")
        self.assertIn("release-quest-apk: build/quest-release/v1.2.3/arena/" + name, result.stderr)

    def test_the_default_recipe_writes_nothing_when_check_arena_refuses(self):
        result = self.default(FAIL_BASE="check-arena")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.dir / "build/quest-release/v1.2.3/arena/nevr-runtime-v1.2.3-arena-quest.apk").exists())

    def test_the_default_recipe_refuses_a_non_release_tag_in_its_own_name_and_builds_nothing(self):
        result = self.default("v1.2")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release-quest-apk: not a release tag (vX.Y.Z): v1.2\n", result.stderr)
        self.assertEqual(self.calls_made(), [])

    def test_the_reference_recipe_writes_the_same_files_and_says_the_same_as_before(self):
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
            f"release-quest-reference-apk: packaging the v1.2.3 loader asset at {self.head[:7]}\n"
            f"release-quest-reference-apk: build/quest-release/v1.2.3/{name} is v1.2.3 at {self.head[:7]}, with the release's loader "
            f"(sha256 {loader_sha}). It is NOT a release asset: publish it outside GitHub (RELEASING.md).\n"))
        self.assertEqual(self.calls_made(), [
            "preflight --tag v1.2.3", "keystore check",
            f"base prepare --apk {self.store} --out build/quest-release/v1.2.3/base/reference-r15.apk",
            "loader fetch --tag v1.2.3 --out build/quest-release/v1.2.3/loader",
            "package quest-library --lib build/quest-release/v1.2.3/loader/libovrplatformloader.so --version 1.2.3 "
            f"--commit {self.head}",
            "repack apk=build/quest-release/v1.2.3/base/reference-r15.apk "
            "shim=build/quest-release/v1.2.3/loader/libovrplatformloader.so "
            "workdir=build/quest-release/v1.2.3/repack plugin= version=1.2.3",
            f"loader verify-apk --apk build/quest-release/v1.2.3/repack/r15_nevr-sentinel_signed.apk --sha256 {loader_sha}",
            "base check-release --apk build/quest-release/v1.2.3/repack/r15_nevr-sentinel_signed.apk "
            "--base build/quest-release/v1.2.3/base/reference-r15.apk"])

    def test_an_input_that_is_not_the_reference_build_is_refused_before_the_repack(self):
        result = self.just("release-quest-reference-apk", "v1.2.3", str(self.store), FAIL_BASE="prepare")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("repack ", "\n".join(self.calls_made()))
        self.assertFalse((self.dir / "build/quest-release/v1.2.3/nevr-runtime-v1.2.3-quest.apk").exists())

    def test_an_apk_that_lacks_an_entry_of_its_base_is_not_written_out(self):
        result = self.just("release-quest-reference-apk", "v1.2.3", str(self.store), FAIL_BASE="check-release")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("repack ", "\n".join(self.calls_made()))
        self.assertFalse((self.dir / "build/quest-release/v1.2.3/nevr-runtime-v1.2.3-quest.apk").exists())

    def test_a_missing_reference_apk_is_refused_naming_its_variable(self):
        result = self.just("release-quest-reference-apk", "v1.2.3", str(self.store) + ".absent")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release-quest-reference-apk: reference APK not found", result.stderr)
        self.assertIn("NEVR_QUEST_REFERENCE_APK", result.stderr)
        self.assertNotIn("repack ", "\n".join(self.calls_made()))

    def test_the_variant_apk_keeps_the_store_apk_it_is_given(self):
        self.assertEqual(self.variant().returncode, 0)
        self.assertFalse([c for c in self.calls_made() if c.startswith("base ")])

    def test_the_reference_recipe_still_refuses_a_non_release_tag_in_its_own_name(self):
        result = self.just("release-quest-reference-apk", "v1.2", str(self.store))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release-quest-reference-apk: not a release tag (vX.Y.Z): v1.2\n", result.stderr)
        self.assertEqual(self.calls_made(), [])

    def test_the_arena_recipe_packages_the_store_apk_with_the_loader_and_no_plugin(self):
        result = self.arena()
        self.assertEqual(result.returncode, 0, result.stderr)
        out = self.dir / "build" / "quest-release" / "v1.2.3" / "arena"
        name = "nevr-runtime-v1.2.3-arena-quest.apk"
        apk = (out / name).read_bytes()
        self.assertEqual((out / f"{name}.sha256").read_text(), f"{SHA(apk)}  {name}\n")
        self.assertEqual(self.variants_given(), ["arena"])
        calls = self.calls_made()
        self.assertNotIn("q15t-build", calls, "arena has no q15t library")
        self.assertFalse([c for c in calls if c.startswith("base prepare")], "the base is the store APK, not the reference build")
        repack = [c for c in calls if c.startswith("repack ")][0]
        self.assertIn(f"apk={self.store} ", repack)
        self.assertEqual(repack.split(" plugin=")[1].split(" ")[0], "")
        self.assertIn(f"base check-arena --apk build/quest-release/v1.2.3/arena/repack/r15_nevr-sentinel_signed.apk", calls)
        self.assertFalse([c for c in calls if c.startswith("base check-release")])
        self.assertFalse((self.dir / "build" / "quest-release" / "v1.2.3" / "variant").exists(), "arena never shares the q15t variant's directory")

    def test_an_arena_apk_that_carries_combat_is_not_written_out(self):
        result = self.arena(FAIL_BASE="check-arena")
        self.assertNotEqual(result.returncode, 0)
        out = self.dir / "build" / "quest-release" / "v1.2.3" / "arena"
        self.assertFalse((out / "nevr-runtime-v1.2.3-arena-quest.apk").exists())

    def test_the_arena_recipe_refuses_a_non_release_tag_in_its_own_name_and_builds_nothing(self):
        result = self.arena("v1.2")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release-quest-arena-apk: not a release tag (vX.Y.Z): v1.2\n", result.stderr)
        self.assertEqual(self.calls_made(), [])

    def test_the_arena_recipe_leaves_the_other_apks_where_they_are(self):
        self.assertEqual(self.release().returncode, 0)
        main = self.dir / "build" / "quest-release" / "v1.2.3" / "nevr-runtime-v1.2.3-quest.apk"
        kept = main.read_bytes()
        self.assertEqual(self.arena().returncode, 0)
        self.assertEqual(main.read_bytes(), kept)

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

    def test_the_variant_recipe_passes_an_empty_plugin_argument_and_the_release_version_to_the_repack(self):
        self.assertEqual(self.variant().returncode, 0)
        repack = [c for c in self.calls_made() if c.startswith("repack ")]
        self.assertEqual(repack, [f"repack apk={self.store} shim=build/quest-release/v1.2.3/variant/loader/"
                                  "libovrplatformloader.so workdir=build/quest-release/v1.2.3/variant/repack "
                                  "plugin= version=1.2.3"])
        apk = (self.dir / "build/quest-release/v1.2.3/variant/nevr-runtime-v1.2.3-q15t-quest.apk").read_bytes()
        self.assertEqual(apk, b"signed plugin=[] version=1.2.3", "the variant APK carries no plugin library")

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

    def test_the_variant_recipe_refuses_a_non_release_tag_in_its_own_name_and_builds_nothing(self):
        result = self.variant("v1.2")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release-quest-variant-apk: not a release tag (vX.Y.Z): v1.2", result.stderr)
        self.assertEqual(self.calls_made(), [])

    def test_each_recipe_names_its_variant_to_the_repack(self):
        """The default is `arena`; the reference APK is a plain release (the empty word); the variant recipe's is `q15t`; the combat
        recipe's is `combat`."""
        for name, run, want in (("default", self.default, "arena"), ("reference", self.release, ""), ("variant", self.variant, "q15t"),
                                ("combat", self.combat, "combat")):
            with self.subTest(name=name):
                self.variants.unlink(missing_ok=True)
                result = run()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.variants_given(), [want])

    def test_the_release_apk_never_carries_a_plugin_and_never_builds_the_q15t_libraries(self):
        self.assertEqual(self.release().returncode, 0)
        calls = self.calls_made()
        self.assertNotIn("q15t-build", calls)
        self.assertEqual([c for c in calls if c.startswith("repack ")][0].split(" plugin=")[1].split(" ")[0], "")

    def test_a_variant_and_a_combat_apk_carry_no_plugin_library_and_the_recipes_run_no_q15t_build(self):
        """The variant word stamps the APK; no library is built from the tag, packaged or handed to the repack."""
        self.use_pack_stub()
        for name, run in (("variant", self.variant), ("combat", self.combat)):
            with self.subTest(name=name):
                self.calls.unlink(missing_ok=True)
                result = run()
                self.assertEqual(result.returncode, 0, result.stderr)
                calls = self.calls_made()
                self.assertNotIn("q15t-build", calls)
                repack = [c for c in calls if c.startswith("repack ")]
                self.assertEqual(len(repack), 1)
                self.assertIn(" plugin= ", repack[0], "the repack's plugin argument is empty")
                self.assertNotIn("libq15t", result.stderr)
                self.assertEqual(len(result.stdout.splitlines()), 1, "stdout is the APK's sha256 line only")
                self.assertFalse((self.dir / "build" / "quest-q15t").exists(), "no library directory is made")

    def test_neither_variant_apk_packages_an_early_plugin(self):
        """The heap sizes and the render-target pool are the sentinel's own early step, so the repack is given no library and no early name."""
        self.use_pack_stub()
        self.assertEqual(self.variant().returncode, 0)
        self.assertEqual(self.combat().returncode, 0)
        variant, combat = [c for c in self.calls_made() if c.startswith("repack ")]
        for apk in (variant, combat):
            self.assertIn(" plugin= ", apk)
            self.assertIn("others=[||||]", apk, "no early plugin")

    def test_a_plugin_library_passed_in_is_refused_in_the_recipes_name_before_anything_runs(self):
        """The old call shape `<tag> <plugins> <store>`: the APK carries no plugin library, so none is an argument."""
        for recipe_name in ("release-quest-variant-apk", "release-quest-combat-apk"):
            for given in (PLUGINS, "/priv/libq15t.so"):
                with self.subTest(recipe=recipe_name, given=given):
                    self.calls.unlink(missing_ok=True)
                    result = self.just(recipe_name, "v1.2.3", given, str(self.store))
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(f"{recipe_name}: ", result.stderr)
                    self.assertIn("no plugin library is an argument: the APK carries none, the combat modules are the sentinel's own", result.stderr)
                    self.assertEqual(self.calls_made(), [], "nothing ran: no preflight, no build, no repack")

    def test_the_combat_recipe_writes_the_release_name_under_the_combat_directory(self):
        self.use_pack_stub()
        result = self.combat()
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

    def test_no_recipe_names_an_early_plugin(self):
        self.use_pack_stub()
        self.assertEqual(self.combat().returncode, 0)
        self.assertEqual(self.variant().returncode, 0)
        self.assertEqual(self.release().returncode, 0)
        repack = [c for c in self.calls_made() if c.startswith("repack ")]
        self.assertEqual(repack[0], f"repack apk={self.store} plugin= version=1.2.3 others=[||||] pack= pack_meta=")
        self.assertEqual(repack[1], f"repack apk={self.store} plugin= version=1.2.3 others=[||||] pack= pack_meta=")
        # The release APK alone is repacked from the reference base (#1994); the combat APK and the variant keep the store APK.
        self.assertEqual(repack[2], "repack apk=build/quest-release/v1.2.3/base/reference-r15.apk plugin= version=1.2.3 "
                         "others=[||||] pack= pack_meta=")

    def test_the_combat_recipe_with_a_pack_passes_the_pack_and_no_early_plugin(self):
        self.use_pack_stub()
        result = self.combat("v1.2.3", "/p/pack.zip", "/p/meta.json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"repack apk={self.store} plugin= version=1.2.3 others=[||||] "
                      "pack=/p/pack.zip pack_meta=/p/meta.json", self.calls_made())

    def test_the_combat_recipe_refuses_a_non_release_tag_in_its_own_name_and_builds_nothing(self):
        result = self.combat("v1.2")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release-quest-combat-apk: not a release tag (vX.Y.Z): v1.2", result.stderr)
        self.assertEqual(self.calls_made(), [])

    # ---- the bundled game-data pack (docs/adr/0029-quest-bundled-content-pack.md) ----

    def use_pack_stub(self):
        texts = [recipe(name) for name in ("release-quest-apk", "release-quest-reference-apk", "release-quest-variant-apk",
                                           "release-quest-combat-apk", "_release-quest-apk")]
        (self.dir / "justfile").write_text("\n".join(texts) + "\n" + STUB_REPACK_PACK + "\n" + STUB_Q15T_TRAP, encoding="utf-8")

    def test_the_variant_recipe_passes_the_pack_and_its_description_to_the_repack(self):
        self.use_pack_stub()
        result = self.variant("v1.2.3", "/p/pack.zip", "/p/meta.json")
        self.assertEqual(result.returncode, 0, result.stderr)
        repack = [c for c in self.calls_made() if c.startswith("repack ")]
        self.assertEqual(repack, [f"repack apk={self.store} plugin= version=1.2.3 others=[||||] "
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
        result = self.just("_release-quest-apk", "release-quest-reference-apk", "v1.2.3", str(self.store), "", "/p/pack.zip", "/p/meta.json")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release-quest-reference-apk: a release APK carries no game-data pack", result.stderr)
        self.assertNotIn("repack ", "\n".join(self.calls_made()))

    def test_the_combat_recipe_passes_the_script_library_directory_to_the_repack(self):
        texts = [recipe(name) for name in ("release-quest-apk", "release-quest-reference-apk", "release-quest-variant-apk",
                                           "release-quest-combat-apk", "_release-quest-apk")]
        (self.dir / "justfile").write_text("\n".join(texts) + "\n" + STUB_REPACK_LIBS + "\n" + STUB_Q15T_TRAP, encoding="utf-8")
        result = self.combat("v1.2.3", "", "", "/p/libs")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"repack apk={self.store} plugin= version=1.2.3 pack= pack_meta= libs=/p/libs",
                      self.calls_made())

    def test_a_release_apk_or_a_variant_never_takes_script_libraries(self):
        result = self.just("_release-quest-apk", "release-quest-reference-apk", "v1.2.3", str(self.store), "", "", "", "/p/libs")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release-quest-reference-apk: a release APK carries no script library", result.stderr)
        self.assertNotIn("repack ", "\n".join(self.calls_made()))

    def test_a_description_with_no_pack_is_refused_before_the_repack(self):
        self.use_pack_stub()
        result = self.variant("v1.2.3", "", "/p/meta.json")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release-quest-variant-apk: pack_meta describes a pack, and no pack is given", result.stderr)
        self.assertNotIn("repack ", "\n".join(self.calls_made()))


class RecipeText(unittest.TestCase):
    def test_the_variant_recipe_sits_beside_the_release_recipe_and_takes_a_tag_and_no_plugins(self):
        self.assertIn("release-quest-variant-apk tag store_apk", recipe("release-quest-variant-apk").splitlines()[0])
        self.assertNotIn("plugins", recipe("release-quest-variant-apk").splitlines()[0])
        self.assertLess(JUSTFILE.index("\nrelease-quest-apk "), JUSTFILE.index("\nrelease-quest-variant-apk "))

    def test_the_default_takes_the_store_apk_and_only_the_reference_recipe_the_reference_build(self):
        """docs/adr/0047: the default recipe's input is the store APK; the reference build is an input only of the recipe that names it."""
        self.assertIn('release-quest-apk tag store_apk=env("NEVR_STORE_APK"):', recipe("release-quest-apk").splitlines()[0])
        self.assertIn('_release-quest-apk "release-quest-apk" tag store_apk "arena" "" "" ""', recipe("release-quest-apk"))
        self.assertIn('reference_apk=env("NEVR_QUEST_REFERENCE_APK")', recipe("release-quest-reference-apk").splitlines()[0])
        users = [m.group(1) for m in re.finditer(r"^([a-z_-]+)\b[^\n]*NEVR_QUEST_REFERENCE_APK", JUSTFILE, re.M)]
        self.assertEqual(users, ["release-quest-reference-apk"])
        self.assertIn('store_apk=env("NEVR_STORE_APK")', recipe("release-quest-variant-apk").splitlines()[0])

    def test_only_the_variant_recipe_takes_a_pack(self):
        self.assertIn('pack="" pack_meta=""', recipe("release-quest-variant-apk").splitlines()[0])
        self.assertNotIn("pack", recipe("release-quest-apk").splitlines()[0])

    def test_the_combat_recipe_takes_a_pack_and_runs_the_shared_body_as_combat(self):
        line = recipe("release-quest-combat-apk").splitlines()[0]
        self.assertIn('release-quest-combat-apk tag store_apk=env("NEVR_STORE_APK") pack="" pack_meta="" libs=""', line)
        self.assertIn('_release-quest-apk "release-quest-combat-apk" tag store_apk "combat" pack pack_meta libs',
                      recipe("release-quest-combat-apk"))

    def test_the_arena_recipe_takes_the_store_apk_and_no_pack_and_runs_the_shared_body_as_arena(self):
        self.assertIn('release-quest-arena-apk tag store_apk=env("NEVR_STORE_APK"):', recipe("release-quest-arena-apk").splitlines()[0])
        self.assertIn('_release-quest-apk "release-quest-arena-apk" tag store_apk "arena" "" "" ""', recipe("release-quest-arena-apk"))
        self.assertIn("the arena APK carries no game-data pack and no script library", recipe("_release-quest-apk"))

    def test_both_release_recipes_run_the_one_shared_body(self):
        self.assertIn('_release-quest-apk "release-quest-reference-apk" tag reference_apk "" "" ""', recipe("release-quest-reference-apk"))
        self.assertIn('_release-quest-apk "release-quest-variant-apk" tag store_apk "q15t" pack pack_meta',
                      recipe("release-quest-variant-apk"))
        self.assertNotIn("android-repack-apk", recipe("release-quest-variant-apk"))


if __name__ == "__main__":
    unittest.main()
