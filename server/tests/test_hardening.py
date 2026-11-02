"""Public-deployment hardening: proxy guard, named ingest tokens, audit log, OTA endpoints."""
from __future__ import annotations

import json
import sqlite3

from fastapi.testclient import TestClient

from app.database.db import MIGRATIONS, migrate
from app.main import create_app

from .conftest import ADMIN, admin_h, ingest_h, make_settings

PROXIED = {"X-Forwarded-For": "203.0.113.7"}


# ------------------------------------------------------------- admin is local-only

def test_admin_api_hidden_through_proxy(client):
    assert client.get("/api/v1/messages", headers=admin_h() | PROXIED).status_code == 404
    assert client.get("/api/v1/devices", headers=admin_h() | PROXIED).status_code == 404
    assert client.post("/api/v1/tokens", json={"name": "x1"}, headers=admin_h() | PROXIED).status_code == 404
    assert client.get("/api/v1/messages", headers=admin_h()).status_code == 200


def test_console_and_docs_hidden_through_proxy(client):
    for path in ("/admin", "/docs", "/openapi.json"):
        assert client.get(path, headers=PROXIED).status_code == 404, path
        assert client.get(path).status_code == 200, path
    assert client.get("/health", headers=PROXIED).status_code == 200


def test_admin_token_cannot_ingest_through_proxy(client):
    r = client.post("/api/v1/messages", json={"body": "x"}, headers=admin_h() | PROXIED)
    assert r.status_code == 401
    assert client.post("/api/v1/messages", json={"body": "x"}, headers=ingest_h() | PROXIED).status_code == 201


def test_admin_via_proxy_opt_in(db_path):
    with TestClient(create_app(make_settings(db_path, admin_via_proxy=True))) as c:
        assert c.get("/api/v1/messages", headers=admin_h() | PROXIED).status_code == 200


def test_device_endpoints_work_through_proxy(client, device):
    r = client.get("/api/v1/devices/pager-1/sync", headers=device["headers"] | PROXIED)
    assert r.status_code == 200


# ------------------------------------------------------------- named ingest tokens

def _new_token(client, name="grafana"):
    r = client.post("/api/v1/tokens", json={"name": name}, headers=admin_h())
    assert r.status_code == 201, r.text
    return r.json()


def test_named_token_lifecycle(client):
    created = _new_token(client)
    tok = created["token"]
    assert tok.startswith("spi_")
    h = {"Authorization": f"Bearer {tok}"}
    m = client.post("/api/v1/messages", json={"subject": "hi"}, headers=h).json()["message"]
    assert m["origin"] == "token:grafana"
    listed = client.get("/api/v1/tokens", headers=admin_h()).json()
    assert listed[0]["name"] == "grafana" and listed[0]["last_used_at"] and "token" not in listed[0]
    assert "token_hash" not in client.get("/api/v1/tokens", headers=admin_h()).text
    # revoke: stops working immediately, other credentials unaffected
    tid = created["token_info"]["id"]
    assert client.delete(f"/api/v1/tokens/{tid}", headers=admin_h()).json()["revoked_at"]
    assert client.post("/api/v1/messages", json={"subject": "hi"}, headers=h).status_code == 401
    assert client.post("/api/v1/messages", json={"subject": "hi"}, headers=ingest_h()).status_code == 201
    # the name can be reused after revocation, duplicates of active names can't
    _new_token(client)
    assert client.post("/api/v1/tokens", json={"name": "grafana"}, headers=admin_h()).status_code == 409


def test_token_names_validated(client):
    for bad in ("", "x", "a" * 41, "../etc", "bad\nname"):
        assert client.post("/api/v1/tokens", json={"name": bad}, headers=admin_h()).status_code == 422


def test_env_ingest_token_optional(db_path):
    with TestClient(create_app(make_settings(db_path, ingest_token=""))) as c:
        tok = c.post("/api/v1/tokens", json={"name": "ci"}, headers=admin_h()).json()["token"]
        r = c.post("/api/v1/messages", json={"body": "x"}, headers={"Authorization": f"Bearer {tok}"})
        assert r.status_code == 201
        # an empty env token must never match an empty/absent credential
        assert c.post("/api/v1/messages", json={"body": "x"}, headers={"Authorization": "Bearer "}).status_code == 401


# ------------------------------------------------------------- webhook URL token

def test_webhook_query_token(client):
    tok = _new_token(client, "uptime")["token"]
    r = client.post(f"/api/v1/webhooks/generic?token={tok}", json={"title": "down"})
    assert r.status_code == 201
    m = client.get(f"/api/v1/messages/{r.json()['id']}", headers=admin_h()).json()
    assert m["origin"] == "token:uptime"
    assert client.post("/api/v1/webhooks/generic?token=spi_wrong", json={"title": "x"}).status_code == 401
    assert client.post("/api/v1/webhooks/generic", json={"title": "x"}).status_code == 401


def test_webhook_query_token_rejects_admin_token(client):
    r = client.post(f"/api/v1/webhooks/generic?token={ADMIN}", json={"title": "x"})
    assert r.status_code == 401


def test_query_token_not_accepted_on_core_api(client):
    tok = _new_token(client, "query")["token"]
    assert client.post(f"/api/v1/messages?token={tok}", json={"body": "x"}).status_code == 401


# ------------------------------------------------------------- audit log

def test_audit_records_admin_actions_without_content(client, device):
    mid = client.post("/api/v1/messages", json={"body": "secret body"}, headers=ingest_h()).json()["message"]["id"]
    client.post(f"/api/v1/messages/{mid}/ack", headers=admin_h())
    client.delete(f"/api/v1/messages/{mid}", headers=admin_h())
    _new_token(client, "audit-me")
    client.post("/api/v1/devices/pager-1/token", headers=admin_h())
    entries = client.get("/api/v1/audit", headers=admin_h()).json()
    actions = [e["action"] for e in entries]
    for expected in ("device.create", "message.ack", "message.delete", "token.create", "device.rotate_token"):
        assert expected in actions
    assert "secret body" not in json.dumps(entries)
    assert all(e["actor"] == "admin" and e["ip"] for e in entries)


# ------------------------------------------------------------- migration v1 -> v2

def test_migration_from_v1_keeps_data(tmp_path):
    path = str(tmp_path / "old.db")
    conn = sqlite3.connect(path, isolation_level=None)
    for stmt in MIGRATIONS[1].split(";"):
        if stmt.strip():
            conn.execute(stmt)
    conn.executemany("INSERT INTO meta VALUES (?, ?)", [("epoch", "abcd1234"), ("seq", "1"), ("purge_horizon", "0")])
    conn.execute("INSERT INTO messages(seq, source, created_at, received_at, subject) VALUES (1, 'manual', "
                 "'2026-11-02T10:00:00Z', '2026-11-02T10:00:00Z', 'old')")
    conn.execute("PRAGMA user_version = 1")
    conn.close()
    migrate(path)
    with TestClient(create_app(make_settings(path))) as c:
        msgs = c.get("/api/v1/messages", headers=admin_h()).json()["messages"]
        assert msgs[0]["subject"] == "old" and msgs[0]["origin"] is None
        assert c.get("/api/v1/tokens", headers=admin_h()).json() == []


# ------------------------------------------------------------- OTA distribution

MANIFEST = {"version": "0.2.0", "size": 4, "sha256": "a" * 64, "signature": "MEUCIQDexample+signature=="}


def _publish(tmp_path, manifest=MANIFEST, image=b"\xe9\x01\x02\x03"):
    d = tmp_path / "fw"
    d.mkdir(exist_ok=True)
    (d / "manifest.json").write_text(json.dumps(manifest))
    (d / "firmware.bin").write_bytes(image)
    return str(d)


def test_firmware_endpoints(tmp_path, db_path):
    with TestClient(create_app(make_settings(db_path, ota_dir=str(tmp_path / "none")))) as c:
        tok = c.post("/api/v1/devices", json={"id": "pager-1"}, headers=admin_h()).json()["token"]
        h = {"Authorization": f"Bearer {tok}"}
        assert c.get("/api/v1/devices/pager-1/firmware", headers=h).status_code == 404
    with TestClient(create_app(make_settings(db_path, ota_dir=_publish(tmp_path)))) as c:
        assert c.get("/api/v1/devices/pager-1/firmware").status_code == 401
        m = c.get("/api/v1/devices/pager-1/firmware", headers=h | PROXIED)
        assert m.status_code == 200 and m.json()["version"] == "0.2.0"
        img = c.get("/api/v1/devices/pager-1/firmware/image", headers=h | PROXIED)
        assert img.status_code == 200 and img.content == b"\xe9\x01\x02\x03"
        assert any(e["action"] == "firmware.download" for e in c.get("/api/v1/audit", headers=admin_h()).json())


def test_firmware_rejects_bad_manifest_or_size_mismatch(tmp_path, db_path):
    for manifest, image in ((MANIFEST | {"version": "latest"}, b"1234"), (MANIFEST, b"123")):
        d = tmp_path / f"case{len(image)}{manifest['version']}"
        d.mkdir()
        (d / "manifest.json").write_text(json.dumps(manifest))
        (d / "firmware.bin").write_bytes(image)
        with TestClient(create_app(make_settings(db_path, ota_dir=str(d)))) as c:
            r = c.post("/api/v1/devices", json={"id": f"p-{len(image)}"}, headers=admin_h())
            h = {"Authorization": f"Bearer {r.json()['token']}"}
            assert c.get(f"/api/v1/devices/p-{len(image)}/firmware", headers=h).status_code == 404
