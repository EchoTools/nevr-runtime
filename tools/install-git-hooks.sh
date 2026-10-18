#!/usr/bin/env bash
# Install the tracked post-checkout hook (tools/git-hooks/post-checkout, #667) into this clone's hooks
# directory as a symlink to the MAIN worktree's copy. Run once per clone: `just install-hooks`.
# Never overwrites: an existing link to the same hook is fine; any other file there is named and refused.
set -u

main_top="$(git worktree list --porcelain 2>/dev/null | sed -n '1s/^worktree //p')"
if [ -z "$main_top" ]; then
    echo "install-git-hooks: not inside a git repository" >&2
    exit 1
fi
src="$main_top/tools/git-hooks/post-checkout"
if [ ! -f "$src" ]; then
    echo "install-git-hooks: $src does not exist" >&2
    exit 1
fi
hooks_dir="$(git rev-parse --path-format=absolute --git-path hooks)" || exit 1
target="$hooks_dir/post-checkout"

if [ -L "$target" ] && [ "$(readlink -- "$target")" = "$src" ]; then
    echo "install-git-hooks: $target already links to $src" >&2
    exit 0
fi
if [ -e "$target" ] || [ -L "$target" ]; then
    echo "install-git-hooks: refusing: $target already exists and is not a link to $src" >&2
    exit 1
fi
mkdir -p -- "$hooks_dir" || exit 1
if ln -s -- "$src" "$target"; then
    echo "install-git-hooks: linked $target -> $src" >&2
    exit 0
fi
echo "install-git-hooks: could not create $target" >&2
exit 1
