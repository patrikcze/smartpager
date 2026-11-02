from __future__ import annotations

import pytest
from fastapi.testclient import TestClient

from app.config import Settings
from app.main import create_app

ADMIN = "admin-" + "a" * 30
INGEST = "ingest-" + "b" * 30


def make_settings(db_path: str, **overrides) -> Settings:
    base = dict(db_path=db_path, admin_token=ADMIN, ingest_token=INGEST, retention_days=30,
                tombstone_days=7, max_request_bytes=64 * 1024, rate_limit_per_min=6000,
                max_long_poll_s=5)
    base.update(overrides)
    return Settings(**base)


@pytest.fixture
def db_path(tmp_path):
    return str(tmp_path / "test.db")


@pytest.fixture
def client(db_path):
    with TestClient(create_app(make_settings(db_path))) as c:
        yield c


def admin_h():
    return {"Authorization": f"Bearer {ADMIN}"}


def ingest_h():
    return {"Authorization": f"Bearer {INGEST}"}


@pytest.fixture
def device(client):
    r = client.post("/api/v1/devices", json={"id": "pager-1", "name": "Desk"}, headers=admin_h())
    assert r.status_code == 201, r.text
    return {"id": "pager-1", "headers": {"Authorization": f"Bearer {r.json()['token']}"}}
