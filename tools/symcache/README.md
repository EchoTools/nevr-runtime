# symcache

Regenerates `src/runtime/log/symcache_data.cpp`, the embedded table that maps
Echo VR symbol hashes to names, from the union of the Echo VR PC and Quest
subsets of [Dualgame/echo-symbols](https://github.com/Dualgame/echo-symbols).

## Input

`tools/symbols.pin` names the echo-symbols commit. The tool reads
`subsets/echo_vr-pc.json` and `subsets/echo_vr-quest.json` at that commit and
merges them:

- PC: 75,345 entries; Quest: 32,604 entries; union at pin `27fd4ce8`: 82,556
  entries (overlap 25,393 hashes, identical names, zero conflicts).
- A hash present in both keeps the PC name. Names that differ only by case (the
  CSymbol64 hash folds case) count as a conflict and the count is logged; names
  that differ beyond case fail the merge.
- `subsets/hash_lookup.json` is the 171,015-entry all-games union; the two
  subset files are the curated PC and Quest lists.

## Commands

```sh
go test ./...
go run . regenerate --pin-file ../../tools/symbols.pin \
  --input pc.json --quest-input quest.json \
  --out ../../src/runtime/log/symcache_data.cpp \
  --previous-from ../../src/runtime/log/symcache_data.cpp
go run . validate --input pc.json --quest-input quest.json --previous 75345
go run . diff --old old-pc.json --new new-pc.json \
  --old-quest old-quest.json --new-quest new-quest.json
```

The nightly `echo-symbols-sync` workflow fetches both subsets at the old and new
pins, validates and regenerates from the union, and opens or updates one PR.

## Validation

`ValidateCorpus` re-hashes every name with CSymbol64 and fails on a mismatch;
each subset is validated before the merge and the merged table afterwards. The
merged count must stay within 20% of the previous pin's count.
