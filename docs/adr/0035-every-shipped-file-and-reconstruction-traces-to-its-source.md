# ADR 0035: every file we ship and every reconstruction we make traces to its source

Status: proposed. Applies to every artifact a release or a dev build ships (the Quest APK, the Windows zip) and to every
reconstruction in the tree (a hook, a build-time row, a replaced function).

## Context

**Shipped files.** The release Quest APK is repacked from the pinned reference APK (`tools/quest_release_base.py`, which
refuses any input whose sha256 is not `REFERENCE_APK_SHA256`). Its docstring names what that base contributes: about a
hundred script libraries, `libassetpatch.so`, the reference runtime's `libvrapi.so` and `assets/afk_default_on.txt`. Some
reach players byte for byte as another build made them (the script libraries, `afk_default_on.txt`). Others pass through
a recipe here first:

- `libvrapi.so`: its libr15 checksum lists are rewritten on every build to the libr15 the APK carries
  (`tools/quest_runtime_pin.py`: "the pin is computed from the staged file on every build"), and `verify-signed` checks the
  signed copy against the pin tool's output (`tools/quest_repack_libs.py`).
- `libassetpatch.so`: the package rename rewrites two strings in it (`tools/quest_apk_rename_package.py`).
- `libr15.so`: the reference build's own byte-patched file, accepted by its sha256 (`kLibR15ReferenceSha256` in
  `src/quest/patch/libr15_patch_table.h`), goes through this repo's patcher (ADR 0021).

The sources that exist for these files are outside this repo: EchoFrame ([heisthecat31/EchoFrame](https://github.com/heisthecat31/EchoFrame),
MIT) for the VrApi-on-OpenXR runtime; Ducky's reference combat build, whose source repository is not public, for the
modules built into the same `libvrapi.so` and for the script library sources that `just build-quest-combat-libs` reads
through `NEVR_COMBAT_LIBSRC`. The asset patch loader's source repository is not one this repo names. Nothing here rebuilds
those files and compares hashes.

**Reconstructions.** The sentinel pins its addresses in `src/quest/sentinel/pinned_targets.h`, and q15t modules pin every
word they were derived from (`kMeshStepWordPins` in `src/quest/plugins/q15t/src/meshstep.h`).
`tools/quest_apk_patch_map.tsv` maps each change the community patch repo makes to the code of ours that replaces it
(#1187); `tools/quest_apk_patch_map.py` checks that every done row names a path and symbol that exist here, and checks
against the patch repo in both directions only when given its checkout. No single index ties every pinned address to the
function it belongs to, the origin of the change, the people to credit and the test that pins it. ADR 0021 carries a
mechanism for patch rows kept outside the repository (its section 11).

**Rules this builds on.** AGENTS.md "Guardrails": nothing from another project's patch is taken verbatim; it is
reconstructed and analysed first. AGENTS.md "Commits, versions and requirement words": a commit that draws on a person's
work carries their `Co-Authored-By` trailer, and someone with no GitHub account is credited by name in the commit body and
in the README thanks. AGENTS.md "Nothing that only one machine has": a per-machine input is an environment variable or a
parameter.

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
   3. built, or transformed, by a recipe in this repo from a pinned input: a game file, a pinned outside source
      (repository, commit, licence) or a pinned outside file (its sha256), where a second machine running the recipe
      gets the same output;
   4. unaudited, with the open issue that moves it into one of the first three.
2. **The release recipe refuses a file in no class.** Class 4 shrinks to empty under its issues, and the release then
   refuses class 4 as well.
3. **Every reconstruction has a row in one tracked index**:
   - the binary, its build ID, the function and its RVA;
   - the bytes or hash it was derived from;
   - its origin: our RE, or another project's file and line at a commit;
   - the people credited;
   - the source file in this repo and the test that pins it.

   A tools test checks the index against the pinned addresses in the source in both directions: every pinned address has
   a row, and every row's address is pinned in the source. That is a stronger check than the patch map's today.
4. **Outside inputs are environment variables or recipe parameters** (AGENTS.md), and the manifest records the commit or
   sha256 each build used.
5. **No patch row and no shipped library is built from an input kept out of the repository.** ADR 0021's mechanism for
   rows outside the repository is retired under #1737.
6. **Credit follows the work.** A port or reconstruction commit credits each person whose work it draws on: a
   `Co-Authored-By` trailer (`<id>+<login>@users.noreply.github.com`) for a person with a GitHub account, and their name in
   the commit body and the README thanks for one without (AGENTS.md).

## Finding a violation

Scope: the `justfile` recipes, `tools/`, `src/quest`, `src/runtime`, `config/`, and the git log of `main`. Out of scope:
tests and `docs/audits/`.

- **P1.** A file a recipe puts into a shipped artifact with no class of rule 1. For example, an entry copied from a base
  APK that is neither a game file nor built or transformed by a recipe here from a pinned input.
- **P2.** A pinned address or instruction word in `src/quest` or `src/runtime` whose site cites no origin: no function
  name, no disassembly address, no source project. Found by reading the sites a search for address-sized constants
  lists; a search alone cannot tell a cited constant from an uncited one.
- **P3.** A file taken byte for byte from another build without class 3 evidence (a recipe and a matching hash).
- **P4.** A patch row of a game library, or a library a release artifact carries, built from an input that is neither in
  the tree nor a pinned outside source: an extra-rows file, a library from a build this repo has no recipe for. Search
  terms: `extra_rows`, `extra-rows`, `never committed`, `private build`.
- **P5.** From this ADR on, a commit on `main` that draws on another person's work without the credit of rule 6. Past
  commits are not rewritten: the remedy for one is a later commit that carries the credit and a credits entry.

## Consequences

- The reference base stays in releases only as long as its files are class 4 with open issues. For its `libvrapi.so`, the
  pinned input (`REFERENCE_APK_SHA256`) and the pin relation `verify-signed` checks already hold the file in place; a
  hash of the shipped file cannot, because its checksum lists change with every libr15 this repo patches.
- If #1737 brings the VrApi-on-OpenXR layer into this repo's scope (its section H15 keeps OpenXR runtime work out of
  scope), the layer becomes class 2: reconstructed here with EchoFrame and Ducky's reference combat build as cited
  references, not built from their sources.
- Reconstruction lists kept outside the repository are replaced by the index.

## Not claimed

That a reconstructed layer would match the shipped `libvrapi.so` byte for byte; it would be checked by its own tests and
headset runs.
