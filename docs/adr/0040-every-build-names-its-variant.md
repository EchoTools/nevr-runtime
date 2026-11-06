---
# atwd read contract
read: whole
read_via: cat docs/adr/0040-every-build-names-its-variant.md
audience: all
---

# ADR 0040: every build names its kind and its variant in its first log line and its crash reports

Status: proposed. Applies to every PC and Quest artifact. Builds on N112 (the build identity in logins and
registrations) and #1997 (the release redesign).

## Context

A tester's log or crash report has to say which build produced it, or a probe build's numbers and a release's are
indistinguishable. What the tree has:

- **Version.** It comes from git: `X.Y.Z` for a CI build of a tag, `X.Y.(Z+1)-dev.<N>+<sha>` for every other build,
  "which sorts after the tag it descends from and before the next release" (AGENTS.md "Build Commands").
- **Identity literal.** `NEVR-BUILD <version> <commit40>` binds the version to the commit. Its format is a cross-repo
  contract: the release signer and `tools/package_release.py` read it with a fixed pattern, and the plugin-trust literal
  is kept separate because the identity literal "has no such field".
- **Build kind.** The words `dev` and `release` mean two things today. On Quest, the login tuple's
  `nevr_build::kIsDev ? "dev" : "release"` (`src/quest/sentinel/entry.cpp`) is `dev` for every build but a CI build of a
  release tag. On PC, `NEVR-PLUGIN-TRUST dev|release` (`src/runtime/ext/plugin_trust.cpp`) and
  `NEVR-HOST-BUILD dev|release` (`src/runtime/host/host_identity.cpp`) name the plugin-trust build
  (`NEVR_ALLOW_UNSIGNED_PLUGINS`, `src/runtime/CMakeLists.txt`), so a `just package-dev` build says `release` there and
  `dev` on Quest, with a `-dev` version on both.
- **versionCode.** Derived from the version alone (`tools/quest_apk_version.py`): a release takes slot 999 and a
  development build slot N, so "a development build of X.Y.Z sits between the release before it and the release X.Y.Z".
  #1997 records the defect this rule exists for: a dev build stamped with a release's code. A release's variant APKs (the
  q15t and combat APKs) are stamped with the release's own version, since they carry the release's loader.
- **versionName.** It stays the store's on purpose: "it names the game, the versionCode names our packaging of it"
  (`tools/quest_apk_version.py`).

What is missing: one meaning of the build kind on both platforms, a name for the variant (a q15t or combat APK, a
measurement build, ADR 0037), and a first log line and crash report that carry them on every platform.

## Decision

1. **Every artifact carries a build kind and a variant name.**
   - The build kind is `release` for a release build (ADR 0037 rule 2: the release recipes' artifacts for a tag, and
     `build.yml`'s from a release tag) and `dev` for every other build. Both platforms decide it by the rule
     `cmake/nevr_build_info.cmake` gives `nevr_build::kIsDev`: a version with no `-dev.` part, or a CI build of a release
     tag (rc tags included) that the CI ref names, is a release build, even when `git describe` gave it a `-dev.`
     version. The plugin-trust word keeps its meaning (N147) and is not the build kind.
   - The variant name is a word for each variant or option the recipe adds (`q15t`, `combat`, `perf-probe`), set by the
     recipe that makes the artifact. A release's variant APKs share the release's loader, so their name is stamped by the
     packaging step, where the sentinel reads it, not by the loader build.
   - On Quest the packaging step stamps the word into the APK: `just android-repack-apk` takes it as its last parameter, every
     recipe that repacks an APK passes it (empty for a plain release or a development build of the tree), refuses a word that is
     not one of the three, and writes `lib/arm64-v8a/libnevr_build_variant.so` holding the word as its exact bytes. The sentinel
     reads that file beside its own library before its first line (`src/quest/sentinel/build_variant.h`); a missing, unreadable or
     wrong stamp is no variant, and a second line says so.
2. **The first log line says it.** The sentinel's first line and the PC runtime's first line carry the version, the build
   kind and the variant name, and so does every crash report (`docs/reference/crash-report.md`).
3. **versionCode stays as it is.** Every Quest APK is stamped through `tools/quest_apk_version.py`; no recipe types a
   versionCode, and a development build is never stamped with a release's version (#1997).
4. **Unchanged:** the identity literal's format and the APK's versionName.

## Finding a violation

Scope: the `justfile`, `tools/`, `src/quest/CMakeLists.txt`, `src/runtime/CMakeLists.txt` and the build-information
sources.

- **B1.** A recipe or tool that stamps a versionCode by hand, or stamps a development build (rule 1) with a release's
  version.
- **B2.** An artifact whose first log line or crash report lacks the version, the build kind or the variant name.

## Consequences

- A tester's log or crash report says which build ran, including which variant of a release.

## Not claimed

The PC side of rule 2: the Quest sentinel's first line and crash report carry the version, the kind and the variant; the PC crash report
carries no build line (KNOWN-WRONG #2231).
