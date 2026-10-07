#!/usr/bin/env bash
# On-device install of the repacked Quest APK and the game data (run by `just quest-install`; see the
# comment above that recipe in the justfile for what it does and why).
# usage: tools/quest-install.sh [yes] APK DATA_URL DATA_SHA256
set -euo pipefail
yes=${1-}
apk=${2:?usage: quest-install.sh [yes] APK DATA_URL DATA_SHA256 (use "" for the first when not passing yes)}
data_url=${3:?data_url missing}
data_sha256=${4:?data_sha256 missing}
pkg=com.readyatdawn.r15
default_data_sha256=fc2eedeacc50d9ddf751e21914bb4188660cf5e79ce48aab24b84e757f4b543c
remote_zip=/data/local/tmp/_data.zip
need_kb=2306867      # 2.2 GiB: the zip, the extracted files and the APK need about 1.98 GB on one partition, plus headroom
max_bytes=1100000000 # download size cap

die() { echo "error: $*" >&2; exit 1; }

# 1. Host inputs, before anything touches the headset.
for tool in adb curl sha256sum timeout awk; do
    command -v "$tool" >/dev/null 2>&1 || die "required tool '$tool' not found on PATH"
done
[ -f "$apk" ] || die "APK not found: $apk (build it with: just android-repack-apk)"
if [ -n "$yes" ] && [ "$yes" != "yes" ]; then
    die "unrecognized argument '$yes' (the only accepted value is: yes)"
fi

# 2. Exactly one authorized device.
devices_out=$(timeout 30 adb devices 2>&1) || die "'adb devices' failed or timed out: $devices_out"
# Only "<serial> <state>" rows count: a server-version warning ("adb server version (..) doesn't
# match ...") or a "* daemon ..." line is not a device.
mapfile -t rows < <(printf '%s\n' "$devices_out" | awk 'NF == 2 && $2 ~ /^(device|offline|unauthorized|authorizing|connecting|recovery|sideload|rescue|bootloader|no)$/ && $1 != "List" && $1 !~ /^\*/')
[ "${#rows[@]}" -ne 0 ] || die "no adb device attached"
[ "${#rows[@]}" -eq 1 ] || die "${#rows[@]} adb devices attached, need exactly one: ${rows[*]}"
serial=$(printf '%s' "${rows[0]}" | awk '{print $1}')
state=$(printf '%s' "${rows[0]}" | awk '{print $2}')
case "$state" in
    device) ;;
    unauthorized) die "device $serial is unauthorized: accept the USB debugging prompt in the headset and retry" ;;
    *) die "device $serial is in state '$state', need 'device'" ;;
esac
# adb against the chosen device with a time limit, so a wedged adb cannot hang the install:
# at SECONDS adb-args...
at() { local secs=$1; shift; timeout "$secs" adb -s "$serial" "$@"; }
timeout 30 adb -s "$serial" wait-for-device || die "device $serial did not become ready within 30 s"

# 3. Installed state: distinguish adb failure from "not installed".
set +e
pm_out=$(timeout 30 adb -s "$serial" shell pm path "$pkg" 2>&1)
pm_rc=$?
set -e
if [ "$pm_rc" -eq 0 ]; then
    echo "$pkg is installed on $serial"
elif [ "$pm_rc" -eq 124 ]; then
    die "package check timed out after 30 s on $serial"
elif [ -z "$pm_out" ]; then
    echo "$pkg is not installed on $serial"
else
    die "package check failed (exit $pm_rc): $pm_out"
fi

# 4. Free space on the headset (KiB available).
for path in /sdcard /data/local/tmp; do
    avail=$(timeout 30 adb -s "$serial" shell "df -Pk $path" 2>&1 | awk 'NR==2 {print $4}') || avail=''
    case "$avail" in ''|*[!0-9]*) die "could not read free space on $path (got '$avail')" ;; esac
    [ "$avail" -ge "$need_kb" ] || die "$path has $((avail / 1024)) MiB free, need $((need_kb / 1024)) MiB"
done

# 5. Game data: cached, hash-verified download.
verify() { [ "$(sha256sum "$1" | awk '{print $1}')" = "$data_sha256" ]; }
# The cache file is named for the hash it must match: a different data_sha256 (a typo, or another
# build of the data) gets its own file instead of deleting a good cached zip that belongs to the pin.
if [ "$data_sha256" = "$default_data_sha256" ]; then
    data=build/android-arm64/quest-data/_data.zip
else
    data="build/android-arm64/quest-data/_data-${data_sha256:0:16}.zip"
fi
mkdir -p "$(dirname "$data")"
if [ -f "$data" ] && ! verify "$data"; then
    echo "Cached $data does not match the pinned SHA-256; deleting it."
    rm -f "$data"
fi
if [ ! -f "$data" ]; then
    echo "Downloading game data (~937 MB) to $data.part ..."
    rm -f "$data.part"
    curl -fL --retry 2 --proto '=https' --proto-redir '=https' \
        --connect-timeout 20 --speed-limit 10000 --speed-time 60 --max-time 3600 \
        --max-filesize "$max_bytes" -o "$data.part" "$data_url" \
        || { rm -f "$data.part"; die "download failed: $data_url"; }
    if ! verify "$data.part"; then
        rm -f "$data.part"
        die "downloaded file does not match pinned SHA-256 $data_sha256"
    fi
    mv "$data.part" "$data"
fi
echo "Game data verified (sha256 $data_sha256)"

# 6. Install the APK; uninstall only if the headset says the signature differs.
echo "Installing $apk ..."
set +e
install_out=$(at 600 install -r -g "$apk" 2>&1)
install_rc=$?
set -e
echo "$install_out"
if [ "$install_rc" -ne 0 ]; then
    case "$install_out" in
        *INSTALL_FAILED_UPDATE_INCOMPATIBLE*)
            if [ "$yes" != "yes" ]; then
                echo "The installed $pkg is signed differently and cannot be updated in place." >&2
                echo "Removing it deletes its app data on the headset (/data/data/$pkg," >&2
                echo "/sdcard/Android/data/$pkg, and its OBB) and the data cannot be recovered." >&2
                die "re-run with the 'yes' argument to allow it: just quest-install yes"
            fi
            echo "Uninstalling $pkg (deletes its app data) ..."
            at 120 uninstall "$pkg" || die "uninstall of $pkg failed or timed out"
            at 600 install -g "$apk" || die "install of $apk failed or timed out after uninstall"
            ;;
        *) die "adb install failed (exit $install_rc)" ;;
    esac
fi

# 7. Push and extract; the temp zip is removed on every exit path.
cleanup_remote() {
    at 60 shell "rm -f $remote_zip" >/dev/null 2>&1 \
        || echo "warning: could not remove $remote_zip from the headset" >&2
}
trap cleanup_remote EXIT
echo "Pushing + extracting game data to /sdcard/readyatdawn ..."
at 1800 push "$data" "$remote_zip" || die "adb push failed or timed out"
at 900 shell "mkdir -p /sdcard/readyatdawn && cd /sdcard/readyatdawn && unzip -o $remote_zip" \
    || die "unzip on the headset failed or timed out (partial files may remain in /sdcard/readyatdawn)"
echo "Done."
