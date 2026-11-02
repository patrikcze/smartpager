"""Administration CLI.

  python -m app.cli token                      # print a random secret for .env
  python -m app.cli device-create pager-1      # register a device, prints its token once
  python -m app.cli device-rotate pager-1      # issue a new device token
  python -m app.cli devices                    # list devices
  python -m app.cli device-delete pager-1      # remove a device and its token
  python -m app.cli purge                      # run retention purge now
  python -m app.cli ingest-create grafana      # named ingest token for one integration (printed once)
  python -m app.cli ingest-list                # list integration tokens
  python -m app.cli ingest-revoke grafana      # revoke an integration token
  python -m app.cli audit [--limit 50]         # recent admin/security events

Device commands operate directly on SMARTPAGER_DB (no running server needed).
"""
from __future__ import annotations

import argparse
import json
import secrets
import sqlite3
import sys

from .config import Settings
from .database.db import connect, migrate, write_tx
from .services import messages as svc
from .services import security


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="smartpager-admin")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("token")
    c = sub.add_parser("device-create"); c.add_argument("id"); c.add_argument("--name", default="SmartPager")
    r = sub.add_parser("device-rotate"); r.add_argument("id")
    sub.add_parser("devices")
    dd = sub.add_parser("device-delete"); dd.add_argument("id")
    sub.add_parser("purge")
    ic = sub.add_parser("ingest-create"); ic.add_argument("name")
    sub.add_parser("ingest-list")
    ir = sub.add_parser("ingest-revoke"); ir.add_argument("name")
    au = sub.add_parser("audit"); au.add_argument("--limit", type=int, default=50)
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
            security.audit(conn, "cli", "device.create", f"device:{args.id}")
            print(f"device: {args.id}\ntoken:  {token}\n(store it now; only its hash is kept)")
        elif args.cmd == "device-rotate":
            token = svc.rotate_device_token(conn, args.id)
            if token is None:
                print("device not found", file=sys.stderr)
                return 1
            security.audit(conn, "cli", "device.rotate_token", f"device:{args.id}")
            print(f"token:  {token}")
        elif args.cmd == "device-delete":
            with write_tx(conn):
                gone = conn.execute("DELETE FROM devices WHERE id = ?", (args.id,)).rowcount
            if not gone:
                print("device not found", file=sys.stderr)
                return 1
            security.audit(conn, "cli", "device.delete", f"device:{args.id}")
            print(f"deleted {args.id}")
        elif args.cmd == "devices":
            for row in svc.list_devices(conn):
                print(json.dumps(svc.device_to_dict(row)))
        elif args.cmd == "purge":
            print(svc.purge(conn, s.retention_days, s.tombstone_days))
        elif args.cmd == "ingest-create":
            try:
                _, token = security.create_ingest_token(conn, args.name)
            except sqlite3.IntegrityError:
                print(f"token {args.name!r} already exists (revoke it first)", file=sys.stderr)
                return 1
            security.audit(conn, "cli", "token.create", f"token:{args.name}")
            print(f"name:   {args.name}\ntoken:  {token}\n(store it now; only its hash is kept)")
        elif args.cmd == "ingest-list":
            for row in security.list_ingest_tokens(conn):
                print(json.dumps(security.token_to_dict(row)))
        elif args.cmd == "ingest-revoke":
            row = conn.execute("SELECT id FROM ingest_tokens WHERE name = ?", (args.name,)).fetchone()
            if row is None:
                print("token not found", file=sys.stderr)
                return 1
            security.revoke_ingest_token(conn, row["id"])
            security.audit(conn, "cli", "token.revoke", f"token:{args.name}")
            print(f"revoked {args.name}")
        elif args.cmd == "audit":
            for entry in reversed(security.list_audit(conn, args.limit)):
                print(f"{entry['at']}  {entry['actor']:<22} {entry['action']:<20} {entry['target']}  {entry['ip'] or ''}")
    finally:
        conn.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
