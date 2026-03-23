#!/usr/bin/env zsh
set -euo pipefail

# Profile an EchoVR dedicated server under Wine.
# Captures perf data for analysis with analyze-profile.sh.
#
# Usage:
#   ./extras/profile-server.sh                  # defaults: timestep=120, 60s capture
#   TIMESTEP=60 ./extras/profile-server.sh      # test at 60Hz
#   DURATION=30 ./extras/profile-server.sh      # shorter capture
#   WINEFSYNC=1 ./extras/profile-server.sh      # test fsync mode

TIMESTEP="${TIMESTEP:-120}"
DURATION="${DURATION:-60}"
PERF_FREQ="${PERF_FREQ:-997}"
SERVER_DIR="${SERVER_DIR:-/mnt/games/CustomLibrary/ready-at-dawn-echo-arena}"
PROFILE_DIR="${PROFILE_DIR:-/var/tmp/nevr-profiles}"
CONFIG_PATH="${CONFIG_PATH:-./_local/l.config.json}"

mkdir -p "$PROFILE_DIR"
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
LABEL="ts${TIMESTEP}-${TIMESTAMP}"
PERF_DATA="$PROFILE_DIR/perf-${LABEL}.data"
CPU_LOG="$PROFILE_DIR/cpu-${LABEL}.log"
SERVER_LOG="$PROFILE_DIR/server-${LABEL}.log"

echo "=== NEVR Server Profiling ==="
echo "  Timestep:  ${TIMESTEP} Hz"
echo "  Duration:  ${DURATION}s"
echo "  Perf freq: ${PERF_FREQ} Hz"
echo "  Output:    ${PROFILE_DIR}/"
echo "  Wine env:  WINEFSYNC=${WINEFSYNC:-unset} WINEESYNC=${WINEESYNC:-unset}"
echo ""

# Start server in background
pushd "$SERVER_DIR" >/dev/null
wine ./bin/win10/echovr.exe -noovr -server -headless -noconsole -timestep "$TIMESTEP" \
    ${CONFIG_PATH:+-config-path "$CONFIG_PATH"} \
    >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
popd >/dev/null

echo "Server PID: $SERVER_PID (wine)"

# Wait for wine to spawn the actual game process
sleep 3
GAME_PID=$(pgrep -f 'echovr.exe' | head -1 || true)
if [[ -z "$GAME_PID" ]]; then
    echo "ERROR: Could not find echovr.exe process"
    kill "$SERVER_PID" 2>/dev/null || true
    exit 1
fi
echo "Game PID:   $GAME_PID"

# Start CPU monitoring in background
echo "timestamp,pid,cpu_pct,mem_pct,vsz_kb,rss_kb,command" > "$CPU_LOG"
pidstat -p "$GAME_PID" 1 "$DURATION" 2>/dev/null | \
    awk 'NR>3 && /echovr/ {print strftime("%H:%M:%S")","$4","$8","$9","$10","$11","$12}' \
    >> "$CPU_LOG" &
PIDSTAT_PID=$!

echo ""
echo "=== Capturing CPU usage for ${DURATION}s ==="
echo "    (connect a client now for in-game profiling)"
echo ""

# Wait a few seconds for server to settle, then start perf
sleep 5
echo "Starting perf record (${DURATION}s at ${PERF_FREQ}Hz)..."
perf record -g -F "$PERF_FREQ" -p "$GAME_PID" -o "$PERF_DATA" -- sleep "$DURATION" &
PERF_PID=$!

# Wait for perf to finish
wait "$PERF_PID" 2>/dev/null || true
echo "perf record complete."

# Stop monitoring
kill "$PIDSTAT_PID" 2>/dev/null || true

# Summary
echo ""
echo "=== Results ==="
echo "  perf data:  $PERF_DATA"
echo "  CPU log:    $CPU_LOG"
echo "  Server log: $SERVER_LOG"
echo ""

# Quick CPU summary
if [[ -s "$CPU_LOG" ]]; then
    echo "=== CPU Summary ==="
    awk -F, 'NR>1 && $2!="" {sum+=$2; n++} END {if(n>0) printf "  Avg CPU: %.1f%%  (n=%d samples)\n", sum/n, n}' "$CPU_LOG"
    awk -F, 'NR>1 && $2!="" {if($2>max) max=$2} END {if(max>0) printf "  Peak CPU: %.1f%%\n", max}' "$CPU_LOG"
fi

echo ""
echo "Run: ./extras/analyze-profile.sh $PERF_DATA"
echo "Or:  perf report -i $PERF_DATA --no-children --percent-limit 1"
echo ""
echo "Server still running (PID $SERVER_PID). Kill with: kill $SERVER_PID"
