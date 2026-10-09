#!/usr/bin/env python3
"""Send one command to a running scenario build's control endpoint, by hand.

For in-game tests where a person plays and watches while the harness forges what the game service
would send (docs/design/2026-10-01-social-features-test-plan.md, "In-game: party join failure codes").
The game (a mingw-scenario build only) logs its port at start:
    [NEVR.SCENARIO] control listening on 127.0.0.1:<port> (test build only)
Pass that port, or a game log to read it from. Every command and reply is printed and appended, with a
UTC timestamp, to /var/tmp/work-nevr-runtime/control-log.jsonl.

    tools/scenario/control.py --port 38015 '{"op":"inject","msg":"PartyJoinFailure","party":0,"code":5}'
    tools/scenario/control.py --log path/to/console.log '{"op":"fire","action":"party_join_failed_callback","code":2}'
    tools/scenario/control.py --port 38015 '{"op":"state"}'
"""

from __future__ import annotations

import argparse
import datetime
import json
import pathlib
import re
import socket
import sys

LOG = pathlib.Path("/var/tmp/work-nevr-runtime/control-log.jsonl")
PORT_LINE = re.compile(r"\[NEVR\.SCENARIO\] control listening on 127\.0\.0\.1:(\d+)")


def port_from_log(path: pathlib.Path) -> int:
    ports = PORT_LINE.findall(path.read_text(errors="replace"))
    if not ports:
        raise SystemExit(f"no control-endpoint line in {path}: is this a mingw-scenario build's log?")
    return int(ports[-1])


def call(port: int, command: dict) -> dict:
    with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
        sock.sendall((json.dumps(command) + "\n").encode())
        buffer = b""
        while b"\n" not in buffer:
            chunk = sock.recv(65536)
            if not chunk:
                raise SystemExit("the control endpoint closed the connection without a reply")
            buffer += chunk
    return json.loads(buffer.split(b"\n", 1)[0])


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    where = ap.add_mutually_exclusive_group(required=True)
    where.add_argument("--port", type=int, help="the control endpoint's port")
    where.add_argument("--log", type=pathlib.Path, help="a game log to read the port from (the last one logged)")
    ap.add_argument("command", help="one JSON command, as the scenario runner sends")
    args = ap.parse_args(argv)
    command = json.loads(args.command)
    port = args.port if args.port is not None else port_from_log(args.log)
    reply = call(port, command)
    record = {"ts": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="milliseconds"),
              "port": port, "command": command, "reply": reply}
    LOG.parent.mkdir(parents=True, exist_ok=True)
    with LOG.open("a") as f:
        f.write(json.dumps(record) + "\n")
    print(json.dumps(reply, indent=2))
    return 0 if reply.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
