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


def seed() -> None:
    sql = f"""
CREATE EXTENSION IF NOT EXISTS pgcrypto;
INSERT INTO users (id, username, custom_id, password)
VALUES (gen_random_uuid(), '{USERNAME}', '{DISCORD_ID}', convert_to(crypt('{PASSWORD}', gen_salt('bf', 6)), 'UTF8'))
ON CONFLICT (username) DO UPDATE
  SET custom_id = EXCLUDED.custom_id, password = EXCLUDED.password, disable_time = '1970-01-01 00:00:00+00'
RETURNING id, username, custom_id;
"""
    p = subprocess.run(["docker", "compose", "-f", str(COMPOSE), "exec", "-T", "postgres",
                        "psql", "-U", "postgres", "-d", "nakama", "-v", "ON_ERROR_STOP=1", "-tA", "-c", sql],
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit(f"seed failed: {p.stderr.strip() or p.stdout.strip()}")
    print(f"seeded: {p.stdout.strip().splitlines()[-1]}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--print", action="store_true", help="print the runtime config.yaml identity block")
    args = ap.parse_args()
    if args.print:
        print(f'identity:\n  discord_id: "{DISCORD_ID}"\n  password: "{PASSWORD}"')
        return 0
    seed()
    return 0


if __name__ == "__main__":
    sys.exit(main())
