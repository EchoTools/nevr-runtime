"""Throwaway WebSocket echo server for the Quest TLS host test (just test-quest-tls).

TLS mode:   tls_ws_server.py tls <cert.pem> <key.pem> <port-file>
Plain mode: tls_ws_server.py plain <stats-file> <port-file>

Binds 127.0.0.1 on an ephemeral port and writes the port to <port-file>. TLS mode completes the WebSocket
upgrade and echoes every data frame back unmasked. Plain mode never speaks WebSocket: it counts accepted
connections and how many of them began with an HTTP "GET " (a plaintext upgrade attempt) and rewrites
"<connections> <get_requests>" into <stats-file> on every accept and every read, so a test can prove the
client never fell back to plaintext.
"""
import base64
import hashlib
import socket
import ssl
import struct
import sys
import threading

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def recv_exact(conn, n):
    data = b""
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            raise EOFError
        data += chunk
    return data


def serve_ws(conn):
    try:
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = conn.recv(4096)
            if not chunk:
                return
            head += chunk
        key = None
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"sec-websocket-key:"):
                key = line.split(b":", 1)[1].strip()
        if key is None:
            conn.sendall(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
            return
        accept = base64.b64encode(hashlib.sha1(key + GUID).digest())
        conn.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     b"Sec-WebSocket-Accept: " + accept + b"\r\n\r\n")
        while True:
            b0, b1 = recv_exact(conn, 2)
            op = b0 & 0x0F
            masked = b1 & 0x80
            length = b1 & 0x7F
            if length == 126:
                length = struct.unpack(">H", recv_exact(conn, 2))[0]
            elif length == 127:
                length = struct.unpack(">Q", recv_exact(conn, 8))[0]
            mask = recv_exact(conn, 4) if masked else b"\0\0\0\0"
            payload = bytearray(recv_exact(conn, length))
            for i in range(len(payload)):
                payload[i] ^= mask[i % 4]
            if op == 0x8:
                conn.sendall(bytes([0x88, len(payload)]) + bytes(payload))
                return
            if op in (0x1, 0x2):
                out = bytes([0x80 | op])
                if len(payload) < 126:
                    out += bytes([len(payload)])
                elif len(payload) < 65536:
                    out += bytes([126]) + struct.pack(">H", len(payload))
                else:
                    out += bytes([127]) + struct.pack(">Q", len(payload))
                conn.sendall(out + bytes(payload))
    except (EOFError, OSError, ssl.SSLError):
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    mode = sys.argv[1]
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(16)
    port = listener.getsockname()[1]
    if mode == "tls":
        cert, key, port_file = sys.argv[2:5]
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert, key)
    else:
        stats_file, port_file = sys.argv[2:4]
        ctx = None
    with open(port_file, "w") as f:
        f.write(str(port))
    stats = {"connections": 0, "gets": 0}
    lock = threading.Lock()

    def write_stats():
        with open(stats_file, "w") as f:
            f.write("%d %d\n" % (stats["connections"], stats["gets"]))

    def plain_conn(conn):
        try:
            conn.settimeout(2)
            data = conn.recv(16)
            with lock:
                if data.startswith(b"GET "):
                    stats["gets"] += 1
                write_stats()
        except OSError:
            pass
        finally:
            conn.close()

    while True:
        conn, _ = listener.accept()
        if mode == "tls":
            try:
                conn = ctx.wrap_socket(conn, server_side=True)
            except (ssl.SSLError, OSError):
                continue  # a client that rejects our certificate aborts the handshake: expected
            threading.Thread(target=serve_ws, args=(conn,), daemon=True).start()
        else:
            with lock:
                stats["connections"] += 1
                write_stats()
            threading.Thread(target=plain_conn, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    main()
