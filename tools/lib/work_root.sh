# The work root for the shell tools: $NEVR_WORK_ROOT, else src/nevr-runtime-work under the home directory.
# Source it; it defines NEVR_WORK_ROOT and `work_dir`, and has no other side effect. The Python tools use
# tools/work_root.py; tools/tests/test_work_dir_contract.py holds the two to the same answer.
#
# work_dir <dir> <what it is for> <when it is removed>
#   Makes <dir>. When <dir> is the root or under it, it also writes README.md (what it is for, whose it is,
#   when it is removed) in the root, if missing, and in the top-level directory under the root that holds
#   <dir>. A README that exists is kept. A directory outside the root (a per-tool override) is only made.
#   Returns non-zero, and says why on stderr, when a directory or README cannot be written.
NEVR_WORK_ROOT="${NEVR_WORK_ROOT:-${HOME:?HOME is not set; set NEVR_WORK_ROOT}/src/nevr-runtime-work}"

_work_readme() {
  local dir=$1 purpose=$2 removed=$3
  [[ -e "$dir/README.md" ]] && return 0
  printf '# %s\n\n%s\n\nOwner: %s\nRemoved: %s\n' "${dir##*/}" "$purpose" "$(id -un)" "$removed" \
    >"$dir/README.md" || { echo "ERROR: cannot write $dir/README.md" >&2; return 1; }
}

work_dir() {
  local dir=$1 purpose=$2 removed=$3 relative top
  mkdir -p -- "$dir" || { echo "ERROR: cannot make $dir" >&2; return 1; }
  case "$dir" in
    "$NEVR_WORK_ROOT"|"$NEVR_WORK_ROOT"/*)
      _work_readme "$NEVR_WORK_ROOT" "Working files of the nevr-runtime tools and the agents that run them." \
        "each item when its named end comes; its creator removes it that turn." || return 1
      [[ "$dir" == "$NEVR_WORK_ROOT" ]] && return 0
      relative=${dir#"$NEVR_WORK_ROOT"/}
      top=$NEVR_WORK_ROOT/${relative%%/*}
      _work_readme "$top" "$purpose" "$removed" || return 1 ;;
  esac
}
