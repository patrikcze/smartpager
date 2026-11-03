"""Messages addressed to specific pagers: filtering, delivery, token defaults, migration."""
from __future__ import annotations

import time

import pytest
from fastapi.testclient import TestClient

from app import cli
from app.database import db
from app.database.db import connect, migrate, write_tx
from app.main import create_app

from .conftest import admin_h, ingest_h, make_settings


def add_device(client, device_id):
    r = client.post("/api/v1/devices", json={"id": device_id}, headers=admin_h())
    assert r.status_code == 201, r.text
    return {"id": device_id, "headers": {"Authorization": f"Bearer {r.json()['token']}"}}


@pytest.fixture
def pagers(client):
    return add_device(client, "pager-1"), add_device(client, "pager-2")


def send(client, headers=None, **kw):
    payload = {"sender": "Ops", "subject": "Hello"} | kw
    return client.post("/api/v1/messages", json=payload, headers=headers or ingest_h())


def sync(client, device, **params):
    r = client.get(f"/api/v1/devices/{device['id']}/sync", params=params, headers=device["headers"])
    assert r.status_code == 200, r.text
    return r.json()


def ids(page):
    return [i["id"] for i in page["items"]]


def test_without_to_every_pager_receives(client, pagers):
    m = send(client).json()["message"]
    assert m["recipients"] is None
    for p in pagers:
        assert ids(sync(client, p)) == [m["id"]]


def test_addressed_message_reaches_only_its_recipients(client, pagers):
    p1, p2 = pagers
    everyone = send(client, subject="all").json()["message"]["id"]
    only2 = send(client, subject="two", to=["pager-2"]).json()["message"]
    assert only2["recipients"] == ["pager-2"]
    s1, s2 = sync(client, p1), sync(client, p2)
    assert ids(s1) == [everyone] and s1["unread"] == 1
    assert ids(s2) == [everyone, only2["id"]] and s2["unread"] == 2
    # both see the same cursor: pager-1 skipped the change that wasn't for it
    assert s1["cursor"] == s2["cursor"]


def test_to_is_deduplicated_and_validated(client, pagers):
    r = send(client, to=["pager-2", "pager-1", "pager-2"])
    assert r.status_code == 201 and r.json()["message"]["recipients"] == ["pager-1", "pager-2"]
    r = send(client, to=["pager-9"])
    assert r.status_code == 422 and "pager-9" in r.json()["detail"]
    assert send(client, to=[]).status_code == 422
    assert send(client, to=["Bad ID"]).status_code == 422
    assert send(client, to="pager-1").status_code == 422  # must be a list


def test_cursor_moves_past_other_pagers_messages(client, pagers):
    p1, _ = pagers
    send(client)
    first = sync(client, p1)
    for _ in range(15):  # more than one page of changes for someone else
        send(client, to=["pager-2"])
    page = sync(client, p1, cursor=first["cursor"], epoch=first["epoch"], limit=10)
    assert page["items"] == [] and page["has_more"] is False
    assert page["cursor"] == client.get("/health").json()["seq"]


def test_long_poll_ignores_messages_for_other_pagers(client, pagers):
    p1, p2 = pagers
    send(client)
    base = sync(client, p1)
    send(client, to=["pager-2"])
    started = time.monotonic()
    page = sync(client, p1, cursor=base["cursor"], epoch=base["epoch"], wait=1)
    assert page["items"] == [] and time.monotonic() - started >= 0.9
    # the recipient gets it immediately
    started = time.monotonic()
    page = sync(client, p2, cursor=base["cursor"], epoch=base["epoch"], wait=5)
    assert len(page["items"]) == 1 and time.monotonic() - started < 1


def test_tombstone_and_state_changes_go_only_to_recipients(client, pagers):
    p1, p2 = pagers
    send(client)
    c1, c2 = sync(client, p1), sync(client, p2)
    mid = send(client, to=["pager-2"]).json()["message"]["id"]
    assert client.post(f"/api/v1/messages/{mid}/read", headers=admin_h()).status_code == 200
    assert client.delete(f"/api/v1/messages/{mid}", headers=admin_h()).status_code == 204
    assert sync(client, p1, cursor=c1["cursor"], epoch=c1["epoch"])["items"] == []
    items = sync(client, p2, cursor=c2["cursor"], epoch=c2["epoch"])["items"]
    assert [(i["id"], i["deleted"]) for i in items] == [(mid, True)]


def test_pager_cannot_read_or_ack_messages_not_addressed_to_it(client, pagers):
    p1, _ = pagers
    mid = send(client, to=["pager-2"]).json()["message"]["id"]
    r = client.post("/api/v1/devices/pager-1/events", headers=p1["headers"],
                    json={"events": [{"id": "e1", "type": "ack", "message_id": mid}]})
    assert r.json()["unknown_messages"] == ["e1"]
    m = client.get(f"/api/v1/messages/{mid}", headers=admin_h()).json()
    assert m["read_at"] is None and m["acknowledged_at"] is None


def test_delivery_is_tracked_per_pager(client, pagers):
    p1, p2 = pagers
    everyone = send(client).json()["message"]["id"]
    only2 = send(client, to=["pager-2"]).json()["message"]["id"]
    s1 = sync(client, p1)
    client.post("/api/v1/devices/pager-1/events", headers=p1["headers"], json={"ack_cursor": s1["cursor"]})
    m = client.get(f"/api/v1/messages/{everyone}", headers=admin_h()).json()
    assert list(m["delivered_to"]) == ["pager-1"] and m["delivered_at"]
    m2 = client.get(f"/api/v1/messages/{only2}", headers=admin_h()).json()
    assert m2["delivered_to"] == {} and m2["delivered_at"] is None  # pager-1 confirmed past it

    s2 = sync(client, p2)
    client.post("/api/v1/devices/pager-2/events", headers=p2["headers"], json={"ack_cursor": s2["cursor"]})
    listed = {m["id"]: m for m in client.get("/api/v1/messages", headers=admin_h()).json()["messages"]}
    assert sorted(listed[everyone]["delivered_to"]) == ["pager-1", "pager-2"]
    assert list(listed[only2]["delivered_to"]) == ["pager-2"]


def test_admin_list_filters_by_pager(client, pagers):
    send(client, subject="all")
    send(client, subject="one", to=["pager-1"])
    send(client, subject="two", to=["pager-2"])
    subjects = lambda q: sorted(m["subject"] for m in client.get(
        "/api/v1/messages", params=q, headers=admin_h()).json()["messages"])
    assert subjects({}) == ["all", "one", "two"]
    assert subjects({"device": "pager-2"}) == ["all", "two"]


def test_token_default_recipients_and_override(client, pagers):
    r = client.post("/api/v1/tokens", json={"name": "grafana", "recipients": ["pager-2"]}, headers=admin_h())
    assert r.status_code == 201 and r.json()["token_info"]["recipients"] == ["pager-2"]
    tok = {"Authorization": f"Bearer {r.json()['token']}"}
    assert send(client, tok, subject="a").json()["message"]["recipients"] == ["pager-2"]
    assert send(client, tok, subject="b", to=["pager-1"]).json()["message"]["recipients"] == ["pager-1"]

    tid = r.json()["token_info"]["id"]
    r = client.patch(f"/api/v1/tokens/{tid}", json={"recipients": None}, headers=admin_h())
    assert r.status_code == 200 and r.json()["recipients"] is None
    assert send(client, tok, subject="c").json()["message"]["recipients"] is None
    # same token still works after re-routing; unknown pagers are rejected
    assert client.patch(f"/api/v1/tokens/{tid}", json={"recipients": ["nope-1"]}, headers=admin_h()).status_code == 422
    assert client.patch("/api/v1/tokens/999", json={"recipients": None}, headers=admin_h()).status_code == 404
    assert client.post("/api/v1/tokens", json={"name": "x2", "recipients": ["nope-1"]}, headers=admin_h()).status_code == 422
    actions = [e["action"] for e in client.get("/api/v1/audit", headers=admin_h()).json()]
    assert "token.recipients" in actions


def test_webhooks_accept_recipients(client, pagers):
    r = client.post("/api/v1/webhooks/generic?to=pager-2", json={"title": "q"}, headers=ingest_h())
    assert r.status_code == 201
    r2 = client.post("/api/v1/webhooks/generic", json={"title": "b", "to": "pager-1, pager-2"}, headers=ingest_h())
    r3 = client.post("/api/v1/webhooks/generic", json={"title": "l", "to": ["pager-1"]}, headers=ingest_h())
    am = {"status": "firing", "groupKey": "g", "commonLabels": {"alertname": "Disk"}, "alerts": [{"fingerprint": "f"}]}
    r4 = client.post("/api/v1/webhooks/alertmanager?to=pager-1", json=am, headers=ingest_h())
    got = lambda r: client.get(f"/api/v1/messages/{r.json()['id']}", headers=admin_h()).json()["recipients"]
    assert got(r) == ["pager-2"] and got(r2) == ["pager-1", "pager-2"] and got(r3) == ["pager-1"]
    assert got(r4) == ["pager-1"]
    bad = client.post("/api/v1/webhooks/generic?to=pager-7", json={"title": "x"}, headers=ingest_h())
    assert bad.status_code == 422
    assert client.post("/api/v1/webhooks/generic", json={"title": "x", "to": "BAD"}, headers=ingest_h()).status_code == 422


def test_cli_routes_tokens(client, pagers, db_path, monkeypatch, capsys):
    monkeypatch.setenv("SMARTPAGER_DB", db_path)
    monkeypatch.setenv("SMARTPAGER_ADMIN_TOKEN", "x" * 32)
    assert cli.main(["ingest-create", "nas", "--to", "pager-2"]) == 0
    assert "to:     pager-2" in capsys.readouterr().out
    assert cli.main(["ingest-route", "nas", "--to", "pager-1,pager-2"]) == 0
    assert cli.main(["ingest-route", "nas", "--to", "pager-9"]) == 1
    assert cli.main(["ingest-route", "nas", "--all"]) == 0
    toks = client.get("/api/v1/tokens", headers=admin_h()).json()
    assert next(t for t in toks if t["name"] == "nas")["recipients"] is None


def test_migration_from_v2_keeps_existing_messages_for_everyone(tmp_path, monkeypatch):
    path = str(tmp_path / "v2.db")
    monkeypatch.setattr(db, "SCHEMA_VERSION", 2)  # build a database as the previous release did
    migrate(path)
    conn = connect(path)
    with write_tx(conn):
        conn.execute(
            """INSERT INTO messages(seq, source, sender, subject, created_at, received_at, delivered_at)
               VALUES (?, 'manual', 'Ops', 'old', '2026-11-01T10:00:00Z', '2026-11-01T10:00:00Z',
                       '2026-11-01T10:00:05Z')""", (db.next_seq(conn),))
    conn.close()
    monkeypatch.undo()

    with TestClient(create_app(make_settings(path))) as client:  # migrates to v3 on start
        old = client.get("/api/v1/messages", headers=admin_h()).json()["messages"][0]
        assert old["recipients"] is None and old["delivered_at"] and old["delivered_to"] == {}
        p1, p2 = add_device(client, "pager-1"), add_device(client, "pager-2")
        assert ids(sync(client, p1)) == [old["id"]] and ids(sync(client, p2)) == [old["id"]]
    migrate(path)  # idempotent
    assert connect(path).execute("PRAGMA user_version").fetchone()[0] == 3
