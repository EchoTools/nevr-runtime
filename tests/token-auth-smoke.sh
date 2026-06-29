#!/usr/bin/env bash
# Token-auth BAC smoke test — verifies the ServerDB token-auth migration end-to-end
# against the live backend. Reads credentials from a config.json (no secrets in repo).
#
# BACs covered (docs/token-auth-migration.md):
#   BAC-1  password RPC issues an operator access JWT
#   BAC-2/3 /nevr accepts a valid JWT (101) and rejects an invalid one (401)
#   BAC-4  regression: /spr WS + representative HTTP routes still serve
#
# Usage: tests/token-auth-smoke.sh [config.json]   (default: echovr/_local/config.json)
set -u
CONFIG="${1:-echovr/_local/config.json}"
WSKEY="dGhlIHNhbXBsZSBub25jZQ=="   # static RFC6455 example Sec-WebSocket-Key (probe only)
fail=0
get() { python3 -c "import json,sys;print(json.load(open('$CONFIG')).get('$1',''))"; }

HTTP_URI=$(get nevr_http_uri); HTTP_KEY=$(get nevr_http_key)
DID=$(get nevr_discord_id);    PW=$(get nevr_password)
SOCKET_URI=$(get nevr_socket_uri); SERVERDB_URI=$(get nevr_serverdb_uri)
# Derive the http(s) base host:port and the ws base for probes.
WS_BASE="${SERVERDB_URI%/nevr}"; WS_BASE="${WS_BASE%/}"   # e.g. ws://g.echovrce.com:80
SPR_BASE="${SOCKET_URI%/spr}";   SPR_BASE="${SPR_BASE%/}"

ws_code() { # url -> prints HTTP status of a WS upgrade attempt; extra args = curl headers
  local url="$1"; shift
  curl -sk -m 8 -o /dev/null -w '%{http_code}' \
    -H "Connection: Upgrade" -H "Upgrade: websocket" \
    -H "Sec-WebSocket-Version: 13" -H "Sec-WebSocket-Key: $WSKEY" "$@" "$url"
}
http_code() { curl -sk -m 8 -o /dev/null -w '%{http_code}' "$1"; }
check() { # label expected actual
  if [ "$2" = "$3" ]; then echo "  PASS  $1 ($3)"; else echo "  FAIL  $1 (expected $2, got $3)"; fail=1; fi
}

echo "== BAC-1: password RPC issues an operator access JWT =="
RESP=$(curl -sS -m 12 -X POST "$HTTP_URI/v2/rpc/account/authenticate/password?http_key=$HTTP_KEY&unwrap" \
  -H 'Content-Type: application/json' -d "{\"discord_id\":\"$DID\",\"password\":\"$PW\"}")
JWT=$(printf '%s' "$RESP" | python3 -c "import sys,json
try: print(json.load(sys.stdin).get('token',''))
except Exception: print('')")
if [ -n "$JWT" ]; then
  UID_OK=$(printf '%s' "$JWT" | python3 -c "import sys,base64,json
t=sys.stdin.read().strip().split('.')
p=t[1]+'='*(-len(t[1])%4)
c=json.loads(base64.urlsafe_b64decode(p))
print('yes' if c.get('uid') and not (c.get('vrs') or {}).get('refresh') else 'no')")
  check "access JWT acquired with operator uid (no refresh flag)" "yes" "$UID_OK"
else
  check "access JWT acquired" "token" "(empty)"
fi

echo "== BAC-2/3: /nevr authenticates the JWT, rejects garbage =="
[ -n "$JWT" ] && check "/nevr + valid JWT -> 101" "101" "$(ws_code "$WS_BASE/nevr" -H "Authorization: Bearer $JWT")"
check "/nevr + garbage -> 401"        "401" "$(ws_code "$WS_BASE/nevr" -H "Authorization: Bearer garbage.invalid.jwt")"
check "/nevr + no auth  -> 401"       "401" "$(ws_code "$WS_BASE/nevr")"

echo "== BAC-4: regression — existing routes unchanged =="
check "/spr WS catch-all -> 101"      "101" "$(ws_code "$SPR_BASE/spr")"
check "GET /status/services,news ->200" "200" "$(http_code "https://g.echovrce.com/status/services,news")"
check "GET echovrce.com SPA -> 200"   "200" "$(http_code "https://echovrce.com/")"

echo ""
[ "$fail" = 0 ] && { echo "ALL BACs PASS"; exit 0; } || { echo "FAILURES ABOVE"; exit 1; }
