#!/usr/bin/env python3
"""A minimal fake Discord for running the EchoTools nakama fork locally.

The fork refuses to start without a live Discord gateway: it calls
discordgo's Open() and then waits 10 s for READY (server/evr_pipeline.go).
There is no offline switch, and pointing it at a real bot token is not
acceptable for a local test (it would register slash commands and sync
members under a real application).

So this answers just enough of Discord for the server to start and for the
login / server-registration paths to see "a Discord with no guilds":

  REST (https, port 443)   GET /api/v9/gateway, /gateway/bot, /users/@me,
                           /applications/@me answer; member/guild/user lookups
                           return Discord's own "Unknown ..." error codes, which
                           the server already handles; DMs return 50007
                           ("cannot send messages to this user").
  Gateway (ws, port 8080)  HELLO -> (IDENTIFY) -> READY, heartbeat ACKs.

Standard library only. Run inside the compose network with the network aliases
discord.com / gateway.discord.gg; nakama trusts the local CA through
SSL_CERT_FILE. Nothing here talks to the real Discord.
"""

from __future__ import annotations

import base64
import hashlib
import json
import re
import ssl
import struct
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BOT_ID = "100000000000000001"
APP_ID = "100000000000000002"
GATEWAY_URL = "ws://gateway.discord.gg:8080"
_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

BOT_USER = {"id": BOT_ID, "username": "local-mock-bot", "discriminator": "0", "bot": True,
            "verified": True, "avatar": None, "global_name": "local-mock-bot"}


def log(msg: str) -> None:
    print(f"[discord-mock] {msg}", flush=True)


# --- REST -----------------------------------------------------------------------

# (method, path regex) -> (status, body). First match wins; order matters.
_UNKNOWN = lambda code, text: (404, {"code": code, "message": text})  # noqa: E731
ROUTES = [
    ("GET", r"^/api/v\d+/gateway$", lambda: (200, {"url": GATEWAY_URL})),
    ("GET", r"^/api/v\d+/gateway/bot$", lambda: (200, {
        "url": GATEWAY_URL, "shards": 1,
        "session_start_limit": {"total": 1000, "remaining": 1000, "reset_after": 86400000, "max_concurrency": 1}})),
    ("GET", r"^/api/v\d+/users/@me$", lambda: (200, BOT_USER)),
    ("GET", r"^/api/v\d+/applications/@me$", lambda: (200, {"id": APP_ID, "name": "local-mock-bot", "flags": 0})),
    ("GET", r"^/api/v\d+/users/@me/guilds", lambda: (200, [])),
    ("GET", r"^/api/v\d+/guilds/\d+/members/\d+$", lambda: _UNKNOWN(10007, "Unknown Member")),
    ("GET", r"^/api/v\d+/guilds/\d+/members", lambda: (200, [])),
    ("GET", r"^/api/v\d+/guilds/\d+/roles", lambda: (200, [])),
    ("GET", r"^/api/v\d+/guilds/\d+/channels", lambda: (200, [])),
    ("GET", r"^/api/v\d+/guilds/\d+", lambda: _UNKNOWN(10004, "Unknown Guild")),
    ("GET", r"^/api/v\d+/users/\d+$", lambda: _UNKNOWN(10013, "Unknown User")),
    ("POST", r"^/api/v\d+/users/@me/channels$", lambda: (403, {"code": 50007, "message": "Cannot send messages to this user"})),
    ("POST", r"^/api/v\d+/channels/\d+/messages", lambda: (403, {"code": 50001, "message": "Missing Access"})),
    ("PUT", r"^/api/v\d+/applications/\d+/(guilds/\d+/)?commands$", lambda: (200, [])),
    ("GET", r"^/api/v\d+/applications/\d+/(guilds/\d+/)?commands", lambda: (200, [])),
]


class Rest(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # noqa: A002
        log("REST " + (fmt % args))

    def _serve(self, method: str) -> None:
        n = int(self.headers.get("Content-Length") or 0)
        if n:
            self.rfile.read(n)
        path = self.path.split("?")[0]
        status, body = 404, {"code": 0, "message": "404: Not Found"}
        for m, pattern, fn in ROUTES:
            if m == method and re.match(pattern, path):
                status, body = fn()
                break
        raw = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self): self._serve("GET")        # noqa: E704,N802
    def do_POST(self): self._serve("POST")      # noqa: E704,N802
    def do_PUT(self): self._serve("PUT")        # noqa: E704,N802
    def do_PATCH(self): self._serve("PATCH")    # noqa: E704,N802
    def do_DELETE(self): self._serve("DELETE")  # noqa: E704,N802


# --- Gateway (a bare RFC 6455 server; just enough for discordgo) -----------------

def _recv_exact(sock, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return buf


def _read_frame(sock) -> tuple[int, bytes]:
    b1, b2 = _recv_exact(sock, 2)
    opcode, masked, length = b1 & 0x0F, b2 & 0x80, b2 & 0x7F
    if length == 126:
        length = struct.unpack(">H", _recv_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", _recv_exact(sock, 8))[0]
    mask = _recv_exact(sock, 4) if masked else b""
    data = _recv_exact(sock, length)
    if masked:
        data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    return opcode, data


def _send_frame(sock, opcode: int, data: bytes) -> None:
    head = bytes([0x80 | opcode])
    if len(data) < 126:
        head += bytes([len(data)])
    elif len(data) < 65536:
        head += bytes([126]) + struct.pack(">H", len(data))
    else:
        head += bytes([127]) + struct.pack(">Q", len(data))
    sock.sendall(head + data)


def _send_json(sock, obj: dict) -> None:
    _send_frame(sock, 0x1, json.dumps(obj).encode())


def gateway_client(sock) -> None:
    try:
        request = b""
        while b"\r\n\r\n" not in request:
            request += sock.recv(4096)
        key = re.search(rb"Sec-WebSocket-Key: *(\S+)", request, re.I)
        if not key:
            return
        accept = base64.b64encode(hashlib.sha1(key.group(1) + _GUID.encode()).digest()).decode()
        sock.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                      f"Connection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n\r\n").encode())
        _send_json(sock, {"op": 10, "d": {"heartbeat_interval": 41250}})
        seq = 0
        while True:
            opcode, data = _read_frame(sock)
            if opcode == 0x8:
                return
            if opcode == 0x9:
                _send_frame(sock, 0xA, data)
                continue
            if opcode not in (0x1, 0x2):
                continue
            op = json.loads(data).get("op")
            if op == 1:                       # heartbeat
                _send_json(sock, {"op": 11})
            elif op in (2, 6):                # identify / resume
                seq += 1
                log("gateway: IDENTIFY -> READY")
                _send_json(sock, {"op": 0, "t": "READY", "s": seq, "d": {
                    "v": 9, "user": BOT_USER, "guilds": [], "session_id": "local-mock-session",
                    "resume_gateway_url": GATEWAY_URL, "application": {"id": APP_ID, "flags": 0},
                    "private_channels": [], "relationships": []}})
    except (ConnectionError, OSError, ValueError) as e:
        log(f"gateway client ended: {e}")
    finally:
        sock.close()


def serve_gateway(port: int) -> None:
    import socket
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(16)
    log(f"gateway listening on :{port}")
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=gateway_client, args=(conn,), daemon=True).start()


def main() -> int:
    cert, key = (sys.argv[1], sys.argv[2]) if len(sys.argv) >= 3 else ("/certs/server.pem", "/certs/server.key")
    threading.Thread(target=serve_gateway, args=(8080,), daemon=True).start()
    httpd = ThreadingHTTPServer(("0.0.0.0", 443), Rest)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    log("REST listening on :443 (TLS)")
    httpd.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
