#!/usr/bin/env python3
"""
Ping one or more EchoVR game servers given as command-line arguments.

Usage:
    python3 ping_servers.py <ip>:<port> [<ip>:<port> ...]

Example:
    python3 ping_servers.py 116.203.155.106:7794 70.57.1.74:4200
"""

import os
import socket
import struct
import sys
import time

NUM_PINGS_PER_SERVER = 5
TIMEOUT_MS = 800


def ping_gameserver(remote_ip: str, port: int, timeout: float):
    PING_REQUEST_SYMBOL = 0x997279DE065A03B0
    RAW_PING_ACKNOWLEDGE_MESSAGE_SYMBOL = 0x4F7AE556E0B77891

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", 0))
    sock.settimeout(timeout)

    token = os.urandom(8)
    request = struct.pack("<Q", PING_REQUEST_SYMBOL) + token

    start = time.time()
    try:
        sock.sendto(request, (remote_ip, port))
        response, _ = sock.recvfrom(16)
    except socket.timeout:
        return -1, f"timed out (>{int(timeout * 1000)}ms)"
    except Exception as e:
        return 0, str(e)
    finally:
        sock.close()

    rtt = time.time() - start

    if struct.unpack("<Q", response[:8])[0] != RAW_PING_ACKNOWLEDGE_MESSAGE_SYMBOL:
        return 0, f"unexpected response symbol"

    if response[8:] != token:
        return 0, f"token mismatch"

    return rtt, None


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <ip>:<port> [<ip>:<port> ...]", file=sys.stderr)
        sys.exit(1)

    servers = []
    for arg in sys.argv[1:]:
        parts = arg.rsplit(":", 1)
        if len(parts) != 2:
            print(f"Invalid server address (expected ip:port): {arg}", file=sys.stderr)
            sys.exit(1)
        ip, port_str = parts
        try:
            port = int(port_str)
        except ValueError:
            print(f"Invalid port in '{arg}'", file=sys.stderr)
            sys.exit(1)
        servers.append((ip, port))

    col_num = 4
    col_server = 25
    col_rtt = 7 * NUM_PINGS_PER_SERVER
    col_avg = 8

    header_num = "#".ljust(col_num)
    header_server = "Game Server".ljust(col_server)
    header_rtts = "RTT (milliseconds)".ljust(col_rtt)
    header_avg = "Average".ljust(col_avg)

    print(f"Pinging {len(servers)} game server(s)\n")
    print(header_num + header_server + header_rtts + header_avg)
    for h in [header_num, header_server, header_rtts, header_avg]:
        print(("=" * (len(h) - 1)).ljust(len(h)), end="")
    print()

    for n, (ip, port) in enumerate(servers):
        rtts = []
        print(f"{str(n + 1).rjust(3)}".ljust(col_num), end="", flush=True)
        print(f"{ip}:{port}".ljust(col_server), end="", flush=True)

        for i in range(NUM_PINGS_PER_SERVER):
            rtt, error = ping_gameserver(ip, port, TIMEOUT_MS / 1000)
            if error:
                print("X".ljust(col_rtt // NUM_PINGS_PER_SERVER), end="", flush=True)
            else:
                rtt_ms = int(rtt * 1000)
                rtts.append(rtt_ms)
                print(
                    f"{rtt_ms}ms".ljust(col_rtt // NUM_PINGS_PER_SERVER),
                    end="",
                    flush=True,
                )

        if rtts:
            avg = int(sum(rtts) / len(rtts))
            print(f"{avg}ms")
        else:
            print("X")


if __name__ == "__main__":
    try:
        main()
        print()
    except KeyboardInterrupt:
        print()
        sys.exit(1)
