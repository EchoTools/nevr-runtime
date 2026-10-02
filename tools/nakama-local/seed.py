#!/usr/bin/env python3
"""Seed the local nakama with a test account a runtime can log in as (idempotent).

The server logs a runtime in from `?discordid=<id>&password=<pw>`: it maps the ID to a
user through `users.custom_id`, then checks username + password with bcrypt
(server/session_ws.go, GetUserIDByDiscordID, AuthenticateUsername). This inserts exactly
that row into the local, throwaway Postgres. SQL rather than the HTTP API because the
fork disables the email and device authenticate resources ("Intercepted a disabled
resource"). pgcrypto's bcrypt output is what Go's CompareHashAndPassword expects.

    tools/nakama-local/seed.py            # seed (or refresh) the account
    tools/nakama-local/seed.py --print    # print the identity block for a runtime config.yaml
    tools/nakama-local/seed.py --discord-id ID   # the account carries this Discord id instead

A client with cached credentials logs in with the Discord id from its token (N20: token first,
config second), and the server names party members by the account's Discord id (custom_id,
sessionAccountID). For the two to agree the local account must carry the client's id, which the
scenario runner passes from the client's own log ("login injected xpid=OVR-ORG-<id>").
"""

from __future__ import annotations

import argparse
import subprocess
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
COMPOSE = HERE / "docker-compose.yml"

DISCORD_ID = "900000000000000001"   # fake; 18 digits like a real snowflake
USERNAME = "localtest"
PASSWORD = "localtestpassword"      # 32 chars max: the query parser truncates at 32
GUILD_ID = "900000000000000002"     # fake Discord guild the account belongs to
PEER_BASE = 900000000000000100      # peer N carries Discord id PEER_BASE + N (evr_peer.py)


def peer(index: int) -> tuple[str, int, str]:
    """The seeded peer account N: (username, Discord id, password)."""
    return f"localpeer{index}", PEER_BASE + index, PASSWORD
GUILD_NAME = "nevr-local-guild"


def psql(sql: str) -> str:
    p = subprocess.run(["docker", "compose", "-f", str(COMPOSE), "exec", "-T", "postgres",
                        "psql", "-U", "postgres", "-d", "nakama", "-v", "ON_ERROR_STOP=1", "-tA", "-c", sql],
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit(f"seed failed: {p.stderr.strip() or p.stdout.strip()}")
    return p.stdout.strip()


def account_sql(username: str, discord_id: str) -> str:
    """An account (username, Discord id in custom_id, the shared test password) in the local guild."""
    return f"""
INSERT INTO users (id, username, custom_id, password)
VALUES (gen_random_uuid(), '{username}', '{discord_id}', convert_to(crypt('{PASSWORD}', gen_salt('bf', 6)), 'UTF8'))
ON CONFLICT (username) DO UPDATE
  SET custom_id = EXCLUDED.custom_id, password = EXCLUDED.password, disable_time = '1970-01-01 00:00:00+00';
INSERT INTO group_edge (position, state, source_id, destination_id)
SELECT 1, 2, g.id, u.id FROM groups g, users u WHERE g.name = '{GUILD_NAME}' AND u.username = '{username}'
ON CONFLICT DO NOTHING;
INSERT INTO group_edge (position, state, source_id, destination_id)
SELECT 1, 2, u.id, g.id FROM groups g, users u WHERE g.name = '{GUILD_NAME}' AND u.username = '{username}'
ON CONFLICT DO NOTHING;
"""


def friends_sql(a: str, b: str, state: int = 0) -> str:
    """user_edge both ways between the accounts with these Discord ids (state 0: mutual friends). The
    primary key is (source_id, state, position), so each edge gets its own position (microseconds)."""
    return f"""
INSERT INTO user_edge (source_id, position, update_time, destination_id, state)
SELECT x.id, (extract(epoch from clock_timestamp()) * 1000000)::bigint, now(), y.id, {state} FROM users x, users y WHERE x.custom_id = '{a}' AND y.custom_id = '{b}'
ON CONFLICT (source_id, destination_id) DO UPDATE SET state = {state};
INSERT INTO user_edge (source_id, position, update_time, destination_id, state)
SELECT y.id, (extract(epoch from clock_timestamp()) * 1000000)::bigint, now(), x.id, {state} FROM users x, users y WHERE x.custom_id = '{a}' AND y.custom_id = '{b}'
ON CONFLICT (source_id, destination_id) DO UPDATE SET state = {state};
"""


def met_sql(owner: str, met: str) -> str:
    """Puts the account with Discord id `met` at the front of `owner`'s recently-met list, the storage
    object nakama writes when a player leaves a match (server/evr_recently_met.go RecentlyMet/list:
    read by its owner, written by the server only)."""
    return f"""
INSERT INTO storage (collection, key, user_id, value, version, read, write)
SELECT 'RecentlyMet', 'list', a.id,
       jsonb_build_object('users', jsonb_build_array(jsonb_build_object(
         'user_id', b.id::text, 'account_id', b.custom_id::numeric, 'display_name', b.username,
         'last_met', to_char(now() at time zone 'utc', 'YYYY-MM-DD"T"HH24:MI:SS"Z"')))),
       md5(random()::text), 1, 0
FROM users a, users b WHERE a.custom_id = '{owner}' AND b.custom_id = '{met}'
ON CONFLICT (collection, key, user_id) DO UPDATE
  SET value = jsonb_set(storage.value, '{{users}}', (EXCLUDED.value->'users') || (storage.value->'users')),
      version = EXCLUDED.version, update_time = now();
"""


def seed(discord_id: str = DISCORD_ID) -> None:
    if not discord_id.isdigit():
        raise SystemExit(f"--discord-id must be digits, not {discord_id!r}")
    sql = f"""
CREATE EXTENSION IF NOT EXISTS pgcrypto;
INSERT INTO users (id, username, custom_id, password)
VALUES (gen_random_uuid(), '{USERNAME}', '{discord_id}', convert_to(crypt('{PASSWORD}', gen_salt('bf', 6)), 'UTF8'))
ON CONFLICT (username) DO UPDATE
  SET custom_id = EXCLUDED.custom_id, password = EXCLUDED.password, disable_time = '1970-01-01 00:00:00+00'
RETURNING id, username, custom_id;

-- Login needs the user in a guild group (lang_tag 'guild', server/evr_guild_group.go
-- GuildUserGroupsList); state 0 is superadmin, and group_edge holds both directions.
INSERT INTO groups (id, creator_id, name, description, lang_tag, metadata, state, edge_count, max_count)
SELECT gen_random_uuid(), u.id, '{GUILD_NAME}', 'local test guild', 'guild',
       jsonb_build_object('guild_id', '{GUILD_ID}', 'owner_id', u.id::text), 1, 1, 100
FROM users u WHERE u.username = '{USERNAME}'
ON CONFLICT (name) DO NOTHING;
INSERT INTO group_edge (position, state, source_id, destination_id)
SELECT 1, 0, g.id, u.id FROM groups g, users u WHERE g.name = '{GUILD_NAME}' AND u.username = '{USERNAME}'
ON CONFLICT DO NOTHING;
INSERT INTO group_edge (position, state, source_id, destination_id)
SELECT 1, 0, u.id, g.id FROM groups g, users u WHERE g.name = '{GUILD_NAME}' AND u.username = '{USERNAME}'
ON CONFLICT DO NOTHING;
"""
    p = subprocess.run(["docker", "compose", "-f", str(COMPOSE), "exec", "-T", "postgres",
                        "psql", "-U", "postgres", "-d", "nakama", "-v", "ON_ERROR_STOP=1", "-tA", "-c", sql],
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit(f"seed failed: {p.stderr.strip() or p.stdout.strip()}")
    print(f"seeded: {p.stdout.strip().splitlines()[0]} (+ guild group {GUILD_NAME})")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--print", action="store_true", help="print the runtime config.yaml identity block")
    ap.add_argument("--discord-id", default=DISCORD_ID, help="the Discord id the account carries")
    ap.add_argument("--peers", type=int, default=0, help="also seed peer accounts 1..N (evr_peer.py)")
    ap.add_argument("--friends", action="append", default=[], metavar="A,B",
                    help="make the accounts with Discord ids A and B mutual friends (repeatable)")
    ap.add_argument("--unfriend", action="append", default=[], metavar="A,B", help="remove that friendship")
    ap.add_argument("--met", action="append", default=[], metavar="A,B",
                    help="put the account with Discord id B on A's recently-met list (repeatable, newest last)")
    ap.add_argument("--reset-met", action="store_true",
                    help="first empty the recently-met lists of the account and peers 1..N")
    ap.add_argument("--reset-friends", action="store_true",
                    help="first remove every friendship among the account and peers 1..N")
    args = ap.parse_args()
    if args.print:
        print(f'identity:\n  discord_id: "{DISCORD_ID}"\n  password: "{PASSWORD}"')
        return 0
    seed(args.discord_id)
    for n in range(1, args.peers + 1):
        name, account, _ = peer(n)
        psql("CREATE EXTENSION IF NOT EXISTS pgcrypto;" + account_sql(name, str(account)))
        print(f"seeded peer {n}: {name} discord id {account}")
    if args.reset_friends:
        ids = [args.discord_id] + [str(peer(n)[1]) for n in range(1, max(args.peers, 4) + 1)]
        listed = ",".join(f"'{i}'" for i in ids)
        psql(f"DELETE FROM user_edge WHERE source_id IN (SELECT id FROM users WHERE custom_id IN ({listed})) "
             f"AND destination_id IN (SELECT id FROM users WHERE custom_id IN ({listed}));")
        print("friendships among the test accounts removed")
    if args.reset_met:
        ids = [args.discord_id] + [str(peer(n)[1]) for n in range(1, max(args.peers, 4) + 1)]
        listed = ",".join(f"'{i}'" for i in ids)
        psql(f"DELETE FROM storage WHERE collection = 'RecentlyMet' "
             f"AND user_id IN (SELECT id FROM users WHERE custom_id IN ({listed}));")
        print("recently-met lists of the test accounts emptied")
    for pair in args.met:
        a, b = pair.split(",")
        psql(met_sql(a.strip(), b.strip()))
        print(f"recently met: {b.strip()} on {a.strip()}'s list")
    for pair in args.friends:
        a, b = pair.split(",")
        psql(friends_sql(a.strip(), b.strip()))
        print(f"friends: {a.strip()} <-> {b.strip()}")
    for pair in args.unfriend:
        a, b = pair.split(",")
        psql(f"DELETE FROM user_edge WHERE source_id IN (SELECT id FROM users WHERE custom_id IN ('{a}','{b}')) "
             f"AND destination_id IN (SELECT id FROM users WHERE custom_id IN ('{a}','{b}'));")
        print(f"not friends: {a} <-> {b}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
