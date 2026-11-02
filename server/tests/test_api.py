from __future__ import annotations

import threading
import time
from concurrent.futures import ThreadPoolExecutor

import pytest
from fastapi.testclient import TestClient

from app.config import Settings
from app.database.db import connect
from app.main import create_app
from app.services import messages as svc

from .conftest import ADMIN, INGEST, admin_h, ingest_h, make_settings


def send(client, **kw):
    payload = {"sender": "Ops", "subject": "Hello", "body": "World"} | kw
    return client.post("/api/v1/messages", json=payload, headers=ingest_h())


def sync(client, device, **params):
    r = client.get(f"/api/v1/devices/{device['id']}/sync", params=params, headers=device["headers"])
    assert r.status_code == 200, r.text
    return r.json()


# ------------------------------------------------------------- config / auth

def test_settings_reject_weak_or_shared_tokens(db_path):
    with pytest.raises(RuntimeError):
        make_settings(db_path, admin_token="short").validate()
    with pytest.raises(RuntimeError):
        make_settings(db_path, ingest_token=ADMIN).validate()


def test_health(client):
    r = client.get("/health")
    assert r.status_code == 200 and r.json()["status"] == "ok"


@pytest.mark.parametrize("method,path", [
    ("get", "/api/v1/messages"), ("get", "/api/v1/messages/1"), ("post", "/api/v1/messages/1/read"),
    ("post", "/api/v1/messages/1/ack"), ("delete", "/api/v1/messages/1"), ("get", "/api/v1/devices"),
])
def test_admin_endpoints_require_admin(client, method, path):
    assert getattr(client, method)(path).status_code == 401
    # the ingest token is not an admin token
    assert getattr(client, method)(path, headers=ingest_h()).status_code == 401


def test_create_requires_token(client):
    assert client.post("/api/v1/messages", json={"body": "x"}).status_code == 401
    bad = {"Authorization": "Bearer nope"}
    assert client.post("/api/v1/messages", json={"body": "x"}, headers=bad).status_code == 401
    assert client.post("/api/v1/messages", json={"body": "x"}, headers=admin_h()).status_code == 201


def test_device_token_is_scoped_to_device(client, device):
    r = client.post("/api/v1/devices", json={"id": "pager-2"}, headers=admin_h())
    other = {"Authorization": f"Bearer {r.json()['token']}"}
    assert client.get("/api/v1/devices/pager-1/sync", headers=other).status_code == 401
    assert client.get("/api/v1/devices/pager-1/sync").status_code == 401
    # device tokens cannot use the admin API
    assert client.get("/api/v1/messages", headers=device["headers"]).status_code == 401


def test_rotated_token_invalidates_old(client, device):
    r = client.post("/api/v1/devices/pager-1/token", headers=admin_h())
    assert r.status_code == 200
    assert client.get("/api/v1/devices/pager-1/sync", headers=device["headers"]).status_code == 401
    new = {"Authorization": f"Bearer {r.json()['token']}"}
    assert client.get("/api/v1/devices/pager-1/sync", headers=new).status_code == 200


def test_device_list_never_exposes_token_hash(client, device):
    r = client.get("/api/v1/devices", headers=admin_h())
    assert r.status_code == 200
    assert "token_hash" not in r.text


def test_auth_failures_are_rate_limited(db_path):
    with TestClient(create_app(make_settings(db_path, rate_limit_per_min=60))) as c:
        codes = [c.get("/api/v1/messages", headers={"Authorization": "Bearer bad"}).status_code
                 for _ in range(30)]
    assert 429 in codes


# ------------------------------------------------------------- messages

def test_create_and_get_message_utf8(client):
    r = send(client, sender="Jiří", subject="Příliš žluťoučký kůň", body="úpěl ďábelské ódy 🐴",
             priority="high", source="monitoring")
    assert r.status_code == 201
    m = r.json()["message"]
    assert r.json()["duplicate"] is False
    got = client.get(f"/api/v1/messages/{m['id']}", headers=admin_h()).json()
    assert got["subject"] == "Příliš žluťoučký kůň"
    assert got["priority"] == "high" and got["source"] == "monitoring"
    assert got["received_at"].endswith("Z")
    assert got["read_at"] is None and got["acknowledged_at"] is None and got["delivered_at"] is None


def test_duplicate_ingestion_is_idempotent(client):
    a = send(client, source="email", external_id="<msg-1@example.com>")
    b = send(client, source="email", external_id="<msg-1@example.com>", body="changed")
    assert a.status_code == 201 and b.status_code == 200
    assert b.json()["duplicate"] is True
    assert a.json()["message"]["id"] == b.json()["message"]["id"]
    assert b.json()["message"]["body"] == "World"
    # same external id from another source is a different message
    c = send(client, source="webhook", external_id="<msg-1@example.com>")
    assert c.status_code == 201


def test_idempotency_key_header(client):
    h = ingest_h() | {"Idempotency-Key": "retry-123"}
    a = client.post("/api/v1/messages", json={"body": "x"}, headers=h)
    b = client.post("/api/v1/messages", json={"body": "x"}, headers=h)
    assert a.json()["message"]["id"] == b.json()["message"]["id"]


def test_concurrent_duplicate_ingestion_creates_one(client):
    def go(_):
        return send(client, source="email", external_id="race-1").json()["message"]["id"]
    with ThreadPoolExecutor(8) as ex:
        ids = set(ex.map(go, range(16)))
    assert len(ids) == 1


@pytest.mark.parametrize("payload", [
    {"priority": "urgent"},
    {"source": "carrier-pigeon"},
    {"body": "x" * 4001},
    {"subject": "x" * 201},
    {"unknown": 1},
    {"created_at": "2026-11-02T10:00:00"},           # naive timestamp
    {"metadata": {"blob": "x" * 3000}},
])
def test_malformed_input_rejected(client, payload):
    r = client.post("/api/v1/messages", json=payload, headers=ingest_h())
    assert r.status_code == 422


def test_invalid_json_and_oversized_body(client):
    r = client.post("/api/v1/messages", content=b"{not json", headers=ingest_h() | {"Content-Type": "application/json"})
    assert r.status_code == 422
    r = client.post("/api/v1/messages", content=b"x" * (70 * 1024),
                    headers=ingest_h() | {"Content-Type": "application/json"})
    assert r.status_code == 413


def test_control_characters_stripped(client):
    m = send(client, body="a\x00b\x1bc\nd").json()["message"]
    assert m["body"] == "abc\nd"


def test_read_and_ack_are_distinct_and_idempotent(client):
    mid = send(client).json()["message"]["id"]
    r = client.post(f"/api/v1/messages/{mid}/read", headers=admin_h()).json()
    assert r["read_at"] and r["acknowledged_at"] is None
    seq = r["seq"]
    again = client.post(f"/api/v1/messages/{mid}/read", headers=admin_h()).json()
    assert again["seq"] == seq and again["read_at"] == r["read_at"]
    acked = client.post(f"/api/v1/messages/{mid}/ack", headers=admin_h()).json()
    assert acked["acknowledged_at"] and acked["read_at"] == r["read_at"]
    # ack directly also implies read
    mid2 = send(client).json()["message"]["id"]
    a2 = client.post(f"/api/v1/messages/{mid2}/ack", headers=admin_h()).json()
    assert a2["read_at"] and a2["acknowledged_at"]
    assert client.post("/api/v1/messages/9999/read", headers=admin_h()).status_code == 404


def test_list_filters_and_pagination(client):
    for i in range(5):
        send(client, source="email" if i % 2 else "system", subject=f"s{i}")
    r = client.get("/api/v1/messages", params={"source": "email"}, headers=admin_h()).json()
    assert [m["subject"] for m in r["messages"]] == ["s3", "s1"]
    page = client.get("/api/v1/messages", params={"limit": 2}, headers=admin_h()).json()
    assert len(page["messages"]) == 2 and page["next_before_id"]
    page2 = client.get("/api/v1/messages", params={"limit": 2, "before_id": page["next_before_id"]},
                       headers=admin_h()).json()
    assert page2["messages"][0]["id"] < page["messages"][-1]["id"]


def test_delete_creates_tombstone_without_content(client):
    mid = send(client, body="secret").json()["message"]["id"]
    assert client.delete(f"/api/v1/messages/{mid}", headers=admin_h()).status_code == 204
    assert client.get("/api/v1/messages", headers=admin_h()).json()["messages"] == []
    m = client.get(f"/api/v1/messages/{mid}", headers=admin_h()).json()
    assert m["deleted_at"] and m["body"] == ""
    assert client.delete("/api/v1/messages/9999", headers=admin_h()).status_code == 404


# ------------------------------------------------------------- sync protocol

def test_inject_and_sync_roundtrip(client, device):
    s0 = sync(client, device)
    assert s0["schema"] == 1 and s0["items"] == [] and s0["unread"] == 0
    mid = send(client, sender="Grafana", subject="CPU high", body="node-3 at 97%",
               priority="critical", source="monitoring").json()["message"]["id"]
    s1 = sync(client, device, cursor=s0["cursor"], epoch=s0["epoch"])
    assert s1["reset"] is False and s1["unread"] == 1
    (item,) = s1["items"]
    assert item == {"id": mid, "seq": item["seq"], "deleted": False, "src": "monitoring", "from": "Grafana",
                    "subj": "CPU high", "body": "node-3 at 97%", "trunc": False, "pri": 3,
                    "ts": item["ts"], "read": 0, "ack": 0}
    assert s1["cursor"] == item["seq"]
    # nothing new: same cursor, no items
    s2 = sync(client, device, cursor=s1["cursor"], epoch=s1["epoch"])
    assert s2["items"] == [] and s2["cursor"] == s1["cursor"]


def test_sync_pagination_is_stable_and_complete(client, device):
    ids = [send(client, subject=f"m{i}").json()["message"]["id"] for i in range(23)]
    cursor, epoch, got = 0, None, []
    for _ in range(10):
        s = sync(client, device, cursor=cursor, limit=5, **({"epoch": epoch} if epoch else {}))
        got += [i["id"] for i in s["items"]]
        cursor, epoch = s["cursor"], s["epoch"]
        if not s["has_more"]:
            break
    assert got == ids


def test_messages_arriving_during_sync_are_not_lost(client, device):
    for i in range(6):
        send(client, subject=f"a{i}")
    s = sync(client, device, limit=3)
    # new message arrives between pages, plus an update to an already-synced one
    late = send(client, subject="late").json()["message"]["id"]
    client.post(f"/api/v1/messages/{s['items'][0]['id']}/read", headers=admin_h())
    seen = {i["id"]: i for i in s["items"]}
    cursor = s["cursor"]
    while True:
        s = sync(client, device, cursor=cursor, epoch=s["epoch"], limit=3)
        for i in s["items"]:
            seen[i["id"]] = i
        cursor = s["cursor"]
        if not s["has_more"]:
            break
    assert late in seen and len(seen) == 7
    first = min(seen)
    assert seen[first]["read"] > 0


def test_sync_reports_updates_and_tombstones(client, device):
    mid = send(client).json()["message"]["id"]
    s = sync(client, device)
    client.post(f"/api/v1/messages/{mid}/ack", headers=admin_h())
    s2 = sync(client, device, cursor=s["cursor"], epoch=s["epoch"])
    assert s2["items"][0]["ack"] > 0 and s2["items"][0]["read"] > 0
    client.delete(f"/api/v1/messages/{mid}", headers=admin_h())
    s3 = sync(client, device, cursor=s2["cursor"], epoch=s2["epoch"])
    assert s3["items"] == [{"id": mid, "seq": s3["cursor"], "deleted": True, "src": "", "from": "",
                            "subj": "", "body": "", "trunc": False, "pri": 1, "ts": 0, "read": 0, "ack": 0}]


def test_full_sync_skips_tombstones(client, device):
    a = send(client).json()["message"]["id"]
    b = send(client).json()["message"]["id"]
    client.delete(f"/api/v1/messages/{a}", headers=admin_h())
    s = sync(client, device)
    assert [i["id"] for i in s["items"]] == [b]


def test_sync_truncates_body_on_utf8_boundary(client, device):
    send(client, body="č" * 200)  # 400 bytes
    item = sync(client, device, max_body=101)["items"][0]
    assert item["trunc"] is True and item["body"] == "č" * 50


def test_epoch_mismatch_forces_reset(client, device):
    send(client)
    s = sync(client, device)
    r = sync(client, device, cursor=s["cursor"], epoch="deadbeef")
    assert r["reset"] is True and len(r["items"]) == 1


def test_cursor_ahead_of_server_forces_reset(client, device):
    send(client)
    r = sync(client, device, cursor=10_000)
    assert r["reset"] is True and len(r["items"]) == 1


def test_expired_cursor_after_purge_forces_reset(client, device, db_path):
    keep = send(client).json()["message"]["id"]
    gone = send(client).json()["message"]["id"]
    s = sync(client, device)
    client.delete(f"/api/v1/messages/{gone}", headers=admin_h())
    conn = connect(db_path)
    try:
        svc.purge(conn, retention_days=30, tombstone_days=-1)  # purge tombstones immediately
    finally:
        conn.close()
    r = sync(client, device, cursor=s["cursor"], epoch=s["epoch"])
    assert r["reset"] is True and [i["id"] for i in r["items"]] == [keep]


def test_expired_messages_become_tombstones(client, device, db_path):
    mid = send(client, expires_at="2020-01-01T00:00:00Z").json()["message"]["id"]
    conn = connect(db_path)
    try:
        assert svc.purge(conn, 30, 7)["expired"] == 1
    finally:
        conn.close()
    assert client.get(f"/api/v1/messages/{mid}", headers=admin_h()).json()["deleted_at"]


def test_long_poll_returns_when_message_arrives(client, device):
    s = sync(client, device)
    result = {}

    def poll():
        t0 = time.monotonic()
        result["resp"] = sync(client, device, cursor=s["cursor"], epoch=s["epoch"], wait=4)
        result["elapsed"] = time.monotonic() - t0

    th = threading.Thread(target=poll)
    th.start()
    time.sleep(0.5)
    send(client, subject="wake up")
    th.join(6)
    assert result["resp"]["items"][0]["subj"] == "wake up"
    assert result["elapsed"] < 2.5


def test_long_poll_times_out_empty(client, device):
    s = sync(client, device)
    t0 = time.monotonic()
    r = sync(client, device, cursor=s["cursor"], epoch=s["epoch"], wait=1)
    assert r["items"] == [] and 0.8 < time.monotonic() - t0 < 3


# ------------------------------------------------------------- device events

def events(client, device, body):
    return client.post(f"/api/v1/devices/{device['id']}/events", json=body, headers=device["headers"])


def test_device_read_ack_events_are_idempotent(client, device):
    mid = send(client).json()["message"]["id"]
    s = sync(client, device)
    body = {"ack_cursor": s["cursor"], "events": [
        {"id": "b1-1", "type": "read", "message_id": mid, "at": int(time.time()) - 60},
        {"id": "b1-2", "type": "ack", "message_id": mid},
        {"id": "b1-3", "type": "read", "message_id": 999},
    ], "status": {"rssi": -60, "heap": 120000, "fw": "0.1.0"}}
    r = events(client, device, body).json()
    assert r == {"accepted": ["b1-1", "b1-2"], "duplicates": [], "unknown_messages": ["b1-3"]}
    m = client.get(f"/api/v1/messages/{mid}", headers=admin_h()).json()
    assert m["read_at"] and m["acknowledged_at"] and m["delivered_at"]
    seq_after = m["seq"]
    # retry of the same batch (e.g. response lost) changes nothing
    r2 = events(client, device, body).json()
    assert sorted(r2["duplicates"]) == ["b1-1", "b1-2", "b1-3"] and r2["accepted"] == []
    assert client.get(f"/api/v1/messages/{mid}", headers=admin_h()).json()["seq"] == seq_after
    dev = client.get("/api/v1/devices", headers=admin_h()).json()[0]
    assert dev["acked_cursor"] == s["cursor"] and dev["status"]["rssi"] == -60


def test_device_clock_in_future_is_clamped(client, device):
    mid = send(client).json()["message"]["id"]
    events(client, device, {"events": [{"id": "x1", "type": "read", "message_id": mid, "at": 4_000_000_000}]})
    m = client.get(f"/api/v1/messages/{mid}", headers=admin_h()).json()
    assert m["read_at"] < "2100"


def test_ack_cursor_cannot_exceed_server_seq_or_go_back(client, device):
    send(client)
    events(client, device, {"ack_cursor": 10_000})
    assert client.get("/api/v1/devices", headers=admin_h()).json()[0]["acked_cursor"] == 0
    events(client, device, {"ack_cursor": 1})
    events(client, device, {"ack_cursor": 0})
    assert client.get("/api/v1/devices", headers=admin_h()).json()[0]["acked_cursor"] == 1


def test_malformed_device_events_rejected(client, device):
    assert events(client, device, {"events": [{"id": "bad id!", "type": "read", "message_id": 1}]}).status_code == 422
    assert events(client, device, {"events": [{"id": "a", "type": "delete", "message_id": 1}]}).status_code == 422
    too_many = [{"id": f"e{i}", "type": "read", "message_id": 1} for i in range(51)]
    assert events(client, device, {"events": too_many}).status_code == 422


# ------------------------------------------------------------- persistence

def test_data_survives_server_restart(db_path):
    settings = make_settings(db_path)
    with TestClient(create_app(settings)) as c:
        token = c.post("/api/v1/devices", json={"id": "pager-1"}, headers=admin_h()).json()["token"]
        dh = {"Authorization": f"Bearer {token}"}
        c.post("/api/v1/messages", json={"body": "persist me"}, headers=ingest_h())
        s = c.get("/api/v1/devices/pager-1/sync", headers=dh).json()
    with TestClient(create_app(settings)) as c:
        s2 = c.get("/api/v1/devices/pager-1/sync",
                   params={"cursor": s["cursor"], "epoch": s["epoch"]}, headers=dh).json()
        assert s2["reset"] is False and s2["epoch"] == s["epoch"] and s2["unread"] == 1
        c.post("/api/v1/messages", json={"body": "after restart"}, headers=ingest_h())
        s3 = c.get("/api/v1/devices/pager-1/sync",
                   params={"cursor": s["cursor"], "epoch": s["epoch"]}, headers=dh).json()
        assert [i["body"] for i in s3["items"]] == ["after restart"]
        assert s3["cursor"] > s["cursor"]


def test_concurrent_writes_get_unique_increasing_seqs(client):
    with ThreadPoolExecutor(8) as ex:
        seqs = list(ex.map(lambda i: send(client, subject=str(i)).json()["message"]["seq"], range(40)))
    assert len(set(seqs)) == 40
