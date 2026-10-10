#!/usr/bin/env bash
# The Quest social frames that are live across a call into the game must carry no exception
# machinery (src/quest/social/social_internal.h, src/quest/sentinel/callback_thunk.h "Exceptions").
#
#   check_quest_social_frames.sh <nm> <readelf> <object>=<symbol-substring>...
#
# For each object: it defines a symbol containing <symbol-substring> (so the check is not vacuous),
# references no __gxx_personality_v0, and has no .gcc_except_table section (no LSDA). Exits nonzero
# on the first object that fails, naming it and the reason on stderr.
set -euo pipefail

if [ "$#" -lt 3 ]; then
    echo "usage: check_quest_social_frames.sh <nm> <readelf> <object>=<symbol-substring>..." >&2
    exit 2
fi
nm_bin="$1"
readelf_bin="$2"
shift 2

for spec in "$@"; do
    obj="${spec%%=*}"
    want="${spec#*=}"
    if [ ! -f "$obj" ]; then
        echo "check_quest_social_frames: no such object: $obj" >&2
        exit 1
    fi
    symbols="$("$nm_bin" "$obj")"
    sections="$("$readelf_bin" -S -W "$obj")"
    if ! grep -q -- "$want" <<<"$symbols"; then
        echo "check_quest_social_frames: $obj defines no symbol containing '$want' (the check would be vacuous)" >&2
        exit 1
    fi
    if grep -q '__gxx_personality_v0' <<<"$symbols"; then
        echo "check_quest_social_frames: $obj references the C++ personality routine (a frame with a landing pad)" >&2
        exit 1
    fi
    if grep -q '\.gcc_except_table' <<<"$sections"; then
        echo "check_quest_social_frames: $obj has an LSDA (.gcc_except_table): a frame with a landing pad" >&2
        exit 1
    fi
    echo "check_quest_social_frames: OK $obj"
done
