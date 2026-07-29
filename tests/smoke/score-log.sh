#!/bin/bash
# score-log.sh — score a captured NEVR run log against the smoke-test signal table.
#
#   ./tests/smoke/score-log.sh <logfile> [all|boot|module|plugin|server|client]
#
# Separates RUNNING the game from JUDGING the log, so a log captured on a client
# is scored by exactly the same table as one captured on a server. See
# docs/process/smoke-tests.md for what each ID means and which run exercises it.
#
# Three outcomes, and the third one is the whole point:
#   PASS    the feature's success signal is present
#   FAIL    the feature's explicit failure signal is present
#   ABSENT  neither — the code path was never reached. NOT "working" and NOT
#           "broken": it means this run did not test it. Most IDs are ABSENT in
#           any single run, by design.
#
# EVERY pass-regex below was verified against real captured output, not copied
# out of a source format string. Those two drift: the logs in
# /var/tmp/work-nevr-runtime/server-runs/ still say "wave0 installing bug fix
# hooks" while current source says "installing binary bug fix hooks" (renamed in
# N113). Signals taken from source alone score ABSENT against a real run and
# look like missing features. When a string changes, fix it HERE and re-score a
# known-good log to prove the row still fires.
#
# Fields are @@-delimited, not |-delimited: | is ERE alternation, and using it as
# the field separator silently truncated every regex that contained one — rows
# scored ABSENT because the tool was broken, not the feature.
#
# Never pipe the game's live output into this script. Capture to a file, then
# score the file: grep block-buffers on a live pipe and the run looks hung.
set -uo pipefail

LOG="${1:?usage: score-log.sh <logfile> [all|boot|module|plugin|server|client]}"
WANT="${2:-all}"

[ -r "$LOG" ] || { echo "cannot read log: $LOG" >&2; exit 2; }

# Strip ANSI colour once up front. The game's log is colourised, so '$'-anchored
# patterns never match the raw file.
PLAIN="$(mktemp -t nevr-smoke-XXXXXX)"
trap 'rm -f "$PLAIN"' EXIT
sed -E 's/\x1b\[[0-9;]*m//g' "$LOG" > "$PLAIN"

# `all` auto-detects server vs client, because some rows are each other's
# negatives: C02 fails on `host=server`, which is CORRECT output for a server run.
# Scoring both profiles against one log therefore manufactured a FAIL out of a
# healthy server. Detect the mode and score only the applicable profile; an
# explicit group argument still overrides, so a deliberate cross-check is possible.
DETECTED=""
if [ "$WANT" = "all" ]; then
  if grep -qE 'Server mode . headless \+ noovr applied|bit0_render=CLEAR\(HEADLESS\)' "$PLAIN"; then
    DETECTED="server"
  elif grep -qE 'bit0_render=SET\(WINDOWED\)|\[NEVR\.AUTH\]' "$PLAIN"; then
    DETECTED="client"
  fi
fi

# ID @@ group @@ feature @@ pass-regex @@ fail-regex
# An empty fail-regex means the feature emits no distinct failure line, so it can
# only be PASS or ABSENT — absence is the only negative evidence available.
read -r -d '' TABLE <<'EOF'
B01@@boot@@MinHook initialised@@minhook initialized@@
B02@@boot@@Game function pointers resolved@@function pointers resolved@@
B03@@boot@@Hook install sequence completed@@\[NEVR\.PATCH\] All hooks installed@@\[NEVR\.FATAL\]
B04@@boot@@Every hook installed, none failed@@hooks installed: [0-9]+ succeeded, 0 failed@@hooks installed: [0-9]+ succeeded, [1-9][0-9]* failed
B05@@boot@@Boot log tee opened (nevr-boot.jsonl)@@boot log opened run=@@
B06@@boot@@Built-in log filter installed@@log filter installed@@
B07@@boot@@Log filter emitting health counters@@\[NEVR\.LOGFILTER\] health emitted=@@CAPTURED ZERO GAME LINES
B08@@boot@@Early config loaded from _local@@Early config loaded from:@@Failed to early-load config
B09@@boot@@DLL-load hooks installed@@dll load hooks installed|LoadLibrary hooks OK@@
B10@@boot@@Vectored exception handler installed@@veh installed@@
B11@@boot@@Crash-recovery hooks installed@@crash recovery hooks installed@@
B12@@boot@@Crash-dump probe hooked@@hooked name=HandleCrashDump@@hook failed name=HandleCrashDump
B13@@boot@@Crash-reporter launch blocked@@CreateProcess[AW] hook installed@@Failed to find CreateProcess[AW]
B14@@boot@@Module cache built (crash attribution)@@module cache refreshed count=@@
M01@@module@@Module loaded: platform_compat@@\[NEVR\.MODULE\] Loaded: platform_compat@@Failed to load platform_compat
M02@@module@@platform_compat: all 3 hooks installed@@platform_compat initialized: 3/3 hooks installed@@platform_compat initialized: [012]/3 hooks installed
M03@@module@@Schannel TLS 1.2/1.3 modernisation@@SSL/TLS moderni[sz]@@Failed to [a-z]+ AcquireCredentialsHandleW
M04@@module@@WinHTTP COM to libcurl bridge@@winhttp=ok@@WinHTTP bridge NOT installed
M05@@module@@Wine _temp directory fix@@createdir=ok@@Failed to install CreateDirectory[AW] hook
M06@@module@@Module loaded: token_auth@@token_auth initialized@@Failed to load token_auth
P01@@plugin@@Plugin directory scanned@@\[NEVR\.PLUGIN\] Scanning for plugins in:@@
P02@@plugin@@At least one plugin candidate found@@Found [1-9][0-9]* plugin candidate@@No plugins directory or no plugins found
P03@@plugin@@Plugin loaded at API v3 with capabilities@@\[NEVR\.PLUGIN\] Loaded: .*\(API v[0-9]+\) caps=0x@@init failed with code
P04@@plugin@@Plugin received host context@@init: base=0x[0-9a-f]+ flags=0x@@init: context is null
P05@@plugin@@Superseded log_filter.dll refused (N89)@@SKIPPED .* superseded by the built-in log filter@@
S01@@server@@Server mode applied@@Server mode . headless \+ noovr applied@@
S02@@server@@Headless engine-flag mask applied@@engine flags 0x[0-9A-Fa-f]+ -> 0x[0-9A-Fa-f]+ \(bit0_render=CLEAR\(HEADLESS\)\)@@
S03@@server@@Loading-tips system disabled@@Disabled loading tips system@@
S04@@server@@OVR platform branch bypassed@@OVR platform branch bypassed@@
S05@@server@@Fatal error handler installed@@Fatal error handler installed@@
S06@@server@@pnsrad enabler patched@@\[pnsrad\] init complete@@\[pnsrad\] unexpected bytes|\[pnsrad\] LdrRegisterDllNotification failed
S07@@server@@Embedded resource override registered@@\[NEVR\.RESOURCE\] Init complete \([0-9]+ overrides registered\)@@\[NEVR\.RESOURCE\] MH_[A-Za-z]+Hook failed
S08@@server@@Binary bug-fix hooks installed@@installing binary bug fix hooks|binary bug fix hooks installed@@already initialized, skipping duplicate
S09@@server@@BusyWait RET patch applied@@patched name=CPrecisionSleep::BusyWait@@
S10@@server@@Timing hooks live (GetTimeMicroseconds)@@hooked name=GetTimeMicroseconds@@
S11@@server@@Per-frame tick dispatching (N111)@@per-frame tick ALIVE@@
S12@@server@@Tick host identity correct: server (N110)@@per-frame tick ALIVE.*host=server@@per-frame tick ALIVE.*host=client
S13@@server@@Hook liveness self-report emitted@@hook_liveness name=@@
S14@@server@@pnsrad log output hooked and filtered@@hooked name=pnsrad!CLog::PrintfImpl@@hook failed name=pnsrad!CLog::PrintfImpl
S15@@server@@Service endpoints redirected from config@@Service redirect \[@@
S16@@server@@WebSocket bridge listening@@\[NEVR\.WS\] Proxy listening on@@no nevr_socket_uri in early config
S17@@server@@Game connected through the bridge@@\[NEVR\.WS\] Proxy: game connected@@
S18@@server@@Login request injected@@\[NEVR\.WS\] login injected xpid=@@
S19@@server@@XPID carries DSC platform prefix@@login injected xpid=DSC@@login injected xpid=(PSN|UNK)
S20@@server@@Service accepted the login@@\[NEVR\.WS\] LOGIN SUCCESS@@\[NEVR\.WS\] LOGIN FAILURE
S21@@server@@GameServer initialised@@\[NEVR\.GAMESERVER\] Initialized game server@@
S22@@server@@IServerLib resolved in-process@@serverlib symbol resolved@@
S23@@server@@ServerDB registration requested@@Requested game server registration@@Failed to initiate WebSocket connection
S24@@server@@ServerDB registration accepted@@Received registration success via protobuf: server_id=@@Error received before registration
S25@@server@@Server authenticated via token@@Server authenticated \(token acquired\)@@
S26@@server@@Asset CDN hook installed@@\[NEVR\.CDN\] Loadout_ResolveDataFromId hook installed@@
S27@@server@@CDN manifest fetched@@\[NEVR\.CDN\] Manifest loaded: [0-9]+ packages@@Manifest fetch failed|Failed to resolve cache directory
S28@@server@@CDN tints loaded into memory@@\[NEVR\.CDN\] Fetch complete: .*[1-9][0-9]* tints loaded@@\.evrp bad magic|\.evrp file too small
S29@@server@@UPnP port mapping added@@\[NEVR\.UPNP\] Port mapping added:@@\[NEVR\.UPNP\] No UPnP devices found|AddPortMapping failed|No valid IGD
S30@@server@@Telemetry stream connected@@\[NEVR\.TELEMETRY\] Connected to telemetry server@@No telemetry_uri in config|\[NEVR\.TELEMETRY\] Connection error
S31@@server@@Shutdown signal handlers installed@@POSIX signal handlers installed|console ctrl handler installed@@SetConsoleCtrlHandler FAILED|Failed to register SIG
S32@@server@@Ctrl handler re-armed to front of chain@@console ctrl handler re-armed to front of chain@@
S33@@server@@Shutdown deps pre-resolved (no loader lock)@@shutdown deps resolved@@
S34@@server@@CTRL+C reached the runtime@@shutdown signal received@@
S35@@server@@Graceful shutdown initiated@@Graceful shutdown initiated@@
S36@@server@@Lobby unregistered from ServerDB@@Unregistered game server@@
S37@@server@@ServerDB teardown completed cleanly@@teardown complete \(lobby unregistered@@
S38@@server@@WebSocket listener released its socket@@listener stopped, [0-9]+ remote connection\(s\) closed@@listener may leak
S39@@server@@Patched BusyWait byte restored on exit@@shutdown restored BusyWait byte@@
S40@@server@@No null-deref during teardown@@shutdown null_deref=0@@shutdown null_deref=[1-9]
S41@@server@@GameServer terminated@@Terminated game server@@
C01@@client@@Windowed engine-flag mask applied@@bit0_render=SET\(WINDOWED\)@@
C02@@client@@Tick host identity correct: client (N110)@@per-frame tick ALIVE.*host=client@@per-frame tick ALIVE.*host=server
C03@@client@@token_auth configured@@\[NEVR\.AUTH\] Configured: url=@@Missing nevr_http_uri or nevr_http_key
C04@@client@@Device-code prompt displayed@@Link your account at:@@Failed to request device auth code
C05@@client@@Device authorised by user@@Device authorized! Signed in successfully@@Device auth timed out|Device code expired|Error polling device code
C06@@client@@Refresh token persisted to disk@@Refresh token saved to@@Failed to write .credentials.json
C07@@client@@Cached token reused on restart@@Using cached credentials|Loaded cached token@@Cached token expired, no refresh token
C08@@client@@Access token refreshed@@Token refreshed successfully@@Token refresh failed
C09@@client@@Bearer token attached to connection@@Attaching Bearer token to remote connection@@Using URL credentials \(no Bearer token\)
C10@@client@@Friends list subscribed@@FriendListSubscribeRequest sent@@
C11@@client@@HTTP served through the curl bridge@@\[NEVR\.HTTP\] Response: 2[0-9][0-9]@@\[NEVR\.HTTP\] curl failed:
C12@@client@@Loadout save round-tripped@@\[SAVE_SUCCESS\]@@\[SAVE_LOADOUT\] Not in active session|\[SAVE_LOADOUT\] No game base address
C13@@client@@Current loadout read back@@\[CURRENT_LOADOUT\] Player:@@\[CURRENT_LOADOUT\] Empty response|\[CURRENT_LOADOUT\] Invalid
EOF

pass=0; fail=0; absent=0; failed_ids=""
printf '%-5s %-7s %-46s %s\n' "ID" "GROUP" "FEATURE" "RESULT"
printf '%s\n' "-------------------------------------------------------------------------------------"

while IFS= read -r row; do
  [ -n "${row:-}" ] || continue
  id="${row%%@@*}";            rest="${row#*@@}"
  group="${rest%%@@*}";        rest="${rest#*@@}"
  feature="${rest%%@@*}";      rest="${rest#*@@}"
  pass_re="${rest%%@@*}";      fail_re="${rest#*@@}"

  case "$WANT" in
    all)
      # Under auto-detection, skip the profile that does not apply to this log.
      case "$group" in
        server) [ "$DETECTED" = "client" ] && continue ;;
        client) [ "$DETECTED" = "server" ] && continue ;;
      esac
      ;;
    "$group") ;;
    *) continue ;;
  esac

  # File input, never a pipe: grep in a pipeline can exit 141 on SIGPIPE (N101).
  hit_pass=$(grep -Ec -- "$pass_re" "$PLAIN" 2>/dev/null || true)
  hit_fail=0
  [ -n "$fail_re" ] && hit_fail=$(grep -Ec -- "$fail_re" "$PLAIN" 2>/dev/null || true)

  if [ "${hit_pass:-0}" -gt 0 ]; then
    result="PASS    (${hit_pass}x)"; pass=$((pass+1))
  elif [ "${hit_fail:-0}" -gt 0 ]; then
    result="FAIL    (${hit_fail}x)"; fail=$((fail+1)); failed_ids="$failed_ids $id"
  else
    result="ABSENT  (not reached)"; absent=$((absent+1))
  fi
  printf '%-5s %-7s %-46s %s\n' "$id" "$group" "$feature" "$result"
done <<< "$TABLE"

printf '%s\n' "-------------------------------------------------------------------------------------"
printf 'log     = %s\n' "$LOG"
printf 'groups  = %s%s\n' "$WANT" "${DETECTED:+ (detected: $DETECTED)}"
printf 'PASS=%d  FAIL=%d  ABSENT=%d\n' "$pass" "$fail" "$absent"
[ -n "$failed_ids" ] && printf 'failed  =%s\n' "$failed_ids"
echo
echo "ABSENT is not a pass. It means this run did not exercise that feature."
echo "docs/process/smoke-tests.md says which run does."

# Exit non-zero only on an explicit failure signal. ABSENT is a coverage gap:
# reported, but not an error, because no single run covers every ID.
[ "$fail" -eq 0 ]
