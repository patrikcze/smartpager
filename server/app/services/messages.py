"""Message, sync and device persistence logic. All functions take an open connection."""
from __future__ import annotations

import hashlib
import json
import secrets
import sqlite3
from datetime import datetime, timedelta, timezone
from typing import Any

from ..database.db import get_meta, next_seq, write_tx
from ..models.schemas import (
    INT_TO_PRIORITY,
    PRIORITY_TO_INT,
    DeviceEventsRequest,
    DeviceEventsResponse,
    Message,
    MessageCreate,
    SyncItem,
    SyncResponse,
)


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


def iso(dt: datetime) -> str:
    return dt.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def to_unix(value: str | None) -> int:
    if not value:
        return 0
    return int(datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc).timestamp())


def row_to_message(row: sqlite3.Row) -> Message:
    return Message(
        id=row["id"], seq=row["seq"], source=row["source"], external_id=row["external_id"],
        sender=row["sender"], subject=row["subject"], body=row["body"],
        priority=INT_TO_PRIORITY[row["priority"]], created_at=row["created_at"],
        received_at=row["received_at"], expires_at=row["expires_at"],
        delivered_at=row["delivered_at"], read_at=row["read_at"],
        acknowledged_at=row["acknowledged_at"], deleted_at=row["deleted_at"],
        metadata=json.loads(row["metadata"]), origin=row["origin"],
    )


# ---------------------------------------------------------------- messages

def create_message(conn: sqlite3.Connection, data: MessageCreate,
                   idempotency_key: str | None = None, origin: str | None = None) -> tuple[sqlite3.Row, bool]:
    """Insert a message. Returns (row, duplicate). Idempotent on (source, external_id)."""
    external_id = data.external_id or (f"idem:{idempotency_key}" if idempotency_key else None)
    now = utcnow()
    with write_tx(conn):
        if external_id is not None:
            existing = conn.execute(
                "SELECT * FROM messages WHERE source = ? AND external_id = ?",
                (data.source.value, external_id),
            ).fetchone()
            if existing is not None:
                return existing, True
        seq = next_seq(conn)
        row = conn.execute(
            """INSERT INTO messages(seq, source, external_id, sender, subject, body, priority,
                                    created_at, received_at, expires_at, metadata, origin)
               VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) RETURNING *""",
            (seq, data.source.value, external_id, data.sender, data.subject, data.body,
             PRIORITY_TO_INT[data.priority], iso(data.created_at or now), iso(now),
             iso(data.expires_at) if data.expires_at else None,
             json.dumps(data.metadata, ensure_ascii=False), origin),
        ).fetchone()
    return row, False


def get_message(conn: sqlite3.Connection, message_id: int) -> sqlite3.Row | None:
    return conn.execute("SELECT * FROM messages WHERE id = ?", (message_id,)).fetchone()


def list_messages(conn: sqlite3.Connection, *, source: str | None, unread_only: bool,
                  include_deleted: bool, before_id: int | None, limit: int) -> list[sqlite3.Row]:
    where, args = [], []
    if source:
        where.append("source = ?"); args.append(source)
    if unread_only:
        where.append("read_at IS NULL")
    if not include_deleted:
        where.append("deleted_at IS NULL")
    if before_id:
        where.append("id < ?"); args.append(before_id)
    sql = "SELECT * FROM messages"
    if where:
        sql += " WHERE " + " AND ".join(where)
    sql += " ORDER BY id DESC LIMIT ?"
    args.append(limit)
    return conn.execute(sql, args).fetchall()


_STATE_COLUMNS = {"read": "read_at", "ack": "acknowledged_at"}


def _set_state(conn: sqlite3.Connection, message_id: int, kind: str, at: datetime) -> sqlite3.Row | None:
    """Set read/ack timestamp inside an open write transaction. Idempotent: an already-set
    state is never overwritten and does not produce a new change. Ack implies read."""
    column = _STATE_COLUMNS[kind]
    row = conn.execute("SELECT * FROM messages WHERE id = ?", (message_id,)).fetchone()
    if row is None or row["deleted_at"] is not None:
        return row
    needs = row[column] is None or (kind == "ack" and row["read_at"] is None)
    if not needs:
        return row
    seq = next_seq(conn)
    stamp = iso(at)
    return conn.execute(
        f"""UPDATE messages SET seq = ?, {column} = COALESCE({column}, ?),
                read_at = COALESCE(read_at, ?) WHERE id = ? RETURNING *""",
        (seq, stamp, stamp, message_id),
    ).fetchone()


def mark_message(conn: sqlite3.Connection, message_id: int, kind: str) -> sqlite3.Row | None:
    with write_tx(conn):
        return _set_state(conn, message_id, kind, utcnow())


def delete_message(conn: sqlite3.Connection, message_id: int) -> bool:
    """Turn the message into a tombstone; contents are wiped, the row stays so devices
    learn about the deletion through sync until the tombstone itself is purged."""
    with write_tx(conn):
        row = conn.execute("SELECT deleted_at FROM messages WHERE id = ?", (message_id,)).fetchone()
        if row is None:
            return False
        if row["deleted_at"] is None:
            _tombstone(conn, message_id, utcnow())
        return True


def _tombstone(conn: sqlite3.Connection, message_id: int, at: datetime) -> None:
    conn.execute(
        """UPDATE messages SET seq = ?, deleted_at = ?, sender = '', subject = '', body = '',
                metadata = '{}' WHERE id = ?""",
        (next_seq(conn), iso(at), message_id),
    )


def purge(conn: sqlite3.Connection, retention_days: int, tombstone_days: int) -> dict[str, int]:
    """Expire, age out and finally hard-delete tombstones. Hard deletion raises the
    purge horizon; devices with an older cursor are told to reset."""
    now = utcnow()
    stats = {"expired": 0, "aged": 0, "purged": 0}
    with write_tx(conn):
        expired = conn.execute(
            "SELECT id FROM messages WHERE deleted_at IS NULL AND expires_at IS NOT NULL AND expires_at <= ?",
            (iso(now),),
        ).fetchall()
        aged = conn.execute(
            "SELECT id FROM messages WHERE deleted_at IS NULL AND received_at < ?",
            (iso(now - timedelta(days=retention_days)),),
        ).fetchall()
        for key, rows in (("expired", expired), ("aged", aged)):
            for r in rows:
                _tombstone(conn, r["id"], now)
            stats[key] = len(rows)
        cutoff = iso(now - timedelta(days=tombstone_days))
        horizon = conn.execute(
            "SELECT MAX(seq) FROM messages WHERE deleted_at IS NOT NULL AND deleted_at < ?", (cutoff,)
        ).fetchone()[0]
        if horizon is not None:
            stats["purged"] = conn.execute(
                "DELETE FROM messages WHERE deleted_at IS NOT NULL AND deleted_at < ?", (cutoff,)
            ).rowcount
            conn.execute(
                "UPDATE meta SET value = MAX(CAST(value AS INTEGER), ?) WHERE key = 'purge_horizon'",
                (horizon,),
            )
        conn.execute("DELETE FROM device_events WHERE received_at < ?", (cutoff,))
    return stats


# ---------------------------------------------------------------- sync

def _truncate_utf8(text: str, max_bytes: int) -> tuple[str, bool]:
    raw = text.encode("utf-8")
    if len(raw) <= max_bytes:
        return text, False
    return raw[:max_bytes].decode("utf-8", errors="ignore"), True


def sync(conn: sqlite3.Connection, *, cursor: int, epoch: str | None, limit: int,
         max_body: int) -> SyncResponse:
    """Return changes after `cursor`, oldest first. Runs in one read transaction so the
    page, the cursor and the unread counter are a consistent snapshot."""
    conn.execute("BEGIN")
    try:
        db_epoch = get_meta(conn, "epoch")
        horizon = int(get_meta(conn, "purge_horizon"))
        current = int(get_meta(conn, "seq"))
        reset = (
            (epoch is not None and epoch != db_epoch)
            or (cursor > 0 and cursor < horizon)
            or cursor > current
        )
        if reset:
            cursor = 0
        # From cursor 0 the device has nothing cached, so tombstones are irrelevant.
        deleted_filter = "AND deleted_at IS NULL" if cursor == 0 else ""
        rows = conn.execute(
            f"SELECT * FROM messages WHERE seq > ? {deleted_filter} ORDER BY seq LIMIT ?",
            (cursor, limit + 1),
        ).fetchall()
        unread = conn.execute(
            "SELECT COUNT(*) FROM messages WHERE deleted_at IS NULL AND read_at IS NULL"
        ).fetchone()[0]
    finally:
        conn.execute("COMMIT")

    has_more = len(rows) > limit
    rows = rows[:limit]
    items = []
    for r in rows:
        if r["deleted_at"] is not None:
            items.append(SyncItem(id=r["id"], seq=r["seq"], deleted=True))
            continue
        body, trunc = _truncate_utf8(r["body"], max_body)
        items.append(SyncItem(
            id=r["id"], seq=r["seq"], src=r["source"], from_=r["sender"], subj=r["subject"],
            body=body, trunc=trunc, pri=r["priority"], ts=to_unix(r["created_at"]),
            read=to_unix(r["read_at"]), ack=to_unix(r["acknowledged_at"]),
        ))
    # With an empty page the cursor stays where it was; when everything after the cursor was
    # filtered (reset snapshot) we still advance to the newest seq we are aware of.
    new_cursor = rows[-1]["seq"] if rows else (current if cursor == 0 else cursor)
    return SyncResponse(epoch=db_epoch, server_time=int(utcnow().timestamp()), reset=reset,
                        cursor=new_cursor, has_more=has_more, unread=unread, items=items)


def current_seq(conn: sqlite3.Connection) -> int:
    return int(get_meta(conn, "seq"))


# ---------------------------------------------------------------- devices

def hash_token(token: str) -> str:
    return hashlib.sha256(token.encode()).hexdigest()


def create_device(conn: sqlite3.Connection, device_id: str, name: str) -> tuple[sqlite3.Row, str]:
    token = "spd_" + secrets.token_urlsafe(32)
    with write_tx(conn):
        row = conn.execute(
            "INSERT INTO devices(id, name, token_hash, created_at) VALUES (?, ?, ?, ?) RETURNING *",
            (device_id, name, hash_token(token), iso(utcnow())),
        ).fetchone()
    return row, token


def rotate_device_token(conn: sqlite3.Connection, device_id: str) -> str | None:
    token = "spd_" + secrets.token_urlsafe(32)
    with write_tx(conn):
        cur = conn.execute("UPDATE devices SET token_hash = ? WHERE id = ?", (hash_token(token), device_id))
    return token if cur.rowcount else None


def authenticate_device(conn: sqlite3.Connection, device_id: str, token: str) -> sqlite3.Row | None:
    # Look up by hash (a constant-time comparison of the hex digest follows as defence in depth).
    digest = hash_token(token)
    row = conn.execute("SELECT * FROM devices WHERE id = ? AND disabled = 0", (device_id,)).fetchone()
    if row is None or not secrets.compare_digest(row["token_hash"], digest):
        return None
    return row


def list_devices(conn: sqlite3.Connection) -> list[sqlite3.Row]:
    return conn.execute("SELECT * FROM devices ORDER BY id").fetchall()


def touch_device(conn: sqlite3.Connection, device_id: str, ip: str | None, firmware: str | None) -> None:
    with write_tx(conn):
        conn.execute(
            "UPDATE devices SET last_seen_at = ?, last_ip = ?, firmware = COALESCE(?, firmware) WHERE id = ?",
            (iso(utcnow()), ip, firmware, device_id),
        )


def apply_device_events(conn: sqlite3.Connection, device_id: str,
                        req: DeviceEventsRequest) -> DeviceEventsResponse:
    """Apply read/ack events exactly once (keyed by device-generated event id) and record
    the device's confirmed sync cursor, which marks messages as delivered."""
    accepted, duplicates, unknown = [], [], []
    now = utcnow()
    with write_tx(conn):
        # delivery first: applying events below moves those messages to a newer seq
        if req.ack_cursor is not None:
            prev = conn.execute("SELECT acked_cursor FROM devices WHERE id = ?", (device_id,)).fetchone()[0]
            if req.ack_cursor > prev and req.ack_cursor <= current_seq(conn):
                conn.execute("UPDATE devices SET acked_cursor = ? WHERE id = ?", (req.ack_cursor, device_id))
                # delivery is server-side bookkeeping only: it does not create a sync change
                conn.execute(
                    "UPDATE messages SET delivered_at = ? WHERE delivered_at IS NULL AND seq <= ?",
                    (iso(now), req.ack_cursor),
                )
        for ev in req.events:
            inserted = conn.execute(
                """INSERT OR IGNORE INTO device_events(device_id, event_id, type, message_id, received_at)
                   VALUES (?, ?, ?, ?, ?)""",
                (device_id, ev.id, ev.type, ev.message_id, iso(now)),
            ).rowcount
            if not inserted:
                duplicates.append(ev.id)
                continue
            at = datetime.fromtimestamp(ev.at, timezone.utc) if ev.at else now
            if at > now:  # never trust a device clock that runs ahead
                at = now
            if _set_state(conn, ev.message_id, ev.type, at) is None:
                unknown.append(ev.id)
            else:
                accepted.append(ev.id)
        if req.status:
            conn.execute("UPDATE devices SET status = ? WHERE id = ?", (json.dumps(req.status), device_id))
    return DeviceEventsResponse(accepted=accepted, duplicates=duplicates, unknown_messages=unknown)


def device_to_dict(row: sqlite3.Row) -> dict[str, Any]:
    return {
        "id": row["id"], "name": row["name"], "created_at": row["created_at"],
        "last_seen_at": row["last_seen_at"], "last_ip": row["last_ip"], "firmware": row["firmware"],
        "acked_cursor": row["acked_cursor"], "status": json.loads(row["status"]),
        "disabled": bool(row["disabled"]),
    }
