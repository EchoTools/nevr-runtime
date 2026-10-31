#!/usr/bin/env bash
# Collect what we need to see when the Quest build will not start: the same steps as quest-diagnose.ps1.
#
# Reads facts from ONE headset over adb (headset model and OS build, the game's version, the data folders, the
# runtime's own log and crash files), starts the game ONCE and reads the log of that launch, prints a one-screen
# verdict, then writes everything to nevr-diagnose-<UTC>.zip. It never installs or uninstalls anything and deletes
# nothing on the headset; here it deletes only its own temporary folder. On the headset it does three things: it closes
# the game if it is running, it erases the headset's log buffer (after saving a copy as logcat_before_launch.txt), and
# it starts the game once.
#
# Not collected: device_login.txt (the sign-in link and code), the credential cache, config files. The adb serial, the
# hardware serial (ro.serialno) and any e-mail address are replaced with <device-serial> and <email> in every text file
# and in the verdict. NOT masked: the zip holds the runtime's crash dumps as they are and the headset's whole system log,
# which can show the Wi-Fi name, the BSSID, IP addresses and numeric user ids. Do not attach it to a public GitHub
# issue; send it to a maintainer by direct message on the EchoVRCE Discord, never in a public channel.
#
# usage: tools/quest-diagnose.sh [--adb PATH] [--out DIR] [--wait SECONDS]
# adb is taken from --adb, else from the PATH. Connect exactly one headset by USB with USB debugging on.
# Needs bash 4 or newer (mapfile); the bash 3.2 that macOS ships is too old.
set -uo pipefail
pkg=com.readyatdawn.r15
activity=com.oculus.gles3jni.MainActivity
media=/sdcard/Android/media/$pkg
legacy=/sdcard/readyatdawn
filesdir=/sdcard/Android/data/$pkg/files
adb_path=""
out_dir=""
wait_s=20
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
log_file="$here/nevr-diagnose.log"

run_log() { printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >>"$log_file" 2>/dev/null || echo "could not write the log $log_file" >&2; }
fail() { echo "ERROR: $*" >&2; echo "(also written to $log_file)" >&2; run_log "quest-diagnose.sh ERROR: $*"; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --adb) adb_path=${2:-}; [ -n "$adb_path" ] || fail "--adb needs a path"; shift 2 ;;
    --out) out_dir=${2:-}; [ -n "$out_dir" ] || fail "--out needs a folder"; shift 2 ;;
    --wait) wait_s=${2:-}; [ -n "$wait_s" ] || fail "--wait needs a number of seconds"; shift 2 ;;
    -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) fail "unknown argument '$1' (see --help)" ;;
  esac
done
case "$wait_s" in ''|*[!0-9]*) fail "--wait needs a whole number of seconds" ;; esac
[ -n "$out_dir" ] || out_dir=$here
[ -d "$out_dir" ] || fail "the output folder does not exist: $out_dir"
# Absolute before anything changes directory: the zip is written from inside the working folder.
out_dir=$(cd "$out_dir" && pwd) || fail "cannot enter the output folder: $out_dir"

if [ -n "$adb_path" ]; then
  [ -f "$adb_path" ] || fail "adb not found: $adb_path"
elif command -v adb >/dev/null 2>&1; then
  adb_path=$(command -v adb)
else
  fail "adb was not found on the PATH. Install Android platform-tools, or run this script with --adb <path to adb>."
fi
echo "Using adb: $adb_path"

# adb with a time limit when `timeout` exists; stdin is /dev/null so a wedged adb cannot hold the terminal.
with_limit() { local secs=$1; shift; if command -v timeout >/dev/null 2>&1; then timeout "$secs" "$@" </dev/null; else "$@" </dev/null; fi; }

# Exactly one headset, in the state `device`.
devices_out=$(with_limit 30 "$adb_path" devices 2>&1) || fail "adb devices failed or timed out: $devices_out"
mapfile -t rows < <(printf '%s\n' "$devices_out" | awk 'NF >= 2 && $1 != "List" && $1 !~ /^\*/ && $2 ~ /^(device|offline|unauthorized|authorizing|connecting|recovery|sideload|rescue|bootloader|host|unknown|detached|no)$/')
[ "${#rows[@]}" -ne 0 ] || fail "No headset is connected. Connect it by USB, turn on USB debugging, accept the prompt in the headset, and run this again."
[ "${#rows[@]}" -eq 1 ] || fail "${#rows[@]} devices are connected; disconnect all but the headset and run this again."
serial=$(printf '%s' "${rows[0]}" | awk '{print $1}')
state=$(printf '%s' "${rows[0]}" | awk '{$1=""; print substr($0, 2)}')
case "$state" in
  device) ;;
  "no permissions"*) fail "The headset reports: $state (usually missing USB rules for the headset on this machine)." ;;
  unauthorized) fail "The headset is unauthorized. Put it on, accept the USB debugging prompt, and run this again." ;;
  *) fail "The headset is in state '$state'; reconnect it and run this again." ;;
esac
dev() { local secs=$1; shift; with_limit "$secs" "$adb_path" -s "$serial" "$@" 2>&1; }

# The hardware serial also shows up in logcat when the headset is connected over Wi-Fi (the adb serial is then an address).
hw_serial=$(dev 30 shell getprop ro.serialno | tr -d '\r')
printf '%s' "$hw_serial" | grep -Eq '^[A-Za-z0-9._:-]{4,}$' || hw_serial=""

work=$(mktemp -d "${TMPDIR:-/var/tmp}/nevr-diagnose.XXXXXX") || fail "could not make a working folder"
trap 'rm -rf "$work"' EXIT
escape_re() { printf '%s' "$1" | sed 's/[][\.*^$/+?(){}|]/\\&/g'; }
mask="s/$(escape_re "$serial")/<device-serial>/g"
if [ -n "$hw_serial" ] && [ "$hw_serial" != "$serial" ]; then mask="$mask;s/$(escape_re "$hw_serial")/<device-serial>/g"; fi
# Replaces both serials and anything shaped like an e-mail address.
protect() { sed -E -e "$mask" -e 's/[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+/<email>/g'; }
save() { local rel=$1; mkdir -p "$work/$(dirname "$rel")"; protect >"$work/$rel"; }

utc=$(date -u '+%Y%m%dT%H%M%SZ')
zip_path="$out_dir/nevr-diagnose-$utc.zip"
[ ! -e "$zip_path" ] || fail "$zip_path exists; not overwritten"

echo "Reading the headset ..."
model=$(dev 30 shell getprop ro.product.model | tr -d '\r')
os_build=$(dev 30 shell getprop ro.build.display.id | tr -d '\r')
release=$(dev 30 shell getprop ro.build.version.release | tr -d '\r')
printf 'model: %s\nos build: %s\nandroid release: %s\n' "$model" "$os_build" "$release" | save device.txt

dump=$(dev 60 shell dumpsys package "$pkg")
version_code=$(printf '%s\n' "$dump" | grep -o -m1 'versionCode=[0-9]*' | head -n1 | cut -d= -f2)
version_name=$(printf '%s\n' "$dump" | grep -o -m1 'versionName=[^ ]*' | head -n1 | cut -d= -f2)
printf 'package: %s\nversionCode: %s\nversionName: %s\n' "$pkg" "$version_code" "$version_name" | save package.txt

installer=$(dev 30 shell "cat $media/.echo_installer_version" | tr -d '\r')
case "$installer" in *"No such file"*|*"not found"*) installer="" ;; esac
if [ -n "$installer" ]; then printf '%s\n' "$installer" | save installer_version.txt; else echo absent | save installer_version.txt; fi

# Listings: names and sizes, one level, plus the one level under _data. Never file contents.
list() { local name=$1 dir=$2; { echo "ls -la $dir"; dev 60 shell "ls -la $dir"; } | tr -d '\r' | save "listings/$name"; }
list readyatdawn.txt "$legacy"
list readyatdawn_data.txt "$legacy/_data"
list media.txt "$media"
list media_files.txt "$media/files"
list media_files_data.txt "$media/files/_data"
list media_asset_patches.txt "$media/asset_patches"
list android_data_files.txt "$filesdir"
remote_dir() { [ "$(dev 30 shell "[ -d $1 ] && echo yes || echo no" | tr -d '\r')" = yes ]; }
data_media=no; remote_dir "$media/files/_data" && data_media=yes
data_legacy=no; remote_dir "$legacy/_data" && data_legacy=yes
# The runtime also serves the Android/media files folder when it exists without _data (android_media_unmarked, #749).
media_dir=no; remote_dir "$media/files" && media_dir=yes

# The runtime's own log and crash files: nevr-sentinel*.log (this session, the kept previous sessions, the size-rotated
# files) and nevr-crashes. The Android/media files folder is read first (where the runtime writes them, and where a player
# can reach them); the Android/data folder is the fallback.
mkdir -p "$work/sentinel"
log_dir=""; log_pull=""; log_rc=1
for d in "$media/files" "$filesdir"; do
  log_pull=$(dev 120 pull "$d/nevr-sentinel.log" "$work/sentinel/nevr-sentinel.log"); log_rc=$?
  if [ "$log_rc" -eq 0 ]; then log_dir=$d; break; fi
done
kept_pull=""
if [ -n "$log_dir" ]; then
  while IFS= read -r name; do
    kept_pull="$kept_pull$name: $(dev 120 pull "$log_dir/$name" "$work/sentinel/$name" 2>&1 | tr '\n' ' ')"$'\n'
  done < <(dev 30 shell "ls $log_dir" | tr -d '\r' | grep -E '^nevr-sentinel\..+\.log$')
fi
crash_pull=""; crash_rc=1
for d in "$media/files" "$filesdir"; do
  crash_pull=$(dev 120 pull "$d/nevr-crashes" "$work/sentinel"); crash_rc=$?
  if [ "$crash_rc" -eq 0 ]; then break; fi
done
for f in "$work"/sentinel/nevr-sentinel*.log; do
  if [ -f "$f" ]; then protect <"$f" >"$f.protected" && mv "$f.protected" "$f"; fi
done
while IFS= read -r f; do
  case "$f" in *.log|*.txt|*.json) protect <"$f" >"$f.protected" && mv "$f.protected" "$f" ;; esac
done < <(find "$work/sentinel/nevr-crashes" -type f 2>/dev/null)
printf 'nevr-sentinel.log: exit %s %s (folder: %s)\n%snevr-crashes: exit %s %s\n' "$log_rc" "$log_pull" "${log_dir:-none}" "$kept_pull" "$crash_rc" "$crash_pull" | save sentinel/pull_results.txt

# ONE clean launch. The headset's log of the run the tester came with is saved first: logcat -c erases it.
echo "Starting the game once and waiting $wait_s s (this closes it first if it is running and erases the headset's log buffer after saving a copy) ..."
dev 120 logcat -d -b all | tr -d '\r' | save logcat_before_launch.txt
dev 30 shell am force-stop "$pkg" >/dev/null
dev 30 logcat -c >/dev/null
start_out=$(dev 60 shell am start -n "$pkg/$activity" | tr -d '\r')
sleep "$wait_s"
pid_out=$(dev 30 shell pidof "$pkg"); pid_rc=$?
running=no; if [ "$pid_rc" -eq 0 ] && printf '%s' "$pid_out" | grep -q '[0-9]'; then running=yes; fi
logcat=$(dev 120 logcat -d -b all | tr -d '\r')
printf 'am start: %s\nrunning after %s s: %s\n' "$start_out" "$wait_s" "$running" | save launch.txt
printf '%s\n' "$logcat" | save logcat.txt

# The verdict, from what was just read.
obb=$(printf '%s\n' "$logcat" | sed -n "s/.*OBB mounted at '\([^']*\)'.*/\1/p" | head -n1)
data_root=""
line=$(printf '%s\n' "$logcat" | grep -m1 '"event":"data_root"' || true)
if [ -n "$line" ]; then
  data_root=present
  r=$(printf '%s' "$line" | sed -n 's/.*"result":"\([^"]*\)".*/\1/p'); [ -n "$r" ] && data_root="result=$r"
  s=$(printf '%s' "$line" | sed -n 's/.*"source":"\([^"]*\)".*/\1/p'); [ -n "$s" ] && data_root="$data_root source=$s"
fi
crash_marker=$(printf '%s\n' "$logcat" | grep -o -m1 -E 'FATAL EXCEPTION|Fatal signal|am_crash|ANR in ' | head -n1)
problems=()
[ "$data_media" = yes ] || [ "$data_legacy" = yes ] || [ "$media_dir" = yes ] || problems+=("NO GAME DATA: neither $media/files nor $legacy/_data exists.")
if printf '%s' "$start_out" | grep -q 'Error'; then problems+=("The game did not start: $start_out")
elif [ "$running" != yes ]; then problems+=("The game was not running $wait_s s after it started (it ended or crashed).")
fi
[ -z "$crash_marker" ] || problems+=("The launch log has a crash marker: $crash_marker")
# #764: a failure is an exit, a death, a crash marker or the game's own Initialization Error. A line a build does not
# write (the OBB mount line) is never a failure of a game that is still running.
init_error=$(printf '%s\n' "$logcat" | grep -m1 'Initialization Error' | sed 's/^.*\(Initialization Error\)/\1/' || true)
init_file=$(printf '%s\n' "$logcat" | grep -m1 -o 'Failed to find package manifest file [^ ]*' || true)
[ -z "$init_error" ] || problems+=("The game logged: $init_error${init_file:+ ($init_file)}")
if [ "${#problems[@]}" -eq 0 ] && [ -z "$obb" ] && [ -z "$data_root" ]; then
  result="No failure seen: running after $wait_s s; this build logged no data-mount line, so the data step can't be confirmed from the log. Send the zip anyway if the game still misbehaves."
elif [ "${#problems[@]}" -eq 0 ]; then result="No problem found in these facts. Send the zip anyway if the game still misbehaves."
else result=$(printf '%s\n           ' "${problems[@]}"); result=${result%$'\n           '}; fi
present() { if [ "$1" = yes ]; then echo present; else echo absent; fi; }
media_note=""
if [ "$data_media" != yes ] && [ "$media_dir" = yes ]; then media_note=" (the files folder exists; the runtime serves it as android_media_unmarked)"; fi
verdict=$(cat <<V
NEVR Quest diagnose  $(date -u '+%Y-%m-%d %H:%M:%S')Z
Headset:   $model, OS build $os_build
Game:      $pkg versionCode ${version_code:-(not installed?)}, versionName ${version_name:--}
Installer: ${installer:-no .echo_installer_version found}
Data:      $media/files/_data: $(present "$data_media")$media_note
           $legacy/_data: $(present "$data_legacy")
Launch:    the game mounted data at: ${obb:-(no line)}
           runtime data_root line: ${data_root:-none (this build does not log one)}
           running after $wait_s s: $running
Verdict:   $result
Zip:       $(basename "$zip_path")  (in the --out folder, else next to this script; send this file)
V
)
verdict=$(printf '%s\n' "$verdict" | protect)
printf '%s\n' "$verdict" | save verdict.txt
echo
printf '%s\n' "$verdict"

# The zip: verdict first.
if command -v zip >/dev/null 2>&1; then
  (cd "$work" && zip -q "$zip_path" verdict.txt && zip -qrD "$zip_path" . -x verdict.txt) || fail "zip failed"
elif command -v python3 >/dev/null 2>&1; then
  python3 -I - "$work" "$zip_path" <<'PY' || fail "python3 could not write the zip"
import os, sys, zipfile
work, out = sys.argv[1:3]
names = sorted(os.path.relpath(os.path.join(d, f), work) for d, _, fs in os.walk(work) for f in fs)
names.sort(key=lambda n: (n != "verdict.txt", n))
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for n in names:
        z.write(os.path.join(work, n), n)
PY
else
  fail "neither zip nor python3 is installed, so the zip could not be written"
fi
run_log "OK quest-diagnose.sh wrote $zip_path problems=${#problems[@]}"
echo "Privacy: this zip holds crash dumps and the headset's system log (Wi-Fi name, BSSID, IP addresses, numeric user ids). Do not attach it to a public GitHub issue; send it to a maintainer by direct message on the EchoVRCE Discord: it is never in a public channel."
