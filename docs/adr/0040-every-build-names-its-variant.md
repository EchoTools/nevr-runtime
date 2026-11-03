# ADR 0040: every build names its variant in its first log line and its crash reports

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
- **Build kind.** `dev` or `release` is named in several places: `NEVR-PLUGIN-TRUST dev|release`
  (`src/runtime/ext/plugin_trust.cpp`), `NEVR-HOST-BUILD dev|release` (`src/runtime/host/host_identity.cpp`), and the
  Quest login tuple (`src/quest/sentinel/entry.cpp`, `nevr_build::kIsDev ? "dev" : "release"`).
- **versionCode.** Derived from the version alone (`tools/quest_apk_version.py`): a release takes slot 999 and a
  development build slot N, so "a development build of X.Y.Z sits between the release before it and the release X.Y.Z".
  #1997 records the defect this rule exists for: a dev build stamped with a release's code. A release's variant APKs (the
  q15t and combat APKs) are stamped with the release's own version, since they carry the release's loader.
- **versionName.** It stays the store's on purpose: "it names the game, the versionCode names our packaging of it"
  (`tools/quest_apk_version.py`).

What is missing: `dev` or `release` does not say which variant a build is (a q15t or combat APK, a measurement build,
ADR 0037), and no first log line or crash report carries even that much on every platform.

## Decision

1. **Every artifact carries a variant name**: `release`, or `dev`, plus a word for each variant or option the recipe adds
   (`q15t`, `combat`, `perf-probe`). The recipe that makes the artifact sets it. A release's variant APKs share the
   release's loader, so their name is stamped by the packaging step, where the sentinel reads it, not by the loader build.
2. **The first log line says it.** The sentinel's first line and the PC runtime's first line carry the version, the build
   kind and the variant name, and so does every crash report (`docs/reference/crash-report.md`).
3. **versionCode stays as it is.** Every artifact is stamped through `tools/quest_apk_version.py`; no recipe types a
   versionCode, and a non-release build is never stamped with a release's version (#1997).
4. **Unchanged:** the identity literal's format and the APK's versionName.

## Finding a violation

Scope: the `justfile`, `tools/`, `src/quest/CMakeLists.txt`, `src/runtime/CMakeLists.txt` and the build-information
sources.

- **B1.** A recipe or tool that stamps a versionCode by hand, or stamps a non-release build with a release's version.
- **B2.** An artifact whose first log line or crash report lacks the version, the build kind or the variant name.

## Consequences

- A tester's log or crash report says which build ran, including which variant of a release.

## Not claimed

How the variant name reaches the sentinel in a variant APK (an asset, a manifest field); the first PR that implements
this decides, against #1997's work.
