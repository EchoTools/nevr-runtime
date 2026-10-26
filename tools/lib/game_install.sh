# Shared by launch-client.sh, launch-server.sh and verify-server.sh (source it after cd'ing into the checkout).
# Defines functions and one variable; running it has no side effects.

# The game's process name (comm) is "Main Thread", so `pgrep -x echovr.exe` never matches; match the
# command line, which starts with ./echovr.exe.
ECHOVR_CMDLINE='(^|[/\\])echovr\.exe( |$)'

# NEVR_WORK_ROOT and work_dir: where a run keeps what it writes (tools/lib/work_root.sh).
# shellcheck source=tools/lib/work_root.sh
source "$(dirname "${BASH_SOURCE[0]}")/work_root.sh"

# The game install (echovr/) exists only in the main checkout, so it is resolved independent of
# where the calling script lives: NEVR_GAME_ROOT if set, else the main checkout of this repository
# (the parent of the git common dir, the same from a worktree), else the current directory.
# Prints an absolute path or exits 2.
resolve_game_root() {
  local root common
  if [[ -n "${NEVR_GAME_ROOT:-}" ]]; then
    root=$NEVR_GAME_ROOT
  elif common=$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null); then
    root=$(dirname "$common")
  else
    root=$(pwd)
  fi
  [[ "$root" == /* ]] || { echo "ERROR: NEVR_GAME_ROOT must be an absolute path (got $root)" >&2; exit 2; }
  printf '%s\n' "$root"
}

# One game run at a time: a second run would save the first run's test DLL as its "original" and
# restore that. The flock covers the caller's whole run (including its restore); the pgrep covers a
# game started some other way. fd 9 holds the lock: the caller closes it (9>&-) for the children it
# starts so a leftover wineserver cannot hold it. Exits 4.
acquire_game_run_lock() {
  local running
  GAME_RUN_LOCK="${NEVR_LAUNCH_LOCK:-$NEVR_WORK_ROOT/launch-client.lock}"
  work_dir "$(dirname "$GAME_RUN_LOCK")" "The game-run lock and what tools keep next to it." \
    "the lock file is kept; it holds no data." || exit 4
  exec 9>"$GAME_RUN_LOCK" || { echo "ERROR: cannot open the lock file $GAME_RUN_LOCK" >&2; exit 4; }
  flock -n 9 || { echo "ERROR: another game run (launch-client.sh, launch-server.sh or verify-server.sh) holds $GAME_RUN_LOCK" >&2; exit 4; }
  if running=$(pgrep -u "$(id -u)" -f "$ECHOVR_CMDLINE"); then
    echo "ERROR: echovr.exe is already running (pid ${running//$'\n'/ }); stop it first" >&2
    exit 4
  fi
}
