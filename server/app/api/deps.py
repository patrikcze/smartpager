"""Authentication, per-request DB connections and rate limiting."""
from __future__ import annotations

import secrets
import sqlite3
import threading
import time
from collections import OrderedDict
from typing import Iterator

from fastapi import Depends, HTTPException, Request, status

from ..database.db import connect
from ..services import messages as svc


def get_conn(request: Request) -> Iterator[sqlite3.Connection]:
    conn = connect(request.app.state.settings.db_path)
    try:
        yield conn
    finally:
        conn.close()


def _bearer(request: Request) -> str:
    header = request.headers.get("authorization", "")
    scheme, _, token = header.partition(" ")
    if scheme.lower() != "bearer" or not token:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "missing bearer token",
                            headers={"WWW-Authenticate": "Bearer"})
    return token.strip()


class RateLimiter:
    """Fixed-capacity token bucket per key. Bounded LRU so it cannot grow without limit."""

    def __init__(self, per_minute: int, max_keys: int = 1024) -> None:
        self.rate = per_minute / 60.0
        self.capacity = float(max(per_minute // 4, 10))
        self.max_keys = max_keys
        self._buckets: OrderedDict[str, tuple[float, float]] = OrderedDict()
        self._lock = threading.Lock()

    def allow(self, key: str) -> bool:
        now = time.monotonic()
        with self._lock:
            tokens, last = self._buckets.pop(key, (self.capacity, now))
            tokens = min(self.capacity, tokens + (now - last) * self.rate)
            ok = tokens >= 1.0
            self._buckets[key] = (tokens - 1.0 if ok else tokens, now)
            while len(self._buckets) > self.max_keys:
                self._buckets.popitem(last=False)
            return ok


def _limit(request: Request, key: str) -> None:
    if not request.app.state.limiter.allow(key):
        raise HTTPException(status.HTTP_429_TOO_MANY_REQUESTS, "rate limit exceeded",
                            headers={"Retry-After": "5"})


def _auth_failed(request: Request) -> HTTPException:
    ip = request.client.host if request.client else "?"
    _limit(request, f"authfail:{ip}")
    return HTTPException(status.HTTP_401_UNAUTHORIZED, "invalid credentials",
                         headers={"WWW-Authenticate": "Bearer"})


def require_admin(request: Request) -> str:
    token = _bearer(request)
    if not secrets.compare_digest(token, request.app.state.settings.admin_token):
        raise _auth_failed(request)
    _limit(request, "admin")
    return "admin"


def require_ingest(request: Request) -> str:
    """Message creation accepts the ingest token (connectors) or the admin token."""
    token = _bearer(request)
    s = request.app.state.settings
    if secrets.compare_digest(token, s.ingest_token):
        _limit(request, "ingest")
        return "ingest"
    if secrets.compare_digest(token, s.admin_token):
        _limit(request, "admin")
        return "admin"
    raise _auth_failed(request)


def require_device(device_id: str, request: Request,
                   conn: sqlite3.Connection = Depends(get_conn)) -> sqlite3.Row:
    token = _bearer(request)
    device = svc.authenticate_device(conn, device_id, token)
    if device is None:
        raise _auth_failed(request)
    _limit(request, f"device:{device_id}")
    return device
