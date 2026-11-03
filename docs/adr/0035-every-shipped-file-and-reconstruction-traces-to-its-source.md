# ADR 0035: every file we ship and every reconstruction we make traces to its source

Status: proposed. Applies to every artifact a release or a dev build ships (the Quest APK, the Windows zip) and to every
reconstruction in the tree (a hook, a build-time row, a replaced function).

## Context

**Shipped files.** The release Quest APK is repacked from the pinned reference APK (`tools/quest_release_base.py`). Its
docstring names what that base contributes: about a hundred script libraries, `libassetpatch.so`, the reference
runtime's `libvrapi.so` and `assets/afk_default_on.txt`. Those files reach players byte for byte as another build made
them. The sources that exist for them are outside this repo:

- [EchoFrame](https://github.com/heisthecat31/EchoFrame) (MIT): the VrApi-on-OpenXR runtime and the Platform SDK stand-in.
- [kotorvr/echoquest-combat](https://github.com/kotorvr/echoquest-combat): the afkxr modules built into that `libvrapi.so`,
  and the script library sources that `just build-quest-combat-libs` reads through `NEVR_COMBAT_LIBSRC`.
- The asset patch loader's source repository is not one this repo names.

Nothing here rebuilds those files and compares hashes.

**Reconstructions.** The sentinel pins its addresses in `src/quest/sentinel/pinned_targets.h`, and q15t modules pin every
word they were derived from (`kMeshStepWordPins` in `src/quest/plugins/q15t/src/meshstep.h`).
`tools/quest_apk_patch_map.tsv` maps each change the community patch repo makes to the code of ours that replaces it
(#1187). No single index ties every pinned address to the function it belongs to, the origin of the change, the people
to credit and the test that pins it. ADR 0021 still carries a mechanism for patch rows kept outside the repository.

**Rules this builds on.** AGENTS.md "Guardrails": nothing from another project's patch is taken verbatim; it is
reconstructed and analysed first. AGENTS.md "Commits, versions and requirement words": a commit that draws on a person's
work carries their `Co-Authored-By` trailer. AGENTS.md "Nothing that only one machine has": an outside input is an
environment key, never a path.

**Prior art.**
- Reproducible Builds: "a build is reproducible if given the same source code, build environment and build instructions,
  any party can recreate bit-by-bit identical copies of all specified artifacts"
  ([reproducible-builds.org: definition](https://reproducible-builds.org/docs/definition/)).
- SLSA build provenance records what was built from which inputs (`buildDefinition`) and how that build ran
  (`runDetails`) ([SLSA: build provenance](https://slsa.dev/spec/draft/build-provenance)).

## Decision

1. **Every file in a shipped artifact is in exactly one class**, recorded in a tracked manifest beside the recipe that
   builds the artifact:
   1. a game file, by its sha256 in the store input;
   2. built from this tree;
   3. built from a pinned outside source (repository, commit, licence) by a recipe in this repo, whose output sha256 a
      second machine reproduces;
   4. unaudited, with the open issue that moves it into one of the first three.
2. **The release recipe refuses a file in no class.** Class 4 shrinks to empty under its issues, and the release then
   refuses class 4 as well.
3. **Every reconstruction has a row in one tracked index**:
   - the binary, its build ID, the function and its RVA;
   - the bytes or hash it was derived from;
   - its origin: our RE, or another project's file and line at a commit;
   - the people credited;
   - the source file in this repo and the test that pins it.

   A tools test checks the index against the pinned addresses in the source in both directions, as
   `tools/quest_apk_patch_map.tsv` is checked today.
4. **Outside sources are environment keys** and recipe inputs. The manifest records the commit each build used.
5. **No patch row and no source is kept private.** ADR 0021's mechanism for rows outside the repository is retired
   under #1737.
6. **Credit follows the work.** A port or reconstruction commit carries the `Co-Authored-By` trailer of every person
   whose work it draws on.

## Finding a violation

Scope: the `justfile` recipes, `tools/`, `src/quest`, `src/runtime`, `config/`, and the git log of `main`. Out of scope:
tests and `docs/audits/`.

- **P1.** A file a recipe puts into a shipped artifact with no class of rule 1. For example, an entry copied from a base
  APK that is neither a game file nor rebuilt.
- **P2.** A pinned address or instruction word in `src/quest` or `src/runtime` whose site cites no origin: no function
  name, no disassembly address, no source project.
- **P3.** A file taken byte for byte from another build without class 3 evidence (a recipe and a matching hash).
- **P4.** A mechanism, option or comment that keeps a patch row or a source outside the repository: an extra-rows
  input, "private", a stock-bytes exclusion.
- **P5.** A commit on `main` that names another project's work in its message or diff without the `Co-Authored-By`
  trailer of its author.

## Consequences

- The reference base stays in releases only as long as its files are class 4 with open issues. The module track turns
  `libvrapi.so` into class 3 (built from EchoFrame and echoquest-combat at pinned commits).
- Reconstruction lists kept outside the repository are replaced by the index.

## Not claimed

That the reference `libvrapi.so` can be reproduced from the published sources at all. Its EchoFrame commit has to be
fetched and built first, and the comparison may show inputs nobody has published.
