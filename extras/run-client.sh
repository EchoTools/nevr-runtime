#!/usr/bin/env zsh
set -euo pipefail

SERVER_LOG="${SERVER_LOG:-server.log}"
CLIENT_LOG="${CLIENT_LOG:-client.log}"
RUNTIME_SECONDS="${RUNTIME_SECONDS:-90}"
NON_INTERACTIVE="${NON_INTERACTIVE:-1}"
CAPTURE_MODE="${CAPTURE_MODE:-nslobby}"
CLIENT_START_REGEX="${CLIENT_START_REGEX:-\[NSLOBBY\] received lobby session success}"

ROOT_DIR="$(pwd)"
CLIENT_DIR="/mnt/games/CustomLibrary/echovr-vanilla"
SERVER_CMD=(
	cargo run -p nevr-server-rs --bin nevr-server-rs --
	--udp-addr 192.168.1.50:6792
	--fleet-url 'ws://g.echovrce.com:80/spr?discord_id=695081603180789771&guilds=1216923249615835156&password=wibIldIn3Twa&regions=spr-dev'
	--server-id nevr-server-rs-1
	--log-level debug
	--advertise-udp-ip 192.168.1.50
	--fleet-legacy-mode
)

sleep 3
function stop_all() {
	set +e
	if [[ -n "${CLIENT_PID:-}" ]]; then
		kill "$CLIENT_PID" >/dev/null 2>&1 || true
	fi
	pkill -f "./bin/win10/echovr.exe" >/dev/null 2>&1 || true
	pkill -f "target/debug/nevr-server-rs --udp-addr 192.168.1.50:6792" >/dev/null 2>&1 || true
}

trap stop_all EXIT INT TERM

echo "[$(date -Is)] starting run" >"$SERVER_LOG"
echo "[$(date -Is)] starting run" >"$CLIENT_LOG"

echo "Preparing lobby..."
LOBBY_ID=$(./prepare_server.py --guild-id 1216923249615835156 --spawned-by 695081603180789771 --region spr-dev | jq -r '.id[:36] | ascii_upcase')
if [[ -z "$LOBBY_ID" || "$LOBBY_ID" == "null" ]]; then
	echo "failed to prepare lobby" | tee -a "$CLIENT_LOG"
	exit 1
fi
echo "Lobby: $LOBBY_ID" | tee -a "$CLIENT_LOG"

echo "Starting EchoVR client..."
pushd "$CLIENT_DIR" >/dev/null
if [[ "$CAPTURE_MODE" == "nslobby" ]]; then
	wine ./bin/win10/echovr.exe -noovr -windowed -mp -lobbyid "$LOBBY_ID" 2>/dev/null |
		grep -v LOADING_TIPS |
		sed -n "/${CLIENT_START_REGEX}/{s/.*${CLIENT_START_REGEX}//; p; :a; n; p; ba}" \
			>>"$ROOT_DIR/$CLIENT_LOG" &
else
	wine ./bin/win10/echovr.exe -noovr -windowed -mp -lobbyid "$LOBBY_ID" >>"$ROOT_DIR/$CLIENT_LOG" 2>&1 &
fi
CLIENT_PID=$!
popd >/dev/null

echo "server -> $SERVER_LOG"
echo "client -> $CLIENT_LOG"
echo "client pid: $CLIENT_PID"

if [[ "$NON_INTERACTIVE" == "1" ]]; then
	echo "Running non-interactive for ${RUNTIME_SECONDS}s..."
	sleep "$RUNTIME_SECONDS"
else
	echo "Press enter to stop server/client"
	read
fi

echo "Stopping..."
