# The q15t plugin's origin citations, module rules and variant libraries

Record of 2026-11-05. It holds what the old q15t plugin repository carried that the tree does not, so that the repository can be
deleted. It is a dated record (`docs/audits/` README): it cites the tree, the repositories and the host as they were measured, and
nothing in it is rewritten later. Tracking issue: #2167; it advances #2011 (the reconstruction index) and #2009 (private inputs).
The commit that adds this file credits the work that `ef714347` (#1591) imported, which is the remedy ADR 0035 P5 names.

## 1. What was measured, and how

| Subject | Fixed at |
|---|---|
| the old q15t plugin repository | its last commit `8b24d663e46c6c2612ff2e7dcea46e36190e7df7` |
| this tree | `origin/main` = `c0aaac1ad9525bd192c1d154dfc54218b6472d10` |
| the reference build's source (16 commits, 2026-10-26 to 2026-11-04) | `35bba9f` to `c5a67e8`; newest `c5a67e81fb7834524fb547f5d6740d6e43ece179` |
| EchoCombat upstream | `c14c3e0a5af6274177df72571dfb288932b9ca1c` (2026-10-15), and the earlier commits that touched `plugin/veilnet.cpp`, `plugin/powerboost.cpp` and `plugin/dronefix.cpp` |

The old repository is all in its last commit. Measured with `git -C <old repository>`:

- `git branch -a`: one branch, `main`; `git tag`: 0 tags; `git stash list`: 0 entries; `git remote`: none; `git for-each-ref`: 1 ref.
- `git fsck --unreachable --no-reflogs`: no output. `git reflog`: 32 `commit` entries and one `commit (initial)`; nothing else.
- `git log --diff-filter=DR --name-status`: no output, so no file was ever deleted or renamed. 33 commits, 212 tracked files,
  all authored by one author, 2026-10-28 08:59 to 2026-10-30 08:19 (-0500).
- Every origin-token string that any earlier revision held is also in the last commit: `git log -p` shows 18 distinct strings
  (`afk_<file>.<ext>` with its line spec, `c14c3e0a`, `vn L..`, `pb L..`, `df ..`, `43dd2ad`, `dbea67e`) on removed lines, and 0 of them
  is absent from the last commit. The last commit is therefore all of the repository's citations.
- `git status --short --ignored`: `!! build/` only (ignored build output, section 8).

**The scan.** One-off scripts, kept out of the tree. They read the old repository, the source checkout and the nevr-runtime
checkout read-only (`git show`, `git blame`, `git log`; no checkout, no write) and take every path as a parameter:

```
python3 -I scan.py <old repository> <source checkout> <EchoCombat files> <nevr-runtime checkout> origin/main rows.json
python3 -I render.py rows.json <old repository> <source checkout> <nevr-runtime checkout> origin/main variants.jsonl <timer log> <scripts> record.md
python3 -I check.py record.md <old repository> <source checkout> <EchoCombat files> <nevr-runtime checkout>
python3 -I variants.py <public base> <scratch directory> v6.6.0 ... v6.16.5 > variants.jsonl
python3 -I control.py <public base> v6.6.0 ... v6.16.5 > control.txt
```

| Script | sha256 |
|---|---|
| `scan.py` | `c5e12a7a497ed9c6f863e7582bdda72eb3e6b0f372f6f747882c365d90e0f030` |
| `overrides.py` | `03874ec5502e9b805f02d06a2fed99c0330166d1835101a2a4c2d54e1d5d63f2` |
| `render.py` | `b33543f3f5a8bd860e79b4ea4df75cc22c4310305c1feaf26cc81bd2fc5ba8fd` |
| `check.py` | `2af586a72e5c7bbe697c71f43ec19d242895e36336de8417dbc759c4c2224ee7` |
| `variants.py` | `f6b9bee5c2efa2aa84db09942944a4213e568b8bb79df5a80282d53824aef9ba` |
| `control.py` | `a8a73c453597ee60586263ca31f62ef6bcea487a0838d6db887b85851a38d060` |

`scan.py` takes every tracked file of the old repository's last commit and finds the lines that match any of `afk_<name>.(cpp|h|py)`,
`c14c3e0a`, `vn L<digit>`, `pb L<digit>`, `df <digit>`, `afkxr`, `43dd2ad`, `dbea67e`: **267 lines in 77 files**. For each, `git blame` names the old commit; the line's citations are read; the pin is searched; the line of the tree at `origin/main` that carries the same text is found by aligning the old file with its tree counterpart. The EchoCombat files at each commit are fetched with `gh api repos/<EchoCombat>/contents/plugin/<file>.cpp?ref=<commit>`.
`check.py` reads only this record and re-derives every claim in it from the repositories (section 10 gives its result).

**How to read section 4.** One row per line, in the order of the old paths.

- *old file:line*: the line at the old repository's last commit. *old commit*: the commit `git blame` names for that line.
- *cited*: the origin tokens of the line, each an exact substring of it. The row quotes nothing else, so no person's words and no local
  path from the line are in the record. *origin*: what the tokens and the line numbers beside them name: the project, the file, the lines.
- *pin* and *held*: the source commit whose file holds the cited text at the cited line. The candidates are the source's commits made at
  or before the old commit of the row, newest first, and the pin is the first one that holds (EchoCombat: `c14c3e0a` first, then the earlier
  versions of the file). *held* lists what was found at the cited lines: strings of the old line, or, where the old line only describes the
  source, a string of the source that the line describes (marked `*`: it is not a substring of the old line). Basis words: `lines` (every held string is at the cited lines of the pin),
  `file` (the old line names a file and no line, or no checkable text: the pin is the newest candidate that has the file, and the cited
  lines are within it), `named` (the old line names the commit; the file is there), `group` (EchoCombat: the version that all the
  `lines` rows of the same file agree on, section 3). `unpinned`: no candidate holds; *held* says what was tried and where it is.
- *tree*: the line of the tree at `origin/main` that carries the same text now (its citation rewritten), or why there is none.

## 2. Result

| | Rows |
|---|---|
| origin-token lines at the old repository's last commit | 267 (every one has a row; none is left out) |
| pinned on the cited text at the cited lines (`lines`) | 191 |
| pinned on the file or on a commit the line names (`file`, `named`, `group`; no checkable line text) | 71 |
| unpinned | 5 (rows 1, 11, 144, 209, 255) |
| tree line found, text rewritten | 249 |
| tree line found, text identical | 2 |
| not carried, the file is not in the tree | 4 |
| not carried, the line is gone from a file the tree has | 12 |

Citations, counted by kind: ec group: 3, ec lines: 52, src file: 64, src lines: 140, src named: 5, src none: 5 (`src` = the reference build's source, `ec` = EchoCombat; a row with two citations counts twice).

Old commit of the rows: `a1b3818` 197, `9f8b01a` 23, `bd4c6ba` 18, `a34ebda` 15, `d78d0da` 8, `1215eb7` 3, `42f7aba` 2, `46e027f` 1.

At the import (`ef714347`, measured with `git show ef714347:<path>`): 3 of the 267 lines are identical to the old text (rows 46, 47, 48), 4 are in files the import did not carry (rows 1, 141, 142, 143), and 260 differ: 258 rewritten, and the 2 lines of `tests/mod/dronemuzzle.sh` that read the source's `afk_dronemuzzle.cpp`, which the carried copy no longer has.

## 3. What the rows show that the issue did not know

1. **The `vn L<n>` numbers of the old text hold at EchoCombat `b0e850fd`, not at `c14c3e0a`.** The old text names `c14c3e0a` for the
   `plugin/veilnet.cpp` it cites. That file is the same at `c14c3e0a` and at `4430174a`, and in it `TOKEN_LIFE` is at 76, `MAGIC` at 77 and `SERVER_TIMEOUT` at 86,
   one line later than the old text says (75, 76-77, 85), and `AbilitySymbol` is at 533 (cited 522-530). At `b0e850fd` (2026-10-11,
   "DRONE health, death and look; QUARK, BLAZAR, PROTOSTAR, UMBRAL VEIL tuning") they are at 75, 76, 85 and 522, and the cited ranges 65-74, 94-100 and 542-543
   are exactly the register block, `struct Packet` and the selected-tac-mod store. Of the versions of the file up to `c14c3e0a`, `b0e850fd` is the only one at which every
   `lines` row of `veilnet.cpp` holds (`check.py` recomputes this). So the `vn` rows are pinned `b0e850fd` (basis `lines`, or `group` for a row that names
   the file and no lines); the `df` rows and the `pb` rows hold at `c14c3e0a`.
2. **Five citations do not resolve, and stay unpinned (rows 1, 11, 144, 209, 255).** Row 1 names a directory, no file. Rows 11 and 144 cite
   `afk_aibots.cpp` :43 for `kEnableFirstWord` and :42-52 for the constants; the file has one version in all 16 commits, with `kEnableFirstWord` at :61 and :273, :42-46 are `#include` lines
   and the constants start at :52. Row 209 cites `afk_fxtrail.cpp:48` for the instruction words behind `kCsData`, `kDataStride` and `kDataInst`; those constants are at :43 and :48 is blank.
   Row 255 cites `afk_voipdrop.cpp:51` for the voice hash table at +0x168; `kTable = 0x168` is at :43 and :51 is the MD5 table. **Inference:** the old text read a copy of these files whose lines differ from every committed one.
3. **`afk_pfx.cpp:9856-9860` (row 7) is pinned `5f4da6b`, older than the commit the same ADR names (row 2).** The ADR 0001 text says "Source tree at dbea67e", but the game-clock
   witness it cites is at 9858 only in `5f4da6b`, `2e57cec` and `35bba9f`; at `dbea67e` those lines hold other code and the witness is at 9885 and 9897. The row is pinned on the cited text, as the rule says.
4. **The import's split differs from the issue's.** See the line below the counts in section 2: the issue's 258 rewritten, 3 unchanged and 6 in files the import did not carry become 260, 3 and 4 when measured, because the 2 lines of
   `tests/mod/dronemuzzle.sh` are in a file the import did carry. The *tree* column is measured at `origin/main`, not at the import.
5. **Lines that are not rows.** The scan finds 48 lines at the last commit that name a local path and carry no origin token (14 in `HANDOFF.md`, 24 in `docs/`, 4 in `rows/`, 3 in `src/`, one each in `PROVENANCE.md`,
   `README.md` and `tests/`). The paths are the old repository's scratch directories, the Android NDK, a game APK, a worktree, the location of the source checkout and a mailbox tool: none is a file of a source, so none cites an origin, and they are left out.
   One row (`tests/memsys_test.cpp:190`) shares its line with such a path; the row quotes the token only. Some lines after a row carry a bare `:<n>` or `lines <a>-<b>` spec and no token
   (for example `docs/adr/0001-match-refresh-rate.md:19`, `docs/modules/droneguard.md:6`); they continue the citation of the row above them, share its file and pin, and the tree keeps their numbers.
   They are not rows and are not checked one by one. Section 5 lists the references to source files by `tools/...` path.

## 4. The rows

| # | old file:line | old commit | cited | origin | pin | held | tree at origin/main |
|---|---|---|---|---|---|---|---|
| 1 | `docs/MODULE_CONTRACT.md:25` | `bd4c6ba` | `afkxr/*.cpp` | source directory `runtime/native/afkxr/` | unpinned | names a directory, no file or line | not carried: the file is not carried |
| 2 | `docs/adr/0001-match-refresh-rate.md:16` | `bd4c6ba` | `dbea67e` | source commit `dbea67e` | `dbea67e` named |  | `docs/quest/q15t/adr/0001-match-refresh-rate.md:17` rewritten |
| 3 | `docs/adr/0001-match-refresh-rate.md:18` | `d78d0da` | `afk_hz.cpp` | source `runtime/native/afkxr/afk_hz.cpp`:1-3 | `dbea67e` lines | `The lobby (App 2-4 ms) holds 118-120, a match (App 6-12 ms) misses the 8.3 ms budget and sits at 75-110`* | `docs/quest/q15t/adr/0001-match-refresh-rate.md:19` rewritten |
| 4 | `docs/adr/0001-match-refresh-rate.md:21` | `d78d0da` | `afk_perflvl_tail.h` | source `runtime/native/afkxr/afk_perflvl_tail.h`:3-4 | `dbea67e` lines | `the combat match is CPU / main-thread bound (~12 ms frame…)` | `docs/quest/q15t/adr/0001-match-refresh-rate.md:22` rewritten |
| 5 | `docs/adr/0001-match-refresh-rate.md:25` | `d78d0da` | `afk_hz.cpp` | source `runtime/native/afkxr/afk_hz.cpp`:3-5 | `dbea67e` lines | `own request passes unchanged, i.e. 120`* | `docs/quest/q15t/adr/0001-match-refresh-rate.md:26` rewritten |
| 6 | `docs/adr/0001-match-refresh-rate.md:28` | `d78d0da` | `afk_hz.cpp` | source `runtime/native/afkxr/afk_hz.cpp`:11 | `dbea67e` lines | `hz72.on` | `docs/quest/q15t/adr/0001-match-refresh-rate.md:29` rewritten |
| 7 | `docs/adr/0001-match-refresh-rate.md:29` | `d78d0da` | `afk_pfx.cpp` | source `runtime/native/afkxr/afk_pfx.cpp`:9856-9860 | `5f4da6b` lines | `RE47_CLK`* | `docs/quest/q15t/adr/0001-match-refresh-rate.md:30` rewritten |
| 8 | `docs/adr/0002-weapon-arm-new-profile.md:7` | `1215eb7` | `afk_gunconnect.cpp` | source `runtime/native/afkxr/afk_gunconnect.cpp`:58-64,496-520 | `a3d0ffd` lines | `T-26` | `docs/quest/q15t/adr/0002-weapon-arm-new-profile.md:7` rewritten |
| 9 | `docs/modules/aibots.md:3` | `bd4c6ba` | `afk_aibots.cpp` | source `runtime/native/afkxr/afk_aibots.cpp`:1-27,306-346 | `1357409` lines | `Moving combat bots`* `afk_aibots_tick` | `docs/quest/q15t/modules/aibots.md:3` rewritten |
| 10 | `docs/modules/aibots.md:9` | `a1b3818` | `afk_match.cpp` | source `runtime/native/afkxr/afk_match.cpp`:735-742,742 | `1357409` lines | `vrapi_PollEvent`* `afk_aibots_tick();`* | `docs/quest/q15t/modules/aibots.md:9` rewritten |
| 11 | `docs/modules/aibots.md:14` | `a1b3818` | `afk_aibots.cpp` | source `runtime/native/afkxr/afk_aibots.cpp`:43 | unpinned | tried `kEnableFirstWord`: at :61,273 of the newest candidate, cited :43 | `docs/quest/q15t/modules/aibots.md:14` rewritten |
| 12 | `docs/modules/bvh.md:4` | `bd4c6ba` | `afk_bvh.cpp` `afk_bvh_core.h` | source `runtime/native/afkxr/afk_bvh.cpp` ; source `runtime/native/afkxr/afk_bvh_core.h` | `1357409` file |  | `docs/quest/q15t/modules/bvh.md:4` rewritten |
| 13 | `docs/modules/bvh.md:36` | `a1b3818` | `afk_bvh.cpp` | source `runtime/native/afkxr/afk_bvh.cpp`:1-12 | `1357409` lines | `Static BVH for the Quest`* `CBVHTree::Initialize(const SBVHTreeData&)`* | `docs/quest/q15t/modules/bvh.md:36` rewritten |
| 14 | `docs/modules/bvh.md:37` | `a1b3818` | `afk_bvh.cpp` | source `runtime/native/afkxr/afk_bvh.cpp`:12-15 | `1357409` lines | `SBVHTreeData` | `docs/quest/q15t/modules/bvh.md:37` rewritten |
| 15 | `docs/modules/bvh.md:55` | `a1b3818` | `afk_bvh.cpp` | source `runtime/native/afkxr/afk_bvh.cpp`:19-22,178-179 | `1357409` lines | `+0x38` | `docs/quest/q15t/modules/bvh.md:55` rewritten |
| 16 | `docs/modules/bvh.md:57` | `a1b3818` | `afk_bvh.cpp` | source `runtime/native/afkxr/afk_bvh.cpp`:56-57 | `1357409` lines | `t in (0, 1)` | `docs/quest/q15t/modules/bvh.md:57` rewritten |
| 17 | `docs/modules/bvh.md:60` | `a1b3818` | `afk_bvh.cpp` | source `runtime/native/afkxr/afk_bvh.cpp`:26-37 | `1357409` lines | `t <= 1` | `docs/quest/q15t/modules/bvh.md:60` rewritten |
| 18 | `docs/modules/bvh.md:84` | `a1b3818` | `afk_bvh_core.h` | source `runtime/native/afkxr/afk_bvh_core.h`:6,14 | `1357409` lines | `SBVHTreeData (0x50 bytes)`* `The root is node 0`* | `docs/quest/q15t/modules/bvh.md:84` rewritten |
| 19 | `docs/modules/bvh.md:92` | `a1b3818` | `afk_bvh_core.h` | source `runtime/native/afkxr/afk_bvh_core.h`:6 | `1357409` lines | `SBVHTreeData (0x50 bytes)`* | `docs/quest/q15t/modules/bvh.md:92` rewritten |
| 20 | `docs/modules/bvh.md:101` | `a1b3818` | `afk_bvh_core.h` | source `runtime/native/afkxr/afk_bvh_core.h`:9,32 | `1357409` lines | `Triangle4 (0xe0 bytes)`* `static_assert(sizeof(Tri4) == 0xe0` | `docs/quest/q15t/modules/bvh.md:101` rewritten |
| 21 | `docs/modules/bvh.md:107` | `a1b3818` | `afk_bvh_core.h` | source `runtime/native/afkxr/afk_bvh_core.h`:10 | `1357409` lines | `v1 = v0 - e1` `v2 = v0 + e2` `Ng = e1 x e2` | `docs/quest/q15t/modules/bvh.md:107` rewritten |
| 22 | `docs/modules/bvh.md:112` | `a1b3818` | `afk_bvh_core.h` | source `runtime/native/afkxr/afk_bvh_core.h`:12-14,31,142 | `1357409` lines | `Node (0x40 bytes)`* `static_assert(sizeof(Node) == 0x40`* `& 0x7fffffffu) >> 5`* | `docs/quest/q15t/modules/bvh.md:112` rewritten |
| 23 | `docs/modules/bvh.md:119` | `a1b3818` | `afk_bvh.cpp` | source `runtime/native/afkxr/afk_bvh.cpp`:12 | `1357409` lines | `CBVHTree::Initialize(const SBVHTreeData&)`* | `docs/quest/q15t/modules/bvh.md:119` rewritten |
| 24 | `docs/modules/bvh.md:141` | `a1b3818` | `afk_bvh.cpp` | source `runtime/native/afkxr/afk_bvh.cpp`:103-105 | `1357409` lines | `0x12b2b4c` `0x12b2bc4` `0x12c0a54` | `docs/quest/q15t/modules/bvh.md:141` rewritten |
| 25 | `docs/modules/bvh.md:150` | `a1b3818` | `afk_bvh.cpp` | source `runtime/native/afkxr/afk_bvh.cpp`:38-57 | `1357409` lines | `The callers' gate`* `t in (0, 1)`* | `docs/quest/q15t/modules/bvh.md:150` rewritten |
| 26 | `docs/modules/bvh.md:153` | `a1b3818` | `afk_bvh.cpp` | source `runtime/native/afkxr/afk_bvh.cpp`:19-22,178-179 | `1357409` lines | `+0x38` | `docs/quest/q15t/modules/bvh.md:153` rewritten |
| 27 | `docs/modules/bvh.md:160` | `a1b3818` | `afk_bvh.cpp` | source `runtime/native/afkxr/afk_bvh.cpp`:38-57 | `1357409` lines | `tmax = inf` `within the segment` | `docs/quest/q15t/modules/bvh.md:160` rewritten |
| 28 | `docs/modules/bvh.md:180` | `a1b3818` | `afk_bvh_core.h` | source `runtime/native/afkxr/afk_bvh_core.h`:14 | `1357409` lines | `node 0` | `docs/quest/q15t/modules/bvh.md:180` rewritten |
| 29 | `docs/modules/custctx.md:5` | `a1b3818` | `afk_custctx.cpp` | source `runtime/native/afkxr/afk_custctx.cpp`:1 | `1357409` lines | `DEFAULT ON` | `docs/quest/q15t/modules/custctx.md:5` rewritten |
| 30 | `docs/modules/custctx.md:6` | `a1b3818` | `afk_tap.cpp` | source `runtime/native/afkxr/afk_tap.cpp`:174 | `1357409` lines | `afk_custctx_install`* | `docs/quest/q15t/modules/custctx.md:6` rewritten |
| 31 | `docs/modules/custctx.md:23` | `a1b3818` | `afk_custctx.cpp` | source `runtime/native/afkxr/afk_custctx.cpp`:73-95 | `1357409` lines | `kWords`* | `docs/quest/q15t/modules/custctx.md:23` rewritten |
| 32 | `docs/modules/custctx.md:41` | `a1b3818` | `afk_custctx.cpp` | source `runtime/native/afkxr/afk_custctx.cpp`:63-65,100-101,168-206,208-230,232-285 | `1357409` lines | `kGotLoad = 0x36cf6f0`* `kRtMargin = 64`* `uint64_t LoadHook(void* self, void* ctx, void* model)`* | `docs/quest/q15t/modules/custctx.md:41` rewritten |
| 33 | `docs/modules/custctx.md:84` | `a1b3818` | `afk_custctx.cpp` | source `runtime/native/afkxr/afk_custctx.cpp`:78-95 | `1357409` lines | `kWords` | `docs/quest/q15t/modules/custctx.md:84` rewritten |
| 34 | `docs/modules/droneguard.md:5` | `a1b3818` | `afk_droneguard.cpp` | source `runtime/native/afkxr/afk_droneguard.cpp`:1 | `1357409` lines | `TEST ONLY` | `docs/quest/q15t/modules/droneguard.md:5` rewritten |
| 35 | `docs/modules/droneguard.md:31` | `a1b3818` | `afk_droneguard.cpp` | source `runtime/native/afkxr/afk_droneguard.cpp`:42-48,66-80,86-99,108-124,126-146 | `1357409` lines | `IdxSkip` `StateLine` `FollowHook` | `docs/quest/q15t/modules/droneguard.md:31` rewritten |
| 36 | `docs/modules/droneguard.md:61` | `a1b3818` | `afk_droneguard.cpp` | source `runtime/native/afkxr/afk_droneguard.cpp`:43-44,57-58,76-77 | `1357409` lines | `RefreshCache` | `docs/quest/q15t/modules/droneguard.md:61` rewritten |
| 37 | `docs/modules/dronekill.md:6` | `a1b3818` | `afk_dronekill.cpp` | source `runtime/native/afkxr/afk_dronekill.cpp` | `1357409` file |  | `docs/quest/q15t/modules/dronekill.md:6` rewritten |
| 38 | `docs/modules/dronekill.md:7` | `a1b3818` | `afk_dronekill_core.h` | source `runtime/native/afkxr/afk_dronekill_core.h` | `1357409` file |  | `docs/quest/q15t/modules/dronekill.md:7` rewritten |
| 39 | `docs/modules/dronekill.md:11` | `a1b3818` | `afk_dronekill.cpp` | source `runtime/native/afkxr/afk_dronekill.cpp`:1 | `1357409` lines | `dronekill.on (TEST ONLY ...)` | `docs/quest/q15t/modules/dronekill.md:11` rewritten |
| 40 | `docs/modules/dronekill.md:84` | `a1b3818` | `afk_dronekill.cpp` | source `runtime/native/afkxr/afk_dronekill.cpp`:8,24,37-38,49 | `1357409` lines | `CEnableComponentNode::Enter(node, data)`* `comparator's JUMP_SLOT`* `STAGE 1 (EC-NATIVES`* `TEST word`* | `docs/quest/q15t/modules/dronekill.md:84` rewritten |
| 41 | `docs/modules/dronekill.md:85` | `a1b3818` | `afk_dronekill_core.h` | source `runtime/native/afkxr/afk_dronekill_core.h` | `1357409` file |  | `docs/quest/q15t/modules/dronekill.md:85` rewritten |
| 42 | `docs/modules/dronekill.md:87` | `a1b3818` | `afk_dronekill_test.cpp` `afk_ec.cpp` | source `native/afkxr/tests/afk_dronekill_test.cpp` ; source `runtime/native/afkxr/afk_ec.cpp`:185-215 | `1357409` lines | `g_gameFd.load`* `received: token here`* | `docs/quest/q15t/modules/dronekill.md:87` rewritten |
| 43 | `docs/modules/dronemuzzle.md:6` | `a1b3818` | `afk_dronemuzzle.cpp` | source `runtime/native/afkxr/afk_dronemuzzle.cpp` | `1357409` file |  | `docs/quest/q15t/modules/dronemuzzle.md:6` rewritten |
| 44 | `docs/modules/dronemuzzle.md:7` | `a1b3818` | `afk_dronemuzzle_core.h` | source `runtime/native/afkxr/afk_dronemuzzle_core.h` | `1357409` file |  | `docs/quest/q15t/modules/dronemuzzle.md:7` rewritten |
| 45 | `docs/modules/dronemuzzle.md:12` | `a1b3818` | `afk_dronemuzzle.cpp` | source `runtime/native/afkxr/afk_dronemuzzle.cpp`:1,29 | `1357409` lines | `dronemuzzle.on (TEST ONLY ...` | `docs/quest/q15t/modules/dronemuzzle.md:12` rewritten |
| 46 | `docs/modules/dronemuzzle.md:55` | `a1b3818` | `df 916-941` | EchoCombat `plugin/dronefix.cpp`:916-941 | EC `c14c3e0a` lines | `MuzzleTransform(void *transforms, Pose *out, uint32_t index)`* | `docs/quest/q15t/modules/dronemuzzle.md:61` same |
| 47 | `docs/modules/dronemuzzle.md:59` | `a1b3818` | `df 943-1019` | EchoCombat `plugin/dronefix.cpp`:943-1019 | EC `c14c3e0a` lines | `FireBullet` | `docs/quest/q15t/modules/dronemuzzle.md:65` same |
| 48 | `docs/modules/dronemuzzle.md:63` | `a1b3818` | `df 132-147` | EchoCombat `plugin/dronefix.cpp`:132-147 | EC `c14c3e0a` lines | `g_frame_dt`* `void TickFrame()`* | not carried: the line is gone from `docs/quest/q15t/modules/dronemuzzle.md` |
| 49 | `docs/modules/dronemuzzle.md:72` | `a1b3818` | `afk_dronemuzzle.cpp` | source `runtime/native/afkxr/afk_dronemuzzle.cpp`:7-10,16,55-56 | `1357409` lines | `0x180a384` `kFnTeleport` | `docs/quest/q15t/modules/dronemuzzle.md:76` rewritten |
| 50 | `docs/modules/dronemuzzle.md:74` | `a1b3818` | `afk_dronemuzzle_core.h` | source `runtime/native/afkxr/afk_dronemuzzle_core.h` | `1357409` file |  | not carried: the line is gone from `docs/quest/q15t/modules/dronemuzzle.md` |
| 51 | `docs/modules/dronemuzzle.md:110` | `a1b3818` | `afk_dronemuzzle.cpp` | source `runtime/native/afkxr/afk_dronemuzzle.cpp`:145,164 | `1357409` lines | `afk_dm_tp_thunk`* `afk_dm_fire_thunk`* | not carried: the line is gone from `docs/quest/q15t/modules/dronemuzzle.md` |
| 52 | `docs/modules/dronemuzzle.md:154` | `a1b3818` | `afk_dronemuzzle.cpp` | source `runtime/native/afkxr/afk_dronemuzzle.cpp`:145-182 | `1357409` lines | `afk_dm_tp_thunk`* | `docs/quest/q15t/modules/dronemuzzle.md:161` rewritten |
| 53 | `docs/modules/ec.md:3` | `bd4c6ba` | `afk_ec.cpp` | source `runtime/native/afkxr/afk_ec.cpp` | `1357409` file |  | `docs/quest/q15t/modules/ec.md:3` rewritten |
| 54 | `docs/modules/ec.md:4` | `a1b3818` | `afk_ec_core.h` `afk_ecfx.cpp` `afk_ecfx_core.h` | source `runtime/native/afkxr/afk_ec_core.h` ; source `runtime/native/afkxr/afk_ecfx.cpp` ; source `runtime/native/afkxr/afk_ecfx_core.h` | `1357409` file |  | `docs/quest/q15t/modules/ec.md:4` rewritten |
| 55 | `docs/modules/ec.md:17` | `a1b3818` | `afk_ec_core.h` | source `runtime/native/afkxr/afk_ec_core.h`:20-33 | `1357409` lines | `kPageRva = 0x2104000`* `kEquipSel = 0xe80`* | `docs/quest/q15t/modules/ec.md:17` rewritten |
| 56 | `docs/modules/ec.md:29` | `a1b3818` | `afk_ec.cpp` | source `runtime/native/afkxr/afk_ec.cpp` | `1357409` file |  | `docs/quest/q15t/modules/ec.md:29` rewritten |
| 57 | `docs/modules/ec.md:68` | `a1b3818` | `afk_ec.cpp` | source `runtime/native/afkxr/afk_ec.cpp`:99,102 | `1357409` lines | `kGotRecvfrom = 0x36d3e88, kGotSendto = 0x36db3d0` `kTacModList = 0x37af5f8` `0x36d3e88` `0x36db3d0` `0x37af5f8` `kGotRecvfrom` `kGotSendto` `kTacModList` | `docs/quest/q15t/modules/ec.md:68` rewritten |
| 58 | `docs/modules/ec.md:84` | `a1b3818` | `afk_ec.cpp` | source `runtime/native/afkxr/afk_ec.cpp`:123-127,419-448 | `1357409` lines | `libpnsovr.so` `libpnsrad.so` `WriteLog` | `docs/quest/q15t/modules/ec.md:84` rewritten |
| 59 | `docs/modules/ec.md:96` | `a1b3818` | `afk_ec_core.h` | source `runtime/native/afkxr/afk_ec_core.h`:162-170,247 | `1357409` lines | `AbilitySymbol(const char* name, size_t n)`* `kNewTacMods[3][8]`* | `docs/quest/q15t/modules/ec.md:96` rewritten |
| 60 | `docs/modules/ec.md:124` | `bd4c6ba` | `afk_ec.cpp` | source `runtime/native/afkxr/afk_ec.cpp`:99-102,123-127,419-448 | `1357409` lines | `kGotRecvfrom = 0x36d3e88`* `kWriteLogSym`* `HookSelection`* | `docs/quest/q15t/modules/ec.md:124` rewritten |
| 61 | `docs/modules/ec.md:125` | `a1b3818` | `afk_ec_core.h` `afk_ecfx.cpp` `afk_ecfx_core.h` | source `runtime/native/afkxr/afk_ec_core.h`:20-33,162-170,247 ; source `runtime/native/afkxr/afk_ecfx.cpp` ; source `runtime/native/afkxr/afk_ecfx_core.h` | `1357409` lines | `kPageRva = 0x2104000`* `AbilitySymbol(const char* name, size_t n)`* `kNewTacMods`* | `docs/quest/q15t/modules/ec.md:125` rewritten |
| 62 | `docs/modules/ec.md:182` | `a1b3818` | `afk_ec_core.h` | source `runtime/native/afkxr/afk_ec_core.h` | `1357409` file |  | `docs/quest/q15t/modules/ec.md:182` rewritten |
| 63 | `docs/modules/fxtrail.md:6` | `a1b3818` | `afk_fxtrail.cpp` `afk_tap.cpp` | source `runtime/native/afkxr/afk_fxtrail.cpp`:1,60-77 ; source `runtime/native/afkxr/afk_tap.cpp`:174 | `1357409` lines | `ON by default` `bool Off()`* `afk_fxtrail_install`* | `docs/quest/q15t/modules/fxtrail.md:6` rewritten |
| 64 | `docs/modules/fxtrail.md:37` | `a1b3818` | `afk_fxtrail.cpp` | source `runtime/native/afkxr/afk_fxtrail.cpp`:41 | `1357409` lines | `0x36e7b98` | `docs/quest/q15t/modules/fxtrail.md:37` rewritten |
| 65 | `docs/modules/fxtrail.md:42` | `a1b3818` | `afk_fxtrail.cpp` | source `runtime/native/afkxr/afk_fxtrail.cpp`:1-24 | `1357409` lines | `fxtrail (RE-1, the combat crash fix; ON by default)`* `Off: setprop`* | `docs/quest/q15t/modules/fxtrail.md:42` rewritten |
| 66 | `docs/modules/fxtrail.md:43` | `a1b3818` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp` | `1357409` file |  | `docs/quest/q15t/modules/fxtrail.md:43` rewritten |
| 67 | `docs/modules/fxtrail.md:106` | `a1b3818` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp` | `1357409` file |  | `docs/quest/q15t/modules/fxtrail.md:106` rewritten |
| 68 | `docs/modules/fxtrail.md:107` | `a1b3818` | `afk_fxtrail_words.py` | source `tests/test_afk_fxtrail_words.py` | `1357409` file |  | `docs/quest/q15t/modules/fxtrail.md:107` rewritten |
| 69 | `docs/modules/gunconnect.md:3` | `bd4c6ba` | `afk_gunconnect.cpp` | source `runtime/native/afkxr/afk_gunconnect.cpp`:1-24 | `1357409` lines | `Gun connect (W-1`* | `docs/quest/q15t/modules/gunconnect.md:3` rewritten |
| 70 | `docs/modules/gunconnect.md:17` | `a1b3818` | `afk_gunconnect.cpp` | source `runtime/native/afkxr/afk_gunconnect.cpp`:92 | `1357409` lines | `kFnConnect = 0x2153438, kFnHasTag = 0x1723760, kFnUserActor = 0x1251654, kTagCombat = 0x37af288` `0x2153438` `0x1723760` `0x1251654` `0x37af288` `kFnConnect` `kFnHasTag` `kFnUserActor` `kTagCombat` | `docs/quest/q15t/modules/gunconnect.md:17` rewritten |
| 71 | `docs/modules/heaps.md:7` | `a1b3818` | `afk_memsys.cpp` | source `runtime/native/afkxr/afk_memsys.cpp` | `1357409` file |  | `docs/quest/q15t/modules/heaps.md:7` rewritten |
| 72 | `docs/modules/heaps.md:36` | `a1b3818` | `afk_memsys.cpp` | source `runtime/native/afkxr/afk_memsys.cpp`:6-7 | `1357409` lines | `M1  android_main`* `reads the System heap size once`* | not carried: the line is gone from `docs/quest/q15t/modules/heaps.md` |
| 73 | `docs/modules/heaps.md:60` | `a1b3818` | `afk_memsys.cpp` | source `runtime/native/afkxr/afk_memsys.cpp`:6-9 | `1357409` lines | `reserves the arena`* | `docs/quest/q15t/modules/heaps.md:60` rewritten |
| 74 | `docs/modules/heaps.md:86` | `a1b3818` | `afk_memsys.cpp` | source `runtime/native/afkxr/afk_memsys.cpp`:9,253 | `1357409` lines | `hollywood` | `docs/quest/q15t/modules/heaps.md:86` rewritten |
| 75 | `docs/modules/hudmeter.md:7` | `bd4c6ba` | `afk_hudmeter.cpp` | source `runtime/native/afkxr/afk_hudmeter.cpp`:49 | `1357409` lines | `hudmeter.on` | `docs/quest/q15t/modules/hudmeter.md:7` rewritten |
| 76 | `docs/modules/hudmeter.md:15` | `a1b3818` | `afk_hudmeter.cpp` | source `runtime/native/afkxr/afk_hudmeter.cpp`:76 | `1357409` lines | `kGot = 0x36c6ce8, kFn = 0x2726eac` `0x36c6ce8` `0x2726eac` `kGot` | `docs/quest/q15t/modules/hudmeter.md:15` rewritten |
| 77 | `docs/modules/hudmeter.md:17` | `a1b3818` | `afk_hudmeter.cpp` | source `runtime/native/afkxr/afk_hudmeter.cpp`:83 | `1357409` lines | `0x2726eac` `0xa9bb6bfc` `0x2726eb0` `0xa90163f9` | `docs/quest/q15t/modules/hudmeter.md:17` rewritten |
| 78 | `docs/modules/hudmeter.md:23` | `a1b3818` | `afk_hudmeter.cpp` | source `runtime/native/afkxr/afk_hudmeter.cpp`:1-9 | `1357409` lines | `e55af932c88ec864` | `docs/quest/q15t/modules/hudmeter.md:23` rewritten |
| 79 | `docs/modules/hudmeter.md:39` | `a1b3818` | `afk_hudmeter.cpp` | source `runtime/native/afkxr/afk_hudmeter.cpp`:47-48 | `1357409` lines | `data = thread+0xb0`* | `docs/quest/q15t/modules/hudmeter.md:39` rewritten |
| 80 | `docs/modules/hudmeter.md:55` | `a1b3818` | `afk_cvbind.cpp` | source `runtime/native/afkxr/afk_cvbind.cpp`:1 | `1357409` lines | `cvbind.on (TEST ONLY, log-only; RE-25 G31 ... DRAFT, not built, no AUD yet)` | `docs/quest/q15t/modules/hudmeter.md:55` rewritten |
| 81 | `docs/modules/hudmeter.md:60` | `a1b3818` | `afk_hudmeter.cpp` `afk_shieldgamma.cpp` | source `runtime/native/afkxr/afk_hudmeter.cpp`:387-392 ; source `runtime/native/afkxr/afk_shieldgamma.cpp`:213-217 | `1357409` lines | `bool Swap(const char** hop)`* `afk wrapper (cvbind)`* `if (have == g_base + kFn) *hop = "stock"`* | `docs/quest/q15t/modules/hudmeter.md:60` rewritten |
| 82 | `docs/modules/hudmeter.md:63` | `a1b3818` | `afk_tap.cpp` | source `runtime/native/afkxr/afk_tap.cpp`:174 | `1357409` lines | `afk_cvbind_install` `afk_hudmeter_install` | `docs/quest/q15t/modules/hudmeter.md:63` rewritten |
| 83 | `docs/modules/hudmeter.md:93` | `a1b3818` | `afk_hudmeter.cpp` | source `runtime/native/afkxr/afk_hudmeter.cpp`:11-32,132-282 | `1357409` lines | `oldbar.on` `oldbarfix.on` `old bar` | `docs/quest/q15t/modules/hudmeter.md:93` rewritten |
| 84 | `docs/modules/hudmeter.md:100` | `a1b3818` | `afk_hudmeter.cpp` | source `runtime/native/afkxr/afk_hudmeter.cpp`:43-48 | `1357409` lines | `0x272711c` `0x2727134` | `docs/quest/q15t/modules/hudmeter.md:100` rewritten |
| 85 | `docs/modules/impactsnd.md:3` | `a1b3818` | `afk_impactsnd.cpp` | source `runtime/native/afkxr/afk_impactsnd.cpp` | `1357409` file |  | `docs/quest/q15t/modules/impactsnd.md:3` rewritten |
| 86 | `docs/modules/impactsnd.md:6` | `a1b3818` | `afk_impactsnd.cpp` | source `runtime/native/afkxr/afk_impactsnd.cpp` | `1357409` file |  | `docs/quest/q15t/modules/impactsnd.md:6` rewritten |
| 87 | `docs/modules/impactsnd.md:163` | `a1b3818` | `afk_impactsnd.cpp` | source `runtime/native/afkxr/afk_impactsnd.cpp`:192-204 | `1357409` lines | `CodeMatches()` `CodeMatches` | `docs/quest/q15t/modules/impactsnd.md:163` rewritten |
| 88 | `docs/modules/memsys.md:5` | `a1b3818` | `afk_memsys.cpp` | source `runtime/native/afkxr/afk_memsys.cpp`:214-226 | `1357409` lines | `memsys: OFF: the libr15 md5 took over 3 s`* | `docs/quest/q15t/modules/memsys.md:5` rewritten |
| 89 | `docs/modules/memsys.md:8` | `bd4c6ba` | `afk_memsys.cpp` | source `runtime/native/afkxr/afk_memsys.cpp`:11-17,65-77,96-105 | `1357409` lines | `M2  CGRawInterface::InitCommon`* `kWords`* | `docs/quest/q15t/modules/memsys.md:8` rewritten |
| 90 | `docs/modules/memsys.md:9` | `a1b3818` | `afk_memsys.cpp` | source `runtime/native/afkxr/afk_memsys.cpp`:4 | `1357409` lines | `Tester crashes F`* | `docs/quest/q15t/modules/memsys.md:9` rewritten |
| 91 | `docs/modules/memsys.md:23` | `a1b3818` | `afk_memsys.cpp` | source `runtime/native/afkxr/afk_memsys.cpp` | `1357409` file |  | `docs/quest/q15t/modules/memsys.md:23` rewritten |
| 92 | `docs/modules/memsys.md:41` | `a1b3818` | `afk_memsys.cpp` | source `runtime/native/afkxr/afk_memsys.cpp`:17 | `1357409` lines | `The layout index is used as a full 32-bit value`* | `docs/quest/q15t/modules/memsys.md:41` rewritten |
| 93 | `docs/modules/memsys.md:67` | `a1b3818` | `afk_memsys.cpp` | source `runtime/native/afkxr/afk_memsys.cpp`:65-77 | `1357409` lines | `kWords`* | `docs/quest/q15t/modules/memsys.md:67` rewritten |
| 94 | `docs/modules/music.md:6` | `a34ebda` | `afk_music.cpp` | source `runtime/native/afkxr/afk_music.cpp` | `43dd2ad` file |  | `docs/quest/q15t/modules/music.md:6` rewritten |
| 95 | `docs/modules/music.md:10` | `a34ebda` | `afk_music.cpp` | source `runtime/native/afkxr/afk_music.cpp`:1 | `43dd2ad` lines | `ON by default` | `docs/quest/q15t/modules/music.md:10` rewritten |
| 96 | `docs/modules/music.md:11` | `a34ebda` | `afk_music.cpp` | source `runtime/native/afkxr/afk_music.cpp`:211 | `43dd2ad` lines | `Off()` | `docs/quest/q15t/modules/music.md:11` rewritten |
| 97 | `docs/modules/music.md:30` | `a34ebda` | `afk_music.cpp` | source `runtime/native/afkxr/afk_music.cpp`:1-34 | `43dd2ad` lines | `music (T-48, ON by default)`* `music: combat map gone: stops`* | `docs/quest/q15t/modules/music.md:30` rewritten |
| 98 | `docs/modules/music.md:80` | `a34ebda` | `afk_music.cpp` | source `runtime/native/afkxr/afk_music.cpp`:51-52 | `43dd2ad` lines | `kGotPostSoundEvent = 0x36d76e8` `kFnPostSoundEvent = 0x2549264` `0x36d76e8` `0x2549264` `kGotPostSoundEvent` `kFnPostSoundEvent` | `docs/quest/q15t/modules/music.md:80` rewritten |
| 99 | `docs/modules/peerclamp.md:5` | `a1b3818` | `afk_peerclamp.cpp` | source `runtime/native/afkxr/afk_peerclamp.cpp`:1 | `1357409` lines | `TEST ONLY` | `docs/quest/q15t/modules/peerclamp.md:5` rewritten |
| 100 | `docs/modules/peerclamp.md:32` | `a1b3818` | `afk_peerclamp.cpp` | source `runtime/native/afkxr/afk_peerclamp.cpp`:39-44,83-111,113-116 | `1357409` lines | `CleanupHook` | `docs/quest/q15t/modules/peerclamp.md:32` rewritten |
| 101 | `docs/modules/peerclamp.md:59` | `a1b3818` | `afk_peerclamp.cpp` | source `runtime/native/afkxr/afk_peerclamp.cpp`:50-62 | `1357409` lines | `kWords` | `docs/quest/q15t/modules/peerclamp.md:59` rewritten |
| 102 | `docs/modules/powerboost.md:7` | `bd4c6ba` | `afk_powerboost.cpp` `afk_powerboost_core.h` | source `runtime/native/afkxr/afk_powerboost.cpp` ; source `runtime/native/afkxr/afk_powerboost_core.h` | `1357409` file |  | `docs/quest/q15t/modules/powerboost.md:7` rewritten |
| 103 | `docs/modules/powerboost.md:14` | `a1b3818` | `afk_powerboost.cpp` | source `runtime/native/afkxr/afk_powerboost.cpp`:1 | `1357409` lines | `powerboost.on (TEST ONLY ... STAGE 0` | `docs/quest/q15t/modules/powerboost.md:14` rewritten |
| 104 | `docs/modules/powerboost.md:59` | `a1b3818` | `afk_powerboost.cpp` | source `runtime/native/afkxr/afk_powerboost.cpp`:80 | `1357409` lines | `kGotHealData = 0x3725b80` `0x3725b80` `kGotHealData` | `docs/quest/q15t/modules/powerboost.md:59` rewritten |
| 105 | `docs/modules/powerboost.md:171` | `a1b3818` | `afk_powerboost.cpp` | source `runtime/native/afkxr/afk_powerboost.cpp` | `1357409` file |  | `docs/quest/q15t/modules/powerboost.md:171` rewritten |
| 106 | `docs/modules/powerboost.md:180` | `a1b3818` | `afk_ec.cpp` | source `runtime/native/afkxr/afk_ec.cpp`:170-195,117,649 | `1357409` lines | `g_recvfrom.load`* `g_netTick[kNetTicks]`* `afk_ec_on_net_tick`* | `docs/quest/q15t/modules/powerboost.md:180` rewritten |
| 107 | `docs/modules/powerboost.md:183` | `a1b3818` | `afk_flowdiag.cpp` | source `runtime/native/afkxr/afk_flowdiag.cpp`:91,93 | `1357409` lines | `0xc9dc08cfc2f5920f` | `docs/quest/q15t/modules/powerboost.md:183` rewritten |
| 108 | `docs/modules/sensorwd.md:5` | `9f8b01a` | `afk_fx.cpp` `43dd2ad` | source `runtime/native/afkxr/afk_fx.cpp` ; source commit `43dd2ad` | `43dd2ad` named |  | `docs/quest/q15t/modules/sensorwd.md:5` rewritten |
| 109 | `docs/modules/sensorwd.md:11` | `9f8b01a` | `43dd2ad` | source commit `43dd2ad` | `43dd2ad` named |  | `docs/quest/q15t/modules/sensorwd.md:11` rewritten |
| 110 | `docs/modules/sensorwd.md:35` | `9f8b01a` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp`:5581-5587 | `43dd2ad` lines | `the wipe band that never ends` | `docs/quest/q15t/modules/sensorwd.md:35` rewritten |
| 111 | `docs/modules/sensorwd.md:50` | `9f8b01a` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp`:5590 | `43dd2ad` lines | `kSnsEnSlot` `kSnsEnFn` `kSnsDisSlot` `kSnsDisFn` | `docs/quest/q15t/modules/sensorwd.md:50` rewritten |
| 112 | `docs/modules/sensorwd.md:51` | `9f8b01a` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp`:5884-5900,5782 | `43dd2ad` lines | `JumpSlot` | `docs/quest/q15t/modules/sensorwd.md:51` rewritten |
| 113 | `docs/modules/sensorwd.md:72` | `9f8b01a` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp`:5610-5620 | `43dd2ad` lines | `SnsIsSphere` | `docs/quest/q15t/modules/sensorwd.md:72` rewritten |
| 114 | `docs/modules/sensorwd.md:73` | `9f8b01a` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp`:5586,5615 | `43dd2ad` lines | `0xcc8` | `docs/quest/q15t/modules/sensorwd.md:73` rewritten |
| 115 | `docs/modules/sensorwd.md:79` | `9f8b01a` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp`:5711 | `43dd2ad` lines | `HookSnsEvent(uintptr_t gs, uint64_t sym, uint64_t a, uint64_t b, uint64_t j`* | `docs/quest/q15t/modules/sensorwd.md:79` rewritten |
| 116 | `docs/modules/sensorwd.md:88` | `9f8b01a` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp`:5660-5669 | `43dd2ad` lines | `HookSnsEn(`* `g_origSnsEn`* | `docs/quest/q15t/modules/sensorwd.md:88` rewritten |
| 117 | `docs/modules/sensorwd.md:94` | `9f8b01a` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp`:5594-5604 | `43dd2ad` lines | `TriggeredPing` | `docs/quest/q15t/modules/sensorwd.md:94` rewritten |
| 118 | `docs/modules/sensorwd.md:99` | `9f8b01a` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp`:5592 | `43dd2ad` lines | `kSnsPairMs = 250`* | `docs/quest/q15t/modules/sensorwd.md:99` rewritten |
| 119 | `docs/modules/sensorwd.md:104` | `9f8b01a` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp`:5593 | `43dd2ad` lines | `kWdMs` | `docs/quest/q15t/modules/sensorwd.md:104` rewritten |
| 120 | `docs/modules/sensorwd.md:107` | `9f8b01a` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp`:5695-5699 | `43dd2ad` lines | `reinterpret_cast<Sns8Fn>(fn)(x.cs, x.h, 0, 0, 0, 0, 0, 0)`* | `docs/quest/q15t/modules/sensorwd.md:107` rewritten |
| 121 | `docs/modules/sensorwd.md:131` | `9f8b01a` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp`:5588-5589 | `43dd2ad` lines | `ac2d3f9f080ba45f` `fd8cf56544b904a3` `7cc621b8bf3248c5` | `docs/quest/q15t/modules/sensorwd.md:131` rewritten |
| 122 | `docs/modules/sensorwd.md:132` | `9f8b01a` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp`:5593 | `43dd2ad` lines | `FinishedPing` | `docs/quest/q15t/modules/sensorwd.md:132` rewritten |
| 123 | `docs/modules/sensorwd.md:133` | `9f8b01a` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp`:5592 | `43dd2ad` lines | `kSnsPairMs = 250`* | `docs/quest/q15t/modules/sensorwd.md:133` rewritten |
| 124 | `docs/modules/sensorwd.md:134` | `9f8b01a` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp`:5594 | `43dd2ad` lines | `kSnsSlots = 32`* | `docs/quest/q15t/modules/sensorwd.md:134` rewritten |
| 125 | `docs/modules/sensorwd.md:135` | `9f8b01a` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp`:5586-5587,5610-5620 | `43dd2ad` lines | `[cs+0xd0]` `[cs+0x100]` `[model+0xcc8]` `+0xcc8` | `docs/quest/q15t/modules/sensorwd.md:135` rewritten |
| 126 | `docs/modules/sensorwd.md:136` | `9f8b01a` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp`:5672-5710 | `43dd2ad` lines | `sensorwd's game-thread work`* `g_wdBackA`* | `docs/quest/q15t/modules/sensorwd.md:136` rewritten |
| 127 | `docs/modules/sensorwd.md:192` | `9f8b01a` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp`:5610-5620 | `43dd2ad` lines | `bool SnsIsSphere(uintptr_t cs, uint64_t h, uint32_t* vis)`* | `docs/quest/q15t/modules/sensorwd.md:192` rewritten |
| 128 | `docs/modules/sensorwd.md:230` | `9f8b01a` | `43dd2ad` | source commit `43dd2ad` | `43dd2ad` named |  | `docs/quest/q15t/modules/sensorwd.md:230` rewritten |
| 129 | `docs/modules/shieldgamma.md:8` | `bd4c6ba` | `afk_shieldgamma.cpp` | source `runtime/native/afkxr/afk_shieldgamma.cpp`:243-244 | `1357409` lines | `form, on by default` | `docs/quest/q15t/modules/shieldgamma.md:8` rewritten |
| 130 | `docs/modules/shieldgamma.md:16` | `a1b3818` | `afk_shieldgamma.cpp` | source `runtime/native/afkxr/afk_shieldgamma.cpp`:57 | `1357409` lines | `kGot = 0x36d2020, kFn = 0x272d55c` `0x36d2020` `0x272d55c` `kGot` | `docs/quest/q15t/modules/shieldgamma.md:16` rewritten |
| 131 | `docs/modules/shieldgamma.md:26` | `a1b3818` | `afk_shieldgamma.cpp` | source `runtime/native/afkxr/afk_shieldgamma.cpp`:1-12 | `1357409` lines | `shieldgamma.on (RE-34, TEXTURE-2 G7 cause 2`* `Scope = the shield material only`* | `docs/quest/q15t/modules/shieldgamma.md:26` rewritten |
| 132 | `docs/modules/shieldgamma.md:82` | `a1b3818` | `afk_shieldgamma.cpp` | source `runtime/native/afkxr/afk_shieldgamma.cpp`:15-19 | `1357409` lines | `CSetMaterialPropertyNode::Update  slot 0x36d2020`* `CModelCS::SetMaterialProperty`* | `docs/quest/q15t/modules/shieldgamma.md:82` rewritten |
| 133 | `docs/modules/voipdrop.md:5` | `a1b3818` | `afk_voipdrop.cpp` | source `runtime/native/afkxr/afk_voipdrop.cpp`:1,114-130 | `1357409` lines | `Switch()` `DEFAULT ON` | `docs/quest/q15t/modules/voipdrop.md:5` rewritten |
| 134 | `docs/modules/voipdrop.md:6` | `a1b3818` | `afk_tap.cpp` | source `runtime/native/afkxr/afk_tap.cpp`:174 | `1357409` lines | `afk_voipdrop_install`* | `docs/quest/q15t/modules/voipdrop.md:6` rewritten |
| 135 | `docs/modules/voipdrop.md:31` | `a1b3818` | `afk_voipdrop.cpp` | source `runtime/native/afkxr/afk_voipdrop.cpp`:42-43,82-94,96-112 | `1357409` lines | `kGotRecv = 0x36c5d80`* `bool PageAvailable(uint8_t* self, uint32_t bytes)`* `void RecvHook(void* self, uint64_t handle`* | `docs/quest/q15t/modules/voipdrop.md:31` rewritten |
| 136 | `docs/modules/voipdrop.md:67` | `a1b3818` | `afk_voipdrop.cpp` | source `runtime/native/afkxr/afk_voipdrop.cpp`:55-64 | `1357409` lines | `kWords` `ReceiveVoip` `FindOrCreate` | `docs/quest/q15t/modules/voipdrop.md:67` rewritten |
| 137 | `docs/modules/wallfire.md:4` | `bd4c6ba` | `afk_wallfire.cpp` | source `runtime/native/afkxr/afk_wallfire.cpp` | `1357409` file |  | `docs/quest/q15t/modules/wallfire.md:4` rewritten |
| 138 | `docs/modules/wallfire.md:26` | `a1b3818` | `afk_wallfire.cpp` | source `runtime/native/afkxr/afk_wallfire.cpp`:1-4 | `1357409` lines | `CR15NetGunCS::FireGunLocal` `FireGunLocal` | `docs/quest/q15t/modules/wallfire.md:26` rewritten |
| 139 | `docs/modules/wallfire.md:29` | `a1b3818` | `afk_wallfire.cpp` | source `runtime/native/afkxr/afk_wallfire.cpp`:1-10 | `1357409` lines | `CBVHTree` | `docs/quest/q15t/modules/wallfire.md:29` rewritten |
| 140 | `docs/modules/wallfire.md:84` | `a1b3818` | `afk_wallfire.cpp` | source `runtime/native/afkxr/afk_wallfire.cpp`:2-4 | `1357409` lines | `muzzle position` | `docs/quest/q15t/modules/wallfire.md:84` rewritten |
| 141 | `rows/README.md:18` | `42f7aba` | `afk_impactsnd.cpp` | source `runtime/native/afkxr/afk_impactsnd.cpp`:90-96 | `a3d0ffd` lines | `0x91003116` `0xb9401bea` `0xf9400beb` `0xb9001be0` `0xf9000bfc` `0x910033f6` `0xb9400bea` `0xf94003eb` `0xb9000be0` `0xf90003fc` `kPatch` | not carried: the file is not carried |
| 142 | `rows/README.md:31` | `46e027f` | `afk_impactsnd.cpp` | source `runtime/native/afkxr/afk_impactsnd.cpp`:79-80 | `a3d0ffd` lines | `kCodeCountVa`* `kCodeCount =`* | not carried: the file is not carried |
| 143 | `rows/gen_rows.py:63` | `42f7aba` | `afk_impactsnd.cpp` | source `runtime/native/afkxr/afk_impactsnd.cpp`:90-96 | `a3d0ffd` lines | `kPatch` | not carried: the file is not carried |
| 144 | `src/aibots.cpp:21` | `bd4c6ba` | `afk_aibots.cpp` | source `runtime/native/afkxr/afk_aibots.cpp`:42-52 | unpinned | tried `kEnableFirstWord`: at :61,273 of the newest candidate, cited :42-52 | `src/quest/plugins/q15t/src/aibots.cpp:21` rewritten |
| 145 | `src/aibots.h:1` | `a1b3818` | `afk_aibots.cpp` | source `runtime/native/afkxr/afk_aibots.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/aibots.h:1` rewritten |
| 146 | `src/bvh.h:1` | `a1b3818` | `afk_bvh.cpp` `afk_bvh_core.h` | source `runtime/native/afkxr/afk_bvh.cpp` ; source `runtime/native/afkxr/afk_bvh_core.h` | `1357409` file |  | `src/quest/plugins/q15t/src/bvh.h:1` rewritten |
| 147 | `src/bvh_core.h:2` | `a1b3818` | `afk_bvh_core.h` | source `runtime/native/afkxr/afk_bvh_core.h` | `1357409` file |  | `src/quest/plugins/q15t/src/bvh_core.h:2` rewritten |
| 148 | `src/custctx.h:1` | `a1b3818` | `afk_custctx.cpp` | source `runtime/native/afkxr/afk_custctx.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/custctx.h:1` rewritten |
| 149 | `src/droneguard.cpp:15` | `a1b3818` | `afk_droneguard.cpp` | source `runtime/native/afkxr/afk_droneguard.cpp`:46-60 | `1357409` lines | `0x21497cc` | `src/quest/plugins/q15t/src/droneguard.cpp:15` rewritten |
| 150 | `src/droneguard.h:1` | `a1b3818` | `afk_droneguard.cpp` | source `runtime/native/afkxr/afk_droneguard.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/droneguard.h:1` rewritten |
| 151 | `src/dronekill.cpp:1` | `a1b3818` | `afk_dronekill.cpp` | source `runtime/native/afkxr/afk_dronekill.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/dronekill.cpp:1` rewritten |
| 152 | `src/dronekill.cpp:449` | `a1b3818` | `df 805-827` | EchoCombat `plugin/dronefix.cpp`:805-827 | EC `c14c3e0a` lines | `LockedOn` | `src/quest/plugins/q15t/src/dronekill.cpp:449` rewritten |
| 153 | `src/dronekill.cpp:494` | `a1b3818` | `df 1059-1095` | EchoCombat `plugin/dronefix.cpp`:1059-1095 | EC `c14c3e0a` lines | `ChaseTarget` | `src/quest/plugins/q15t/src/dronekill.cpp:494` rewritten |
| 154 | `src/dronekill.cpp:543` | `a1b3818` | `df 1069-1076` | EchoCombat `plugin/dronefix.cpp`:1069-1076 | EC `c14c3e0a` lines | `if (!target) {`* `if (c && c->moving) {`* | `src/quest/plugins/q15t/src/dronekill.cpp:543` rewritten |
| 155 | `src/dronekill.cpp:558` | `a1b3818` | `df 1093` | EchoCombat `plugin/dronefix.cpp`:1093 | EC `c14c3e0a` lines | `g_chases`* | `src/quest/plugins/q15t/src/dronekill.cpp:558` rewritten |
| 156 | `src/dronekill.h:2` | `a1b3818` | `afk_dronekill.cpp` | source `runtime/native/afkxr/afk_dronekill.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/dronekill.h:2` rewritten |
| 157 | `src/dronekill_core.h:1` | `bd4c6ba` | `afk_dronekill_core.h` | source `runtime/native/afkxr/afk_dronekill_core.h` | `1357409` file |  | not carried: the line is gone from `src/quest/plugins/q15t/src/dronekill_core.h` |
| 158 | `src/dronekill_core.h:19` | `a1b3818` | `afk_dronekill.py` | source `tests/test_afk_dronekill.py` | `1357409` file |  | `src/quest/plugins/q15t/src/dronekill_core.h:19` rewritten |
| 159 | `src/dronekill_core.h:59` | `a1b3818` | `afk_dronekill.py` | source `tests/test_afk_dronekill.py` | `1357409` file |  | `src/quest/plugins/q15t/src/dronekill_core.h:59` rewritten |
| 160 | `src/dronekill_core.h:118` | `a1b3818` | `dronefix.cpp L434-560` | EchoCombat `plugin/dronefix.cpp`:434-560 | EC `c14c3e0a` lines | `struct DroneGroup`* `AcquireSRWLockExclusive(&g_drones_lock)`* | `src/quest/plugins/q15t/src/dronekill_core.h:118` rewritten |
| 161 | `src/dronekill_core.h:290` | `a1b3818` | `df 778-793` | EchoCombat `plugin/dronefix.cpp`:778-793 | EC `c14c3e0a` lines | `TouchLive` | `src/quest/plugins/q15t/src/dronekill_core.h:290` rewritten |
| 162 | `src/dronekill_core.h:308` | `a1b3818` | `df 731-835` | EchoCombat `plugin/dronefix.cpp`:731-835 | EC `c14c3e0a` lines | `the drone stays grabbable until it locks on`* | `src/quest/plugins/q15t/src/dronekill_core.h:308` rewritten |
| 163 | `src/dronekill_core.h:322` | `a1b3818` | `df 772` | EchoCombat `plugin/dronefix.cpp`:772 | EC `c14c3e0a` lines | `g_touch[slot] = { system, index, 0, GetTickCount64(), false };`* | `src/quest/plugins/q15t/src/dronekill_core.h:322` rewritten |
| 164 | `src/dronekill_core.h:337` | `a1b3818` | `df 810-826` | EchoCombat `plugin/dronefix.cpp`:810-826 | EC `c14c3e0a` lines | `int mine = -1, newest = -1;`* | `src/quest/plugins/q15t/src/dronekill_core.h:337` rewritten |
| 165 | `src/dronekill_core.h:372` | `a1b3818` | `df 700-729` | EchoCombat `plugin/dronefix.cpp`:700-729 | EC `c14c3e0a` lines | `Each drone's last gun state (by actor)`* | `src/quest/plugins/q15t/src/dronekill_core.h:372` rewritten |
| 166 | `src/dronekill_core.h:396` | `a1b3818` | `df 1037-1095` | EchoCombat `plugin/dronefix.cpp`:1037-1095 | EC `c14c3e0a` lines | `CHASE_SPEED`* | `src/quest/plugins/q15t/src/dronekill_core.h:396` rewritten |
| 167 | `src/dronekill_core.h:433` | `a1b3818` | `df 1077-1082` | EchoCombat `plugin/dronefix.cpp`:1077-1082 | EC `c14c3e0a` lines | `{ c = &e; break; }`* | `src/quest/plugins/q15t/src/dronekill_core.h:433` rewritten |
| 168 | `src/dronemuzzle.cpp:1` | `a1b3818` | `afk_dronemuzzle.cpp` | source `runtime/native/afkxr/afk_dronemuzzle.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/dronemuzzle.cpp:1` rewritten |
| 169 | `src/dronemuzzle.cpp:32` | `a1b3818` | `df 56` `df 1014` | EchoCombat `plugin/dronefix.cpp`:56 ; EchoCombat `plugin/dronefix.cpp`:1014 | EC `c14c3e0a` lines ; EC `c14c3e0a` lines | `SHOT_LOG` `g_tracers`* | `src/quest/plugins/q15t/src/dronemuzzle.cpp:32` rewritten |
| 170 | `src/dronemuzzle.cpp:41` | `a1b3818` | `df 136` | EchoCombat `plugin/dronefix.cpp`:136 | EC `c14c3e0a` lines | `g_frame_dt = 1.0f / 90.0f`* | `src/quest/plugins/q15t/src/dronemuzzle.cpp:41` rewritten |
| 171 | `src/dronemuzzle.cpp:64` | `a1b3818` | `df 916-941` | EchoCombat `plugin/dronefix.cpp`:916-941 | EC `c14c3e0a` lines | `MuzzleTransform`* | `src/quest/plugins/q15t/src/dronemuzzle.cpp:64` rewritten |
| 172 | `src/dronemuzzle.cpp:78` | `a1b3818` | `df 921` | EchoCombat `plugin/dronefix.cpp`:921 | EC `c14c3e0a` lines | `len < 1e-4f`* | `src/quest/plugins/q15t/src/dronemuzzle.cpp:78` rewritten |
| 173 | `src/dronemuzzle.cpp:89` | `a1b3818` | `df 943-1019` | EchoCombat `plugin/dronefix.cpp`:943-1019 | EC `c14c3e0a` lines | `FireBulletLocal` | `src/quest/plugins/q15t/src/dronemuzzle.cpp:89` rewritten |
| 174 | `src/dronemuzzle.cpp:115` | `a1b3818` | `df 1012` | EchoCombat `plugin/dronefix.cpp`:1012 | EC `c14c3e0a` lines | `shooter_vel[i] = 0.0f`* | `src/quest/plugins/q15t/src/dronemuzzle.cpp:115` rewritten |
| 175 | `src/dronemuzzle.cpp:237` | `a1b3818` | `df 138-147` | EchoCombat `plugin/dronefix.cpp`:138-147 | EC `c14c3e0a` lines | `void TickFrame()`* | `src/quest/plugins/q15t/src/dronemuzzle.cpp:237` rewritten |
| 176 | `src/dronemuzzle.h:2` | `a1b3818` | `afk_dronemuzzle.cpp` | source `runtime/native/afkxr/afk_dronemuzzle.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/dronemuzzle.h:2` rewritten |
| 177 | `src/dronemuzzle.h:37` | `a1b3818` | `df 132-147` | EchoCombat `plugin/dronefix.cpp`:132-147 | EC `c14c3e0a` lines | `g_frame_dt`* | `src/quest/plugins/q15t/src/dronemuzzle.h:37` rewritten |
| 178 | `src/dronemuzzle_core.h:1` | `a1b3818` | `afk_dronemuzzle_core.h` | source `runtime/native/afkxr/afk_dronemuzzle_core.h` | `1357409` file |  | not carried: the line is gone from `src/quest/plugins/q15t/src/dronemuzzle_core.h` |
| 179 | `src/dronemuzzle_core.h:2` | `a1b3818` | `c14c3e0a` | EchoCombat `plugin/dronefix.cpp` | EC `c14c3e0a` group |  | not carried: the line is gone from `src/quest/plugins/q15t/src/dronemuzzle_core.h` |
| 180 | `src/dronemuzzle_core.h:10` | `a1b3818` | `df 51` | EchoCombat `plugin/dronefix.cpp`:51 | EC `c14c3e0a` lines | `MUZZLE[3] = { 0.000024f, 0.075634f, 0.147283f }`* | `src/quest/plugins/q15t/src/dronemuzzle_core.h:67` rewritten |
| 181 | `src/dronemuzzle_core.h:11` | `a1b3818` | `df 52` | EchoCombat `plugin/dronefix.cpp`:52 | EC `c14c3e0a` lines | `BACK_MAX = 0.25f`* | `src/quest/plugins/q15t/src/dronemuzzle_core.h:68` rewritten |
| 182 | `src/dronemuzzle_core.h:13` | `a1b3818` | `df 143-144` | EchoCombat `plugin/dronefix.cpp`:143-144 | EC `c14c3e0a` lines | `if (dt > 1.0f / 240.0f && dt < 0.1f)`* `g_frame_dt = g_frame_dt * 0.9f + dt * 0.1f`* | `src/quest/plugins/q15t/src/dronemuzzle_core.h:70` rewritten |
| 183 | `src/dronemuzzle_core.h:16` | `a1b3818` | `df 928-929` | EchoCombat `plugin/dronefix.cpp`:928-929 | EC `c14c3e0a` lines | `float dt = g_frame_dt;`* | `src/quest/plugins/q15t/src/dronemuzzle_core.h:3` rewritten |
| 184 | `src/dronemuzzle_core.h:19` | `a1b3818` | `df 916-934` | EchoCombat `plugin/dronefix.cpp`:916-934 | EC `c14c3e0a` lines | `AttackTarget` | `src/quest/plugins/q15t/src/dronemuzzle_core.h:76` rewritten |
| 185 | `src/dronemuzzle_core.h:20` | `a1b3818` | `df 921` | EchoCombat `plugin/dronefix.cpp`:921 | EC `c14c3e0a` lines | `len < 1e-4f`* | not carried: the line is gone from `src/quest/plugins/q15t/src/dronemuzzle_core.h` |
| 186 | `src/dronemuzzle_core.h:30` | `a1b3818` | `df 933-934` | EchoCombat `plugin/dronefix.cpp`:933-934 | EC `c14c3e0a` lines | `p[i] += ...` | `src/quest/plugins/q15t/src/dronemuzzle_core.h:87` rewritten |
| 187 | `src/ec.h:2` | `a1b3818` | `afk_ec.cpp` | source `runtime/native/afkxr/afk_ec.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/ec.h:2` rewritten |
| 188 | `src/ec_core.h:3` | `bd4c6ba` | `afk_ec_core.h` | source `runtime/native/afkxr/afk_ec_core.h` | `1357409` file |  | `src/quest/plugins/q15t/src/ec_core.h:3` rewritten |
| 189 | `src/ec_core.h:4` | `a1b3818` | `c14c3e0a` | EchoCombat `plugin/veilnet.cpp` | EC `b0e850fd` group |  | `src/quest/plugins/q15t/src/ec_core.h:4` rewritten |
| 190 | `src/ec_core.h:28` | `a1b3818` | `vn L65-74` | EchoCombat `plugin/veilnet.cpp`:65-74 | EC `b0e850fd` lines | `TOKEN_RVA = 0x2104cc0`* `POINT_FIRED_RVA = 0x2104d44`* | `src/quest/plugins/q15t/src/ec_core.h:28` rewritten |
| 191 | `src/ec_core.h:38` | `a1b3818` | `vn L75` | EchoCombat `plugin/veilnet.cpp`:75 | EC `b0e850fd` lines | `TOKEN_LIFE = 15000000`* | `src/quest/plugins/q15t/src/ec_core.h:38` rewritten |
| 192 | `src/ec_core.h:39` | `a1b3818` | `vn L76-77` | EchoCombat `plugin/veilnet.cpp`:76-77 | EC `b0e850fd` lines | `ECVL` `VEIL` `0x4c564345` `0x4c494556` | `src/quest/plugins/q15t/src/ec_core.h:39` rewritten |
| 193 | `src/ec_core.h:40` | `a1b3818` | `vn L85` | EchoCombat `plugin/veilnet.cpp`:85 | EC `b0e850fd` lines | `SERVER_TIMEOUT = 6000`* | `src/quest/plugins/q15t/src/ec_core.h:40` rewritten |
| 194 | `src/ec_core.h:42` | `a1b3818` | `vn L94-100` | EchoCombat `plugin/veilnet.cpp`:94-100 | EC `b0e850fd` lines | `struct Packet {` `uint32_t magic2;`* | `src/quest/plugins/q15t/src/ec_core.h:42` rewritten |
| 195 | `src/ec_core.h:51` | `a1b3818` | `vn L159` | EchoCombat `plugin/veilnet.cpp`:159 | EC `b0e850fd` lines | `bool Carried(uint8_t type)`* | `src/quest/plugins/q15t/src/ec_core.h:51` rewritten |
| 196 | `src/ec_core.h:60` | `a1b3818` | `vn L146-151` | EchoCombat `plugin/veilnet.cpp`:146-151 | EC `b0e850fd` lines | `void AddToken(Shared *t)`* | `src/quest/plugins/q15t/src/ec_core.h:60` rewritten |
| 197 | `src/ec_core.h:78` | `a1b3818` | `vn L175` | EchoCombat `plugin/veilnet.cpp`:175 | EC `b0e850fd` lines | `uint32_t g_seen[32];`* | `src/quest/plugins/q15t/src/ec_core.h:78` rewritten |
| 198 | `src/ec_core.h:92` | `a1b3818` | `vn L178-185` | EchoCombat `plugin/veilnet.cpp`:178-185 | EC `b0e850fd` lines | `FirstTime` | `src/quest/plugins/q15t/src/ec_core.h:92` rewritten |
| 199 | `src/ec_core.h:98` | `a1b3818` | `vn L219-237` | EchoCombat `plugin/veilnet.cpp`:219-237 | EC `b0e850fd` lines | `SawSend` | `src/quest/plugins/q15t/src/ec_core.h:98` rewritten |
| 200 | `src/ec_core.h:113` | `a1b3818` | `vn L207-217` | EchoCombat `plugin/veilnet.cpp`:207-217 | EC `b0e850fd` lines | `PickServer` | `src/quest/plugins/q15t/src/ec_core.h:113` rewritten |
| 201 | `src/ec_core.h:130` | `a1b3818` | `vn L316-330` | EchoCombat `plugin/veilnet.cpp`:316-330 | EC `b0e850fd` lines | `p.type == 'A'`* `FirstTime(p.id)`* | `src/quest/plugins/q15t/src/ec_core.h:130` rewritten |
| 202 | `src/ec_core.h:193` | `a1b3818` | `vn L508-545` | EchoCombat `plugin/veilnet.cpp`:508-545 | EC `b0e850fd` lines | `The tac mod the player selected`* | `src/quest/plugins/q15t/src/ec_core.h:193` rewritten |
| 203 | `src/ec_core.h:194` | `a1b3818` | `vn L522-530` | EchoCombat `plugin/veilnet.cpp`:522-530 | EC `b0e850fd` lines | `AbilitySymbol(const char *name, size_t n)`* | `src/quest/plugins/q15t/src/ec_core.h:194` rewritten |
| 204 | `src/ec_core.h:224` | `a1b3818` | `vn L542-543` | EchoCombat `plugin/veilnet.cpp`:542-543 | EC `b0e850fd` lines | `EQUIP_RVA + 0x20`* `EQUIP_RVA) = sym;`* | `src/quest/plugins/q15t/src/ec_core.h:224` rewritten |
| 205 | `src/ec_core.h:313` | `a1b3818` | `vn L363-409` | EchoCombat `plugin/veilnet.cpp`:363-409 | EC `b0e850fd` lines | `The tac mod list as the server allows`* | `src/quest/plugins/q15t/src/ec_core.h:313` rewritten |
| 206 | `src/ec_core.h:398` | `a1b3818` | `vn L440-456` | EchoCombat `plugin/veilnet.cpp`:440-456 | EC `b0e850fd` lines | `"VPTO"[k]`* | `src/quest/plugins/q15t/src/ec_core.h:398` rewritten |
| 207 | `src/ecfx.h:2` | `a1b3818` | `afk_ecfx.cpp` | source `runtime/native/afkxr/afk_ecfx.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/ecfx.h:2` rewritten |
| 208 | `src/ecfx_core.h:2` | `a1b3818` | `afk_ecfx_core.h` `dronefix.cpp WallFxDue L374-409` | source `runtime/native/afkxr/afk_ecfx_core.h` ; EchoCombat `plugin/dronefix.cpp`:374-409 | `1357409` file ; EC `c14c3e0a` lines | `WallFxDue` `SoundAgain` | `src/quest/plugins/q15t/src/ecfx_core.h:2` rewritten |
| 209 | `src/fxtrail.cpp:17` | `a1b3818` | `afk_fxtrail.cpp` | source `runtime/native/afkxr/afk_fxtrail.cpp`:48 | unpinned | tried `kCsData = 0xf8, kDataStride = 0x18, kDataInst = 0x10`: at :43 of the newest candidate, cited :48 | `src/quest/plugins/q15t/src/fxtrail.cpp:17` rewritten |
| 210 | `src/fxtrail.h:1` | `a1b3818` | `afk_fxtrail.cpp` | source `runtime/native/afkxr/afk_fxtrail.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/fxtrail.h:1` rewritten |
| 211 | `src/gamespace.h:2` | `1215eb7` | `afk_gunconnect.cpp` | source `runtime/native/afkxr/afk_gunconnect.cpp` | `a3d0ffd` file |  | `src/quest/plugins/q15t/src/gamespace.h:2` rewritten |
| 212 | `src/gunconnect.cpp:19` | `bd4c6ba` | `afk_gunconnect.cpp` | source `runtime/native/afkxr/afk_gunconnect.cpp`:91-92 | `1357409` lines | `UserActor` | `src/quest/plugins/q15t/src/gunconnect.cpp:19` rewritten |
| 213 | `src/gunconnect.cpp:136` | `a1b3818` | `afk_gunconnect.cpp` | source `runtime/native/afkxr/afk_gunconnect.cpp`:222-240 | `1357409` lines | `ReadLocal` | `src/quest/plugins/q15t/src/gunconnect.cpp:136` rewritten |
| 214 | `src/gunconnect.h:1` | `a1b3818` | `afk_gunconnect.cpp` | source `runtime/native/afkxr/afk_gunconnect.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/gunconnect.h:1` rewritten |
| 215 | `src/heaps.cpp:33` | `a1b3818` | `afk_memsys.cpp` | source `runtime/native/afkxr/afk_memsys.cpp`:253 | `1357409` lines | `hollywood` | `src/quest/plugins/q15t/src/heaps.cpp:33` rewritten |
| 216 | `src/heaps.h:6` | `a1b3818` | `afk_memsys.cpp` | source `runtime/native/afkxr/afk_memsys.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/heaps.h:6` rewritten |
| 217 | `src/hudmeter.h:1` | `a1b3818` | `afk_hudmeter.cpp` | source `runtime/native/afkxr/afk_hudmeter.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/hudmeter.h:1` rewritten |
| 218 | `src/impactsnd.cpp:25` | `a1b3818` | `afk_impactsnd.cpp` | source `runtime/native/afkxr/afk_impactsnd.cpp`:74 | `1357409` lines | `0x37af8d0` `kDefaultSymVa` | `src/quest/plugins/q15t/src/impactsnd.cpp:28` rewritten |
| 219 | `src/impactsnd.cpp:26` | `a1b3818` | `afk_impactsnd.cpp` | source `runtime/native/afkxr/afk_impactsnd.cpp`:75 | `1357409` lines | `0x37af6a0` `kSoundSettingsVa` `kSoundSettings` | `src/quest/plugins/q15t/src/impactsnd.cpp:29` rewritten |
| 220 | `src/impactsnd.cpp:29` | `a1b3818` | `afk_impactsnd.cpp` | source `runtime/native/afkxr/afk_impactsnd.cpp`:86 | `1357409` lines | `0x2121d04` `LoadSoundSettings` | `src/quest/plugins/q15t/src/impactsnd.cpp:32` rewritten |
| 221 | `src/impactsnd.cpp:33` | `a1b3818` | `afk_impactsnd.cpp` | source `runtime/native/afkxr/afk_impactsnd.cpp`:77 | `1357409` lines | `kMapCount` | `src/quest/plugins/q15t/src/impactsnd.cpp:36` rewritten |
| 222 | `src/impactsnd.cpp:34` | `a1b3818` | `afk_impactsnd.cpp` | source `runtime/native/afkxr/afk_impactsnd.cpp`:100 | `1357409` lines | `kMapFlags` | `src/quest/plugins/q15t/src/impactsnd.cpp:37` rewritten |
| 223 | `src/impactsnd.cpp:36` | `a1b3818` | `afk_impactsnd.cpp` | source `runtime/native/afkxr/afk_impactsnd.cpp`:78-80 | `1357409` lines | `kCodeCountVa = 0x21208b4`* `kCodeCount = 0xf9401ac1`* | `src/quest/plugins/q15t/src/impactsnd.cpp:79` rewritten |
| 224 | `src/impactsnd.cpp:48` | `a1b3818` | `afk_impactsnd.cpp` | source `runtime/native/afkxr/afk_impactsnd.cpp`:81 | `1357409` lines | `kWaitS` | `src/quest/plugins/q15t/src/impactsnd.cpp:91` rewritten |
| 225 | `src/impactsnd.cpp:53` | `a1b3818` | `afk_impactsnd.cpp` | source `runtime/native/afkxr/afk_impactsnd.cpp`:101 | `1357409` lines | `kStockId` `CalculateSymbolValue` | `src/quest/plugins/q15t/src/impactsnd.cpp:96` rewritten |
| 226 | `src/impactsnd.cpp:59` | `a1b3818` | `afk_impactsnd.cpp` | source `runtime/native/afkxr/afk_impactsnd.cpp`:103-119 | `1357409` lines | `kIds[] = {`* `play_bullet_impact_player`* | `src/quest/plugins/q15t/src/impactsnd.cpp:102` rewritten |
| 227 | `src/impactsnd.h:1` | `a1b3818` | `afk_impactsnd.cpp` | source `runtime/native/afkxr/afk_impactsnd.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/impactsnd.h:1` rewritten |
| 228 | `src/memsys.cpp:22` | `a1b3818` | `afk_memsys.cpp` | source `runtime/native/afkxr/afk_memsys.cpp`:65-77 | `1357409` lines | `kWords`* | `src/quest/plugins/q15t/src/memsys.cpp:22` rewritten |
| 229 | `src/memsys.h:2` | `a1b3818` | `afk_memsys.cpp` | source `runtime/native/afkxr/afk_memsys.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/memsys.h:2` rewritten |
| 230 | `src/memsys.h:16` | `a1b3818` | `afk_memsys.cpp` | source `runtime/native/afkxr/afk_memsys.cpp`:65-77 | `1357409` lines | `kWords[] = {`* | `src/quest/plugins/q15t/src/memsys.h:16` rewritten |
| 231 | `src/music.cpp:15` | `a34ebda` | `afk_music.cpp` | source `runtime/native/afkxr/afk_music.cpp`:52 | `43dd2ad` lines | `kFnPostSoundEvent = 0x2549264`* | `src/quest/plugins/q15t/src/music.cpp:15` rewritten |
| 232 | `src/music.cpp:17` | `a34ebda` | `afk_music.cpp` | source `runtime/native/afkxr/afk_music.cpp`:67,98 | `43dd2ad` lines | `0xFFFFFFFF` | `src/quest/plugins/q15t/src/music.cpp:17` rewritten |
| 233 | `src/music.cpp:18` | `a34ebda` | `afk_music.cpp` | source `runtime/native/afkxr/afk_music.cpp`:59 | `43dd2ad` lines | `kMaxLines` | `src/quest/plugins/q15t/src/music.cpp:18` rewritten |
| 234 | `src/music.cpp:19` | `a34ebda` | `afk_music.cpp` | source `runtime/native/afkxr/afk_music.cpp`:157 | `43dd2ad` lines | `g_nextLook = now + 0.5`* | `src/quest/plugins/q15t/src/music.cpp:19` rewritten |
| 235 | `src/music.cpp:22` | `a34ebda` | `afk_music.cpp` | source `runtime/native/afkxr/afk_music.cpp`:83 | `43dd2ad` lines | `PostSoundEvent(self, event w1`* | `src/quest/plugins/q15t/src/music.cpp:22` rewritten |
| 236 | `src/music.h:1` | `a34ebda` | `afk_music.cpp` | source `runtime/native/afkxr/afk_music.cpp` | `43dd2ad` file |  | `src/quest/plugins/q15t/src/music.h:1` rewritten |
| 237 | `src/music.h:61` | `a34ebda` | `afk_music.cpp` | source `runtime/native/afkxr/afk_music.cpp`:54-58 | `43dd2ad` lines | `SetState` | `src/quest/plugins/q15t/src/music.h:61` rewritten |
| 238 | `src/peerclamp.cpp:13` | `a1b3818` | `afk_peerclamp.cpp` | source `runtime/native/afkxr/afk_peerclamp.cpp`:55-57 | `1357409` lines | `ldr w8,[x19,#0x560]`* `ldr x24,[x19,#0x478]`* | `src/quest/plugins/q15t/src/peerclamp.cpp:13` rewritten |
| 239 | `src/peerclamp.h:1` | `a1b3818` | `afk_peerclamp.cpp` | source `runtime/native/afkxr/afk_peerclamp.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/peerclamp.h:1` rewritten |
| 240 | `src/plugin.cpp:152` | `d78d0da` | `afk_gunconnect.cpp` | source `runtime/native/afkxr/afk_gunconnect.cpp` | `dbea67e` file |  | `src/quest/plugins/q15t/src/plugin.cpp:183` rewritten |
| 241 | `src/powerboost.cpp:1` | `a1b3818` | `afk_powerboost.cpp` | source `runtime/native/afkxr/afk_powerboost.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/powerboost.cpp:1` rewritten |
| 242 | `src/powerboost.cpp:51` | `a1b3818` | `afk_flowdiag.cpp` | source `runtime/native/afkxr/afk_flowdiag.cpp`:91 | `1357409` lines | `0xc9dc08cfc2f5920f` `kDamageableKey` | `src/quest/plugins/q15t/src/powerboost.cpp:51` rewritten |
| 243 | `src/powerboost.cpp:52` | `a1b3818` | `afk_flowdiag.cpp` | source `runtime/native/afkxr/afk_flowdiag.cpp`:93 | `1357409` lines | `0x3693ea0` `kDamageableVptr` | `src/quest/plugins/q15t/src/powerboost.cpp:52` rewritten |
| 244 | `src/powerboost.h:1` | `a1b3818` | `afk_powerboost.cpp` | source `runtime/native/afkxr/afk_powerboost.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/powerboost.h:1` rewritten |
| 245 | `src/powerboost_core.h:2` | `bd4c6ba` | `afk_powerboost_core.h` | source `runtime/native/afkxr/afk_powerboost_core.h` | `1357409` file |  | not carried: the line is gone from `src/quest/plugins/q15t/src/powerboost_core.h` |
| 246 | `src/powerboost_core.h:16` | `a1b3818` | `pb L111-112, L330-333` | EchoCombat `plugin/powerboost.cpp`:111-112,330-333 | EC `c14c3e0a` lines | `this one left my cone` | `src/quest/plugins/q15t/src/powerboost_core.h:16` rewritten |
| 247 | `src/powerboost_core.h:17` | `a1b3818` | `pb L501-502` | EchoCombat `plugin/powerboost.cpp`:501-502 | EC `c14c3e0a` lines | `UNLINK_BASE`* | `src/quest/plugins/q15t/src/powerboost_core.h:17` rewritten |
| 248 | `src/powerboost_core.h:61` | `a1b3818` | `c14c3e0a` | EchoCombat `plugin/powerboost.cpp` | EC `c14c3e0a` group |  | `src/quest/plugins/q15t/src/powerboost_core.h:61` rewritten |
| 249 | `src/powerboost_thunk.inc:1` | `a1b3818` | `afk_powerboost.cpp` | source `runtime/native/afkxr/afk_powerboost.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/powerboost_thunk.inc:1` rewritten |
| 250 | `src/refresh_rate.cpp:20` | `d78d0da` | `afk_hz.cpp` | source `runtime/native/afkxr/afk_hz.cpp` | `dbea67e` file |  | `src/quest/plugins/q15t/src/refresh_rate.cpp:30` rewritten |
| 251 | `src/refresh_rate.cpp:23` | `d78d0da` | `afk_gunconnect.cpp` | source `runtime/native/afkxr/afk_gunconnect.cpp` | `dbea67e` file |  | `src/quest/plugins/q15t/src/refresh_rate.cpp:41` rewritten |
| 252 | `src/sensorwd.cpp:19` | `9f8b01a` | `afk_fx.cpp` `43dd2ad` | source `runtime/native/afkxr/afk_fx.cpp` ; source commit `43dd2ad` | `43dd2ad` named |  | `src/quest/plugins/q15t/src/sensorwd.cpp:19` rewritten |
| 253 | `src/sensorwd.h:1` | `9f8b01a` | `afk_fx.cpp` | source `runtime/native/afkxr/afk_fx.cpp` | `43dd2ad` file |  | `src/quest/plugins/q15t/src/sensorwd.h:1` rewritten |
| 254 | `src/shieldgamma.h:2` | `a1b3818` | `afk_shieldgamma.cpp` | source `runtime/native/afkxr/afk_shieldgamma.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/shieldgamma.h:2` rewritten |
| 255 | `src/voipdrop.cpp:16` | `a1b3818` | `afk_voipdrop.cpp` | source `runtime/native/afkxr/afk_voipdrop.cpp`:51 | unpinned | tried `kTable = 0x168`: at :43 of the newest candidate, cited :51 | `src/quest/plugins/q15t/src/voipdrop.cpp:16` rewritten |
| 256 | `src/voipdrop.h:1` | `a1b3818` | `afk_voipdrop.cpp` | source `runtime/native/afkxr/afk_voipdrop.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/voipdrop.h:1` rewritten |
| 257 | `src/wallfire.h:1` | `a1b3818` | `afk_wallfire.cpp` | source `runtime/native/afkxr/afk_wallfire.cpp` | `1357409` file |  | `src/quest/plugins/q15t/src/wallfire.h:1` rewritten |
| 258 | `src/weapon_arm.h:1` | `1215eb7` | `afk_gunconnect.cpp` | source `runtime/native/afkxr/afk_gunconnect.cpp` | `a3d0ffd` file |  | `src/quest/plugins/q15t/src/weapon_arm.h:1` rewritten |
| 259 | `tests/dronekill_test.cpp:376` | `a1b3818` | `afk_dronekill_core.h` | source `runtime/native/afkxr/afk_dronekill_core.h` | `1357409` file |  | `src/quest/tests/q15t/dronekill_test.cpp:376` rewritten |
| 260 | `tests/dronemuzzle_test.cpp:62` | `a1b3818` | `plugin/dronefix.cpp 132-147 / 916-934` | EchoCombat `plugin/dronefix.cpp`:132-147,916-934 | EC `c14c3e0a` lines | `void TickFrame()`* `Pose *__fastcall MuzzleTransform`* | not carried: the line is gone from `src/quest/tests/q15t/dronemuzzle_test.cpp` |
| 261 | `tests/ecfx_wall_stub.cpp:2` | `a1b3818` | `afk_ecfx.cpp` | source `runtime/native/afkxr/afk_ecfx.cpp` | `1357409` file |  | `src/quest/tests/q15t/ecfx_wall_stub.cpp:2` rewritten |
| 262 | `tests/memsys_test.cpp:190` | `a1b3818` | `afk_memsys.cpp` | source `runtime/native/afkxr/afk_memsys.cpp`:56-58 | `1357409` lines | `kGotSys = 0x371d020`* `kGotAlloc = 0x36e9b48`* `kGotInit = 0x36de5c0`* | `src/quest/tests/q15t/memsys_test.cpp:190` rewritten |
| 263 | `tests/mod/dronemuzzle.sh:24` | `bd4c6ba` | `afk_dronemuzzle.cpp` | source `runtime/native/afkxr/afk_dronemuzzle.cpp` | `1357409` file |  | not carried: the line is gone from `src/quest/tests/q15t/mod/dronemuzzle.cmake` |
| 264 | `tests/mod/dronemuzzle.sh:25` | `a1b3818` | `afk_dronemuzzle.cpp` | source `runtime/native/afkxr/afk_dronemuzzle.cpp` | `1357409` file |  | not carried: the line is gone from `src/quest/tests/q15t/mod/dronemuzzle.cmake` |
| 265 | `tests/music_test.cpp:49` | `a34ebda` | `afk_music.cpp` | source `runtime/native/afkxr/afk_music.cpp`:54-58 | `43dd2ad` lines | `kPlay[] = {0x2f22c384, 0xb1b06cfb}`* `kStop[] = {0xde586538, 0x2acba759}`* | `src/quest/tests/q15t/music_test.cpp:49` rewritten |
| 266 | `tests/music_test.cpp:55` | `a34ebda` | `afk_music.cpp` | source `runtime/native/afkxr/afk_music.cpp`:61-62 | `43dd2ad` lines | `0x43e2da7914642604` `0x43e2da7a0c623a19` | `src/quest/tests/q15t/music_test.cpp:55` rewritten |
| 267 | `tests/music_test.cpp:303` | `a34ebda` | `afk_music.cpp` | source `runtime/native/afkxr/afk_music.cpp`:102 | `43dd2ad` lines | `if (target == 0xFFFFFFFF) continue;`* | `src/quest/tests/q15t/music_test.cpp:303` rewritten |

## 5. Other references to the source's files

Lines of the old repository's last commit that name a file of the source by a `tools/...` path, with or without a line spec, and carry
no row of section 4's kind of token. The pin is the newest source commit, made at or before the line's old commit, that has the file.
A path that is in no commit of the source's history is marked so. These are file-level facts only.

| old file:line | old commit | cited | pin |
|---|---|---|---|
| `docs/adr/0001-match-refresh-rate.md:31` | `d78d0da` | `tools/re23/AGE_X2_READ.txt:44-46` | `dbea67e` |
| `docs/modules/ec.md:14` | `a1b3818` | `tools/scripts/regen_so.py:8504` | `1357409` |
| `docs/modules/ec.md:30` | `a1b3818` | `tools/build_runtime.py:52-53` | `1357409` |
| `docs/modules/heaps.md:5` | `a1b3818` | `tools/trials/qa-w15c-1028.json` | `a3d0ffd` |
| `docs/modules/heaps.md:6` | `a1b3818` | `tools/qcp_patches.py:62-71` | `1357409` |
| `docs/modules/heaps.md:84` | `a1b3818` | `tools/re3/res_heap_patch.py:7-12,163-170` | `1357409` |
| `docs/modules/heaps.md:85` | `a1b3818` | `tools/trials/qa-w15c-1028.json:4041` | `a3d0ffd` |
| `docs/modules/heaps.md:87` | `a1b3818` | `tools/re33/mempatch/mem_patch.py:1` | `1357409` |
| `docs/modules/heaps.md:89` | `a1b3818` | `tools/qcp_patches.py:69-70` | `1357409` |
| `docs/modules/heaps.md:90` | `a1b3818` | `tools/qcp_patches.py:11,67-68` | `1357409` |
| `docs/modules/sensorwd.md:5` | `9f8b01a` | `tools/fxlane/mk_sensorwd.py` | `43dd2ad` |
| `docs/modules/sensorwd.md:12` | `9f8b01a` | `tools/trials/qa-w18v-1028.json:3968` | `43dd2ad` |
| `docs/modules/sensorwd.md:137` | `9f8b01a` | `tools/trials/qa-w18v-1028.json:3968` | `43dd2ad` |
| `rows/README.md:20` | `728fdff` | `tools/re3/res_heap_patch.py:14-24` | `a3d0ffd` |
| `rows/gen_rows.py:66` | `728fdff` | `tools/re3/res_heap_patch.py:14-24` | `a3d0ffd` |
| `src/dronekill_core.h:85` | `a1b3818` | `tools/re33/DRONEFIX_F357_DESIGN.md` | in no commit of the source's history |
| `src/dronekill_core.h:118` | `a1b3818` | `tools/re33/DRONEKILL_STAGE1_DESIGN.md` | in no commit of the source's history |
| `src/dronekill_core.h:308` | `a1b3818` | `tools/re33/DRONEFIX_F357_DESIGN.md` | in no commit of the source's history |
| `src/levelname.cpp:13` | `a1b3818` | `tools/qcp_patches.py:386` | `1357409` |
| `src/powerboost_core.h:2` | `bd4c6ba` | `tools/re33/POWERBOOST_DESIGN.md` | in no commit of the source's history |
| `src/powerboost_core.h:3` | `a1b3818` | `tools/eclibs117/POWERBOOST_STAGE2_DESIGN.md` | in no commit of the source's history |
| `src/powerboost_core.h:60` | `a1b3818` | `tools/eclibs117/POWERBOOST_STAGE2_DESIGN.md` | in no commit of the source's history |

## 6. The old repository's 33 commits

| Commit | Date | Subject |
|---|---|---|
| `23a4d60` | 2026-10-28T08:59:40-05:00 | feat(plugin): clock-level witness and candidate as an X/Y build pair, GOT data-page swap only |
| `4bd253c` | 2026-10-28T09:02:30-05:00 | feat(plugin): refresh-rate witness and 90 Hz cap arm; one import-hook installer for both hooks |
| `47ad546` | 2026-10-28T09:03:13-05:00 | docs(plugin): the plugins entry the headset's config.yaml needs |
| `b579fbf` | 2026-10-28T09:06:16-05:00 | feat(plugin): return-address-gated headset wrappers for the permissions expression and the bot-body chooser |
| `275c5b5` | 2026-10-28T09:06:31-05:00 | docs(plugin): provenance row for the gates APK |
| `d78d0da` | 2026-10-28T09:11:27-05:00 | feat(plugin): the Hz arm is the source build's hzmatch90: 90 Hz while a combat map is loaded, the game's rate elsewhere |
| `04d444a` | 2026-10-28T09:11:40-05:00 | docs(plugin): provenance for the hzmatch APK; the global-cap Hz APK is superseded |
| `1215eb7` | 2026-10-28T09:43:10-05:00 | feat(plugin): T-26 weapon arm on the game tick, as plugin behaviour (Q15T_T26, default on) |
| `73f9b5e` | 2026-10-28T09:44:24-05:00 | test(plugin): per-module test and mutant harness, module contract, 8 tick callbacks |
| `46e027f` | 2026-10-28T09:47:48-05:00 | feat(rows): kWordPatch rows for R01, R03, R05, R16 (ADR 0021 s11), generated and self-checked |
| `42f7aba` | 2026-10-28T09:50:38-05:00 | fix(rows): convert to --extra-rows format 1 (memory-order bytes, x. ids, stock_build_id) |
| `0c16e72` | 2026-10-28T10:21:59-05:00 | test(plugin): guarded rm -rf, 2 GiB cap per test process, per-run mutant scratch, single-module mutant mode |
| `32bd191` | 2026-10-28T10:23:21-05:00 | test(plugin): every build and test in nevr-builds.slice (MemoryMax=4G), one mutant run at a time |
| `728fdff` | 2026-10-28T10:24:43-05:00 | feat(rows): B3 resource table-step rows (4 words), generator writes its first argument |
| `a1b3818` | 2026-10-28T13:19:27-05:00 | feat(plugin): gameplay modules ported as GOT wrappers and data transforms, wired with per-module options |
| `b4a702d` | 2026-10-28T13:21:24-05:00 | docs: handoff before the mirror directory rename |
| `d14323a` | 2026-10-28T14:30:52-05:00 | docs(handoff): full mutant run on a1b3818 finished, 1478 killed, 0 survived |
| `bd4c6ba` | 2026-10-28T14:33:22-05:00 | docs: replace the upstream project's name and the old scratch path with neutral/q15t names |
| `a8ef503` | 2026-10-28T18:51:16-05:00 | feat(clock): one binary, clock raise chosen at run time by args {clock: raise} |
| `a34ebda` | 2026-10-28T18:52:54-05:00 | feat(music): PostSoundEvent wrapper stops stacked combat music; wired as Q15T_MUSIC (default ON, OFF in the test arms) |
| `ee42cc9` | 2026-10-28T19:00:31-05:00 | feat(early): libq15t_early.so, heaps and memsys as their own early:true plugin |
| `9f8b01a` | 2026-10-28T19:03:04-05:00 | feat(sensorwd): scan-sphere watchdog (T-07) ported and wired as Q15T_SENSORWD (default ON, OFF in the test arms) |
| `60e077a` | 2026-10-28T19:04:12-05:00 | docs(handoff): state after the single binary, music, early library and sensorwd |
| `504ed0f` | 2026-10-29T10:40:05-05:00 | docs(handoff): the APK, the PR patch and the open steps (subject rewritten: the original names a person or a seat) |
| `318a5b8` | 2026-10-29T10:50:38-05:00 | docs(handoff): standing rule on how the plugin ships |
| `a679530` | 2026-10-29T11:57:24-05:00 | docs(handoff): the repack PR merged, scratch removed |
| `5560d46` | 2026-10-29T11:57:39-05:00 | docs(handoff): the PR patch and repack log are removed |
| `484a803` | 2026-10-30T03:51:46-05:00 | docs(handoff): the launch variant APK recipe and the v6.6.0 result |
| `7c0da85` | 2026-10-30T05:21:04-05:00 | docs(handoff): resume line, v6.7.0 variant, full mutant run result |
| `5089021` | 2026-10-30T07:03:01-05:00 | feat(ec): ec and ecfx are on in a player install with no switch file |
| `4e0263c` | 2026-10-30T07:16:28-05:00 | feat(gate): the menu gate: the headset-type expression is told no headset kind (Q15T_GATE_LINK, default ON) |
| `c9c9014` | 2026-10-30T07:56:53-05:00 | docs(handoff): integration into nevr-runtime replaces the earlier distribution rule (subject rewritten: the original names a person or a seat) |
| `8b24d66` | 2026-10-30T08:19:55-05:00 | fix(build): the library carries a config marker, so a build whose ec default words are empty is visible |

Every commit's content reached the tree through `ef714347` except the files of section 9 and the citations of section 4.

## 7. The four module rules

The old repository's worker contract (`docs/MODULE_CONTRACT.md`, lines 47-48 and 56-57) held four design rules for a module that the tree did not
state. They are now in `src/quest/plugins/q15t/README.md`, "Rules this plugin keeps" (which already held three other rules: no code page,
the build ID, no game bytes):

1. Every read of game memory goes through `SafeRead`.
2. A wrapper never faults the game; on any doubt it calls the real function.
3. A wrapper on a high-fan-in function keeps counters and logs nothing per call.
4. Every address, offset and layout is a cited source line or a quoted disassembly word; anything unread is labelled unverified.

The contract's other rules concern the old repository's process, and the tree already has each: the build slice, the 4 GiB memory cap, the
2 GiB address-space cap and the guarded `rm -rf` are in `justfile` (`test-quest-q15t`, `Q15T_TEST_VMEM_KB`) and in
`src/quest/tests/q15t/mutants.sh`; the static-initialiser rule is `just check-android-static-init`; logging is `docs/standards/logging.md`;
the build-ID gate is the plugin's `InitEx`.

## 8. Variant libraries

A variant APK is `nevr-runtime-<tag>-q15t-quest.apk`, the release's own loader asset plus two q15t libraries built outside the tree
(`tools/quest_release_timer.py`, the variant steps). Nothing in the tree builds the variant on a schedule.

**What the release host answers now.** The timer's own `variant_exists` request, for every tag from v6.6.0: a GET, without following a redirect,
of `<public base>/nevr-runtime/<tag>/nevr-runtime-<tag>-q15t-quest.apk` and, if that is there, of its `.sha256`, with the timer's user agent and
`Accept-Encoding: identity` (`variants.py` repeats that request; it also reads the zip of an APK that answers 200). The base is the public one that
`docs/reference/verifying-a-release.md` documents and is not repeated here. As a control `control.py` makes the same request for the release's own
`nevr-runtime-<tag>-quest.apk.sha256`.

| Tag | variant APK | variant `.sha256` | the release's own APK `.sha256` (control) |
|---|---|---|---|
| v6.6.0 | 404 | 404 | 404 |
| v6.7.0 | 404 | 404 | 404 |
| v6.8.0 | 404 | 404 | 404 |
| v6.9.0 | 404 | 404 | 404 |
| v6.10.0 | 404 | 404 | 404 |
| v6.11.0 | 404 | 404 | 404 |
| v6.12.0 | 404 | 404 | 404 |
| v6.13.0 | 404 | 404 | 404 |
| v6.14.0 | 404 | 404 | 404 |
| v6.15.0 | 404 | 404 | 404 |
| v6.16.1 | 404 | 404 | 404 |
| v6.16.2 | 404 | 404 | 200 |
| v6.16.3 | 404 | 404 | 200 |
| v6.16.4 | 404 | 404 | 200 |
| v6.16.5 | 404 | 404 | 200 |

**Result: the host has no variant APK for any of the 15 tags (404 for each), so there is no variant APK on the host whose digest or libraries could be read.** The host also answers 404 for the release's own APK of every tag before v6.16.2, so a 404 for a variant of those tags does not say that none was served when it was published.

**What the timer's log holds.** The timer's JSONL log (749 records) has no `variant_build`, `variant_publish` or `variant_library` record: the timer never
built a variant. Its variant records are `variant_exists` with `listed: false` and `variant_pins` failures, for v6.11.0 to v6.14.0, from
2026-10-30T18:40:57Z to 2026-10-31T07:00:10Z. Each failure's text says the same: the library found has sha256
`1b5fbdc38b866b1d00a3ea825f0e3167cd2507c779f24624c4301878324bc271`, the pin is `a2932a7c8fb38a313dda3b1da6a9c142f110573eded60ac6c93c5a7ee300cd65`.

**What the old repository and its local scratch record.** Hand-built variants, as the old repository's `HANDOFF.md` (lines 24-25, 28, 35, 37, 44) records them,
with the digests read from the files:

| Tag | variant APK sha256 | libraries (sha256) | the old commit whose build they are |
|---|---|---|---|
| v6.1.1-dev.3 (a development build of 2026-10-29, built from nevr-runtime `152d9684`; not a release tag) | `3611376667390b602fb853d0aa5717543fc009bd4fac8a4d3685970dde5821fc` (`HANDOFF.md` line 24) | `libq15t.so` `a2932a7c...` and `libq15t_early.so` `387b7aff...` (`HANDOFF.md` line 25: eight hex digits each). **Inference:** the two files of the v6.6.0 row, whose digests start the same | the default preset (`HANDOFF.md` line 28), not tied to an old commit there |
| v6.6.0 | `b83770c75739193de7e5f54fa9956517e83dc201f0b8c7b69aeef3bdda2582bc` (the `.sha256` left by the repack; `HANDOFF.md` says it was serving) | `libq15t.so` `a2932a7c8fb38a313dda3b1da6a9c142f110573eded60ac6c93c5a7ee300cd65`; `libq15t_early.so` `387b7aff7539f88c015785e5a5d904e0662d4ce2c4a1175a29afa14540de38bf` (the two files the repack used) | `9f8b01a`, the default preset (the old `HANDOFF.md`'s statement; the libraries were not rebuilt for this record) |
| v6.7.0 | `52166b82c58e795d78343610d2f99686052a70f1a76b0d0a0fcff8b43dec7a98` (`HANDOFF.md`: serving from 2026-10-30 09:25Z); a later local repack left the `.sha256` `b6eb58acac3139d91097470be6afc7df730602bd12a8dbbc32ccee1e262c8699` (file time 2026-10-30 07:17 -0500), whether it was uploaded is not established | not established: `HANDOFF.md` records no library for it, and the repack log names the libraries' paths, not their digests | not established |
| v6.8.0 to v6.16.5 | none found: no record of a hand-built variant, none on the host | | |

**The libraries in the old repository's ignored `build/`.** Read from the files (`sha256sum`, `stat`); these are the only plugin libraries left in the repository's folder:

| File | Size | Modified (UTC) | sha256 |
|---|---|---|---|
| `build/android-arm64-gates/libq15t.so` | 269344 | 2026-10-29T00:03:41Z | `0b6eafda620830e5ebb4a1eb4ed012a6487639b5982df685f35654a3795dac4a` |
| `build/android-arm64-hz/libq15t.so` | 273344 | 2026-10-29T00:03:41Z | `010eba0293380c2b8a74094516d6036f354d4cd636d1298b3adcdc1cb6a93833` |
| `build/android-arm64-t26/libq15t.so` | 282784 | 2026-10-29T00:03:41Z | `4ef1aaa9f9c12356863659ff80d085fbb5ba912c46e0acecfa4aeb653a26f31d` |
| `build/android-arm64-x/libq15t.so` | 267088 | 2026-10-29T00:03:49Z | `1913137785c588f1c4c2e0e53247e74b45ff0a2b98cd987880cbdd7d5b0e2b57` |
| `build/android-arm64-y/libq15t.so` | 257184 | 2026-10-28T23:25:52Z | `5a55d1c4ee717916efe97b8e0b607c95eb9f45ecac4411142dd999c03a150d86` |
| `build/android-arm64/libq15t.so` | 1242896 | 2026-10-30T13:14:02Z | `1b5fbdc38b866b1d00a3ea825f0e3167cd2507c779f24624c4301878324bc271` |
| `build/android-arm64/libq15t_early.so` | 173080 | 2026-10-30T12:02:50Z | `2206efaf199f6f4d0086ca569b4f1c4650a57754365688cb5527f34d9e3ccb45` |

`build/android-arm64/libq15t.so` is the library of the timer's failures above (`1b5fbdc3...`), built 2026-10-30T13:14Z, five minutes before the commit `8b24d66` (2026-10-30T13:19:55Z).
**Inference:** it is the build of the tree committed as `8b24d66`: the library carries a `q15t-config` marker with the four fields `src/plugin.cpp` writes at lines 173-175 of that commit
(`ec_words=[types=VPTO equip hide] ecfx_default_on=1 gate_link=1 build_id=b243509c...`), and `git log -S q15t_config_marker` finds only `8b24d66`, the commit that added the marker.
The other build trees (`android-arm64-x`, `-y`, `-hz`, `-gates`, `-t26`) hold the libraries of the 2026-10-28 test arms. They are not the plugins in the test APKs of section 9: the old `PROVENANCE.md` says an arm rebuilt
from a later commit gives different bytes, and the APK's plugin is the row of that table. The old `HANDOFF.md` names `1913137785c5` (the first 12 hex of `android-arm64-x`'s library) as the plugin of a package for the APK builder.

## 9. Files of the old repository with no counterpart in the tree

212 tracked files. Against `origin/main`, by path mapping (`docs/modules/*` to `docs/quest/q15t/modules/*`, `src/*` to `src/quest/plugins/q15t/src/*`,
`tests/mod/*.sh` to `src/quest/tests/q15t/mod/*.cmake`, `tests/*` to `src/quest/tests/q15t/*`): 38 identical, 161 different (descendants, and 26
`tests/mod/*.sh` compared as text with the `.cmake` files that replaced them), 13 with no file at the mapped path:

| File | What became of it |
|---|---|
| `.gitignore` | ignored `build/` and `out/` only; the tree has its own |
| `CMakeLists.txt`, `CMakePresets.json` | replaced by the plugin's and the host tests' CMake and presets in the tree; the old A/B arm presets (X, Y, hz, gates, t26) are replaced by `args {clock: raise}` and the module options |
| `HANDOFF.md` | the old repository's session state. Its variant facts are section 8; the rest is commands for a session that no longer exists |
| `PROVENANCE.md` | the five test APKs of 2026-10-28, below |
| `README.md` | superseded by `src/quest/plugins/q15t/README.md` |
| `docs/MODULE_CONTRACT.md` | section 7 |
| `rows/README.md`, `rows/gen_rows.py`, `rows/libr15_word_rows.json` | the private word-patch rows R01, R03, R05 and the four B3 rows are replaced by GOT hooks; R16 is documented in `docs/quest/q15t/modules/impactsnd.md` ("the source's `kPatch` (`:90-96`)") and rebuilt in C++ (rows 141 and 143 hold R16's source lines, row 142 R15's); the generator belongs to the retired mechanism (ADR 0035 rule 5, #2009) |
| `tests/mod/pbhud_ndk.sh`, `tests/run_host_tests.sh` | imported at `ef714347`, then replaced by the CMake project and `just test-quest-q15t` (ADR 0041) |
| `tools/config.yaml.example` | superseded by the packaged-plugin manifest (ADR 0019 section 2) |

**The five test APKs of PROVENANCE.md.** Built in a generic nevr-runtime worktree at `origin/main` `b237acd8`, sentinel build-id `9540967c5d5328778569764ffda8682a91eb5c3b`, store APK
`r15_goldmaster_store-4987566.apk`, signer `CN=NEVR Debug`, versionCode 51602006. No tracked file, issue or PR of this repository names these digests.

| APK | plugin source commit | plugin sha256 (in the APK) | plugin build-id | APK sha256 |
|---|---|---|---|---|
| repack-x | `4bd253c` | `7ec30abf18fd670c7b5fd07b8053c6d0dde10edf7eeb4a766f2cb60adb6100a7` | `b6a1b8a849f36cbd512c0a6f5521e254a3b93fa8` | `0b8c3ad20bc87eee330c91d5e981f3256f5684e4b9ce1c114d28a1b439a446e5` |
| repack-y | `4bd253c` | `ee6e32275e20af4ec04f34b4b175e05742a707785b35de2be7d0cf286230d22c` | `aa47f32e2b8d0c2be4c7c25e45288f35c18e6516` | `9b39853120f457630c1826fe388bf4af6163fb8839542aab0e0a02744e4f4caf` |
| repack-hz (superseded: a global 90 Hz cap, not the match behaviour) | `4bd253c` | `adda657252e342175ef4a8d636679db97a31e0daaa153c346051cdfec2bccd11` | `dd62e0ad4e21fcef545680097bdf5fa00cb06823` | `29d2560a101a8bbe94531ca11ee4d00331ec8d4a54b597bb2a6b6178005f146f` |
| repack-gates | `b579fbf` | `9f328f9b2c7a5938e4e1698b2fc679373004afda60f152ea355a9fc6e5027e4d` | `ff12ae0c7a4177f8bd7bc7ae6d54515b45a32d6a` | `abe61805b0540172df0e9ca91989137bb7c099b37c830f8b440047980d254088` |
| repack-hzmatch (the Hz arm: 90 Hz in combat maps, ADR 0001) | `d78d0da` | `e920af629ad40fc55b74bbedbff2fd086ca03a814a557dce43a11d0e37b46d38` | `c9b210b7cb63f48c9d06501a4586711863db3b04` | `436435c62daf42caf7a7739295bfefdd0182530b446516238b6e98050dec0485` |

## 10. Deletion check

To be read with the commands of section 1.

1. The old repository's last commit is all its content (section 1: one branch, no tag, stash, remote or unreachable object; no file ever deleted or
   renamed; no origin-token string in any earlier revision that is absent from the last commit).
2. Its working tree is clean but for the ignored `build/`: `git status --short --ignored` prints `!! build/` and nothing else. The library digests of
   `build/` are in section 8.
3. Items 1 to 7 of the issue have landed, in the commit that adds this file: the rows (section 4), the rewritten citation lines and commit subjects
   (sections 4 and 6), the commit log (section 6), the four rules (section 7 and the README), the variant provenance (section 8), and the credit trailers.
4. `check.py` against this record: see the line below, which `check.py` prints and the commit message repeats.

`check.py` on this record, run before this line was written: `check: 267 rows, 267 token lines at the old last commit, 22 other references; 0 failures`; counts: {'ec dronefix agree': 1, 'ec group': 3, 'ec lines': 52, 'ec powerboost agree': 1, 'ec veilnet agree': 1, 'named': 5, 'src file': 64, 'src lines': 140, 'tree gone': 12, 'tree nofile': 4, 'tree rewritten': 249, 'tree same': 2, 'unpinned': 5}

