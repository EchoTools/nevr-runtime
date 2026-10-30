# Shared by launch-client.sh, launch-server.sh and verify-server.sh (source it after cd'ing into the checkout).
# Defines functions and one variable; running it has no side effects.

# The game's process name (comm) is "Main Thread", so `pgrep -x echovr.exe` never matches. This pattern only narrows
# the candidates for game_pids; a process that merely names the file (a shell's argument, a hash tool) matches it too,
# so game_pids decides on the command line itself (#1490).
ECHOVR_CMDLINE='(^|[/\\])echovr\.exe( |$)'

# game_pids: prints the pid of each of this user's game processes, one per line, and nothing when there is none. A
# process is the game when argv[0] (the first NUL-separated field of /proc/<pid>/cmdline, its last path element split on
# / or \) is echovr.exe, as in ./echovr.exe or C:\...\echovr.exe; or argv[0] is a Wine loader (wine, wine64,
# wine-preloader) and argv[1] is echovr.exe. A shell or a tool whose argument names the file is not the game. The
# Python twin is tools/scenario/run_scenario.py game_pids. Always returns 0, so a caller under set -e can test the output.
game_pids() {
  local pid name0 name1
  local -a argv
  for pid in $(pgrep -u "$(id -u)" -f "$ECHOVR_CMDLINE"); do
    [[ -r /proc/$pid/cmdline ]] || continue
    mapfile -d '' -t argv < "/proc/$pid/cmdline" || continue
    name0=${argv[0]:-}; name0=${name0##*[/\\]}
    name1=${argv[1]:-}; name1=${name1##*[/\\]}
    if [[ $name0 == echovr.exe || ( $name0 == wine* && $name1 == echovr.exe ) ]]; then
      printf '%s\n' "$pid"
    fi
  done
  return 0
}

# game_pids_in <root>: game_pids, narrowed to the games whose working directory is inside <root> (the launchers start the
# game with its working directory in <root>/echovr/bin/win10). A game on another copy of the install is not listed.
game_pids_in() {
  local root pid cwd
  root=$(readlink -f -- "$1") || return 0
  for pid in $(game_pids); do
    cwd=$(readlink -f -- "/proc/$pid/cwd" 2>/dev/null) || continue
    if [[ $cwd == "$root" || $cwd == "$root"/* ]]; then printf '%s\n' "$pid"; fi
  done
  return 0
}

# run_game_pids: the games a run on GAME_ROOT answers to. All of them, unless the caller sets NEVR_SCOPE_RUN_TO_COPY=1
# together with NEVR_GAME_ROOT: that says this run is one of several the caller starts side by side, each on its own copy
# of the install and its own NEVR_LAUNCH_LOCK, and only a game on this copy counts (tools/invite_e2e.py runs two clients).
run_game_pids() {
  if [[ "${NEVR_SCOPE_RUN_TO_COPY:-}" == 1 && -n "${NEVR_GAME_ROOT:-}" ]]; then
    game_pids_in "$NEVR_GAME_ROOT"
  else
    game_pids
  fi
}

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
# restore that. The flock covers the caller's whole run (including its restore); game_pids covers a
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
  running=$(run_game_pids)
  if [[ -n $running ]]; then
    echo "ERROR: echovr.exe is already running (pid ${running//$'\n'/ }); stop it first" >&2
    exit 4
  fi
}

# block_host_browser: a test launch never opens a browser on the owner's desktop (#1447). Wine opens a URL by running the
# `xdg-open` it finds on PATH (winebrowser; the registry's WineBrowser setting and $BROWSER are not consulted by the Wine in
# use), so this puts stubs for xdg-open and xdg-email first on PATH for the launcher and the game it starts. Each stub
# appends the address without its query (the device code is in the query) to <work root>/browser-stub/blocked.log and
# opens nothing. Call it after the run lock is held: it rewrites the stubs. Sets BROWSER_BLOCK_LOG and changes PATH.
block_host_browser() {
  local dir="$NEVR_WORK_ROOT/browser-stub" name
  work_dir "$dir" "Stubs for xdg-open and xdg-email that stand in for a host browser while a test launch runs (#1447); blocked.log is the record of each blocked open." \
    "the stubs are rewritten by every launch; blocked.log is kept as the record of blocked opens." || return 1
  for name in xdg-open xdg-email; do
    cat > "$dir/$name" <<'STUB'
#!/bin/bash
# Installed by the nevr-runtime launchers (tools/lib/game_install.sh block_host_browser): a test never opens a browser.
url=${1:-}
printf '%s blocked %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "${url%%[?#]*}" >> "$(dirname "$0")/blocked.log"
exit 1
STUB
    chmod 755 "$dir/$name" || { echo "ERROR: cannot make $dir/$name executable" >&2; return 1; }
  done
  # shellcheck disable=SC2034 # read by the launcher that sources this file (its note names the record), not here
  BROWSER_BLOCK_LOG="$dir/blocked.log"
  export PATH="$dir:$PATH"
}

# creds_store_file <account>: where one test account's stored sign-in lives, $NEVR_WORK_ROOT/creds/<account>/.credentials.json.
# The account is a plain name (letters, digits, _ . -, not starting with a dot), so it cannot name another path. Prints the
# path; exits 2 for any other name.
creds_store_file() {
  local account=$1
  [[ "$account" =~ ^[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}$ ]] || {
    echo "ERROR: an account name is letters, digits, _ . - (not starting with a dot): got '$account'" >&2; exit 2; }
  printf '%s\n' "$NEVR_WORK_ROOT/creds/$account/.credentials.json"
}

# check_stored_sign_in <path> <account>: the stored sign-in must be a plain file that no one but its owner can read (mode
# 0600 or tighter). Exits 3 and names the path and the reason, never the content. A missing file is the first run of an
# account: the message says how to make it.
check_stored_sign_in() {
  local path=$1 account=$2 mode
  if [[ ! -e "$path" && ! -L "$path" ]]; then
    echo "ABORT: no cached credentials for $account: run once with --device-code and --save-creds $account (the sign-in is kept at $path)" >&2
    exit 3
  fi
  if [[ -L "$path" || ! -f "$path" ]]; then
    echo "ABORT: $path is not a plain file; a stored sign-in is one (AGENTS.md \"Client login test mechanics\")" >&2
    exit 3
  fi
  mode=$(stat -c %a -- "$path") || { echo "ERROR: cannot read the mode of $path" >&2; exit 2; }
  if (( 8#$mode & 077 )); then
    echo "ABORT: $path is mode $(printf '%04o' "$((8#$mode))"); a stored sign-in must be 0600 (chmod 600 it)" >&2
    exit 3
  fi
}

# save_stored_sign_in <the runtime's sign-in file> <account's stored path>: keeps a new sign-in once, at mode 0600. The
# file is made under a temporary name at that mode and given its final name with a hard link, which fails when the path
# already exists, so a stored sign-in is never replaced. Prints the path, never the content.
save_stored_sign_in() {
  local from=$1 path=$2 tmp
  if [[ ! -f "$from" || -L "$from" || ! -s "$from" ]]; then
    echo "FAIL: the login wrote no sign-in file to $from; nothing was saved" >&2
    exit 1
  fi
  # The sign-in directories hold credentials: made 0700 here, before work_dir's mkdir -p, which would take the umask (755).
  install -d -m 700 -- "$(dirname "$(dirname "$path")")" && install -d -m 700 -- "$(dirname "$path")" \
    || { echo "ERROR: cannot make the sign-in directory for $path" >&2; exit 1; }
  work_dir "$(dirname "$path")" "Stored sign-ins of test accounts, one directory per account, each .credentials.json at mode 0600; a test client signs in with one (launch-client.sh --creds)." \
    "by hand: an account's directory is removed when its account is retired; no script removes one." || exit 1
  tmp="$path.new.$$"
  (umask 077 && install -m 600 -- "$from" "$tmp") || { echo "ERROR: cannot write $tmp" >&2; rm -f -- "${tmp:?}"; exit 1; }
  if ln -- "$tmp" "$path" 2>/dev/null; then
    rm -f -- "${tmp:?}"
    echo "=== sign-in saved for later runs: $path (mode 0600) ==="
  else
    rm -f -- "${tmp:?}"
    echo "FAIL: $path already exists; a stored sign-in is not replaced" >&2
    exit 1
  fi
}

# save_back_stored_sign_in <the run's sign-in file in the nevr root> <the account's stored path> <where to keep a refreshed
# sign-in if the save fails>: a runtime that refreshes its token replaces the link to the store with a regular file, and
# the store keeps the old token (#1469). A regular file that differs from the store is copied over it: a temporary name in
# the store's folder, mode 0600, then mv over the stored file. A link (nothing refreshed) or an identical file writes
# nothing. When the save fails the stored file is unchanged and the refreshed file is moved to the keep path (the run's
# copy is removed by the caller, so this keeps it); returns 1 and names both paths, never the content.
save_back_stored_sign_in() {
  local live=$1 path=$2 keep=$3 tmp
  if [[ -L "$live" || ! -f "$live" ]]; then return 0; fi
  if cmp -s -- "$live" "$path"; then return 0; fi
  tmp="$path.saveback.$$"
  if (umask 077 && cp -- "$live" "$tmp") && chmod 600 -- "$tmp" && mv -f -- "$tmp" "$path"; then
    echo "=== refreshed sign-in saved back to the store: $path (mode 0600) ==="
    return 0
  fi
  rm -f -- "$tmp"
  if mv -f -- "$live" "$keep" && chmod 600 -- "$keep"; then
    echo "ERROR: could not save the refreshed sign-in back to $path; the stored sign-in is unchanged; the refreshed one is kept at $keep" >&2
  else
    echo "ERROR: could not save the refreshed sign-in back to $path, and could not keep it at $keep; the refreshed sign-in is lost" >&2
  fi
  return 1
}

# host_stop_cause <the nevr root's logs folder> <the run's start, unix seconds>: when the host stopped the game
# (nevr-runtime.dll absent or refused), the runtime never ran and wrote no game log; the host's own record says why.
# Prints one line and returns 0, or prints nothing and returns 3 when the record holds no stop since then (#1122).
host_stop_cause() {
  python3 -I "$(dirname "${BASH_SOURCE[0]}")/../host_stop_cause.py" --log "$1/nevr-bootstrap.jsonl" --since "$2"
}
