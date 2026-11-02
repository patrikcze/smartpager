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
    ota_dir: str = ""
    admin_via_proxy: bool = False

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
            ota_dir=os.environ.get("SMARTPAGER_OTA_DIR", ""),
            admin_via_proxy=os.environ.get("SMARTPAGER_ADMIN_VIA_PROXY", "0").lower() in ("1", "true", "yes"),
        )

    @property
    def firmware_dir(self) -> str:
        """Where signed OTA images are published (default: <db dir>/firmware)."""
        return self.ota_dir or os.path.join(os.path.dirname(os.path.abspath(self.db_path)), "firmware")

    def validate(self) -> None:
        hint = "(generate one with: python -m app.cli token)"
        if len(self.admin_token) < 24:
            raise RuntimeError(f"SMARTPAGER_ADMIN_TOKEN must be set to a random value of at least 24 characters {hint}")
        # The environment ingest token is optional: named per-integration tokens (app.cli
        # ingest-create) can be revoked individually and are preferred.
        if self.ingest_token and len(self.ingest_token) < 24:
            raise RuntimeError(f"SMARTPAGER_INGEST_TOKEN must be empty or at least 24 characters {hint}")
        if self.ingest_token and self.admin_token == self.ingest_token:
            raise RuntimeError("SMARTPAGER_ADMIN_TOKEN and SMARTPAGER_INGEST_TOKEN must differ")
