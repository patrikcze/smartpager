"""Authentication, per-request DB connections and rate limiting."""
from __future__ import annotations

import logging
import secrets
import sqlite3
import threading
import time
from collections import OrderedDict
from typing import Iterator

from fastapi import Depends, HTTPException, Request, status

from ..database.db import connect
from ..services import messages as svc
from ..services import security

log = logging.getLogger("smartpager.auth")


def get_conn(request: Request) -> Iterator[sqlite3.Connection]:
    conn = connect(request.app.state.settings.db_path)
    try:
        yield conn
    finally:
        conn.close()


def client_ip(request: Request) -> str | None:
    # uvicorn rewrites this from X-Forwarded-For only when the peer is in FORWARDED_ALLOW_IPS
    return request.client.host if request.client else None


def via_proxy(request: Request) -> bool:
    """True when the request passed through the public reverse proxy. Caddy always sets
    X-Forwarded-For; a client adding it on a direct request only locks itself out."""
    return "x-forwarded-for" in request.headers or "forwarded" in request.headers


def _bearer(request: Request) -> str | None:
    header = request.headers.get("authorization", "")
    scheme, _, token = header.partition(" ")
    if scheme.lower() != "bearer" or not token.strip():
        return None
    return token.strip()


def _require_bearer(request: Request) -> str:
    token = _bearer(request)
    if token is None:
        raise HTTPException(status.HTTP_401_UNAUTHORIZED, "missing bearer token",
                            headers={"WWW-Authenticate": "Bearer"})
    return token


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
    ip = client_ip(request) or "?"
    _limit(request, f"authfail:{ip}")
    log.warning("authentication failed: %s %s from %s", request.method, request.url.path, ip)
    return HTTPException(status.HTTP_401_UNAUTHORIZED, "invalid credentials",
                         headers={"WWW-Authenticate": "Bearer"})


def require_admin(request: Request) -> str:
    # Admin is never served through the public proxy unless explicitly allowed. The proxy
    # only routes device/ingest paths; this is the second line of defence.
    if via_proxy(request) and not request.app.state.settings.admin_via_proxy:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "Not Found")
    token = _require_bearer(request)
    if not secrets.compare_digest(token, request.app.state.settings.admin_token):
        raise _auth_failed(request)
    _limit(request, "admin")
    return "admin"


def _check_ingest(request: Request, conn: sqlite3.Connection, token: str) -> str:
    s = request.app.state.settings
    if s.ingest_token and secrets.compare_digest(token, s.ingest_token):
        _limit(request, "ingest")
        return "ingest"
    name = security.authenticate_ingest(conn, token)
    if name is not None:
        _limit(request, f"ingest:{name}")
        return f"token:{name}"
    # the admin token may create messages too, but never through the public proxy
    if secrets.compare_digest(token, s.admin_token) and (not via_proxy(request) or s.admin_via_proxy):
        _limit(request, "admin")
        return "admin"
    raise _auth_failed(request)


def require_ingest(request: Request, conn: sqlite3.Connection = Depends(get_conn)) -> str:
    """Message creation: an ingest token (environment or named) or, locally, the admin token.
    Returns the actor name recorded as the message origin."""
    return _check_ingest(request, conn, _require_bearer(request))


def require_ingest_webhook(request: Request, conn: sqlite3.Connection = Depends(get_conn)) -> str:
    """Like require_ingest, but also accepts `?token=` for senders that can't set headers.
    Only ingest tokens are accepted in the URL: URLs end up in logs and browser history."""
    token = _bearer(request)
    if token is None:
        token = request.query_params.get("token")
        if not token:
            raise HTTPException(status.HTTP_401_UNAUTHORIZED, "missing token",
                                headers={"WWW-Authenticate": "Bearer"})
        if secrets.compare_digest(token, request.app.state.settings.admin_token):
            raise _auth_failed(request)
    return _check_ingest(request, conn, token)


def require_device(device_id: str, request: Request,
                   conn: sqlite3.Connection = Depends(get_conn)) -> sqlite3.Row:
    token = _require_bearer(request)
    device = svc.authenticate_device(conn, device_id, token)
    if device is None:
        raise _auth_failed(request)
    _limit(request, f"device:{device_id}")
    return device
