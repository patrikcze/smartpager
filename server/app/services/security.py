"""Named per-integration ingest tokens and the audit log."""
from __future__ import annotations

import json
import secrets
import sqlite3
from datetime import timedelta

from ..database.db import write_tx
from .messages import check_recipients, hash_token, iso, utcnow

TOKEN_PREFIX = "spi_"  # ingest tokens: easy to recognise in a leak scan


def create_ingest_token(conn: sqlite3.Connection, name: str,
                        recipients: list[str] | None = None) -> tuple[sqlite3.Row, str]:
    """Create a token. `recipients` is its default `to` (None = every device); raises
    UnknownDevices for unregistered ids."""
    token = TOKEN_PREFIX + secrets.token_urlsafe(32)
    with write_tx(conn):
        stored = check_recipients(conn, recipients)
        # a revoked token's name may be reused; the audit log keeps the history
        conn.execute("DELETE FROM ingest_tokens WHERE name = ? AND revoked_at IS NOT NULL", (name,))
        row = conn.execute(
            """INSERT INTO ingest_tokens(name, token_hash, created_at, recipients)
               VALUES (?, ?, ?, ?) RETURNING *""",
            (name, hash_token(token), iso(utcnow()), stored),
        ).fetchone()
    return row, token


def set_ingest_recipients(conn: sqlite3.Connection, token_id: int,
                          recipients: list[str] | None) -> sqlite3.Row | None:
    """Change where a token's messages go by default, without issuing a new token."""
    with write_tx(conn):
        stored = check_recipients(conn, recipients)
        return conn.execute("UPDATE ingest_tokens SET recipients = ? WHERE id = ? RETURNING *",
                            (stored, token_id)).fetchone()


def ingest_recipients(conn: sqlite3.Connection, actor: str) -> list[str] | None:
    """Default recipients of the credential that sent a message ("token:<name>"), if any."""
    if not actor.startswith("token:"):
        return None
    row = conn.execute("SELECT recipients FROM ingest_tokens WHERE name = ? AND revoked_at IS NULL",
                       (actor[len("token:"):],)).fetchone()
    return json.loads(row["recipients"]) if row and row["recipients"] else None


def list_ingest_tokens(conn: sqlite3.Connection) -> list[sqlite3.Row]:
    return conn.execute("SELECT * FROM ingest_tokens ORDER BY revoked_at IS NOT NULL, name").fetchall()


def revoke_ingest_token(conn: sqlite3.Connection, token_id: int) -> sqlite3.Row | None:
    with write_tx(conn):
        return conn.execute(
            "UPDATE ingest_tokens SET revoked_at = COALESCE(revoked_at, ?) WHERE id = ? RETURNING *",
            (iso(utcnow()), token_id),
        ).fetchone()


def authenticate_ingest(conn: sqlite3.Connection, token: str) -> str | None:
    """Return the token's name if it is a valid, unrevoked ingest token."""
    if not token.startswith(TOKEN_PREFIX):
        return None
    row = conn.execute(
        "SELECT id, name, last_used_at FROM ingest_tokens WHERE token_hash = ? AND revoked_at IS NULL",
        (hash_token(token),),
    ).fetchone()
    if row is None:
        return None
    now = utcnow()
    # record usage at most once a minute to avoid a write per request
    if not row["last_used_at"] or row["last_used_at"] < iso(now - timedelta(minutes=1)):
        with write_tx(conn):
            conn.execute("UPDATE ingest_tokens SET last_used_at = ? WHERE id = ?", (iso(now), row["id"]))
    return row["name"]


def token_to_dict(row: sqlite3.Row) -> dict:
    return {"id": row["id"], "name": row["name"], "created_at": row["created_at"],
            "last_used_at": row["last_used_at"], "revoked_at": row["revoked_at"],
            "recipients": json.loads(row["recipients"]) if row["recipients"] else None}


def audit(conn: sqlite3.Connection, actor: str, action: str, target: str = "", ip: str | None = None) -> None:
    """Append an audit record. Never pass message contents or secrets as target."""
    with write_tx(conn):
        conn.execute("INSERT INTO audit_log(at, actor, action, target, ip) VALUES (?, ?, ?, ?, ?)",
                     (iso(utcnow()), actor[:80], action[:40], target[:200], ip))


def list_audit(conn: sqlite3.Connection, limit: int) -> list[dict]:
    rows = conn.execute("SELECT * FROM audit_log ORDER BY id DESC LIMIT ?", (limit,)).fetchall()
    return [dict(r) for r in rows]


def purge_audit(conn: sqlite3.Connection, keep_days: int) -> int:
    with write_tx(conn):
        return conn.execute("DELETE FROM audit_log WHERE at < ?",
                            (iso(utcnow() - timedelta(days=keep_days)),)).rowcount
