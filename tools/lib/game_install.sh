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

# refuse_copy_credentials <game root>: a throwaway copy of the install carries no sign-in. When NEVR_GAME_ROOT names the
# root (a copy; unset is the main checkout's own install, which holds the owner's sign-in), any `.credentials.json*`
# file under <game root>/echovr, in `_local` or in the nevr root of the Wine prefix, makes the run sign in as whoever
# the file belongs to. Prints one line naming the files by path (never their content) and exits 3 (the exit of
# launch-client.sh's other "not pristine" aborts); exits 2 when the tree cannot be searched, so a failed search is
# not read as "none". A symbolic link counts; a directory does not. AGENTS.md "The game install", "Make it safe".
refuse_copy_credentials() {
  local root=$1 listing
  [[ -n "${NEVR_GAME_ROOT:-}" ]] || return 0
  listing=$(find "$root/echovr" -name '.credentials.json*' ! -type d -print) \
    || { echo "ERROR: cannot search $root/echovr for sign-in files (.credentials.json*); not starting the game" >&2; exit 2; }
  [[ -z "$listing" ]] && return 0
  echo "ABORT: the game copy holds sign-in file(s) the game would sign in with: ${listing//$'\n'/ } (remove them: AGENTS.md \"The game install\", \"Make it safe\")" >&2
  exit 3
}

# One game run at a time: a second run would save the first run's test DLL as its "original" and
# restore that. The flock covers the caller's whole run (including its restore); the pgrep covers a
# game started some other way. fd 9 holds the lock: the caller closes it (9>&-) for the children it
# starts so a leftover wineserver cannot hold it. Exits 4; exits 3 first when refuse_copy_credentials refuses the copy.
acquire_game_run_lock() {
  local running
  # Before the lock file, the work directory or any deploy is touched. GAME_ROOT is the caller's resolve_game_root.
  refuse_copy_credentials "${GAME_ROOT:?acquire_game_run_lock needs GAME_ROOT (resolve_game_root)}"
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

# host_stop_cause <the nevr root's logs folder> <the run's start, unix seconds>: when the host stopped the game
# (nevr-runtime.dll absent or refused), the runtime never ran and wrote no game log; the host's own record says why.
# Prints one line and returns 0, or prints nothing and returns 3 when the record holds no stop since then (#1122).
host_stop_cause() {
  python3 -I "$(dirname "${BASH_SOURCE[0]}")/../host_stop_cause.py" --log "$1/nevr-bootstrap.jsonl" --since "$2"
}
