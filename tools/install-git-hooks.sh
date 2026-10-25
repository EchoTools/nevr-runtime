#!/usr/bin/env bash
# Install the tracked git hooks (tools/git-hooks/*: post-checkout, #667; pre-push, #794) into this
# clone's hooks directory as symlinks to the MAIN worktree's copies. Run once per clone:
# `just install-hooks`. Never overwrites: an existing link to the same hook is fine; any other file
# there is named and refused (the other hooks are still installed, and the exit status is 1).
set -u

main_top="$(git worktree list --porcelain 2>/dev/null | sed -n '1s/^worktree //p')"
if [ -z "$main_top" ]; then
    echo "install-git-hooks: not inside a git repository" >&2
    exit 1
fi
hooks_dir="$(git rev-parse --path-format=absolute --git-path hooks)" || exit 1

status=0
found=0
for src in "$main_top"/tools/git-hooks/*; do
    [ -f "$src" ] || continue
    found=1
    target="$hooks_dir/$(basename -- "$src")"
    if [ -L "$target" ] && [ "$(readlink -- "$target")" = "$src" ]; then
        echo "install-git-hooks: $target already links to $src" >&2
        continue
    fi
    if [ -e "$target" ] || [ -L "$target" ]; then
        echo "install-git-hooks: refusing: $target already exists and is not a link to $src" >&2
        status=1
        continue
    fi
    mkdir -p -- "$hooks_dir" || { status=1; continue; }
    if ln -s -- "$src" "$target"; then
        echo "install-git-hooks: linked $target -> $src" >&2
    else
        echo "install-git-hooks: could not create $target" >&2
        status=1
    fi
done
if [ "$found" -eq 0 ]; then
    echo "install-git-hooks: $main_top/tools/git-hooks has no hooks" >&2
    exit 1
fi
exit "$status"
