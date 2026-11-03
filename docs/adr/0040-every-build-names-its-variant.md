# ADR 0040: every build names its variant, from one place, in its first log line and its crash reports

Status: proposed. Applies to every PC and Quest artifact. Builds on N112 (the build identity in logins and
registrations) and #1997 (the release redesign).

## Context

A tester's log or crash report has to say which build produced it, or a probe build's numbers and a release's are
indistinguishable. Today:

- The version comes from git (`X.Y.(Z+1)-dev.<N>+<sha>` for every build but a tag, AGENTS.md "Build Commands"), and the
  identity literal `NEVR-BUILD <version> <commit40>` binds the version to the commit.
- Nothing names the variant: a measurement build, the q15t variant and a release built at the same commit carry the same
  version string.
- A repacked Quest APK keeps the store's `versionName` (`4987566`); only the sentinel's build information carries the
  runtime's version.
- #1997 records that a dev build reused the release's versionCode `61604999`, and sets the rule that dev and variant
  builds derive a lower versionCode automatically, with the version and the variant name from one place.

## Decision

1. **One source.** One build step derives the version and the variant name, and everything else reads it:
   - the identity literal;
   - the sentinel's and the runtime's build information;
   - the Quest APK's versionCode and versionName;
   - the Windows zip's name.

   No recipe types a version or a versionCode.
2. **A variant name.** `release` for a tagged release; otherwise `dev` plus each option the build carries (for example
   `dev+q15t`, `dev+perf-probe`, `dev+combat`). ADR 0037's measurement options are among them.
3. **versionCode follows #1997.** A dev or variant build derives a versionCode below any release's, so it never reuses
   one.
4. **The first log line says it.** The sentinel's first line and the PC runtime's first line carry the version and the
   variant name, and so does every crash report (`docs/reference/crash-report.md`).

## Finding a violation

Scope: the `justfile`, `tools/`, `src/quest/CMakeLists.txt`, `src/runtime/CMakeLists.txt` and the build-information
sources.

- **B1.** A recipe or tool that sets a version or versionCode by hand.
- **B2.** An artifact whose identity, first log line or crash report lacks the variant name.
- **B3.** A Quest APK whose versionName is not the runtime's version and variant.

## Consequences

- A dev build cannot be installed over a release without a downgrade (Android refuses a lower versionCode unless the
  install allows it), so a tester moving between a release and a dev build is told how in the build's notes.

## Not claimed

How the derivation is implemented: that is #1997's work. This ADR fixes what every artifact and log must say.
