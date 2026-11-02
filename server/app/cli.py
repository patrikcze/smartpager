"""Administration CLI.

  python -m app.cli token                      # print a random secret for .env
  python -m app.cli device-create pager-1      # register a device, prints its token once
  python -m app.cli device-rotate pager-1      # issue a new device token
  python -m app.cli devices                    # list devices
  python -m app.cli purge                      # run retention purge now

Device commands operate directly on SMARTPAGER_DB (no running server needed).
"""
from __future__ import annotations

import argparse
import json
import secrets
import sqlite3
import sys

from .config import Settings
from .database.db import connect, migrate
from .services import messages as svc


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="smartpager-admin")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("token")
    c = sub.add_parser("device-create"); c.add_argument("id"); c.add_argument("--name", default="SmartPager")
    r = sub.add_parser("device-rotate"); r.add_argument("id")
    sub.add_parser("devices")
    sub.add_parser("purge")
    args = p.parse_args(argv)

    if args.cmd == "token":
        print(secrets.token_urlsafe(32))
        return 0

    s = Settings.from_env()
    migrate(s.db_path)
    conn = connect(s.db_path)
    try:
        if args.cmd == "device-create":
            try:
                _, token = svc.create_device(conn, args.id, args.name)
            except sqlite3.IntegrityError:
                print(f"device {args.id!r} already exists (use device-rotate)", file=sys.stderr)
                return 1
            print(f"device: {args.id}\ntoken:  {token}\n(store it now; only its hash is kept)")
        elif args.cmd == "device-rotate":
            token = svc.rotate_device_token(conn, args.id)
            if token is None:
                print("device not found", file=sys.stderr)
                return 1
            print(f"token:  {token}")
        elif args.cmd == "devices":
            for row in svc.list_devices(conn):
                print(json.dumps(svc.device_to_dict(row)))
        elif args.cmd == "purge":
            print(svc.purge(conn, s.retention_days, s.tombstone_days))
    finally:
        conn.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
