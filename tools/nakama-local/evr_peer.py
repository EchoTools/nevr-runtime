#!/usr/bin/env python3
"""A second (third, fourth) player for local scenarios: a minimal EVR websocket client.

It logs a seeded peer account (`seed.py --peers`) in to the local nakama exactly as the runtime's
bridge does (ws URL with the server key and the account's discordid/password, then the same
SNSLogInRequestv2 frame: previous session UUID(16), platform(8), account(8), profile JSON), and speaks
the SNS party messages with the bridge's layouts (src/runtime/compat/social_party.h). Every frame it
sends or receives is logged by name, so a scenario's run folder shows what the peer did and saw.

Used by tools/scenario/run_scenario.py's `peer` step; runnable by hand:
    tools/nakama-local/evr_peer.py --peer 1 create_party set_policy:0
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import struct
import sys
import threading
import time
import uuid

import websocket  # websocket-client

HERE = pathlib.Path(__file__).resolve().parent
MARKER = bytes([0xF6, 0x40, 0xBB, 0x78, 0xA2, 0xE7, 0x8C, 0xBB])
PLATFORM_OVR_ORG = 4  # the bridge's kBridgeLoginPlatform (game numbering)

# Requests (src/runtime/compat/social_party.h kXxxRequest).
LOGIN_REQUEST = 0xBDB41EA9E67B200A
CREATE_REQUEST = 0x0B7BD21332523994
JOIN_REQUEST = 0xB57B22CC5352E00C
LEAVE_REQUEST = 0xB77B0BE7A94A9FB6
INVITE_REQUEST = 0xCF13F934540B5F5E
LOCK_REQUEST = 0xC2478AA479F3E16A
UNLOCK_REQUEST = 0x5A4E99802FA3D704
SET_JOIN_POLICY_REQUEST = 0xE1D46B6FB78FD9E6
INVITE_RESPONSE = 0xE3654A09203555A3

NAMES = {  # replies the peer waits on (social_party.h ReplyTable, nakama core_hash_lookup.go)
    0xA5ACC1A90D0CCE47: "LogInSuccess", 0xA5B9D5A3021CCF51: "LogInFailure",
    0x0B7AC20124523993: "PartyCreateSuccess", 0x0B6FD60B2B423885: "PartyCreateFailure",
    0xB57A32DE4552E00B: "PartyJoinSuccess", 0xB56F26D44A42E11D: "PartyJoinFailure",
    0xCC38103E64879E53: "PartyJoinNotify", 0x05315ABEFC8F804B: "PartyLeaveNotify",
    0xB77A1BF5BF4A9FB1: "PartyLeaveSuccess",
    0xC2469AB66FF3E16D: "PartyLockSuccess", 0x5A4F899239A3D703: "PartyUnlockSuccess",
    0x218F721F09026DAB: "PartyInviteNotify",
    0xDEE671B237A5278D: "PartyUpdateSuccess", 0xDEF365B838B5269B: "PartyUpdateFailure",
    0x23C834CB3BC6ECF5: "PartyUpdateNotify",
}
for sym, name in list(NAMES.items()):
    NAMES[sym] = name
REQUEST_NAMES = {LOGIN_REQUEST: "LogInRequest", CREATE_REQUEST: "PartyCreateRequest", JOIN_REQUEST: "PartyJoinRequest",
                 LEAVE_REQUEST: "PartyLeaveRequest", INVITE_REQUEST: "PartyInviteRequest", LOCK_REQUEST: "PartyLockRequest",
                 UNLOCK_REQUEST: "PartyUnlockRequest", SET_JOIN_POLICY_REQUEST: "PartySetJoinPolicyRequest",
                 INVITE_RESPONSE: "PartyInviteResponse"}


def member_uuid(account_id: int) -> bytes:
    """UUIDv5 (nil namespace) of "OVR-ORG-<id>", as the bridge's MemberUuid."""
    return uuid.uuid5(uuid.UUID(int=0), f"OVR-ORG-{account_id}").bytes


def frame(symbol: int, payload: bytes) -> bytes:
    return MARKER + struct.pack("<QQ", symbol, len(payload)) + payload


def split_frames(data: bytes):
    while len(data) >= 24 and data[:8] == MARKER:
        sym, length = struct.unpack_from("<QQ", data, 8)
        yield sym, data[24:24 + length]
        data = data[24 + length:]


class Peer:
    def __init__(self, name: str, account_id: int, password: str, server_key: str, log=print):
        self.name, self.account_id, self.password, self.server_key = name, account_id, password, server_key
        self.log = log
        self.ws: websocket.WebSocket | None = None
        self.received: list[tuple[float, str, bytes]] = []
        self.lock = threading.Condition()
        self.party_id = 0
        self.reader: threading.Thread | None = None

    # ---- transport
    def connect(self, timeout: float = 30) -> None:
        url = (f"ws://127.0.0.1:7350/ws?format=evr&token={self.server_key}"
               f"&discordid={self.account_id}&password={self.password}")
        self.ws = websocket.create_connection(url, timeout=10)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        profile = {"accountid": self.account_id, "displayname": self.name, "bypassauth": False, "access_token": "",
                   "password": self.password, "nonce": "", "buildversion": 631547, "lobbyversion": 0, "appid": 0,
                   "publisher_lock": "", "hmdserialnumber": "nEVR-peer", "desiredclientprofileversion": 0,
                   "nevr_social": 1, "system_info": {"headset_type": "No VR"}}
        payload = bytes(16) + struct.pack("<QQ", PLATFORM_OVR_ORG, self.account_id) + json.dumps(profile).encode() + b"\0"
        self._send(LOGIN_REQUEST, payload)
        self.wait_for("LogInSuccess", timeout)

    def close(self) -> None:
        if self.ws is not None:
            try:
                self.ws.close()
            finally:
                self.ws = None
                self.log(f"peer {self.name}: disconnected")

    def _send(self, symbol: int, payload: bytes) -> None:
        self.log(f"peer {self.name}: -> {REQUEST_NAMES.get(symbol, hex(symbol))} payload_bytes={len(payload)}")
        self.ws.send_binary(frame(symbol, payload))

    def _read(self) -> None:
        while self.ws is not None:
            try:
                data = self.ws.recv()
            except Exception as exc:  # noqa: BLE001  the socket closed or timed out; the peer is done
                if self.ws is not None and not isinstance(exc, websocket.WebSocketTimeoutException):
                    self.log(f"peer {self.name}: read ended: {type(exc).__name__}")
                    return
                continue
            if not isinstance(data, bytes):
                continue
            for sym, payload in split_frames(data):
                name = NAMES.get(sym, f"0x{sym:016x}")
                if sym in NAMES:
                    self.log(f"peer {self.name}: <- {name} payload_bytes={len(payload)}")
                with self.lock:
                    self.received.append((time.monotonic(), name, payload))
                    self.lock.notify_all()

    def wait_for(self, name: str, timeout: float = 15, since: float = 0.0) -> bytes:
        deadline = time.monotonic() + timeout
        with self.lock:
            while True:
                for t, n, payload in self.received:
                    if n == name and t >= since:
                        return payload
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(f"peer {self.name}: no {name} within {timeout} s")
                self.lock.wait(remaining)

    # ---- SNS party (layouts from social_party.h: Standard 0x28, Targeted 0x30)
    def standard(self, symbol: int, last: int) -> None:
        self._send(symbol, struct.pack("<Q", 0) + member_uuid(self.account_id) + struct.pack("<QQ", 0, last))

    def create_party(self) -> int:
        start = time.monotonic()
        self.standard(CREATE_REQUEST, 0)
        payload = self.wait_for("PartyCreateSuccess", since=start)
        self.party_id = struct.unpack_from("<Q", payload, 0)[0]
        self.log(f"peer {self.name}: party {self.party_id}")
        return self.party_id

    def join(self, party_id: int) -> str:
        start = time.monotonic()
        self.standard(JOIN_REQUEST, party_id)
        return self._outcome(start, "PartyJoinSuccess", "PartyJoinFailure")

    def set_policy(self, policy: int) -> None:
        start = time.monotonic()
        self.standard(SET_JOIN_POLICY_REQUEST, policy)
        self.wait_for("PartyUpdateSuccess", since=start)

    def lock_party(self) -> None:
        start = time.monotonic()
        self.standard(LOCK_REQUEST, 0)
        self.wait_for("PartyLockSuccess", since=start)

    def invite(self, account_id: int) -> None:
        self.standard(INVITE_REQUEST, account_id)

    def _outcome(self, start: float, ok: str, fail: str, timeout: float = 15) -> str:
        deadline = time.monotonic() + timeout
        with self.lock:
            while True:
                for t, n, payload in self.received:
                    if t >= start and n == ok:
                        return ok
                    if t >= start and n == fail:
                        return f"{fail} code={payload[8] if len(payload) > 8 else payload[0]}"
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return "no answer"
                self.lock.wait(remaining)


def peer_account(index: int) -> tuple[str, int, str]:
    sys.path.insert(0, str(HERE))
    import seed  # noqa: E402
    return seed.peer(index)


def server_key() -> str:
    state = (HERE / ".state/nakama.yml").read_text()
    m = re.search(r"server_key:\s*(\S+)", state)
    if not m:
        raise SystemExit("no server_key in tools/nakama-local/.state/nakama.yml")
    return m.group(1)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--peer", type=int, default=1)
    ap.add_argument("actions", nargs="*", help="create_party, set_policy:N, lock, invite:ID, join:PARTY")
    args = ap.parse_args(argv)
    name, account, password = peer_account(args.peer)
    p = Peer(name, account, password, server_key())
    p.connect()
    for action in args.actions:
        op, _, arg = action.partition(":")
        result = getattr(p, {"lock": "lock_party"}.get(op, op))(*([int(arg)] if arg else []))
        print(f"{action}: {result}")
    p.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
