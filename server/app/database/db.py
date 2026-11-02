"""SQLite access and schema migrations.

Concurrency model: SQLite allows a single writer at a time. Every mutation of a
message runs inside ``BEGIN IMMEDIATE`` and takes the next value of the global
change counter (``meta.seq``) in the same transaction. Because write
transactions are serialized, a change with sequence N is always committed
before a change with sequence N+1 can be written, so a reader that has seen
seq N can never later miss a change with a smaller seq. This is what makes the
device sync cursor safe while new messages arrive.
"""
from __future__ import annotations

import secrets
import sqlite3
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator

SCHEMA_VERSION = 1

MIGRATIONS: dict[int, str] = {
    1: """
    CREATE TABLE meta (
        key   TEXT PRIMARY KEY,
        value TEXT NOT NULL
    );
    CREATE TABLE messages (
        id              INTEGER PRIMARY KEY AUTOINCREMENT,
        seq             INTEGER NOT NULL UNIQUE,
        source          TEXT NOT NULL,
        external_id     TEXT,
        sender          TEXT NOT NULL DEFAULT '',
        subject         TEXT NOT NULL DEFAULT '',
        body            TEXT NOT NULL DEFAULT '',
        priority        INTEGER NOT NULL DEFAULT 1,
        created_at      TEXT NOT NULL,
        received_at     TEXT NOT NULL,
        expires_at      TEXT,
        delivered_at    TEXT,
        read_at         TEXT,
        acknowledged_at TEXT,
        deleted_at      TEXT,
        metadata        TEXT NOT NULL DEFAULT '{}'
    );
    CREATE UNIQUE INDEX ux_messages_source_ext ON messages(source, external_id)
        WHERE external_id IS NOT NULL;
    CREATE INDEX ix_messages_received ON messages(received_at);
    CREATE INDEX ix_messages_expires ON messages(expires_at) WHERE expires_at IS NOT NULL;
    CREATE INDEX ix_messages_deleted ON messages(deleted_at) WHERE deleted_at IS NOT NULL;

    CREATE TABLE devices (
        id            TEXT PRIMARY KEY,
        name          TEXT NOT NULL,
        token_hash    TEXT NOT NULL UNIQUE,
        created_at    TEXT NOT NULL,
        last_seen_at  TEXT,
        last_ip       TEXT,
        firmware      TEXT,
        acked_cursor  INTEGER NOT NULL DEFAULT 0,
        status        TEXT NOT NULL DEFAULT '{}',
        disabled      INTEGER NOT NULL DEFAULT 0
    );
    CREATE TABLE device_events (
        device_id   TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
        event_id    TEXT NOT NULL,
        type        TEXT NOT NULL,
        message_id  INTEGER,
        received_at TEXT NOT NULL,
        PRIMARY KEY (device_id, event_id)
    );
    CREATE INDEX ix_device_events_received ON device_events(received_at);
    """,
}


def connect(db_path: str) -> sqlite3.Connection:
    conn = sqlite3.connect(db_path, timeout=10, isolation_level=None, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    conn.execute("PRAGMA busy_timeout = 10000")
    return conn


@contextmanager
def write_tx(conn: sqlite3.Connection) -> Iterator[sqlite3.Connection]:
    conn.execute("BEGIN IMMEDIATE")
    try:
        yield conn
    except BaseException:
        conn.execute("ROLLBACK")
        raise
    else:
        conn.execute("COMMIT")


def next_seq(conn: sqlite3.Connection) -> int:
    """Allocate the next change sequence number. Must run inside write_tx."""
    row = conn.execute(
        "UPDATE meta SET value = CAST(value AS INTEGER) + 1 WHERE key = 'seq' RETURNING value"
    ).fetchone()
    return int(row[0])


def get_meta(conn: sqlite3.Connection, key: str) -> str:
    return conn.execute("SELECT value FROM meta WHERE key = ?", (key,)).fetchone()[0]


def migrate(db_path: str) -> None:
    if db_path != ":memory:":
        Path(db_path).parent.mkdir(parents=True, exist_ok=True)
    conn = connect(db_path)
    try:
        conn.execute("PRAGMA journal_mode = WAL")
        current = conn.execute("PRAGMA user_version").fetchone()[0]
        if current > SCHEMA_VERSION:
            raise RuntimeError(f"database schema v{current} is newer than this server (v{SCHEMA_VERSION})")
        for version in range(current + 1, SCHEMA_VERSION + 1):
            with write_tx(conn):
                # executescript() would COMMIT implicitly, so run statements one by one
                for stmt in MIGRATIONS[version].split(";"):
                    if stmt.strip():
                        conn.execute(stmt)
                if version == 1:
                    # epoch identifies this database instance; if the DB is recreated the
                    # device sees a different epoch and resynchronizes from scratch.
                    conn.executemany(
                        "INSERT INTO meta(key, value) VALUES (?, ?)",
                        [("epoch", secrets.token_hex(4)), ("seq", "0"), ("purge_horizon", "0")],
                    )
                conn.execute(f"PRAGMA user_version = {version}")
    finally:
        conn.close()
