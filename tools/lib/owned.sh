# owned.sh: the removal a shell tool may make (ORDER.md, "The manifest rule"). Source it; do not run it.
#
#   owned_build_tree_rm <path> <allowed parent>
#
# Removes <path> recursively, and only when its real path is strictly inside the real path of <allowed parent> and no
# component between the two is a symbolic link. Otherwise it prints why on standard error, removes nothing, and returns
# 2. A path that is already absent returns 0. It never follows a link and never removes the allowed parent itself.
#
# Use it for a build tree, a tool's own scratch folder made under a named parent, or a folder the tool made with a
# recorded name. A single named file the tool made is removed with rm -f on its literal name, not with this function.

owned_build_tree_rm() {
  local path="$1" parent="$2" rel part cur real_parent real_path
  if [ -z "$path" ] || [ -z "$parent" ]; then
    echo "owned_build_tree_rm: needs a path and an allowed parent" >&2
    return 2
  fi
  if [ -L "$parent" ]; then
    echo "owned_build_tree_rm: refused: $parent is a symbolic link" >&2
    return 2
  fi
  if [ ! -e "$path" ] && [ ! -L "$path" ]; then
    return 0
  fi
  real_parent="$(realpath -e -- "$parent")" || { echo "owned_build_tree_rm: refused: cannot resolve $parent" >&2; return 2; }
  # The path, lexically under the parent, with no '..' left after normalising.
  rel="$(realpath -m --relative-to="$parent" -- "$path")"
  case "$rel" in
    ""|.|..|../*|/*)
      echo "owned_build_tree_rm: refused: $path is not inside $parent" >&2
      return 2 ;;
  esac
  cur="$parent"
  local IFS=/
  for part in $rel; do
    cur="$cur/$part"
    if [ -L "$cur" ]; then
      echo "owned_build_tree_rm: refused: $cur is a symbolic link" >&2
      return 2
    fi
  done
  unset IFS
  real_path="$(realpath -e -- "$path")" || { echo "owned_build_tree_rm: refused: cannot resolve $path" >&2; return 2; }
  case "$real_path" in
    "$real_parent"/?*) ;;
    *)
      echo "owned_build_tree_rm: refused: real path $real_path is not strictly inside $real_parent" >&2
      return 2 ;;
  esac
  rm -rf -- "$real_path"
}
