from __future__ import annotations

from .conftest import admin_h, ingest_h


def test_generic_webhook_creates_and_dedupes(client):
    payload = {"title": "Deploy done", "message": "v1.2.3 is live", "sender": "CI", "priority": "high",
               "id": "deploy-123", "extra": {"ignored": True}}
    r1 = client.post("/api/v1/webhooks/generic", json=payload, headers=ingest_h())
    r2 = client.post("/api/v1/webhooks/generic", json=payload, headers=ingest_h())
    assert r1.status_code == 201 and r2.status_code == 200
    assert r1.json()["id"] == r2.json()["id"] and r2.json()["duplicate"] is True
    m = client.get(f"/api/v1/messages/{r1.json()['id']}", headers=admin_h()).json()
    assert m["source"] == "webhook" and m["priority"] == "high" and m["subject"] == "Deploy done"


def test_generic_webhook_clips_long_fields_and_requires_content(client):
    r = client.post("/api/v1/webhooks/generic", json={"title": "t" * 900, "message": "m" * 9000},
                    headers=ingest_h())
    assert r.status_code == 201
    m = client.get(f"/api/v1/messages/{r.json()['id']}", headers=admin_h()).json()
    assert len(m["subject"]) == 200 and len(m["body"]) == 4000 and m["body"].endswith("…")
    assert client.post("/api/v1/webhooks/generic", json={}, headers=ingest_h()).status_code == 422
    assert client.post("/api/v1/webhooks/generic", json={"title": "x"}).status_code == 401


def test_alertmanager_webhook(client):
    hook = {
        "version": "4", "status": "firing", "receiver": "pager", "groupKey": "{}:{alertname=\"DiskFull\"}",
        "commonLabels": {"alertname": "DiskFull", "severity": "critical"},
        "alerts": [{"status": "firing", "labels": {"instance": "nas:9100"}, "fingerprint": "abc",
                    "startsAt": "2026-11-02T10:00:00Z", "annotations": {"summary": "/volume1 at 98%"}}],
    }
    r = client.post("/api/v1/webhooks/alertmanager", json=hook, headers=ingest_h())
    assert r.status_code == 201
    again = client.post("/api/v1/webhooks/alertmanager", json=hook, headers=ingest_h())
    assert again.json()["duplicate"] is True
    m = client.get(f"/api/v1/messages/{r.json()['id']}", headers=admin_h()).json()
    assert m["priority"] == "critical" and m["source"] == "monitoring"
    assert m["subject"] == "[FIRING] DiskFull" and "/volume1 at 98%" in m["body"]
    resolved = client.post("/api/v1/webhooks/alertmanager", json=hook | {"status": "resolved"}, headers=ingest_h())
    assert resolved.status_code == 201
    assert client.get(f"/api/v1/messages/{resolved.json()['id']}", headers=admin_h()).json()["priority"] == "low"


def test_admin_page_served_with_csp(client):
    r = client.get("/admin")
    assert r.status_code == 200 and "SmartPager Console" in r.text
    assert "frame-ancestors 'none'" in r.headers["content-security-policy"]


# ------------------------------------------------------------- IMAP mapping (no network)

from app.integrations.imap_connector import sender_allowed, to_payload  # noqa: E402

RAW = (
    "From: =?utf-8?q?Jana_Nov=C3=A1kov=C3=A1?= <jana@example.cz>\r\n"
    "To: me@example.com\r\nSubject: =?utf-8?q?Sch=C5=AFzka_z=C3=ADtra?=\r\n"
    "Date: Mon, 02 Nov 2026 13:05:00 +0100\r\nMessage-ID: <abc123@example.cz>\r\nX-Priority: 1\r\n"
    "MIME-Version: 1.0\r\nContent-Type: multipart/alternative; boundary=b\r\n\r\n"
    "--b\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: 8bit\r\n\r\n"
    "Ahoj,\r\n\r\n\r\nzítra v 10:00.\r\n--b\r\nContent-Type: text/html; charset=utf-8\r\n\r\n<p>ignored</p>\r\n--b--\r\n"
).encode()


def test_imap_payload_mapping():
    p = to_payload(RAW)
    assert p["sender"] == "Jana Nováková" and p["subject"] == "Schůzka zítra"
    assert p["external_id"] == "<abc123@example.cz>" and p["priority"] == "high"
    assert p["body"] == "Ahoj,\nzítra v 10:00." and p["created_at"].startswith("2026-11-02T12:05:00")
    assert p["metadata"]["from"] == "jana@example.cz"


def test_imap_html_only_and_missing_id():
    raw = b"From: a@b.c\r\nSubject: x\r\nContent-Type: text/html\r\n\r\n<style>p{}</style><p>Hello <b>world</b></p>"
    p = to_payload(raw)
    assert p["body"] == "Hello world" and p["external_id"].startswith("sha256:")


def test_imap_sender_filters():
    p = to_payload(RAW)
    assert sender_allowed(p, [], [])
    assert sender_allowed(p, ["@example.cz"], [])
    assert not sender_allowed(p, ["@other.com"], [])
    assert not sender_allowed(p, [], ["jana@"])


def test_imap_payload_is_accepted_by_api(client):
    r = client.post("/api/v1/messages", json=to_payload(RAW), headers=ingest_h())
    assert r.status_code == 201
