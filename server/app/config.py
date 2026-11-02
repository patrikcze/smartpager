"""Runtime configuration, read from environment variables (see .env.example)."""
from __future__ import annotations

import os
from dataclasses import dataclass


def _int(name: str, default: int) -> int:
    raw = os.environ.get(name)
    return int(raw) if raw else default


@dataclass(frozen=True)
class Settings:
    db_path: str
    admin_token: str
    ingest_token: str
    retention_days: int
    tombstone_days: int
    max_request_bytes: int
    rate_limit_per_min: int
    max_long_poll_s: int

    @classmethod
    def from_env(cls) -> "Settings":
        return cls(
            db_path=os.environ.get("SMARTPAGER_DB", "./data/smartpager.db"),
            admin_token=os.environ.get("SMARTPAGER_ADMIN_TOKEN", ""),
            ingest_token=os.environ.get("SMARTPAGER_INGEST_TOKEN", ""),
            retention_days=_int("SMARTPAGER_RETENTION_DAYS", 30),
            tombstone_days=_int("SMARTPAGER_TOMBSTONE_DAYS", 7),
            max_request_bytes=_int("SMARTPAGER_MAX_REQUEST_BYTES", 64 * 1024),
            rate_limit_per_min=_int("SMARTPAGER_RATE_LIMIT_PER_MIN", 240),
            max_long_poll_s=_int("SMARTPAGER_MAX_LONG_POLL_S", 25),
        )

    def validate(self) -> None:
        for name, value in (("SMARTPAGER_ADMIN_TOKEN", self.admin_token),
                            ("SMARTPAGER_INGEST_TOKEN", self.ingest_token)):
            if len(value) < 24:
                raise RuntimeError(f"{name} must be set to a random value of at least 24 characters "
                                   "(generate one with: python -m app.cli token)")
        if self.admin_token == self.ingest_token:
            raise RuntimeError("SMARTPAGER_ADMIN_TOKEN and SMARTPAGER_INGEST_TOKEN must differ")
