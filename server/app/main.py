"""SmartPager backend entry point: `uvicorn --factory app.main:create_app`."""
from __future__ import annotations

import asyncio
import logging
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.concurrency import run_in_threadpool
from fastapi.responses import FileResponse, JSONResponse

from . import __version__
from .api.deps import RateLimiter
from .api.routes import router
from .config import Settings
from .database.db import connect, migrate
from .integrations import webhooks
from .services import messages as svc
from .services.notifier import ChangeNotifier

log = logging.getLogger("smartpager")
STATIC = Path(__file__).parent / "static"


async def _purge_loop(app: FastAPI) -> None:
    s: Settings = app.state.settings
    while True:
        try:
            def run() -> dict[str, int]:
                conn = connect(s.db_path)
                try:
                    return svc.purge(conn, s.retention_days, s.tombstone_days)
                finally:
                    conn.close()
            stats = await run_in_threadpool(run)
            if any(stats.values()):
                log.info("retention purge: %s", stats)
                app.state.notifier.notify()
        except Exception:  # keep the loop alive; never log message contents
            log.exception("retention purge failed")
        await asyncio.sleep(3600)


class BodySizeLimit:
    """Reject request bodies larger than the configured limit, also for chunked uploads."""

    def __init__(self, app, max_bytes: int) -> None:
        self.app = app
        self.max_bytes = max_bytes

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            return await self.app(scope, receive, send)
        for name, value in scope.get("headers", []):
            if name == b"content-length" and value.isdigit() and int(value) > self.max_bytes:
                return await JSONResponse({"detail": "request body too large"}, 413)(scope, receive, send)
        seen = 0

        async def limited_receive():
            nonlocal seen
            message = await receive()
            if message["type"] == "http.request":
                seen += len(message.get("body", b""))
                if seen > self.max_bytes:
                    raise _TooLarge()
            return message

        try:
            await self.app(scope, limited_receive, send)
        except _TooLarge:
            await JSONResponse({"detail": "request body too large"}, 413)(scope, receive, send)


class _TooLarge(Exception):
    pass


def create_app(settings: Settings | None = None) -> FastAPI:
    settings = settings or Settings.from_env()
    settings.validate()

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        migrate(settings.db_path)
        app.state.notifier.bind(asyncio.get_running_loop())
        task = asyncio.create_task(_purge_loop(app))
        yield
        task.cancel()

    app = FastAPI(title="SmartPager", version=__version__, lifespan=lifespan,
                  docs_url="/docs", redoc_url=None)
    app.state.settings = settings
    app.state.notifier = ChangeNotifier()
    app.state.limiter = RateLimiter(settings.rate_limit_per_min)
    app.include_router(router)
    app.include_router(webhooks.router)
    app.add_middleware(BodySizeLimit, max_bytes=settings.max_request_bytes)

    @app.middleware("http")
    async def security_headers(request: Request, call_next):
        response = await call_next(request)
        response.headers.setdefault("X-Content-Type-Options", "nosniff")
        response.headers.setdefault("Referrer-Policy", "no-referrer")
        response.headers.setdefault("Cache-Control", "no-store")
        if request.url.path.startswith("/admin"):
            response.headers["Content-Security-Policy"] = (
                "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; "
                "frame-ancestors 'none'")
        return response

    @app.exception_handler(Exception)
    async def unhandled(request: Request, exc: Exception):
        log.exception("unhandled error on %s %s", request.method, request.url.path)
        return JSONResponse({"detail": "internal server error"}, status_code=500)

    @app.get("/health")
    def health():
        conn = connect(settings.db_path)
        try:
            seq = svc.current_seq(conn)
        finally:
            conn.close()
        return {"status": "ok", "version": __version__, "seq": seq}

    @app.get("/admin", include_in_schema=False)
    def admin():
        return FileResponse(STATIC / "admin.html")

    return app
