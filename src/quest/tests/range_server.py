"""Throwaway HTTP server for the data fetch host test (just test-quest-data-net).

  range_server.py <port-file> <request-log> <zip-file> [<cert.pem> <key.pem> [tls11]]

Binds 127.0.0.1 on an ephemeral port, writes it to <port-file>, and appends one line per request to <request-log>.
With a certificate and key it serves HTTPS ("tls11": it offers nothing newer than TLS 1.1, for the floor test).
Each line is:
"<path> range=<Range header or ->". Paths:
  /blob        100000 deterministic bytes; honours "Range: bytes=N-" with 206 and Content-Range, else 200
  /ignore      the same bytes, always 200 from byte 0 (a server that ignores Range)
  /drop        promises the whole blob, sends the first half, closes the connection
  /missing     404 with an HTML body
  /zip         the file <zip-file>, ranged like /blob
  /zip-flaky   like /zip, but the first request sends 5000 bytes and closes; later ones are served in full
  /redirect    302 to /blob (a client that follows it makes a second logged request)
  /stall       promises the whole blob, sends 100 bytes, then sends nothing for 20 s
"""
import http.server
import ssl
import sys
import threading
import time

PORT_FILE, REQUEST_LOG, ZIP_FILE = sys.argv[1], sys.argv[2], sys.argv[3]
CERT = sys.argv[4] if len(sys.argv) > 4 else None
KEY = sys.argv[5] if len(sys.argv) > 5 else None
TLS11 = len(sys.argv) > 6 and sys.argv[6] == "tls11"
BLOB = bytes((i * 7 + i // 251) % 256 for i in range(100000))
LOCK = threading.Lock()
FLAKY_SERVED = [False]


def log(path, rng):
    with LOCK:
        with open(REQUEST_LOG, "a") as f:
            f.write("%s range=%s\n" % (path, rng or "-"))


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def send_ranged(self, data, cut=None):
        rng = self.headers.get("Range")
        log(self.path, rng)
        start = 0
        status = 200
        if rng and rng.startswith("bytes=") and rng.endswith("-"):
            start = int(rng[6:-1])
            if start >= len(data):
                self.send_response(416)
                self.send_header("Content-Range", "bytes */%d" % len(data))
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            status = 206
        body = data[start:]
        self.send_response(status)
        if status == 206:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, len(data) - 1, len(data)))
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if cut is not None:
            self.wfile.write(body[:cut])
            self.wfile.flush()
            self.close_connection = True
            return
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/blob":
            self.send_ranged(BLOB)
        elif self.path == "/ignore":
            log(self.path, self.headers.get("Range"))
            self.send_response(200)
            self.send_header("Content-Length", str(len(BLOB)))
            self.end_headers()
            self.wfile.write(BLOB)
        elif self.path == "/drop":
            self.send_ranged(BLOB, cut=len(BLOB) // 2)
        elif self.path == "/redirect":
            log(self.path, self.headers.get("Range"))
            self.send_response(302)
            self.send_header("Location", "/blob")
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif self.path == "/stall":
            log(self.path, self.headers.get("Range"))
            self.send_response(206)
            self.send_header("Content-Range", "bytes 0-%d/%d" % (len(BLOB) - 1, len(BLOB)))
            self.send_header("Content-Length", str(len(BLOB)))
            self.end_headers()
            self.wfile.write(BLOB[:100])
            self.wfile.flush()
            time.sleep(20)
            self.close_connection = True
        elif self.path == "/missing":
            log(self.path, self.headers.get("Range"))
            body = b"<html>not found</html>"
            self.send_response(404)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path in ("/zip", "/zip-flaky"):
            data = open(ZIP_FILE, "rb").read()
            cut = None
            if self.path == "/zip-flaky":
                with LOCK:
                    if not FLAKY_SERVED[0]:
                        FLAKY_SERVED[0] = True
                        cut = 5000
            self.send_ranged(data, cut=cut)
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()


class Server(http.server.ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        pass  # a client that refuses the certificate or drops the stream is the point of several tests


server = Server(("127.0.0.1", 0), Handler)
if CERT:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    if TLS11:
        context.set_ciphers("ALL:@SECLEVEL=0")
        context.minimum_version = ssl.TLSVersion.TLSv1
        context.maximum_version = ssl.TLSVersion.TLSv1_1
    else:
        context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(CERT, KEY)
    server.socket = context.wrap_socket(server.socket, server_side=True)
with open(PORT_FILE, "w") as f:
    f.write(str(server.server_address[1]))
server.serve_forever()
